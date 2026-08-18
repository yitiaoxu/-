/**
 * rknn_infer_one CLI ¡ª self-contained RKNN model validation.
 *
 * Drop new .rknn into weights/, then:
 *   ./quick_verify.sh
 *   ./quick_verify.sh --limit 5 --no-save
 */
#include "plate/plate_rknn_pipeline.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <direct.h>
#include <windows.h>
#endif

namespace {

struct CliArgs {
    std::string detect_rknn;
    std::string rec_rknn;
    std::string image;
    std::string out;
    std::string image_dir;
    std::string out_dir;
    plate_rknn::PlateRknnConfig cfg;
    bool recursive = false;
    bool no_save = false;
    bool random_sample = false;
    int limit = 0;
};

struct JobResult {
    std::string image;
    std::string out;
    int code = 1;
    double ms = 0.0;
    std::string plates;
    plate_rknn::PlateTimings timings;
    double ms_read = 0.;
    double ms_write = 0.;
    bool has_timings = false;
};

std::string findFirstExisting(const char* candidates[], size_t n) {
    for (size_t i = 0; i < n; i++) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return "";
}

bool envFlagEnabled(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}

bool resolveModelPaths(std::string& detect_path, std::string& rec_path, bool& use_int8) {
    const char* detect_int8[] = {
        "./weights/plate_detect_int8.rknn",
        "./weights/plate_detect_i8.rknn",
        "./plate_detect_int8.rknn",
    };
    const char* rec_int8[] = {
        "./weights/plate_rec_color_int8.rknn",
        "./weights/plate_rec_i8.rknn",
        "./plate_rec_color_int8.rknn",
    };
    const char* detect_fp[] = {
        "./weights/plate_detect_fp.rknn",
        "./plate_detect_fp.rknn",
    };
    const char* rec_fp[] = {
        "./weights/plate_rec_fp.rknn",
        "./plate_rec_fp.rknn",
    };

    use_int8 = false;
    if (!envFlagEnabled("PLATE_USE_FP")) {
        detect_path = findFirstExisting(detect_int8, 3);
        rec_path = findFirstExisting(rec_int8, 3);
        if (!detect_path.empty() && !rec_path.empty()) {
            use_int8 = true;
            return true;
        }
    }

    detect_path = findFirstExisting(detect_fp, 3);
    rec_path = findFirstExisting(rec_fp, 3);
    return !detect_path.empty() && !rec_path.empty();
}

bool applyAutoDefaults(CliArgs& a, bool& use_int8) {
    if (a.detect_rknn.empty() || a.rec_rknn.empty()) {
        if (!resolveModelPaths(a.detect_rknn, a.rec_rknn, use_int8)) {
            std::cerr << "[ERROR] no RKNN models found. Put into weights/:\n"
                      << "  INT8: plate_detect_int8.rknn + plate_rec_color_int8.rknn\n"
                      << "  FP:   plate_detect_fp.rknn + plate_rec_fp.rknn\n"
                      << "  Or pass --detect-rknn / --rec-rknn explicitly.\n"
                      << "  Force FP: PLATE_USE_FP=1\n";
            return false;
        }
    } else {
        use_int8 = a.detect_rknn.find("int8") != std::string::npos ||
                   a.detect_rknn.find("_i8") != std::string::npos;
    }

    if (a.cfg.font.empty()) {
        const char* font_candidates[] = {"./fonts/platech.ttf"};
        a.cfg.font = findFirstExisting(font_candidates, 1);
    }

    if (a.cfg.input_mode.empty() || a.cfg.input_mode == "auto")
        a.cfg.input_mode = "uint8_nhwc";

    if (!a.cfg.profile && envFlagEnabled("PLATE_PROFILE")) a.cfg.profile = true;

    std::cout << "[model] detect=" << a.detect_rknn << "\n"
              << "[model] rec=" << a.rec_rknn << "\n"
              << "[model] quant=" << (use_int8 ? "int8" : "fp")
              << " input_mode=" << a.cfg.input_mode;
    if (a.cfg.profile) std::cout << " profile=on";
    std::cout << "\n";
    return true;
}

std::string currentTimestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_buf);
    return buf;
}

std::string withTimestampInFilename(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + "_" + currentTimestamp();
    return path.substr(0, dot) + "_" + currentTimestamp() + path.substr(dot);
}

std::string batchOutRoot() {
    return std::string("output/verify_") + currentTimestamp();
}

