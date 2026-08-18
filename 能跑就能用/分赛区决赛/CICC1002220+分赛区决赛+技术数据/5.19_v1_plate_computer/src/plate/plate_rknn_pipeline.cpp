/**
 * rknn_infer_one.cpp ¡ª single-file RKNN plate detect + recognize (parity with rknn_infer_one.py).
 * C++14, OpenCV + rknn_api.h
 */
#include "plate_rknn_pipeline.h"
#include "rknn_api.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef USE_OPENCV_FREETYPE
#include <opencv2/freetype.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
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

namespace plate_rknn {

constexpr int kDetectSize = 640;
constexpr int kDetectRawDim = 15;
constexpr int kDetectOutDim = 14;
constexpr int kRecW = 168;
constexpr int kRecH = 48;
constexpr int kNumPlateClasses = 78;
constexpr float kRecMean = 0.588f;
constexpr float kRecStd = 0.193f;
const cv::Scalar kVizQuadBgr(255, 255, 0);

const char* kPlateColorList[] = {u8"\u9ed1\u8272", u8"\u84dd\u8272", u8"\u7eff\u8272", u8"\u767d\u8272",
                                 u8"\u9ec4\u8272"};
const int kPlateColorListN = 5;

const char kPlateMarkerForceBlack[][8] = {u8"\u9886", u8"\u4f7f"};

// UTF-8 blob: 78 chars, same order as Python PLATE_NAME (source file UTF-8)
const char kPlateNameBlob[] =
    u8"#\u4eac\u6caa\u6d25\u6e1d\u5180\u6649\u8499\u8fbd\u5409\u9ed1"
    u8"\u82cf\u6d59\u7696\u95fd\u8d63\u9c81\u8c6b\u9102\u6e58\u7ca4\u6842\u743c"
    u8"\u5ddd\u8d35\u4e91\u85cf\u9655\u7518\u9752\u5b81\u65b0\u5b66\u8b66\u6e2f\u6fb3"
    u8"\u6302\u4f7f\u9886\u6c11\u822a\u5371"
    "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"
    u8"\u9669\u54c1";

const char* kDetectModes[] = {"uint8_nhwc", "float_nchw_rgb", "float_nchw_bgr"};
const int kDetectModesN = 3;

struct PlateDetectRow {
    float v[kDetectOutDim];
};

struct PlateSeqLayout {
    int seq_len = 21;
    int num_classes = 78;
    bool classes_first = false; // [1,78,21]
};

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------
int utf8CharLen(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::vector<std::string> buildPlateNameTable() {
    std::vector<std::string> out;
    const unsigned char* p = (const unsigned char*)kPlateNameBlob;
    while (*p) {
        int len = utf8CharLen(*p);
        out.emplace_back((const char*)p, (size_t)len);
        p += len;
    }
    return out;
}

const std::vector<std::string>& plateNameTable() {
    static std::vector<std::string> t = buildPlateNameTable();
    return t;
}

bool strContainsUtf8(const std::string& hay, const char* needleUtf8) {
    return hay.find(needleUtf8) != std::string::npos;
}

std::string vizPlateColorForNumber(const std::string& plate_no, const std::string& model_color) {
    if (!plate_no.empty()) {
        for (size_t i = 0; i < sizeof(kPlateMarkerForceBlack) / sizeof(kPlateMarkerForceBlack[0]); i++) {
            if (strContainsUtf8(plate_no, kPlateMarkerForceBlack[i]))
                return kPlateColorList[0];
        }
    }
    return model_color;
}

// ---------------------------------------------------------------------------
// Model I/O
// ---------------------------------------------------------------------------
unsigned char* loadModelData(const char* filename, int* model_size) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    int size = (int)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char* data = (unsigned char*)malloc((size_t)size);
    if (data) fread(data, 1, (size_t)size, fp);
    fclose(fp);
    *model_size = size;
    return data;
}

class RknnSession {
public:
    RknnSession() : ctx_(0), in_attrs_(nullptr), out_attrs_(nullptr), core_mask_(0) {
        memset(&io_num_, 0, sizeof(io_num_));
    }

    ~RknnSession() { release(); }

    int load(const char* path, uint32_t core_mask) {
        release();
        core_mask_ = core_mask;
        int model_len = 0;
        unsigned char* model_data = loadModelData(path, &model_len);
        if (!model_data) {
            std::cerr << "[ERROR] cannot open model: " << path << std::endl;
            return -1;
        }
        int ret = rknn_init(&ctx_, model_data, model_len, 0, nullptr);
        free(model_data);
        if (ret < 0) {
            ctx_ = 0;
            std::cerr << "[ERROR] rknn_init failed " << path << " ret=" << ret << std::endl;
            return ret;
        }
        rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_, sizeof(io_num_));
        in_attrs_ = (rknn_tensor_attr*)calloc(io_num_.n_input, sizeof(rknn_tensor_attr));
        out_attrs_ = (rknn_tensor_attr*)calloc(io_num_.n_output, sizeof(rknn_tensor_attr));
        for (uint32_t i = 0; i < io_num_.n_input; i++) {
            in_attrs_[i].index = i;
            rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attrs_[i], sizeof(rknn_tensor_attr));
        }
        for (uint32_t i = 0; i < io_num_.n_output; i++) {
            out_attrs_[i].index = i;
            rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attrs_[i], sizeof(rknn_tensor_attr));
        }
        if (core_mask_)
            rknn_set_core_mask(ctx_, (rknn_core_mask)core_mask_);
        return 0;
    }

    void release() {
        if (in_attrs_) {
            free(in_attrs_);
            in_attrs_ = nullptr;
        }
        if (out_attrs_) {
            free(out_attrs_);
            out_attrs_ = nullptr;
        }
        if (ctx_) {
            rknn_destroy(ctx_);
            ctx_ = 0;
        }
    }

    bool ok() const { return ctx_ != 0; }

    uint32_t nOutput() const { return io_num_.n_output; }

    const rknn_tensor_attr& outAttr(int i) const { return out_attrs_[i]; }
    const rknn_tensor_attr& inAttr(int i) const { return in_attrs_[i]; }

    bool inputIsQuantized() const {
        return io_num_.n_input > 0 && in_attrs_[0].qnt_type != RKNN_TENSOR_QNT_NONE;
    }

    void logIoAttrs(const char* tag) const {
        if (!ctx_) return;
        for (uint32_t i = 0; i < io_num_.n_input; i++) {
            const rknn_tensor_attr& a = in_attrs_[i];
            std::cout << "[rknn:" << tag << "] in[" << i << "] type=" << get_type_string(a.type)
                      << " fmt=" << get_format_string(a.fmt) << " qnt=" << get_qnt_type_string(a.qnt_type)
                      << " size=" << a.size << std::endl;
        }
        for (uint32_t i = 0; i < io_num_.n_output; i++) {
            const rknn_tensor_attr& a = out_attrs_[i];
            std::cout << "[rknn:" << tag << "] out[" << i << "] type=" << get_type_string(a.type)
                      << " fmt=" << get_format_string(a.fmt) << " qnt=" << get_qnt_type_string(a.qnt_type)
                      << " elems=" << a.n_elems << std::endl;
        }
    }

    bool outputsAreInt8Affine() const {
        if (io_num_.n_output == 0) return false;
        for (uint32_t i = 0; i < io_num_.n_output; i++) {
            const rknn_tensor_attr& a = out_attrs_[i];
            if (a.type != RKNN_TENSOR_INT8) return false;
            if (a.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                a.qnt_type != RKNN_TENSOR_QNT_DFP)
                return false;
        }
        return true;
    }

    /** Copy INT8 outputs (no driver float dequant). Faster det_output path for detect. */
    int inferInt8(const rknn_input* inputs, int n_in, std::vector<std::vector<int8_t>>& outs_i8,
                  double* out_ms_run = nullptr, double* out_ms_fetch = nullptr) {
        outs_i8.clear();
        int ret = rknn_inputs_set(ctx_, (uint32_t)n_in, const_cast<rknn_input*>(inputs));
        if (ret != RKNN_SUCC) return ret;
        auto t1 = std::chrono::steady_clock::now();
        ret = rknn_run(ctx_, nullptr);
        if (ret != RKNN_SUCC) return ret;
        auto t2 = std::chrono::steady_clock::now();

        std::vector<rknn_output> outputs(io_num_.n_output);
        memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
        for (uint32_t i = 0; i < io_num_.n_output; i++) {
            outputs[i].index = i;
            outputs[i].want_float = 0;
        }
        ret = rknn_outputs_get(ctx_, io_num_.n_output, outputs.data(), nullptr);
        if (ret != RKNN_SUCC) {
            if (out_ms_run)
                *out_ms_run += std::chrono::duration<double, std::milli>(t2 - t1).count();
            return ret;
        }

        outs_i8.resize(io_num_.n_output);
        for (uint32_t i = 0; i < io_num_.n_output; i++) {
            uint32_t n = out_attrs_[i].n_elems;
            const int8_t* buf = (const int8_t*)outputs[i].buf;
            outs_i8[i].assign(buf, buf + n);
        }
        rknn_outputs_release(ctx_, io_num_.n_output, outputs.data());
        auto t3 = std::chrono::steady_clock::now();
        if (out_ms_run)
            *out_ms_run += std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (out_ms_fetch)
            *out_ms_fetch += std::chrono::duration<double, std::milli>(t3 - t2).count();
        return RKNN_SUCC;
    }

    int infer(const rknn_input* inputs, int n_in, std::vector<std::vector<float>>& outs,
              double* out_ms_run = nullptr, double* out_ms_fetch = nullptr) {
        outs.clear();
        int ret = rknn_inputs_set(ctx_, (uint32_t)n_in, const_cast<rknn_input*>(inputs));
        if (ret != RKNN_SUCC) return ret;
        auto t1 = std::chrono::steady_clock::now();
        ret = rknn_run(ctx_, nullptr);
        if (ret != RKNN_SUCC) return ret;
        auto t2 = std::chrono::steady_clock::now();

        auto fetchOutputs = [&](bool want_float, std::vector<std::vector<float>>& out_vecs) -> int {
            std::vector<rknn_output> outputs(io_num_.n_output);
            memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
            for (uint32_t i = 0; i < io_num_.n_output; i++) {
                outputs[i].index = i;
                outputs[i].want_float = want_float ? 1 : 0;
            }
            int r = rknn_outputs_get(ctx_, io_num_.n_output, outputs.data(), nullptr);
            if (r != RKNN_SUCC) return r;

            out_vecs.resize(io_num_.n_output);
            for (uint32_t i = 0; i < io_num_.n_output; i++) {
                uint32_t n = out_attrs_[i].n_elems;
                const rknn_tensor_attr& a = out_attrs_[i];
                out_vecs[i].resize(n);
                if (want_float) {
                    float* buf = (float*)outputs[i].buf;
                    out_vecs[i].assign(buf, buf + n);
                } else if (a.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && a.type == RKNN_TENSOR_INT8) {
                    const int8_t* buf = (const int8_t*)outputs[i].buf;
                    const float zp = (float)a.zp;
                    const float scale = a.scale;
                    for (uint32_t j = 0; j < n; j++)
                        out_vecs[i][j] = ((float)buf[j] - zp) * scale;
                } else if (a.qnt_type == RKNN_TENSOR_QNT_DFP && a.type == RKNN_TENSOR_INT8) {
                    const int8_t* buf = (const int8_t*)outputs[i].buf;
                    const float scale = (a.fl > 0) ? (1.f / (float)(1 << a.fl)) : 1.f;
                    for (uint32_t j = 0; j < n; j++) out_vecs[i][j] = (float)buf[j] * scale;
                } else {
                    float* buf = (float*)outputs[i].buf;
                    out_vecs[i].assign(buf, buf + n);
                }
            }
            rknn_outputs_release(ctx_, io_num_.n_output, outputs.data());
            return RKNN_SUCC;
        };

        // Prefer native float get; only fall back to manual INT8 dequant if float path is dead.
        ret = fetchOutputs(true, outs);
        auto t3 = std::chrono::steady_clock::now();
        if (out_ms_run)
            *out_ms_run += std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (out_ms_fetch)
            *out_ms_fetch += std::chrono::duration<double, std::milli>(t3 - t2).count();
        if (ret != RKNN_SUCC) return ret;

        if (io_num_.n_output > 0 && out_attrs_[0].qnt_type != RKNN_TENSOR_QNT_NONE && !outs[0].empty()) {
            // Cheap dead check: sample stride instead of full max_element on ~300k floats.
            const std::vector<float>& o0 = outs[0];
            float mx = 0.f;
            const size_t step = std::max<size_t>(1, o0.size() / 4096);
            for (size_t j = 0; j < o0.size(); j += step) mx = std::max(mx, std::abs(o0[j]));
            if (mx < 1e-4f) {
                std::vector<std::vector<float>> raw_outs;
                if (fetchOutputs(false, raw_outs) == RKNN_SUCC && !raw_outs.empty()) {
                    auto t4 = std::chrono::steady_clock::now();
                    if (out_ms_fetch)
                        *out_ms_fetch += std::chrono::duration<double, std::milli>(t4 - t3).count();
                    float mx2 = 0.f;
                    const size_t step2 = std::max<size_t>(1, raw_outs[0].size() / 4096);
                    for (size_t j = 0; j < raw_outs[0].size(); j += step2)
                        mx2 = std::max(mx2, std::abs(raw_outs[0][j]));
                    if (mx2 > 1e-4f) {
                        std::cout << "[rknn] detect output recovered via manual INT8 dequant\n";
                        outs.swap(raw_outs);
                    }
                }
            }
        }
        return RKNN_SUCC;
    }

private:
    rknn_context ctx_;
    rknn_input_output_num io_num_;
    rknn_tensor_attr* in_attrs_;
    rknn_tensor_attr* out_attrs_;
    uint32_t core_mask_;
};

// ---------------------------------------------------------------------------
// Detect: letterbox + tensor layout + post
// ---------------------------------------------------------------------------
void letterBox(const cv::Mat& src, int target, cv::Mat& dst, double& r, int& left, int& top) {
    int h = src.rows, w = src.cols;
    r = std::min((double)target / h, (double)target / w);
    int new_h = (int)(h * r), new_w = (int)(w * r);
    top = (target - new_h) / 2;
    left = (target - new_w) / 2;
    int bottom = target - new_h - top;
    int right = target - new_w - left;
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(new_w, new_h));
    cv::copyMakeBorder(resized, dst, top, bottom, left, right, cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
}

void copyTranspose15N(const float* raw_in, std::vector<float>& dst, int bigN) {
    dst.resize((size_t)bigN * kDetectRawDim);
    for (int anch = 0; anch < bigN; anch++)
        for (int c = 0; c < kDetectRawDim; c++)
            dst[(size_t)anch * kDetectRawDim + (size_t)c] = raw_in[c * bigN + anch];
}

bool layoutRawToAnchors(const rknn_tensor_attr& attr, const float* raw_in, std::vector<float>& dst,
                        int& N_out) {
    uint32_t n_elems = attr.n_elems;
    if (n_elems % (uint32_t)kDetectRawDim != 0) return false;
    N_out = (int)(n_elems / kDetectRawDim);
    dst.resize(n_elems);
    if (attr.n_dims < 3) {
        memcpy(dst.data(), raw_in, dst.size() * sizeof(float));
        return true;
    }
    uint32_t d1 = attr.dims[1], d2 = attr.dims[2];
    if (d2 == (uint32_t)kDetectRawDim && d1 != (uint32_t)kDetectRawDim && (int)d1 == N_out) {
        memcpy(dst.data(), raw_in, dst.size() * sizeof(float));
        return true;
    }
    if (d1 == (uint32_t)kDetectRawDim && d2 != (uint32_t)kDetectRawDim && (int)d2 == N_out) {
        copyTranspose15N(raw_in, dst, N_out);
        return true;
    }
    memcpy(dst.data(), raw_in, dst.size() * sizeof(float));
    return true;
}

// ---------------------------------------------------------------------------
// 3-head RKNN detect decode (plate_detect_rknn.onnx / INT8)
// ---------------------------------------------------------------------------
struct DetectRknnMeta {
    bool valid = false;
    int nc = 2;
    int na = 3;
    float strides[3] = {8.f, 16.f, 32.f};
    float anchors[3][3][2] = {};
};

inline float sigmoidf(float x) {
    if (x >= 0.f) {
        float z = std::exp(-x);
        return 1.f / (1.f + z);
    }
    float z = std::exp(x);
    return z / (1.f + z);
}

/** sigmoid(x) > prob  <=>  x > logit(prob). Used to skip low-conf anchors before full decode. */
inline float logitThreshold(float prob) {
    prob = std::max(1e-4f, std::min(1.f - 1e-4f, prob));
    return std::log(prob / (1.f - prob));
}

std::string dirnameOfPath(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::string findDetectMetaPath(const std::string& detect_rknn) {
    const char* candidates[] = {
        "./weights/RK_plate_detect_meta.txt",
        "weights/RK_plate_detect_meta.txt",
    };
    for (const char* c : candidates) {
        FILE* f = fopen(c, "rb");
        if (f) {
            fclose(f);
            return c;
        }
    }
    std::string beside = dirnameOfPath(detect_rknn) + "/RK_plate_detect_meta.txt";
    FILE* f = fopen(beside.c_str(), "rb");
    if (f) {
        fclose(f);
        return beside;
    }
    return "./weights/RK_plate_detect_meta.txt";
}

bool loadDetectMeta(const std::string& path, DetectRknnMeta& meta) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    DetectRknnMeta m;
    std::vector<std::vector<float>> anchor_rows;
    std::string line;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("nc=", 0) == 0) {
            m.nc = std::atoi(line.c_str() + 3);
        } else if (line.rfind("anchors=", 0) == 0) {
            std::istringstream ss(line.substr(8));
            std::vector<float> row;
            float v = 0.f;
            while (ss >> v) row.push_back(v);
            if (row.size() >= 6) anchor_rows.push_back(row);
        }
    }
    ifs.clear();
    ifs.seekg(0);
    std::vector<float> strides;
    while (std::getline(ifs, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("stride=", 0) == 0) strides.push_back((float)std::atof(line.c_str() + 7));
    }
    if (strides.size() != 3 || anchor_rows.size() != 3) return false;
    for (int i = 0; i < 3; i++) m.strides[i] = strides[(size_t)i];
    for (int h = 0; h < 3; h++) {
        for (int a = 0; a < 3; a++) {
            m.anchors[h][a][0] = anchor_rows[(size_t)h][(size_t)(a * 2)];
            m.anchors[h][a][1] = anchor_rows[(size_t)h][(size_t)(a * 2 + 1)];
        }
    }
    m.valid = true;
    meta = m;
    return true;
}

bool inferHeadShape(const rknn_tensor_attr& attr, int channels, int& ny, int& nx) {
    if (channels <= 0 || attr.n_elems % (uint32_t)channels != 0) return false;
    int spatial = (int)(attr.n_elems / (uint32_t)channels);
    if (attr.n_dims >= 4) {
        uint32_t d1 = attr.dims[1], d2 = attr.dims[2], d3 = attr.dims[3];
        if (d1 == (uint32_t)channels && d2 > 0 && d3 > 0 && (int)(d2 * d3) == spatial) {
            ny = (int)d2;
            nx = (int)d3;
            return true;
        }
        if (d3 == (uint32_t)channels && d1 > 0 && d2 > 0 && (int)(d1 * d2) == spatial) {
            ny = (int)d1;
            nx = (int)d2;
            return true;
        }
    }
    int root = (int)std::sqrt((double)spatial);
    for (int h = root; h >= 1; h--) {
        if (spatial % h == 0) {
            ny = h;
            nx = spatial / h;
            return true;
        }
    }
    return false;
}

inline void appendDecodedAnchor(std::vector<float>& out_flat, const float* yv, int no, float gx,
                                float gy, float stride, float ag_w, float ag_h) {
    const float gx_s = gx * stride;
    const float gy_s = gy * stride;
    out_flat.push_back((yv[0] * 2.f - 0.5f + gx) * stride);
    out_flat.push_back((yv[1] * 2.f - 0.5f + gy) * stride);
    const float pw = yv[2] * 2.f;
    const float ph = yv[3] * 2.f;
    out_flat.push_back(pw * pw * ag_w);
    out_flat.push_back(ph * ph * ag_h);
    out_flat.push_back(yv[4]);
    out_flat.push_back(yv[5] * ag_w + gx_s);
    out_flat.push_back(yv[6] * ag_h + gy_s);
    out_flat.push_back(yv[7] * ag_w + gx_s);
    out_flat.push_back(yv[8] * ag_h + gy_s);
    out_flat.push_back(yv[9] * ag_w + gx_s);
    out_flat.push_back(yv[10] * ag_h + gy_s);
    out_flat.push_back(yv[11] * ag_w + gx_s);
    out_flat.push_back(yv[12] * ag_h + gy_s);
    for (int f = 13; f < no; f++) out_flat.push_back(yv[f]);
}

void decodeOneHeadAppend(const float* raw, int channels, int ny, int nx, float stride,
                         const float anchors_na[3][2], int na, int no, int nc, float obj_logit_min,
                         std::vector<float>& out_flat) {
    (void)channels;
    (void)nc;
    const size_t plane = (size_t)ny * (size_t)nx;
    // Cache channel plane bases once per head (avoids repeated ch*ny*nx mul in inner loop).
    const float* planes[48];
    if (na * no > 48) return;
    for (int c = 0; c < na * no; c++) planes[c] = raw + (size_t)c * plane;

    for (int a = 0; a < na; a++) {
        const float ag_w = anchors_na[a][0] * stride;
        const float ag_h = anchors_na[a][1] * stride;
        const int base = a * no;
        const float* obj_plane = planes[base + 4];
        for (int y = 0; y < ny; y++) {
            const float* row = obj_plane + (size_t)y * (size_t)nx;
            const size_t yoff = (size_t)y * (size_t)nx;
            for (int x = 0; x < nx; x++) {
                if (row[x] <= obj_logit_min) continue;

                float xin[16] = {};
                float yv[16] = {};
                const size_t idx = yoff + (size_t)x;
                for (int f = 0; f < no; f++) xin[f] = planes[base + f][idx];
                for (int f = 0; f < 5; f++) yv[f] = sigmoidf(xin[f]);
                for (int f = 5; f < 13; f++) yv[f] = xin[f];
                for (int f = 13; f < no; f++) yv[f] = sigmoidf(xin[f]);
                appendDecodedAnchor(out_flat, yv, no, (float)x, (float)y, stride, ag_w, ag_h);
            }
        }
    }
}