void ensureParentDirs(const std::string& file_path) {
    size_t pos = file_path.find_last_of("/\\");
    if (pos == std::string::npos) return;
    std::string dir = file_path.substr(0, pos);
    std::string built;
    for (size_t i = 0; i < dir.size(); i++) {
        built.push_back(dir[i]);
        if (dir[i] == '/' || dir[i] == '\\') {
#ifndef _WIN32
            if (built.size() > 1) mkdir(built.c_str(), 0755);
#else
            _mkdir(built.c_str());
#endif
        }
    }
#ifndef _WIN32
    mkdir(dir.c_str(), 0755);
#else
    _mkdir(dir.c_str());
#endif
}

bool hasImageExt(const std::string& name) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".webp";
}

void collectImages(const std::string& dir, bool recursive, std::vector<std::string>& out) {
#ifndef _WIN32
    struct DirJob {
        std::string path;
    };
    std::vector<DirJob> stack;
    stack.push_back({dir});
    while (!stack.empty()) {
        DirJob job = stack.back();
        stack.pop_back();
        DIR* d = opendir(job.path.c_str());
        if (!d) continue;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            std::string full = job.path + "/" + ent->d_name;
            struct stat st;
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                if (recursive) stack.push_back({full});
            } else if (S_ISREG(st.st_mode) && hasImageExt(ent->d_name)) {
                out.push_back(full);
            }
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
#else
    (void)recursive;
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && hasImageExt(fd.cFileName))
            out.push_back(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
#endif
}

void applyImageLimit(std::vector<std::string>& imgs, int limit, bool random_sample) {
    if (limit <= 0 || (int)imgs.size() <= limit) return;
    if (random_sample) {
        std::mt19937 gen(static_cast<unsigned>(std::time(nullptr)));
        std::shuffle(imgs.begin(), imgs.end(), gen);
        std::cout << "[INFO] random sample " << limit << " from " << imgs.size() << " images\n";
    } else {
        std::cout << "[INFO] take first " << limit << " of " << imgs.size() << " images\n";
    }
    imgs.resize(static_cast<size_t>(limit));
}

std::string joinPlates(const std::vector<plate_rknn::PlateItem>& items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); i++) {
        if (i) oss << "; ";
        oss << items[i].plate_no << "(" << items[i].plate_color << ")";
    }
    return oss.str();
}

void writeSummaryCsv(const std::string& path, const std::vector<JobResult>& results,
                     bool with_timings) {
    ensureParentDirs(path);
    std::ofstream ofs(path);
    if (!ofs) {
        std::cerr << "[WARN] cannot write summary: " << path << std::endl;
        return;
    }
    if (with_timings) {
        ofs << "image,exit_code,plates,total_ms,read_ms,letterbox_ms,det_npu_ms,det_out_ms,"
               "det_decode_ms,det_post_ms,det_input_ms,detect_ms,rec_roi_ms,rec_npu_ms,rec_ms,"
               "draw_ms,write_ms,infer_wall_ms,process_wall_ms,out\n";
        for (const JobResult& r : results) {
            const auto& t = r.timings;
            ofs << '"' << r.image << "\"," << r.code << ",\"" << r.plates << "\","
                << std::fixed << std::setprecision(2) << r.ms << "," << r.ms_read << ","
                << t.letterbox << "," << t.det_npu << "," << t.det_output << "," << t.det_decode
                << "," << t.det_post << "," << t.det_input << "," << t.detect_total << ","
                << t.rec_roi << "," << t.rec_npu << "," << t.rec_total << "," << t.draw << ","
                << r.ms_write << "," << t.infer_wall << "," << t.process_wall << ",\"" << r.out
                << "\"\n";
        }
    } else {
        ofs << "image,exit_code,plates,ms,out\n";
        for (const JobResult& r : results) {
            ofs << '"' << r.image << "\"," << r.code << ",\"" << r.plates << "\","
                << std::fixed << std::setprecision(1) << r.ms << ",\"" << r.out << "\"\n";
        }
    }
    std::cout << "[INFO] summary -> " << path << std::endl;
}

void printBatchSummary(const std::vector<JobResult>& results, double total_ms) {
    int ok = 0, no_plate = 0, err = 0;
    for (const JobResult& r : results) {
        if (r.code == 0) ok++;
        else if (r.code == 2) no_plate++;
        else err++;
    }
    std::cout << "\n========== verify summary ==========\n"
              << "  total:    " << results.size() << "\n"
              << "  ok:       " << ok << "\n"
              << "  no_plate: " << no_plate << "\n"
              << "  error:    " << err << "\n"
              << "  wall:     " << std::fixed << std::setprecision(0) << total_ms << " ms\n";
    if (!results.empty())
        std::cout << "  avg:      " << std::setprecision(1) << (total_ms / results.size()) << " ms/img\n";
    std::cout << "====================================\n";
}