/** INT8 affine/DFP decode: gate on int8 obj, dequant only surviving anchors. */
void decodeOneHeadAppendInt8(const int8_t* raw, int channels, int ny, int nx, float stride,
                             const float anchors_na[3][2], int na, int no, int nc, float obj_logit_min,
                             float zp, float scale, std::vector<float>& out_flat) {
    (void)channels;
    (void)nc;
    const size_t plane = (size_t)ny * (size_t)nx;
    const int8_t* planes[48];
    if (na * no > 48) return;
    for (int c = 0; c < na * no; c++) planes[c] = raw + (size_t)c * plane;

    // obj_float = (q - zp) * scale  > obj_logit_min  =>  q > zp + obj_logit_min/scale
    // Use int gate when scale>0 so most cells skip without float dequant.
    int8_t obj_q_min = -128;
    if (scale > 0.f) {
        const float qf = zp + obj_logit_min / scale;
        if (qf > 127.f)
            obj_q_min = 127;
        else if (qf < -128.f)
            obj_q_min = -128;
        else
            obj_q_min = (int8_t)std::ceil(qf);
    }

    for (int a = 0; a < na; a++) {
        const float ag_w = anchors_na[a][0] * stride;
        const float ag_h = anchors_na[a][1] * stride;
        const int base = a * no;
        const int8_t* obj_plane = planes[base + 4];
        for (int y = 0; y < ny; y++) {
            const int8_t* row = obj_plane + (size_t)y * (size_t)nx;
            const size_t yoff = (size_t)y * (size_t)nx;
            for (int x = 0; x < nx; x++) {
                if (row[x] < obj_q_min) continue;
                const float obj_f = ((float)row[x] - zp) * scale;
                if (obj_f <= obj_logit_min) continue;

                float xin[16] = {};
                float yv[16] = {};
                const size_t idx = yoff + (size_t)x;
                for (int f = 0; f < no; f++)
                    xin[f] = ((float)planes[base + f][idx] - zp) * scale;
                for (int f = 0; f < 5; f++) yv[f] = sigmoidf(xin[f]);
                for (int f = 5; f < 13; f++) yv[f] = xin[f];
                for (int f = 13; f < no; f++) yv[f] = sigmoidf(xin[f]);
                appendDecodedAnchor(out_flat, yv, no, (float)x, (float)y, stride, ag_w, ag_h);
            }
        }
    }
}

bool decodeThreeHeads(RknnSession& sess, const std::vector<std::vector<float>>& outs,
                      const DetectRknnMeta& meta, float conf_thresh, std::vector<float>& rawflat,
                      int& N) {
    if (outs.size() < 3 || !meta.valid) return false;
    const int no = meta.nc + 5 + 8;
    const int channels = meta.na * no;
    const float early_prob = std::max(0.01f, conf_thresh * 0.45f);
    const float obj_logit_min = logitThreshold(early_prob);
    rawflat.clear();
    rawflat.reserve(256 * (size_t)kDetectRawDim);
    for (int hi = 0; hi < 3; hi++) {
        if (outs[(size_t)hi].empty()) return false;
        int ny = 0, nx = 0;
        if (!inferHeadShape(sess.outAttr(hi), channels, ny, nx)) {
            std::cerr << "[ERROR] cannot infer 3-head shape for output " << hi << std::endl;
            return false;
        }
        decodeOneHeadAppend(outs[(size_t)hi].data(), channels, ny, nx, meta.strides[hi],
                            meta.anchors[hi], meta.na, no, meta.nc, obj_logit_min, rawflat);
    }
    if (rawflat.empty()) {
        N = 0;
        return false;
    }
    if (rawflat.size() % (size_t)kDetectRawDim != 0) return false;
    N = (int)(rawflat.size() / (size_t)kDetectRawDim);
    return N > 0;
}

bool decodeThreeHeadsInt8(RknnSession& sess, const std::vector<std::vector<int8_t>>& outs,
                          const DetectRknnMeta& meta, float conf_thresh, std::vector<float>& rawflat,
                          int& N) {
    if (outs.size() < 3 || !meta.valid) return false;
    const int no = meta.nc + 5 + 8;
    const int channels = meta.na * no;
    const float early_prob = std::max(0.01f, conf_thresh * 0.45f);
    const float obj_logit_min = logitThreshold(early_prob);
    rawflat.clear();
    rawflat.reserve(256 * (size_t)kDetectRawDim);
    for (int hi = 0; hi < 3; hi++) {
        if (outs[(size_t)hi].empty()) return false;
        int ny = 0, nx = 0;
        const rknn_tensor_attr& attr = sess.outAttr(hi);
        if (!inferHeadShape(attr, channels, ny, nx)) {
            std::cerr << "[ERROR] cannot infer 3-head shape for output " << hi << std::endl;
            return false;
        }
        float zp = 0.f, scale = 1.f;
        if (attr.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
            zp = (float)attr.zp;
            scale = attr.scale;
        } else if (attr.qnt_type == RKNN_TENSOR_QNT_DFP) {
            zp = 0.f;
            scale = (attr.fl > 0) ? (1.f / (float)(1 << attr.fl)) : 1.f;
        } else {
            return false;
        }
        decodeOneHeadAppendInt8(outs[(size_t)hi].data(), channels, ny, nx, meta.strides[hi],
                                meta.anchors[hi], meta.na, no, meta.nc, obj_logit_min, zp, scale,
                                rawflat);
    }
    if (rawflat.empty()) {
        N = 0;
        return false;
    }
    if (rawflat.size() % (size_t)kDetectRawDim != 0) return false;
    N = (int)(rawflat.size() / (size_t)kDetectRawDim);
    return N > 0;
}

float stdPopulation(const std::vector<float>& v) {
    if (v.empty()) return 0.f;
    double m = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double s2 = 0.0;
    for (float x : v) {
        double d = x - m;
        s2 += d * d;
    }
    return (float)std::sqrt(s2 / (double)v.size());
}

bool scoresAreDegenerate(const std::vector<float>& raw, int N) {
    if (N <= 0) return true;
    std::vector<float> objs((size_t)N);
    for (int i = 0; i < N; i++) objs[(size_t)i] = raw[(size_t)i * kDetectRawDim + 4];
    float s = stdPopulation(objs);
    float mn = *std::min_element(objs.begin(), objs.end());
    float mx = *std::max_element(objs.begin(), objs.end());
    if (s < 1e-5f) return true;
    if (mx < 1e-4f) return true;
    if (std::abs(mx - 0.5f) < 0.02f && std::abs(mn - 0.5f) < 0.02f) return true;
    return false;
}

bool warnDeadDetectOutput(const std::vector<float>& raw, int N, const char* tag) {
    if (N <= 0) return true;
    std::vector<float> objs((size_t)N);
    for (int i = 0; i < N; i++) objs[(size_t)i] = raw[(size_t)i * kDetectRawDim + 4];
    float o_std = stdPopulation(objs);
    float o_max = *std::max_element(objs.begin(), objs.end());
    float o_min = *std::min_element(objs.begin(), objs.end());
    if (o_std < 1e-6f) {
        if (std::abs(o_max) < 0.05f) {
            std::cerr << "[WARN:" << tag << "] detect head all zeros (bad INT8 or wrong input).\n"
                      << "  Try: --input-mode float_nchw_rgb OR reconvert on PC with --no-quant\n";
            return true;
        }
        if (std::abs(o_max - 0.5f) < 0.01f && std::abs(o_min - 0.5f) < 0.01f) {
            std::cerr << "[WARN:" << tag
                      << "] detect head all 0.5 (25200 identical anchors, INT8 quant failed).\n"
                      << "  Reconvert INT8 with calibration, or use plate_detect_fp.rknn\n";
            return true;
        }
    }
    return false;
}

void debugDetectTensor(const std::vector<float>& raw, int N, const char* tag) {
    if (N <= 0) return;
    std::vector<float> objs((size_t)N);
    for (int i = 0; i < N; i++) objs[(size_t)i] = raw[(size_t)i * kDetectRawDim + 4];
    float o_min = *std::min_element(objs.begin(), objs.end());
    float o_max = *std::max_element(objs.begin(), objs.end());
    int above = 0;
    for (float o : objs)
        if (o > 0.3f) above++;
    std::cerr << "[debug:" << tag << "] anchors=" << N << " raw_obj=[" << o_min << "," << o_max
              << "] above_conf=" << above << std::endl;
}

float iouRow(const PlateDetectRow& a, const PlateDetectRow& b) {
    float x1 = std::max(a.v[0], b.v[0]), y1 = std::max(a.v[1], b.v[1]);
    float x2 = std::min(a.v[2], b.v[2]), y2 = std::min(a.v[3], b.v[3]);
    float w = std::max(0.f, x2 - x1), h = std::max(0.f, y2 - y1);
    float inter = w * h;
    float a1 = std::max(0.f, a.v[2] - a.v[0]) * std::max(0.f, a.v[3] - a.v[1]);
    float b1 = std::max(0.f, b.v[2] - b.v[0]) * std::max(0.f, b.v[3] - b.v[1]);
    return inter / (a1 + b1 - inter + 1e-6f);
}

void nmsRows(std::vector<PlateDetectRow>& rows, float iou_thresh) {
    std::sort(rows.begin(), rows.end(),
              [](const PlateDetectRow& a, const PlateDetectRow& b) { return a.v[4] > b.v[4]; });
    std::vector<PlateDetectRow> keep;
    for (const PlateDetectRow& c : rows) {
        bool ok = true;
        for (const PlateDetectRow& k : keep) {
            if (iouRow(k, c) > iou_thresh) {
                ok = false;
                break;
            }
        }
        if (ok) keep.push_back(c);
    }
    rows.swap(keep);
}

void restorePlate14(float* v14, float r, float pad_l, float pad_t) {
    const int xi[] = {0, 2, 5, 7, 9, 11};
    const int yi[] = {1, 3, 6, 8, 10, 12};
    for (int i : xi) {
        v14[i] -= pad_l;
        v14[i] /= r;
    }
    for (int i : yi) {
        v14[i] -= pad_t;
        v14[i] /= r;
    }
}

bool outputScoresLookFake(const std::vector<PlateDetectRow>& rows) {
    if ((int)rows.size() < 8) return false;
    std::vector<float> sc(rows.size());
    for (size_t i = 0; i < rows.size(); i++) sc[i] = rows[i].v[4];
    float s = stdPopulation(sc);
    float mx = *std::max_element(sc.begin(), sc.end());
    return s < 0.02f && mx <= 0.26f;
}

void postProcessing(const std::vector<float>& rawAnch, double scale_r, int pad_left, int pad_top,
                    float conf_thresh, float iou_thresh, int topk, const cv::Size& img_shape,
                    std::vector<PlateDetectRow>& out) {
    out.clear();
    int N = (int)rawAnch.size() / kDetectRawDim;
    if (N <= 0 || scoresAreDegenerate(rawAnch, N)) return;

    std::vector<float> obj((size_t)N);
    for (int i = 0; i < N; i++) obj[(size_t)i] = rawAnch[(size_t)i * kDetectRawDim + 4];

    std::vector<int> idx;
    for (int i = 0; i < N; i++)
        if (obj[(size_t)i] > conf_thresh) idx.push_back(i);
    if (idx.empty()) return;
    if ((int)idx.size() > topk) {
        std::sort(idx.begin(), idx.end(),
                  [&](int a, int b) { return obj[(size_t)a] > obj[(size_t)b]; });
        idx.resize((size_t)topk);
    }

    std::vector<PlateDetectRow> cand;
    std::vector<float> row(kDetectRawDim);
    for (int id : idx) {
        memcpy(row.data(), rawAnch.data() + (size_t)id * kDetectRawDim, kDetectRawDim * sizeof(float));
        row[13] *= row[4];
        row[14] *= row[4];
        float cx = row[0], cy = row[1], w = row[2], h = row[3];
        float x1 = cx - w * 0.5f, y1 = cy - h * 0.5f, x2 = cx + w * 0.5f, y2 = cy + h * 0.5f;
        float c0 = row[13], c1 = row[14];
        float score = std::max(c0, c1);
        float cls_ix = (c0 >= c1) ? 0.f : 1.f;
        PlateDetectRow pr{};
        pr.v[0] = x1;
        pr.v[1] = y1;
        pr.v[2] = x2;
        pr.v[3] = y2;
        pr.v[4] = score;
        memcpy(pr.v + 5, row.data() + 5, 8 * sizeof(float));
        pr.v[13] = cls_ix;
        cand.push_back(pr);
    }
    nmsRows(cand, iou_thresh);
    float rf = (float)scale_r;
    for (PlateDetectRow& pr : cand) restorePlate14(pr.v, rf, (float)pad_left, (float)pad_top);

    if (outputScoresLookFake(cand)) {
        out.clear();
        return;
    }

    int ih = img_shape.height, iw = img_shape.width;
    float min_score = std::max(0.2f, conf_thresh * 0.85f);
    std::vector<PlateDetectRow> filtered;
    for (const PlateDetectRow& pr : cand) {
        float x1 = pr.v[0], y1 = pr.v[1], x2 = pr.v[2], y2 = pr.v[3], score = pr.v[4];
        float bw = x2 - x1, bh = y2 - y1;
        if (score < min_score || bw < 24.f || bh < 8.f) continue;
        if (bw > iw * 0.85f || bh > ih * 0.85f) continue;
        if (x2 <= x1 || y2 <= y1) continue;
        if (x1 < -iw * 0.1f || y1 < -ih * 0.1f || x2 > iw * 1.1f || y2 > ih * 1.1f) continue;
        filtered.push_back(pr);
    }
    cand.swap(filtered);
    if (outputScoresLookFake(cand)) {
        out.clear();
        return;
    }
    if ((int)cand.size() > 8) {
        std::cerr << "[WARN] " << cand.size() << " boxes; keep top-3 by score\n";
        std::sort(cand.begin(), cand.end(),
                  [](const PlateDetectRow& a, const PlateDetectRow& b) { return a.v[4] > b.v[4]; });
        cand.resize(3);
    }
    out.swap(cand);
}

bool buildDetectInput(const cv::Mat& img_lb, const char* mode, const rknn_tensor_attr* model_in,
                      bool pass_through, bool rgb_swap, std::vector<uint8_t>& u8buf,
                      std::vector<float>& fbuf, rknn_input& inp) {
    memset(&inp, 0, sizeof(inp));
    inp.index = 0;
    if (strcmp(mode, "uint8_nhwc") == 0) {
        if (rgb_swap) {
            cv::Mat rgb;
            cv::cvtColor(img_lb, rgb, cv::COLOR_BGR2RGB);
            u8buf.assign(rgb.data, rgb.data + rgb.total() * rgb.elemSize());
        } else {
            u8buf.assign(img_lb.data, img_lb.data + img_lb.total() * img_lb.elemSize());
        }
        inp.type = RKNN_TENSOR_UINT8;
        inp.fmt = RKNN_TENSOR_NHWC;
        if (model_in) {
            if (model_in->type == RKNN_TENSOR_UINT8 || model_in->type == RKNN_TENSOR_INT8)
                inp.type = model_in->type;
            if (model_in->fmt == RKNN_TENSOR_NHWC || model_in->fmt == RKNN_TENSOR_NCHW)
                inp.fmt = model_in->fmt;
        }
        inp.pass_through = pass_through ? 1 : 0;
        inp.size = (uint32_t)u8buf.size();
        inp.buf = u8buf.data();
        return true;
    }
    if (strcmp(mode, "float_nchw_rgb") == 0 || strcmp(mode, "float_nchw_bgr") == 0) {
        cv::Mat f32;
        img_lb.convertTo(f32, CV_32FC3, 1.0 / 255.0);
        if (strcmp(mode, "float_nchw_rgb") == 0) cv::cvtColor(f32, f32, cv::COLOR_BGR2RGB);
        std::vector<cv::Mat> ch(3);
        cv::split(f32, ch);
        fbuf.resize((size_t)3 * img_lb.rows * img_lb.cols);
        size_t plane = (size_t)img_lb.rows * img_lb.cols;
        for (int c = 0; c < 3; c++) memcpy(fbuf.data() + c * plane, ch[c].data, plane * sizeof(float));
        inp.type = RKNN_TENSOR_FLOAT32;
        inp.fmt = RKNN_TENSOR_NCHW;
        inp.size = (uint32_t)(fbuf.size() * sizeof(float));
        inp.buf = fbuf.data();
        return true;
    }
    return false;
}

int runDetectOnce(RknnSession& sess, const cv::Mat& img_lb, const char* mode, bool pass_through,
                  bool rgb_swap, const DetectRknnMeta* meta3, std::vector<float>& rawflat, int& N,
                  float conf_thresh, PlateTimings* tm = nullptr) {
    auto t_in0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> u8;
    std::vector<float> f32;
    rknn_input inp{};
    const rknn_tensor_attr* model_in = sess.ok() ? &sess.inAttr(0) : nullptr;
    if (!buildDetectInput(img_lb, mode, model_in, pass_through, rgb_swap, u8, f32, inp)) return -1;
    if (tm)
        tm->det_input +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_in0).count();

    // Fast path: INT8 outputs + 3-head ? copy int8 only, dequant inside early-gated decode.
    const bool use_i8 =
        sess.nOutput() == 3 && meta3 && meta3->valid && sess.outputsAreInt8Affine();
    if (use_i8) {
        std::vector<std::vector<int8_t>> outs_i8;
        double ms_run = 0., ms_fetch = 0.;
        int ret_i8 = sess.inferInt8(&inp, 1, outs_i8, &ms_run, &ms_fetch);
        if (ret_i8 == RKNN_SUCC && outs_i8.size() >= 3) {
            if (tm) {
                tm->det_npu += ms_run;
                tm->det_output += ms_fetch;
            }
            static bool logged_i8 = false;
            if (!logged_i8) {
                logged_i8 = true;
                std::cout << "[rknn] detect fast path: INT8 outputs + gated dequant decode\n";
            }
            auto t_dec0 = std::chrono::steady_clock::now();
            if (!decodeThreeHeadsInt8(sess, outs_i8, *meta3, conf_thresh, rawflat, N)) return -99;
            if (tm)
                tm->det_decode += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t_dec0)
                                      .count();
            return RKNN_SUCC;
        }
        std::cerr << "[WARN] detect inferInt8 failed ret=" << ret_i8 << ", fallback float\n";
    }

    std::vector<std::vector<float>> outs;
    double ms_run = 0., ms_fetch = 0.;
    int ret = sess.infer(&inp, 1, outs, &ms_run, &ms_fetch);
    if (tm) {
        tm->det_npu += ms_run;
        tm->det_output += ms_fetch;
    }
    if (ret != RKNN_SUCC || outs.empty()) {
        if (ret != RKNN_SUCC)
            std::cerr << "[ERROR] detect infer failed mode=" << mode << " ret=" << ret << std::endl;
        return ret;
    }
    auto t_dec0 = std::chrono::steady_clock::now();
    if (sess.nOutput() == 3) {
        if (!meta3 || !meta3->valid) {
            std::cerr << "[ERROR] 3-head detect requires weights/RK_plate_detect_meta.txt\n";
            return -98;
        }
        if (!decodeThreeHeads(sess, outs, *meta3, conf_thresh, rawflat, N)) return -99;
        if (tm)
            tm->det_decode +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_dec0)
                    .count();
        return RKNN_SUCC;
    }
    bool ok = layoutRawToAnchors(sess.outAttr(0), outs[0].data(), rawflat, N);
    if (tm)
        tm->det_decode +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_dec0).count();
    return ok ? RKNN_SUCC : -99;
}

int runDetect(RknnSession& sess, const cv::Mat& img_lb, const char* mode, const DetectRknnMeta* meta3,
              std::vector<float>& rawflat, int& N, float conf_thresh, PlateTimings* tm = nullptr) {
    auto try_once = [&](bool pass_through, const char* tag) -> int {
        int ret = runDetectOnce(sess, img_lb, mode, pass_through, false, meta3, rawflat, N,
                                conf_thresh, tm);
        if (ret != RKNN_SUCC) return ret;
        if (warnDeadDetectOutput(rawflat, N, tag)) return -100;
        return RKNN_SUCC;
    };

    int ret = try_once(false, "uint8_nhwc");
    if (ret == RKNN_SUCC) return RKNN_SUCC;

    if (sess.inputIsQuantized()) {
        std::cout << "[rknn] detect retry pass_through (INT8)\n";
        ret = try_once(true, "uint8_nhwc_pt");
        if (ret == RKNN_SUCC) {
            std::cout << "[rknn] detect input recovered via uint8_nhwc_pt\n";
            return RKNN_SUCC;
        }
    }
    return ret == -100 ? RKNN_SUCC : ret;
}

// ---------------------------------------------------------------------------
// Geometry (onnx / python parity)
// ---------------------------------------------------------------------------
cv::Point2f orderPoints(const cv::Point2f pts[4]) {
    cv::Point2f rect[4];
    float s[4], d[4];
    for (int i = 0; i < 4; i++) {
        s[i] = pts[i].x + pts[i].y;
        d[i] = pts[i].y - pts[i].x;
    }
    int i0 = 0, i2 = 0, i1 = 0, i3 = 0;
    for (int i = 1; i < 4; i++) {
        if (s[i] < s[i0]) i0 = i;
        if (s[i] > s[i2]) i2 = i;
        if (d[i] < d[i1]) i1 = i;
        if (d[i] > d[i3]) i3 = i;
    }
    rect[0] = pts[i0];
    rect[1] = pts[i1];
    rect[2] = pts[i2];
    rect[3] = pts[i3];
    return rect[0]; // unused ¡ª use array version below
}

void orderPointsArr(const cv::Point2f in[4], cv::Point2f out[4]) {
    float s[4], d[4];
    for (int i = 0; i < 4; i++) {
        s[i] = in[i].x + in[i].y;
        d[i] = in[i].y - in[i].x;
    }
    int i0 = 0, i2 = 0, i1 = 0, i3 = 0;
    for (int i = 1; i < 4; i++) {
        if (s[i] < s[i0]) i0 = i;
        if (s[i] > s[i2]) i2 = i;
        if (d[i] < d[i1]) i1 = i;
        if (d[i] > d[i3]) i3 = i;
    }
    out[0] = in[i0];
    out[1] = in[i1];
    out[2] = in[i2];
    out[3] = in[i3];
}