bool parseArgs(int argc, char** argv, CliArgs& a) {
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << std::endl;
                return nullptr;
            }
            return argv[++i];
        };
        if (k == "--detect-rknn") {
            const char* v = need("--detect-rknn");
            if (!v) return false;
            a.detect_rknn = v;
        } else if (k == "--rec-rknn") {
            const char* v = need("--rec-rknn");
            if (!v) return false;
            a.rec_rknn = v;
        } else if (k == "--image") {
            const char* v = need("--image");
            if (!v) return false;
            a.image = v;
        } else if (k == "--out") {
            const char* v = need("--out");
            if (!v) return false;
            a.out = v;
        } else if (k == "--img-size") {
            const char* v = need("--img-size");
            if (!v) return false;
            a.cfg.img_size = atoi(v);
        } else if (k == "--conf") {
            const char* v = need("--conf");
            if (!v) return false;
            a.cfg.conf = (float)atof(v);
        } else if (k == "--iou") {
            const char* v = need("--iou");
            if (!v) return false;
            a.cfg.iou = (float)atof(v);
        } else if (k == "--input-mode") {
            const char* v = need("--input-mode");
            if (!v) return false;
            a.cfg.input_mode = v;
        } else if (k == "--rec-input-mode") {
            const char* v = need("--rec-input-mode");
            if (!v) return false;
            a.cfg.rec_input_mode = v;
        } else if (k == "--debug") {
            a.cfg.debug = true;
        } else if (k == "--profile") {
            a.cfg.profile = true;
        } else if (k == "--topk") {
            const char* v = need("--topk");
            if (!v) return false;
            a.cfg.topk = atoi(v);
        } else if (k == "--left-pad") {
            const char* v = need("--left-pad");
            if (!v) return false;
            a.cfg.left_pad = (float)atof(v);
        } else if (k == "--viz-margin") {
            const char* v = need("--viz-margin");
            if (!v) return false;
            a.cfg.viz_margin = (float)atof(v);
        } else if (k == "--label-scale") {
            const char* v = need("--label-scale");
            if (!v) return false;
            a.cfg.label_scale = (float)atof(v);
        } else if (k == "--rec-left-pad") {
            const char* v = need("--rec-left-pad");
            if (!v) return false;
            a.cfg.rec_left_pad = (float)atof(v);
        } else if (k == "--font") {
            const char* v = need("--font");
            if (!v) return false;
            a.cfg.font = v;
        } else if (k == "--core-mask") {
            const char* v = need("--core-mask");
            if (!v) return false;
            a.cfg.core_mask = atoi(v);
        } else if (k == "--image-dir") {
            const char* v = need("--image-dir");
            if (!v) return false;
            a.image_dir = v;
        } else if (k == "--out-dir") {
            const char* v = need("--out-dir");
            if (!v) return false;
            a.out_dir = v;
        } else if (k == "--limit") {
            const char* v = need("--limit");
            if (!v) return false;
            a.limit = atoi(v);
        } else if (k == "--random") {
            a.random_sample = true;
        } else if (k == "--no-save") {
            a.no_save = true;
        } else if (k == "--recursive") {
            a.recursive = true;
        } else if (k == "-h" || k == "--help") {
            return false;
        } else {
            std::cerr << "unknown arg: " << k << std::endl;
            return false;
        }
    }
    return true;
}

void printUsage() {
    std::cerr
        << "Quick RKNN verify — models auto-loaded from weights/ (INT8 preferred, FP fallback)\n\n"
        << "Default verify (random 10 + save to output/verify_???/):\n"
        << "  ./quick_verify.sh\n\n"
        << "Manual:\n"
        << "  rknn_infer_one --image-dir test123 --limit 10 --random\n\n"
        << "Single image:\n"
        << "  rknn_infer_one --image IN.jpg --out output/result.jpg\n\n"
        << "Options:\n"
        << "  --detect-rknn / --rec-rknn   override auto model discovery\n"
        << "  --image-dir DIR [--recursive] [--limit N] [--random] [--no-save]\n"
        << "  --out-dir DIR                default: output/verify_YYYYMMDD_HHMMSS\n"
        << "  --input-mode uint8_nhwc      (default; do not use auto for FP detect)\n"
        << "  --conf / --iou / --debug / --profile / --font PATH\n"
        << "  --profile                  print [timing] per-stage ms (read/write/det/rec/draw)\n"
        << "Env: PLATE_USE_FP=1 force FP; PLATE_PROFILE=1 same as --profile\n";
}