cv::Mat fourPointTransform(const cv::Mat& image, const cv::Point2f pts[4]) {
    cv::Point2f rect[4];
    orderPointsArr(pts, rect);
    float wa = std::sqrt(std::pow(rect[2].x - rect[3].x, 2) + std::pow(rect[2].y - rect[3].y, 2));
    float wb = std::sqrt(std::pow(rect[1].x - rect[0].x, 2) + std::pow(rect[1].y - rect[0].y, 2));
    int maxW = std::max((int)wa, (int)wb);
    float ha = std::sqrt(std::pow(rect[1].x - rect[2].x, 2) + std::pow(rect[1].y - rect[2].y, 2));
    float hb = std::sqrt(std::pow(rect[0].x - rect[3].x, 2) + std::pow(rect[0].y - rect[3].y, 2));
    int maxH = std::max((int)ha, (int)hb);
    cv::Point2f dst[4] = {cv::Point2f(0, 0), cv::Point2f((float)maxW - 1, 0),
                          cv::Point2f((float)maxW - 1, (float)maxH - 1), cv::Point2f(0, (float)maxH - 1)};
    cv::Mat M = cv::getPerspectiveTransform(rect, dst);
    cv::Mat warped;
    cv::warpPerspective(image, warped, M, cv::Size(maxW, maxH));
    return warped;
}

cv::Mat getSplitMerge(const cv::Mat& img) {
    int h = img.rows;
    cv::Mat upper = img(cv::Rect(0, 0, img.cols, (int)(5.0 / 12.0 * h)));
    cv::Mat lower = img(cv::Rect(0, (int)(1.0 / 3.0 * h), img.cols, h - (int)(1.0 / 3.0 * h)));
    cv::resize(upper, upper, lower.size());
    cv::Mat out;
    cv::hconcat(upper, lower, out);
    return out;
}

void expandLandmarksLeftEdgeOnly(cv::Point2f p[4], float left_ratio) {
    cv::Point2f o[4];
    orderPointsArr(p, o);
    float x_min = o[0].x, x_max = o[0].x;
    for (int i = 1; i < 4; i++) {
        x_min = std::min(x_min, o[i].x);
        x_max = std::max(x_max, o[i].x);
    }
    float w = std::max(4.f, x_max - x_min);
    for (int i = 0; i < 4; i++) {
        p[i] = o[i];
        if (p[i].x <= x_min + w * 0.42f) p[i].x -= w * left_ratio;
    }
}

void expandLandmarksLeftByHeight(cv::Point2f p[4], float height_frac) {
    float y_min = p[0].y, y_max = p[0].y, x_min = p[0].x, x_max = p[0].x;
    for (int i = 1; i < 4; i++) {
        y_min = std::min(y_min, p[i].y);
        y_max = std::max(y_max, p[i].y);
        x_min = std::min(x_min, p[i].x);
        x_max = std::max(x_max, p[i].x);
    }
    float h_plate = std::max(6.f, y_max - y_min);
    float w_plate = std::max(6.f, x_max - x_min);
    float dx = height_frac * h_plate;
    float cut = x_min + 0.38f * w_plate;
    for (int i = 0; i < 4; i++)
        if (p[i].x <= cut) p[i].x -= dx;
}

cv::Mat makeRecRoiOnnxStyle(const cv::Mat& img0, const float landmarks[8], float rec_left_pad) {
    cv::Point2f lm[4];
    for (int i = 0; i < 4; i++) lm[i] = cv::Point2f(landmarks[2 * i], landmarks[2 * i + 1]);
    if (rec_left_pad > 1e-6f) expandLandmarksLeftEdgeOnly(lm, rec_left_pad);
    return fourPointTransform(img0, lm);
}

void landmarksToVizQuad(const float landmarks[8], float margin_h, const cv::Size& img_shape,
                        cv::Point pts[4]) {
    cv::Point2f lm[4];
    for (int i = 0; i < 4; i++) lm[i] = cv::Point2f(landmarks[2 * i], landmarks[2 * i + 1]);
    cv::Point2f ordered[4];
    orderPointsArr(lm, ordered);
    if (margin_h > 1e-9f) {
        float side_h = (cv::norm(ordered[0] - ordered[3]) + cv::norm(ordered[1] - ordered[2])) * 0.5f;
        float h_plate = std::max(4.f, side_h);
        float dy = margin_h * h_plate;
        ordered[0].y -= dy;
        ordered[1].y -= dy;
        ordered[2].y += dy;
        ordered[3].y += dy;
    }
    int iw = img_shape.width, ih = img_shape.height;
    for (int i = 0; i < 4; i++) {
        float ox = std::max(0.f, std::min((float)(iw - 1), ordered[i].x));
        float oy = std::max(0.f, std::min((float)(ih - 1), ordered[i].y));
        pts[i] = cv::Point((int)std::lrint(ox), (int)std::lrint(oy));
    }
}

// ---------------------------------------------------------------------------
// Recognition
// ---------------------------------------------------------------------------
PlateSeqLayout parsePlateSeqDims(const rknn_tensor_attr& attr) {
    PlateSeqLayout L;
    uint32_t d0 = 1, d1 = 1, d2 = 1;
    if (attr.n_dims >= 3) {
        d0 = attr.dims[0];
        d1 = attr.dims[1];
        d2 = attr.dims[2];
    } else if (attr.n_dims == 2) {
        d1 = attr.dims[0];
        d2 = attr.dims[1];
    }
    if (d1 == (uint32_t)kNumPlateClasses && d2 != (uint32_t)kNumPlateClasses) {
        L.classes_first = true;
        L.num_classes = (int)d1;
        L.seq_len = (int)d2;
    } else if (d2 == (uint32_t)kNumPlateClasses) {
        L.classes_first = false;
        L.num_classes = (int)d2;
        L.seq_len = (int)d1;
    } else if (d1 == 21 && d2 == 78) {
        L.classes_first = false;
        L.seq_len = 21;
        L.num_classes = 78;
    } else if (d1 == 78 && d2 == 21) {
        L.classes_first = true;
        L.seq_len = 21;
        L.num_classes = 78;
    }
    (void)d0;
    return L;
}

float logitAt(const std::vector<float>& logits, const PlateSeqLayout& L, int t, int c) {
    if (L.classes_first)
        return logits[(size_t)c * (size_t)L.seq_len + (size_t)t];
    return logits[(size_t)t * (size_t)L.num_classes + (size_t)c];
}

std::string decodePlate(const std::vector<int>& preds) {
    const std::vector<std::string>& table = plateNameTable();
    int pre = 0;
    std::string out;
    for (int pi : preds) {
        if (pi != 0 && pi != pre) {
            if (pi >= 0 && pi < (int)table.size()) out += table[(size_t)pi];
        }
        pre = pi;
    }
    return out;
}

bool buildRecInput(const cv::Mat& roi, const char* mode, cv::Mat& resized, std::vector<uint8_t>& u8buf,
                   std::vector<float>& fbuf, rknn_input& inp) {
    cv::resize(roi, resized, cv::Size(kRecW, kRecH));
    memset(&inp, 0, sizeof(inp));
    inp.index = 0;
    if (strcmp(mode, "uint8_nhwc") == 0) {
        u8buf.assign(resized.data, resized.data + resized.total() * resized.elemSize());
        inp.type = RKNN_TENSOR_UINT8;
        inp.fmt = RKNN_TENSOR_NHWC;
        inp.size = (uint32_t)u8buf.size();
        inp.buf = u8buf.data();
        return true;
    }
    if (strcmp(mode, "float_nchw_rgb") == 0 || strcmp(mode, "float_nchw_bgr") == 0) {
        cv::Mat f32;
        resized.convertTo(f32, CV_32FC3);
        f32 = (f32 / 255.0f - kRecMean) / kRecStd;
        if (strcmp(mode, "float_nchw_rgb") == 0) cv::cvtColor(f32, f32, cv::COLOR_BGR2RGB);
        std::vector<cv::Mat> ch(3);
        cv::split(f32, ch);
        fbuf.resize((size_t)3 * kRecH * kRecW);
        size_t plane = (size_t)kRecH * kRecW;
        for (int c = 0; c < 3; c++) memcpy(fbuf.data() + c * plane, ch[c].data, plane * sizeof(float));
        inp.type = RKNN_TENSOR_FLOAT32;
        inp.fmt = RKNN_TENSOR_NCHW;
        inp.size = (uint32_t)(fbuf.size() * sizeof(float));
        inp.buf = fbuf.data();
        return true;
    }
    return false;
}

bool getPlateResult(RknnSession& rec, const cv::Mat& roi, const char* rec_mode, std::string& plate_no,
                    std::string& plate_color, double* ms_npu = nullptr) {
    cv::Mat resized;
    std::vector<uint8_t> u8;
    std::vector<float> f32;
    rknn_input inp{};
    if (!buildRecInput(roi, rec_mode, resized, u8, f32, inp)) return false;
    std::vector<std::vector<float>> outs;
    double ms_run = 0., ms_fetch = 0.;
    if (rec.infer(&inp, 1, outs, &ms_run, &ms_fetch) != RKNN_SUCC || outs.size() < 2) return false;
    if (ms_npu) *ms_npu += ms_run + ms_fetch;

    PlateSeqLayout L = parsePlateSeqDims(rec.outAttr(0));
    const std::vector<float>& plate_logits = outs[0];
    std::vector<int> seq_preds((size_t)L.seq_len);
    for (int t = 0; t < L.seq_len; t++) {
        int best = 0;
        float bestv = -1e30f;
        for (int c = 0; c < L.num_classes; c++) {
            float v = logitAt(plate_logits, L, t, c);
            if (v > bestv) {
                bestv = v;
                best = c;
            }
        }
        seq_preds[(size_t)t] = best;
    }
    plate_no = decodePlate(seq_preds);

    const std::vector<float>& color_logits = outs[1];
    int color_idx = 0;
    float cv_max = color_logits[0];
    for (size_t i = 1; i < color_logits.size(); i++) {
        if (color_logits[i] > cv_max) {
            cv_max = color_logits[i];
            color_idx = (int)i;
        }
    }
    if (color_idx < 0 || color_idx >= kPlateColorListN) color_idx = 0;
    plate_color = vizPlateColorForNumber(plate_no, kPlateColorList[color_idx]);
    return !plate_no.empty();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
std::string findPlateFont(const std::string& user_font) {
    if (!user_font.empty()) return user_font;
    const char* candidates[] = {"fonts/platech.ttf", "../fonts/platech.ttf",
                                "../Chinese_license_plate_detection_recognition-main/fonts/platech.ttf"};
    for (const char* c : candidates) {
        FILE* f = fopen(c, "rb");
        if (f) {
            fclose(f);
            return c;
        }
    }
    return "";
}

void printCnRenderStatus(const std::string& user_font) {
#ifdef USE_OPENCV_FREETYPE
    std::string path = findPlateFont(user_font);
    if (!path.empty()) {
        std::cout << "[INFO] Chinese label overlay: ENABLED (OpenCV freetype, font=" << path
                  << ")\n";
    } else {
        std::cout << "[WARN] Chinese label overlay: ENABLED (OpenCV freetype) but no font file; "
                     "use --font fonts/platech.ttf\n";
    }
#else
    (void)user_font;
    std::cout << "[WARN] Chinese label overlay: DISABLED (built without opencv_freetype); "
                 "labels show ASCII only. Install/link opencv_freetype and rebuild.\n";
#endif
}

std::string toAsciiFallback(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 128) out.push_back((char)c);
    }
    if (out.empty()) return "plate";
    return out;
}

#ifdef USE_OPENCV_FREETYPE
cv::Ptr<cv::freetype::FreeType2> getFreeTypeEngine(const std::string& font_path) {
    static cv::Ptr<cv::freetype::FreeType2> ft;
    static std::string loaded_path;
    if (ft.empty()) ft = cv::freetype::createFreeType2();
    if (loaded_path != font_path) {
        ft->loadFontData(font_path, 0); // OpenCV 4.6: returns void
        loaded_path = font_path;
    }
    return ft;
}

bool freetypeTextSize(const std::string& font_path, const std::string& text, int font_px,
                      cv::Size& out, int& baseline) {
    if (font_path.empty()) return false;
    FILE* f = fopen(font_path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    try {
        cv::Ptr<cv::freetype::FreeType2> ft = getFreeTypeEngine(font_path);
        baseline = 0;
        out = ft->getTextSize(text, font_px, -1, &baseline);
        return out.width > 0 && out.height > 0;
    } catch (...) {
        return false;
    }
}

bool drawTextFreetype(cv::Mat& img, const std::string& text, int x, int y, const std::string& font_path,
                      int text_size, const cv::Scalar& color) {
    if (font_path.empty() || text.empty()) return false;
    FILE* f = fopen(font_path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    try {
        cv::Ptr<cv::freetype::FreeType2> ft = getFreeTypeEngine(font_path);
        ft->putText(img, text, cv::Point(x, y), text_size, color, -1, cv::LINE_AA, false);
        return true;
    } catch (...) {
        return false;
    }
}

struct DisplayColorStyle {
    cv::Scalar bgr;
    bool stroke;
};

DisplayColorStyle bgrForDisplayColorCn(const std::string& name_cn) {
    static const cv::Scalar kBgr[] = {
        cv::Scalar(34, 34, 34),       // ?? RGB(34,34,34)
        cv::Scalar(220, 110, 32),     // ?? RGB(32,110,220)
        cv::Scalar(70, 160, 55),      // ?? RGB(55,160,70)
        cv::Scalar(252, 252, 252),    // ?? RGB(252,252,252)
        cv::Scalar(45, 189, 226),     // ?? RGB(226,189,45)
    };
    for (int i = 0; i < kPlateColorListN; i++) {
        if (name_cn == kPlateColorList[i]) return {kBgr[i], i == 3};
    }
    return {cv::Scalar(80, 80, 80), false};
}

int freetypeTextWidth(const std::string& font_path, const std::string& text, int font_px) {
    cv::Size ts;
    int baseline = 0;
    if (!freetypeTextSize(font_path, text, font_px, ts, baseline)) return 0;
    return std::max(1, ts.width);
}

void drawFreetypeColorWord(cv::Mat& img, const std::string& text, int x, int y,
                           const std::string& font_path, int font_px, const DisplayColorStyle& style) {
    if (style.stroke) {
        int sw = std::max(1, font_px / 9);
        cv::Scalar stroke(55, 55, 55);
        for (int dx = -sw; dx <= sw; dx++) {
            for (int dy = -sw; dy <= sw; dy++) {
                if (dx == 0 && dy == 0) continue;
                drawTextFreetype(img, text, x + dx, y + dy, font_path, font_px, stroke);
            }
        }
    }
    drawTextFreetype(img, text, x, y, font_path, font_px, style.bgr);
}
#endif

void drawLabelWhiteBg(cv::Mat& orgimg, const std::string& plate_no, const std::string& /*plate_color_cn*/,
                      int anchor_x, int box_top_y, const std::string& font_path, int font_px) {
    if (plate_no.empty()) return;
    int pad = std::max(4, font_px / 6);
    const int border_th = std::max(1, font_px / 18);
    const std::string& full_line = plate_no;
    std::string path = findPlateFont(font_path);

#ifdef USE_OPENCV_FREETYPE
    if (!path.empty()) {
        cv::Size ts;
        int baseline = 0;
        if (freetypeTextSize(path, full_line, font_px, ts, baseline)) {
            int tw = std::max(1, ts.width);
            int th = std::max(1, ts.height + baseline);
            int bx1 = std::max(0, anchor_x);
            int by2 = std::max(0, box_top_y - 2);
            int by1 = std::max(0, by2 - th - 2 * pad);
            int bx2 = std::min(orgimg.cols - 1, bx1 + tw + 2 * pad);
            cv::rectangle(orgimg, cv::Point(bx1, by1), cv::Point(bx2, by2), cv::Scalar(255, 255, 255), -1);
            cv::rectangle(orgimg, cv::Point(bx1, by1), cv::Point(bx2, by2), cv::Scalar(0, 0, 0), border_th);
            drawTextFreetype(orgimg, plate_no, bx1 + pad, by1 + pad, path, font_px, cv::Scalar(0, 0, 0));
            return;
        }
    }
#endif

    std::string ascii_line = toAsciiFallback(full_line);
    if (ascii_line.empty()) ascii_line = plate_no;
    double fs = std::max(0.4, font_px / 28.0);
    int baseline = 0;
    cv::Size ts = cv::getTextSize(ascii_line, cv::FONT_HERSHEY_SIMPLEX, fs, 1, &baseline);
    int th = ts.height + baseline;
    int by2 = std::max(0, box_top_y - 2);
    int by1 = std::max(0, by2 - th - 2 * pad);
    int bx1 = std::max(0, anchor_x);
    int bx2 = std::min(orgimg.cols - 1, bx1 + ts.width + 2 * pad);
    cv::rectangle(orgimg, cv::Point(bx1, by1), cv::Point(bx2, by2), cv::Scalar(255, 255, 255), -1);
    cv::rectangle(orgimg, cv::Point(bx1, by1), cv::Point(bx2, by2), cv::Scalar(0, 0, 0), border_th);
    const int text_th = std::max(1, font_px / 14);
    cv::putText(orgimg, ascii_line, cv::Point(bx1 + pad, by2 - pad), cv::FONT_HERSHEY_SIMPLEX, fs,
                cv::Scalar(0, 0, 0), text_th, cv::LINE_AA);
}

void drawResults(cv::Mat& orgimg, const std::vector<PlateItem>& items, const std::string& font_path,
                 float label_scale, float viz_margin, bool stable_rect_style, bool draw_labels,
                 bool log_plate_text) {
    std::string result_str;
    for (const PlateItem& item : items) {
        float lm[8];
        for (int i = 0; i < 8; i++) lm[i] = item.landmarks[(size_t)i];
        cv::Point quad[4];
        landmarksToVizQuad(lm, viz_margin, orgimg.size(), quad);
        int lx = quad[0].x, ly = quad[0].y;
        int rx = quad[0].x, ry = quad[0].y;
        for (int i = 1; i < 4; i++) {
            lx = std::min(lx, quad[i].x);
            ly = std::min(ly, quad[i].y);
            rx = std::max(rx, quad[i].x);
            ry = std::max(ry, quad[i].y);
        }
        if (log_plate_text) result_str += item.plate_no + " ";
        const int bw = std::max(1, rx - lx);
        const int bh = std::max(1, ry - ly);
        double eds[4];
        for (int i = 0; i < 4; i++) {
            cv::Point2f a(quad[i]), b(quad[(i + 1) % 4]);
            eds[i] = cv::norm(a - b);
        }
        double edge_h = (eds[0] + eds[1] + eds[2] + eds[3]) * 0.25;
        int thickness = std::max(2, std::min(6, (int)std::lround(edge_h) / 40 + 2));

        if (stable_rect_style) {
            cv::rectangle(orgimg, cv::Point(lx, ly), cv::Point(rx, ry), kVizQuadBgr, thickness,
                          cv::LINE_AA);
        } else {
            int pt = std::max(4, std::min(24, (int)std::lround(edge_h) / 35 + 4));
            thickness = std::max(1, pt / 3);
            for (int i = 0; i < 4; i++) cv::circle(orgimg, quad[i], pt, kVizQuadBgr, -1, cv::LINE_AA);
            const cv::Point poly_pts[1][4] = {{quad[0], quad[1], quad[2], quad[3]}};
            const cv::Point* ppt[1] = {poly_pts[0]};
            int npt[] = {4};
            cv::polylines(orgimg, ppt, npt, 1, true, kVizQuadBgr, thickness, cv::LINE_AA);
        }

        const int kLabelMinPx = 20;
        const int kLabelMaxPx = 42;
        int th = std::max(kLabelMinPx, std::min(kLabelMaxPx, (int)std::lround((double)bh * label_scale)));
        if (draw_labels)
            drawLabelWhiteBg(orgimg, item.plate_no, item.plate_color, lx, ly, font_path, th);
    }
    if (log_plate_text && !result_str.empty()) std::cout << result_str << std::endl;
}

// ---------------------------------------------------------------------------
// Temporal viz stabilizer (display only; detect/rec use raw landmarks)
// ---------------------------------------------------------------------------
struct VizTrack {
    float cx = 0.f;
    float cy = 0.f;
    float w = 0.f;
    float h = 0.f;
    std::string plate_no;
    int miss = 0;
    bool init = false;
};

class VizStabilizer {
public:
    void reset() { tracks_.clear(); }

    void configure(float alpha, int hold_frames, bool stable_rect) {
        alpha_ = std::max(0.05f, std::min(1.f, alpha));
        hold_frames_ = std::max(0, hold_frames);
        stable_rect_ = stable_rect;
    }

    void apply(std::vector<PlateItem>& items) {
        std::vector<bool> matched(tracks_.size(), false);

        for (auto& item : items) {
            if (item.landmarks.size() < 8) continue;

            float cx = 0.f, cy = 0.f, bw = 0.f, bh = 0.f;
            landmarksAabb(item.landmarks.data(), cx, cy, bw, bh);

            const int ti = matchTrack(item.plate_no, cx, cy, bw, bh, matched);
            VizTrack* tr = nullptr;
            if (ti >= 0) {
                tr = &tracks_[(size_t)ti];
                matched[(size_t)ti] = true;
            } else {
                tracks_.push_back(VizTrack{});
                tr = &tracks_.back();
                matched.push_back(true);
            }

            if (!tr->init) {
                tr->cx = cx;
                tr->cy = cy;
                tr->w = bw;
                tr->h = bh;
                tr->init = true;
            } else {
                ema(tr->cx, cx);
                ema(tr->cy, cy);
                ema(tr->w, bw);
                ema(tr->h, bh);
            }
            tr->plate_no = item.plate_no;
            tr->miss = 0;

            if (stable_rect_) {
                aabbToLandmarks(tr->cx, tr->cy, tr->w, tr->h, item.landmarks);
            }
        }

        for (size_t i = 0; i < tracks_.size();) {
            if (i < matched.size() && matched[i]) {
                ++i;
                continue;
            }
            tracks_[i].miss++;
            if (tracks_[i].miss > hold_frames_) {
                tracks_.erase(tracks_.begin() + (ptrdiff_t)i);
                matched.erase(matched.begin() + (ptrdiff_t)i);
            } else {
                ++i;
            }
        }
    }

    /** When detect misses briefly, reuse last smoothed box for display. */
    bool appendHeldDrawItems(std::vector<PlateItem>& items) const {
        bool any = false;
        for (const VizTrack& tr : tracks_) {
            if (!tr.init || tr.miss <= 0 || tr.miss > hold_frames_) continue;
            PlateItem it;
            it.plate_no = tr.plate_no;
            it.plate_color = "";
            aabbToLandmarks(tr.cx, tr.cy, tr.w, tr.h, it.landmarks);
            items.push_back(std::move(it));
            any = true;
        }
        return any;
    }

    /** Active + briefly-held tracks (for video skip-frame overlay). */
    bool appendVisibleDrawItems(std::vector<PlateItem>& items) const {
        bool any = false;
        for (const VizTrack& tr : tracks_) {
            if (!tr.init || tr.miss > hold_frames_) continue;
            PlateItem it;
            it.plate_no = tr.plate_no;
            it.plate_color = "";
            aabbToLandmarks(tr.cx, tr.cy, tr.w, tr.h, it.landmarks);
            items.push_back(std::move(it));
            any = true;
        }
        return any;
    }

    void markMissedFrame() {
        for (size_t i = 0; i < tracks_.size();) {
            if (tracks_[i].init) tracks_[i].miss++;
            if (tracks_[i].miss > hold_frames_) {
                tracks_.erase(tracks_.begin() + (ptrdiff_t)i);
            } else {
                ++i;
            }
        }
    }

private:
    static void landmarksAabb(const float* lm, float& cx, float& cy, float& bw, float& bh) {
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int i = 0; i < 4; i++) {
            minx = std::min(minx, lm[2 * i]);
            maxx = std::max(maxx, lm[2 * i]);
            miny = std::min(miny, lm[2 * i + 1]);
            maxy = std::max(maxy, lm[2 * i + 1]);
        }
        bw = std::max(4.f, maxx - minx);
        bh = std::max(4.f, maxy - miny);
        cx = minx + bw * 0.5f;
        cy = miny + bh * 0.5f;
    }

    static void aabbToLandmarks(float cx, float cy, float bw, float bh, std::vector<float>& lm) {
        lm.resize(8);
        const float x0 = cx - bw * 0.5f;
        const float y0 = cy - bh * 0.5f;
        const float x1 = cx + bw * 0.5f;
        const float y1 = cy + bh * 0.5f;
        lm[0] = x0;
        lm[1] = y0;
        lm[2] = x1;
        lm[3] = y0;
        lm[4] = x1;
        lm[5] = y1;
        lm[6] = x0;
        lm[7] = y1;
    }

    void ema(float& state, float value) { state = alpha_ * value + (1.f - alpha_) * state; }

    int matchTrack(const std::string& plate_no, float cx, float cy, float bw, float bh,
                   const std::vector<bool>& matched) const {
        int best = -1;
        float best_cost = 1e9f;
        const float gate = std::max(64.f, (bw + bh) * 0.75f);
        const float gate2 = gate * gate;

        for (size_t i = 0; i < tracks_.size(); i++) {
            if (matched[i] || !tracks_[i].init) continue;
            if (!plate_no.empty() && !tracks_[i].plate_no.empty() &&
                tracks_[i].plate_no == plate_no) {
                return (int)i;
            }
            const float dx = tracks_[i].cx - cx;
            const float dy = tracks_[i].cy - cy;
            const float cost = dx * dx + dy * dy;
            if (cost < best_cost && cost <= gate2) {
                best_cost = cost;
                best = (int)i;
            }
        }
        return best;
    }

    float alpha_ = 0.7f;
    int hold_frames_ = 10;
    bool stable_rect_ = true;
    std::vector<VizTrack> tracks_;
};

// ---------------------------------------------------------------------------
// Detect pick (auto modes)
// ---------------------------------------------------------------------------
bool pickDetectInput(RknnSession& det, const cv::Mat& img_lb, double r, int left, int top, float conf,
                     float iou, const char* prefer, bool debug, int topk, const cv::Size& img_shape,
                     const DetectRknnMeta* meta3, std::vector<PlateDetectRow>& out,
                     std::string& mode_used, PlateTimings* tm = nullptr) {
    const char* effective_prefer = prefer;
    if (det.inputIsQuantized() &&
        (strcmp(prefer, "auto") == 0 || strncmp(prefer, "float_", 6) == 0)) {
        effective_prefer = "uint8_nhwc";
    }

    const char* modes[4];
    int nm = 0;
    if (strcmp(effective_prefer, "auto") != 0) {
        modes[0] = effective_prefer;
        nm = 1;
    } else if (det.inputIsQuantized()) {
        modes[0] = "uint8_nhwc";
        nm = 1;
    } else {
        for (int i = 0; i < kDetectModesN; i++) modes[nm++] = kDetectModes[i];
    }

    std::vector<PlateDetectRow> best_out;
    std::string best_mode = modes[0];
    float best_obj = -1.f;

    for (int mi = 0; mi < nm; mi++) {
        const char* mode = modes[mi];
        std::vector<float> raw;
        int N = 0;
        if (runDetect(det, img_lb, mode, meta3, raw, N, conf, tm) != RKNN_SUCC) continue;
        bool dead = warnDeadDetectOutput(raw, N, mode);
        if (debug) debugDetectTensor(raw, N, mode);
        if (dead || scoresAreDegenerate(raw, N)) continue;
        float obj_max = 0.f;
        for (int i = 0; i < N; i++) obj_max = std::max(obj_max, raw[(size_t)i * kDetectRawDim + 4]);
        std::vector<PlateDetectRow> cur;
        auto t_post0 = std::chrono::steady_clock::now();
        postProcessing(raw, r, left, top, conf, iou, topk, img_shape, cur);
        if (tm)
            tm->det_post +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_post0)
                    .count();
        if (!cur.empty() && !outputScoresLookFake(cur)) {
            out.swap(cur);
            mode_used = mode;
            return true;
        }
        if (!cur.empty() && obj_max > best_obj) {
            best_obj = obj_max;
            best_out.swap(cur);
            best_mode = mode;
        }
    }
    out.swap(best_out);
    mode_used = best_mode;
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Frame process
// ---------------------------------------------------------------------------
void printPlateTimingReport(const std::string& prefix, const PlateTimings& t, double read_ms,
                            double write_ms, double total_ms) {
    const double det_cpu = t.det_input + t.det_decode + t.det_post;
    const double sum_check = read_ms + t.process_wall + write_ms;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << prefix << "[timing] ---- ms breakdown ----\n";
    std::cout << prefix << "  read(imread)     " << read_ms << "\n";
    std::cout << prefix << "  letterbox        " << t.letterbox << "\n";
    std::cout << prefix << "  det_npu          " << t.det_npu << "\n";
    std::cout << prefix << "  det_out          " << t.det_output << "\n";
    std::cout << prefix << "  det_decode       " << t.det_decode << "\n";
    std::cout << prefix << "  det_post         " << t.det_post << "\n";
    std::cout << prefix << "  det_input        " << t.det_input << "\n";
    std::cout << prefix << "  detect(total)    " << t.detect_total << "  (cpu=" << det_cpu
              << ")\n";
    std::cout << prefix << "  rec_roi          " << t.rec_roi << "\n";
    std::cout << prefix << "  rec_npu          " << t.rec_npu << "\n";
    std::cout << prefix << "  rec(total)       " << t.rec_total << "\n";
    std::cout << prefix << "  draw             " << t.draw << "\n";
    std::cout << prefix << "  write(imwrite)   " << write_ms << "\n";
    std::cout << prefix << "  infer_wall       " << t.infer_wall
              << "  (= letterbox + detect + rec, no draw/IO)\n";
    std::cout << prefix << "  process_wall     " << t.process_wall
              << "  (= letterbox + detect + rec + draw)\n";
    std::cout << prefix << "  total            " << total_ms << "  (= read + process_wall + write"
              << ", check=" << sum_check << ")\n";
}

void printFrameLatencyReport(const std::string& prefix, const PlateTimings& t,
                             const HostFrameTimings& host, int frame_index) {
    const double det_cpu = t.det_input + t.det_decode + t.det_post;
    const double host_io = host.pcie_dma + host.rgb_convert + host.read_imread + host.write_imwrite;
    const double sum_check =
        host_io + t.process_wall + host.qimage + host.e2e_total * 0.0; // e2e is wall, not additive

    std::cout << std::fixed << std::setprecision(2);
    std::cout << prefix << "[latency] ---- ms breakdown ----";
    if (frame_index >= 0) std::cout << "  frame=" << frame_index;
    std::cout << "\n";

    if (host.pcie_dma > 0.) std::cout << prefix << "  pcie_dma         " << host.pcie_dma << "\n";
    if (host.rgb_convert > 0.) std::cout << prefix << "  rgb_convert      " << host.rgb_convert << "\n";
    if (host.read_imread > 0.) std::cout << prefix << "  read(imread)     " << host.read_imread << "\n";

    std::cout << prefix << "  letterbox        " << t.letterbox << "\n";
    std::cout << prefix << "  det_input        " << t.det_input << "\n";
    std::cout << prefix << "  det_npu          " << t.det_npu << "\n";
    std::cout << prefix << "  det_output       " << t.det_output << "\n";
    std::cout << prefix << "  det_decode       " << t.det_decode << "\n";
    std::cout << prefix << "  det_post         " << t.det_post << "\n";
    std::cout << prefix << "  detect(total)    " << t.detect_total << "  (cpu=" << det_cpu << ")\n";
    std::cout << prefix << "  rec_roi          " << t.rec_roi << "\n";
    std::cout << prefix << "  rec_npu          " << t.rec_npu << "\n";
    std::cout << prefix << "  rec(total)       " << t.rec_total << "\n";
    std::cout << prefix << "  draw             " << t.draw << "\n";
    std::cout << prefix << "  infer_wall       " << t.infer_wall
              << "  (= letterbox + detect + rec)\n";
    std::cout << prefix << "  process_wall     " << t.process_wall
              << "  (= infer_wall + draw)\n";

    if (host.qimage > 0.) std::cout << prefix << "  qimage           " << host.qimage << "\n";
    if (host.write_imwrite > 0.)
        std::cout << prefix << "  write(imwrite)   " << host.write_imwrite << "\n";
    if (host.e2e_total > 0.)
        std::cout << prefix << "  e2e_total        " << host.e2e_total
                  << "  (host_io=" << host_io << " + process=" << t.process_wall
                  << " + qimage=" << host.qimage << ")\n";
    (void)sum_check;
}

int processFrame(RknnSession& det, RknnSession& rec, const cv::Mat& img0, cv::Mat& out_img,
                 std::vector<PlateItem>& items, const PlateRknnConfig& cfg,
                 const std::vector<const char*>& rec_modes, const DetectRknnMeta* meta3,
                 const std::string& tag, PlateTimings* timings_out, VizStabilizer* viz_smooth) {
    std::string prefix = tag.empty() ? "" : tag + " ";
    if (img0.empty()) return 1;

    // Throttle plate=/[OK]/HINT spam (PCIe ~15FPS would otherwise flood the terminal).
    static int s_result_log_cnt = 0;
    s_result_log_cnt++;
    const bool log_results =
        cfg.result_log_every <= 0 || (s_result_log_cnt % cfg.result_log_every) == 0;

    PlateTimings tm{};
    PlateTimings* tm_ptr = (timings_out != nullptr || cfg.profile) ? &tm : nullptr;

    double r = 0.;
    int left = 0, top = 0;
    cv::Mat img_lb;
    auto t_lb0 = std::chrono::steady_clock::now();
    letterBox(img0, cfg.img_size, img_lb, r, left, top);
    if (tm_ptr)
        tm_ptr->letterbox =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_lb0)
                .count();

    auto t0 = std::chrono::steady_clock::now();
    auto t_det0 = t0;

    std::vector<PlateDetectRow> dets;
    std::string det_mode;
    pickDetectInput(det, img_lb, r, left, top, cfg.conf, cfg.iou, cfg.input_mode.c_str(), cfg.debug,
                    cfg.topk, img0.size(), meta3, dets, det_mode, tm_ptr);

    auto t_det1 = std::chrono::steady_clock::now();
    double ms_detect = std::chrono::duration<double, std::milli>(t_det1 - t_det0).count();

    if (log_results && dets.empty()) {
        if (det.nOutput() == 3)
            std::cout << prefix
                      << "[HINT] 3-head INT8 detect: no boxes. Check RK_plate_detect_meta.txt, "
                         "calibration, or try PLATE_USE_FP=1\n";
        else if (det.inputIsQuantized())
            std::cout << prefix
                      << "[HINT] INT8 detect failed (all-zero head). Reconvert with calibration "
                         "(scripts/convert_rknn_models.py, target rk3568) or use FP: PLATE_USE_FP=1\n";
        else
            std::cout << prefix << "[HINT] detect failed. Try --input-mode uint8_nhwc --conf 0.3 --debug\n";
    }
    if (cfg.debug || (log_results && dets.empty()))
        std::cout << prefix << "[debug] detect input mode: " << det_mode
                  << ", boxes after nms: " << dets.size() << std::endl;

    items.clear();
    auto t_rec0 = std::chrono::steady_clock::now();
    for (const PlateDetectRow& row : dets) {
        float lm[8];
        for (int i = 0; i < 8; i++) lm[i] = row.v[5 + i];
        auto t_roi0 = std::chrono::steady_clock::now();
        cv::Mat roi = makeRecRoiOnnxStyle(img0, lm, cfg.rec_left_pad);
        if ((int)row.v[13] == 1) roi = getSplitMerge(roi);
        if (tm_ptr)
            tm_ptr->rec_roi +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_roi0)
                    .count();

        std::string plate_no, plate_color;
        for (const char* rm : rec_modes) {
            if (getPlateResult(rec, roi, rm, plate_no, plate_color,
                               tm_ptr ? &tm_ptr->rec_npu : nullptr) &&
                !plate_no.empty())
                break;
        }

        cv::Point2f lm_draw[4];
        for (int i = 0; i < 4; i++) lm_draw[i] = cv::Point2f(lm[2 * i], lm[2 * i + 1]);
        if (cfg.left_pad > 1e-6f) expandLandmarksLeftByHeight(lm_draw, cfg.left_pad);

        PlateItem it;
        it.landmarks.resize(8);
        for (int i = 0; i < 4; i++) {
            it.landmarks[(size_t)(2 * i)] = lm_draw[i].x;
            it.landmarks[(size_t)(2 * i + 1)] = lm_draw[i].y;
        }
        it.plate_no = plate_no;
        it.plate_color = plate_color;
        items.push_back(it);
        if (log_results)
            std::cout << prefix << "  plate=" << plate_no << "  color=" << plate_color
                      << "  score=" << row.v[4] << std::endl;
    }

    auto t_rec1 = std::chrono::steady_clock::now();
    double ms_rec = std::chrono::duration<double, std::milli>(t_rec1 - t_rec0).count();
    double ms_draw = 0.;

    if (items.empty()) {
        if (tm_ptr) {
            tm_ptr->detect_total = ms_detect;
            tm_ptr->rec_total = ms_rec;
            tm_ptr->infer_wall = tm_ptr->letterbox + ms_detect + ms_rec;
            tm_ptr->process_wall = tm_ptr->infer_wall + ms_draw;
            if (timings_out) *timings_out = *tm_ptr;
        }
        // When profile is on, Qt prints [latency]; skip per-frame [time]/[FAIL] spam.
        if (log_results && !cfg.profile) {
            double ms_wall =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::cout << prefix << "[time] detect=" << ms_detect << "ms rec=" << ms_rec
                      << "ms infer=" << (ms_detect + ms_rec) << "ms wall=" << ms_wall << "ms\n";
            std::cout << prefix << "[FAIL] no valid plates.";
            if (det.inputIsQuantized())
                std::cout << " INT8 model output invalid — check conversion/calibration.\n";
            else
                std::cout << " Try --input-mode uint8_nhwc --debug\n";
        }
        out_img = img0.clone();
        if (viz_smooth && cfg.viz_temporal_smooth) {
            viz_smooth->configure(cfg.viz_smooth_alpha, cfg.viz_hold_frames, cfg.viz_stable_rect);
            viz_smooth->markMissedFrame();
            std::vector<PlateItem> draw_items;
            if (viz_smooth->appendHeldDrawItems(draw_items)) {
                drawResults(out_img, draw_items, cfg.font, cfg.label_scale, cfg.viz_margin,
                            cfg.viz_stable_rect, cfg.viz_draw_labels, false);
                return 0;
            }
        }
        return 2;
    }

    auto t_draw0 = std::chrono::steady_clock::now();
    out_img = img0.clone();
    std::vector<PlateItem> draw_items = items;
    if (viz_smooth && cfg.viz_temporal_smooth && !draw_items.empty()) {
        viz_smooth->configure(cfg.viz_smooth_alpha, cfg.viz_hold_frames, cfg.viz_stable_rect);
        viz_smooth->apply(draw_items);
    }
    drawResults(out_img, draw_items, cfg.font, cfg.label_scale, cfg.viz_margin,
                cfg.viz_stable_rect, cfg.viz_draw_labels, log_results);
    ms_draw =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_draw0).count();
    if (tm_ptr) tm_ptr->draw = ms_draw;
    double ms_wall =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (tm_ptr) {
        tm_ptr->detect_total = ms_detect;
        tm_ptr->rec_total = ms_rec;
        tm_ptr->infer_wall = tm_ptr->letterbox + ms_detect + ms_rec;
        tm_ptr->process_wall = tm_ptr->infer_wall + ms_draw;
        if (timings_out) *timings_out = *tm_ptr;
    }
    if (log_results && !cfg.profile) {
        std::cout << prefix << "[time] detect=" << ms_detect << "ms rec=" << ms_rec
                  << "ms draw=" << ms_draw << "ms infer=" << (ms_detect + ms_rec)
                  << "ms wall=" << ms_wall << "ms\n";
        std::cout << prefix << "[OK] " << items.size() << " plate(s)\n";
    } else if (log_results && cfg.profile) {
        // Compact one-liner; detailed [latency] comes from Qt every PLATE_PROFILE_EVERY frames.
        std::cout << prefix << "[OK] " << items.size() << " plate(s)"
                  << " process_wall=" << std::fixed << std::setprecision(1)
                  << (tm_ptr ? tm_ptr->process_wall : ms_wall) << "ms\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// PlateRknnPipeline
// ---------------------------------------------------------------------------
struct PlateRknnPipeline::Impl {
    RknnSession det;
    RknnSession rec;
    DetectRknnMeta det_meta;
    std::vector<std::string> rec_mode_storage;
    std::vector<const char*> rec_modes;
    VizStabilizer viz_smooth;
};

PlateRknnPipeline::PlateRknnPipeline() : impl_(new Impl()), ready_(false) {}

PlateRknnPipeline::~PlateRknnPipeline() { delete impl_; }

int PlateRknnPipeline::init(const std::string& detect_rknn, const std::string& rec_rknn,
                            const PlateRknnConfig& cfg) {
    cfg_ = cfg;
    ready_ = false;
    impl_->rec_mode_storage.clear();
    impl_->rec_modes.clear();

    if (impl_->det.load(detect_rknn.c_str(), (uint32_t)cfg_.core_mask) != 0 ||
        impl_->rec.load(rec_rknn.c_str(), (uint32_t)cfg_.core_mask) != 0) {
        return 1;
    }
    impl_->det_meta.valid = false;
    if (impl_->det.nOutput() == 3) {
        const std::string meta_path = findDetectMetaPath(detect_rknn);
        if (!loadDetectMeta(meta_path, impl_->det_meta)) {
            std::cerr << "[ERROR] 3-head detect model needs meta file: " << meta_path << std::endl;
            return 1;
        }
        std::cout << "[detect] 3-head CPU decode enabled, meta=" << meta_path << std::endl;
    } else if (impl_->det.nOutput() == 1) {
        std::cout << "[detect] legacy single-output decode (FP / old ONNX)\n";
    } else {
        std::cerr << "[WARN] unexpected detect outputs: " << impl_->det.nOutput() << std::endl;
    }
    if (impl_->det.inputIsQuantized()) cfg_.input_mode = "uint8_nhwc";

    if (cfg_.rec_input_mode != "auto") {
        impl_->rec_mode_storage.push_back(cfg_.rec_input_mode);
    } else {
        impl_->rec_mode_storage.push_back("uint8_nhwc");
        if (!impl_->rec.inputIsQuantized())
            impl_->rec_mode_storage.push_back("float_nchw_rgb");
    }
    for (const auto& s : impl_->rec_mode_storage) impl_->rec_modes.push_back(s.c_str());

    impl_->det.logIoAttrs("detect");
    impl_->rec.logIoAttrs("rec");
    ready_ = true;
    return 0;
}

int PlateRknnPipeline::process(const cv::Mat& img_bgr, cv::Mat& out_bgr, std::vector<PlateItem>& items,
                              const std::string& log_tag) {
    if (!ready_) return 1;
    items.clear();
    last_timings_ = PlateTimings{};
    VizStabilizer* vz = cfg_.viz_temporal_smooth ? &impl_->viz_smooth : nullptr;
    return processFrame(impl_->det, impl_->rec, img_bgr, out_bgr, items, cfg_, impl_->rec_modes,
                        impl_->det.nOutput() == 3 ? &impl_->det_meta : nullptr, log_tag,
                        &last_timings_, vz);
}

void PlateRknnPipeline::setLabelScale(float scale) {
    if (scale < 0.15f) scale = 0.15f;
    if (scale > 1.0f) scale = 1.0f;
    cfg_.label_scale = scale;
}

void PlateRknnPipeline::resetVizSmoothState() { impl_->viz_smooth.reset(); }

void PlateRknnPipeline::setVizSmoothAlpha(float alpha) {
    cfg_.viz_smooth_alpha = std::max(0.05f, std::min(1.f, alpha));
}

void PlateRknnPipeline::setVizHoldFrames(int frames) {
    cfg_.viz_hold_frames = std::max(0, frames);
}

bool PlateRknnPipeline::drawVizOverlay(cv::Mat& img_bgr) {
    if (img_bgr.empty() || !cfg_.viz_temporal_smooth) return false;
    impl_->viz_smooth.configure(cfg_.viz_smooth_alpha, cfg_.viz_hold_frames, cfg_.viz_stable_rect);
    std::vector<PlateItem> draw_items;
    if (!impl_->viz_smooth.appendVisibleDrawItems(draw_items)) return false;
    drawResults(img_bgr, draw_items, cfg_.font, cfg_.label_scale, cfg_.viz_margin,
                cfg_.viz_stable_rect, cfg_.viz_draw_labels, false);
    return true;
}

} // namespace plate_rknn