JobResult processOneFile(plate_rknn::PlateRknnPipeline& pipe, const std::string& img_path,
                         const std::string& out_path, const std::string& tag, bool no_save) {
    JobResult jr;
    jr.image = img_path;
    jr.out = out_path;

    auto t0 = std::chrono::steady_clock::now();
    cv::Mat img0 = cv::imread(img_path);
    auto t_read1 = std::chrono::steady_clock::now();
    if (img0.empty()) {
        std::cerr << tag << "[ERROR] cannot read: " << img_path << std::endl;
        return jr;
    }

    cv::Mat out_img;
    std::vector<plate_rknn::PlateItem> items;
    jr.code = pipe.process(img0, out_img, items, tag);
    auto t_proc1 = std::chrono::steady_clock::now();
    jr.plates = joinPlates(items);

    double ms_write = 0.;
    if (!no_save) {
        ensureParentDirs(out_path);
        cv::imwrite(out_path, out_img.empty() ? img0 : out_img);
        ms_write = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_proc1)
                       .count();
    }

    jr.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (pipe.config().profile) {
        jr.ms_read =
            std::chrono::duration<double, std::milli>(t_read1 - t0).count();
        jr.ms_write = ms_write;
        jr.timings = pipe.lastTimings();
        jr.has_timings = true;
        plate_rknn::printPlateTimingReport(tag, jr.timings, jr.ms_read, jr.ms_write, jr.ms);
    }

    const char* status = (jr.code == 0) ? "OK" : (jr.code == 2 ? "NO_PLATE" : "FAIL");
    std::cout << tag << "[" << status << "] " << img_path;
    if (!jr.plates.empty()) std::cout << " -> " << jr.plates;
    std::cout << " (total=" << std::fixed << std::setprecision(1) << jr.ms << " ms)";
    if (!no_save) std::cout << " saved=" << out_path;
    std::cout << std::endl;
    return jr;
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    if (!parseArgs(argc, argv, args)) {
        printUsage();
        return 1;
    }

    bool use_int8 = false;
    if (!applyAutoDefaults(args, use_int8)) return 1;
    (void)use_int8;

    plate_rknn::printCnRenderStatus(args.cfg.font);

    if (!args.out_dir.empty() && args.image_dir.empty()) {
        std::cerr << "[WARN] --out-dir ignored; batch uses output/verify_YYYYMMDD_HHMMSS/\n";
    }

    std::vector<std::pair<std::string, std::string>> jobs;
    std::string summary_dir = "output";

    if (!args.image_dir.empty()) {
        const std::string effective_out_dir =
            args.out_dir.empty() ? batchOutRoot() : args.out_dir;
        summary_dir = effective_out_dir;
        if (!args.no_save) {
            ensureParentDirs(effective_out_dir + "/._");
            std::cout << "[INFO] results -> " << effective_out_dir << std::endl;
        }

        std::vector<std::string> imgs;
        collectImages(args.image_dir, args.recursive, imgs);
        if (imgs.empty()) {
            std::cerr << "[ERROR] no images in " << args.image_dir << std::endl;
            return 1;
        }
        applyImageLimit(imgs, args.limit, args.random_sample);

        std::string in_root = args.image_dir;
        while (!in_root.empty() && (in_root.back() == '/' || in_root.back() == '\\')) in_root.pop_back();
        for (const std::string& p : imgs) {
            std::string dest;
            if (args.recursive) {
                size_t pos = p.find(in_root);
                if (pos != std::string::npos) {
                    std::string rel = p.substr(pos + in_root.size());
                    while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
                    dest = effective_out_dir + "/" + rel;
                } else {
                    dest = effective_out_dir + "/" + p.substr(p.find_last_of("/\\") + 1);
                }
            } else {
                dest = effective_out_dir + "/" + p.substr(p.find_last_of("/\\") + 1);
            }
            jobs.push_back({p, dest});
        }
    } else {
        if (args.image.empty()) {
            std::cerr << "[ERROR] specify --image or --image-dir\n";
            printUsage();
            return 1;
        }
        if (!args.no_save && !args.out.empty()) args.out = withTimestampInFilename(args.out);
        jobs.push_back({args.image, args.out});
    }

    plate_rknn::PlateRknnPipeline pipe;
    if (pipe.init(args.detect_rknn, args.rec_rknn, args.cfg) != 0) return 1;

    auto batch_t0 = std::chrono::steady_clock::now();
    std::vector<JobResult> results;
    results.reserve(jobs.size());

    int worst = 0;
    int nj = (int)jobs.size();
    for (int idx = 0; idx < nj; idx++) {
        std::string tag;
        if (nj > 1) tag = "[" + std::to_string(idx + 1) + "/" + std::to_string(nj) + "] ";
        JobResult jr = processOneFile(pipe, jobs[(size_t)idx].first, jobs[(size_t)idx].second, tag,
                                      args.no_save);
        results.push_back(jr);
        if (jr.code > worst) worst = jr.code;
    }

    double total_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - batch_t0)
                          .count();

    if (nj > 1) {
        printBatchSummary(results, total_ms);
        if (!args.no_save)
            writeSummaryCsv(summary_dir + "/summary.csv", results, args.cfg.profile);
    }

    return worst;
}
