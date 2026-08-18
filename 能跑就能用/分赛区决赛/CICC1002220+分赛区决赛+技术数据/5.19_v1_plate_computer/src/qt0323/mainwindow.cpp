#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QFrame>
#include <QFile>
#include <QFont>
#include <QStyle>
#include <QPainter>
#include <QFontMetrics>
#include <QPen>
#include <QColor>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace {

constexpr int kPcieWidth = PCIE_FRAME_WIDTH;
constexpr int kPcieHeight = PCIE_FRAME_HEIGHT;
constexpr int kPcieRowBytes = PCIE_ROW_BYTES;

static std::string findFirstExisting(const char* candidates[], size_t n) {
    for (size_t i = 0; i < n; i++) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return "";
}

static bool envFlagEnabled(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}

static bool envDefaultTrue(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return true;
    return v[0] == '1';
}

static float envFloatOr(const char* name, float def) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return def;
    return static_cast<float>(std::atof(v));
}

static int envIntOr(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return def;
    return std::atoi(v);
}

static void logProcessMemoryIfEnabled(int frame_index) {
    if (!envFlagEnabled("PLATE_MEM_LOG")) return;
    const int every = std::max(1, envIntOr("PLATE_MEM_EVERY", 60));
    if (frame_index > 0 && (frame_index % every) != 0) return;

    long vmrss_kb = -1;
    long vmswap_kb = 0;
    long mem_avail_kb = -1;

    FILE* fp = std::fopen("/proc/self/status", "r");
    if (fp) {
        char line[256];
        while (std::fgets(line, sizeof(line), fp)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0) std::sscanf(line + 6, "%ld", &vmrss_kb);
            else if (std::strncmp(line, "VmSwap:", 7) == 0) std::sscanf(line + 7, "%ld", &vmswap_kb);
        }
        std::fclose(fp);
    }

    fp = std::fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        while (std::fgets(line, sizeof(line), fp)) {
            if (std::strncmp(line, "MemAvailable:", 13) == 0) {
                std::sscanf(line + 13, "%ld", &mem_avail_kb);
                break;
            }
        }
        std::fclose(fp);
    }

    std::cout << "[mem] frame=" << frame_index << " VmRSS=" << (vmrss_kb / 1024.0) << " MiB"
              << " VmSwap=" << (vmswap_kb / 1024.0) << " MiB"
              << " MemAvailable=" << (mem_avail_kb / 1024.0) << " MiB\n";
}

static bool resolveModelPaths(std::string& detect_path, std::string& rec_path, bool& use_int8) {
    const char* detect_int8[] = {
        "./weights/plate_detect_int8.rknn",
        "./weights/plate_detect_i8.rknn",
        "./test_rknn_infer/plate_detect_int8.rknn",
        "./plate_detect_int8.rknn",
    };
    const char* rec_int8[] = {
        "./weights/plate_rec_color_int8.rknn",
        "./weights/plate_rec_i8.rknn",
        "./test_rknn_infer/plate_rec_color_int8.rknn",
        "./plate_rec_color_int8.rknn",
    };
    const char* detect_fp[] = {
        "./weights/plate_detect_fp.rknn",
        "./test_rknn_infer/plate_detect_fp.rknn",
        "./plate_detect_fp.rknn",
    };
    const char* rec_fp[] = {
        "./weights/plate_rec_fp.rknn",
        "./test_rknn_infer/plate_rec_fp.rknn",
        "./plate_rec_fp.rknn",
    };

    use_int8 = false;
    if (!envFlagEnabled("PLATE_USE_FP")) {
        detect_path = findFirstExisting(detect_int8, 4);
        rec_path = findFirstExisting(rec_int8, 4);
        if (!detect_path.empty() && !rec_path.empty()) {
            use_int8 = true;
            return true;
        }
    }

    detect_path = findFirstExisting(detect_fp, 3);
    rec_path = findFirstExisting(rec_fp, 3);
    return !detect_path.empty() && !rec_path.empty();
}

QStringList imageNameFilters() {
    return {"*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp",
            "*.JPG", "*.JPEG", "*.PNG", "*.BMP", "*.WEBP"};
}

bool isImageFilePath(const QString& path) {
    static const QStringList exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"};
    return exts.contains("." + QFileInfo(path).suffix().toLower());
}

bool isVideoFilePath(const QString& path) {
    static const QStringList exts = {".mp4", ".avi", ".mkv", ".mov", ".mpeg", ".mpg", ".wmv", ".flv"};
    return exts.contains("." + QFileInfo(path).suffix().toLower());
}

void rgb565_to_rgb888_local(const uint16_t* image565, uint8_t* image888, size_t num_pixels) {
    // Restored from QT?????7.6 (stable imaging path).
    for (size_t i = 0; i < num_pixels; ++i) {
        uint16_t pixel = image565[i];
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        image888[3 * i] = r;
        image888[3 * i + 1] = g;
        image888[3 * i + 2] = b;
    }
}

} // namespace

namespace {
std::atomic<float> g_label_scale{0.52f};
} // namespace

InferenceThread::InferenceThread(QObject* parent)
    : QThread(parent),
      keep_running(false),
      stop_requested_(false),
      current_input_type(INPUT_MEDIA),
      pipeline_(new plate_rknn::PlateRknnPipeline()),
      pcie_fd_(-1),
      use_fpga_rgb888_(false),
      infer_map_ready_(false),
      profile_enabled_(false),
      profile_every_(1),
      profile_frame_cnt_(0),
      fpga_roi_preview_(false),
      roi_margin_(32),
      roi_log_fp_(nullptr),
      profile_rgb_ms_(0.0) {}

void InferenceThread::setLabelScale(float scale) {
    if (scale < 0.20f) scale = 0.20f;
    if (scale > 0.80f) scale = 0.80f;
    g_label_scale.store(scale, std::memory_order_relaxed);
    if (pipeline_->ready()) pipeline_->setLabelScale(scale);
}

InferenceThread::~InferenceThread() {
    stop();
    wait();
    if (roi_log_fp_) {
        std::fclose(roi_log_fp_);
        roi_log_fp_ = nullptr;
    }
}

bool InferenceThread::initPipeline() {
    const char* font_candidates[] = {
        "./fonts/platech.ttf",
        "./test_rknn_infer/fonts/platech.ttf",
    };

    std::string detect_path;
    std::string rec_path;
    bool use_int8 = false;
    if (!resolveModelPaths(detect_path, rec_path, use_int8)) {
        emit showMessage(QString::fromUtf8(
            "\u672a\u627e\u5230 RKNN \u6a21\u578b\uff0c\u8bf7\u653e\u5230 weights/\n"
            "INT8: plate_detect_int8.rknn + plate_rec_color_int8.rknn\n"
            "FP: plate_detect_fp.rknn + plate_rec_fp.rknn"));
        return false;
    }

    plate_rknn::PlateRknnConfig cfg;
    cfg.input_mode = "uint8_nhwc";
    cfg.rec_input_mode = "auto";
    cfg.conf = 0.3f;
    cfg.label_scale = g_label_scale.load(std::memory_order_relaxed);
    cfg.font = findFirstExisting(font_candidates, 2);
    cfg.profile = envDefaultTrue("PLATE_PROFILE") || envFlagEnabled("PCIE_PROFILE");
    // Throttle plate=/[OK] spam so [latency] remains readable (default every 60 frames).
    cfg.result_log_every = std::max(0, envIntOr("PLATE_RESULT_EVERY", 60));
    // Match QT?????7.6 defaults for stable PCIe imaging
    cfg.viz_temporal_smooth =
        (current_input_type == INPUT_PCIE) && envDefaultTrue("PLATE_VIZ_SMOOTH");
    cfg.viz_smooth_alpha = envFloatOr("PLATE_VIZ_SMOOTH_ALPHA", 0.7f);
    cfg.viz_hold_frames = envIntOr("PLATE_VIZ_HOLD", 0);
    cfg.viz_stable_rect = envDefaultTrue("PLATE_VIZ_STABLE_RECT");
    cfg.viz_draw_labels = envDefaultTrue("PLATE_DRAW_LABELS");

    std::cout << "[model] detect=" << detect_path << " rec=" << rec_path
              << " quant=" << (use_int8 ? "int8" : "fp")
              << " input_mode=" << cfg.input_mode
              << " profile=" << (cfg.profile ? "1" : "0")
              << " result_log_every=" << cfg.result_log_every
              << " viz_smooth=" << (cfg.viz_temporal_smooth ? "1" : "0")
              << " viz_alpha=" << cfg.viz_smooth_alpha
              << " viz_hold=" << cfg.viz_hold_frames
              << " draw_labels=" << (cfg.viz_draw_labels ? "1" : "0") << std::endl;

    plate_rknn::printCnRenderStatus(cfg.font);
    if (pipeline_->init(detect_path, rec_path, cfg) != 0) {
        emit showMessage(QString::fromUtf8("RKNN \u6a21\u578b\u52a0\u8f7d\u5931\u8d25"));
        return false;
    }
    pipeline_->resetVizSmoothState();

    profile_enabled_ = cfg.profile;
    // Default every 60 frames (~4s @15FPS): keep [latency] visible but not spammy.
    profile_every_ = std::max(1, envIntOr("PLATE_PROFILE_EVERY", 60));
    profile_frame_cnt_ = 0;
    if (profile_enabled_) {
        std::cout << "[profile] latency report ON every " << profile_every_
                  << " frames (PLATE_PROFILE=0 off, PLATE_PROFILE_EVERY=N)\n";
    }
    emit showMessage(QString::fromUtf8(use_int8 ? "INT8 \u6a21\u578b\u5df2\u52a0\u8f7d" : "FP \u6a21\u578b\u5df2\u52a0\u8f7d"));
    return true;
}

void InferenceThread::setInputPath(const QString& path) {
    file_path = path;
    current_input_type = INPUT_MEDIA;
    resetPlateResultCache();
}
void InferenceThread::setInputPCIe() {
    current_input_type = INPUT_PCIE;
    resetPlateResultCache();
}
void InferenceThread::stop() {
    keep_running.store(false, std::memory_order_relaxed);
    stop_requested_.store(true, std::memory_order_relaxed);
}

void InferenceThread::resetPlateResultCache() { last_plate_numbers_.clear(); }

void InferenceThread::emitPlateResultsIfChanged(const QStringList& plate_numbers) {
    if (plate_numbers == last_plate_numbers_) return;
    last_plate_numbers_ = plate_numbers;
    emit plateResultsUpdated(plate_numbers);
}

void InferenceThread::ensureOutputDirectory() {
    QDir dir("output");
    if (!dir.exists()) dir.mkpath(".");
}

QImage InferenceThread::matToQImage(const cv::Mat& mat) {
    return QImage((const unsigned char*)(mat.data), mat.cols, mat.rows, mat.step, QImage::Format_BGR888)
        .copy();
}

QImage InferenceThread::rgb565ToQImage(const uint8_t* data565, int width, int height) {
    return QImage(data565, width, height, width * 2, QImage::Format_RGB16).copy();
}

cv::Mat InferenceThread::rgb565BufferToBgr(const uint8_t* src565, int width, int height,
                                           int src_stride_bytes) {
    const size_t num_pixels = static_cast<size_t>(width) * height;
    const int compact_stride = width * 2;
    std::vector<uint8_t> compact_565;
    const uint8_t* data_for_convert = src565;

    if (src_stride_bytes > 0 && src_stride_bytes != compact_stride) {
        compact_565.resize(num_pixels * 2);
        for (int row = 0; row < height; ++row) {
            std::memcpy(compact_565.data() + static_cast<size_t>(row) * compact_stride,
                        src565 + static_cast<size_t>(row) * src_stride_bytes, compact_stride);
        }
        data_for_convert = compact_565.data();
    }

    std::vector<uint8_t> image_buf_888(num_pixels * 3);
    rgb565_to_rgb888_local(reinterpret_cast<const uint16_t*>(data_for_convert),
                           image_buf_888.data(), num_pixels);
    cv::Mat img(height, width, CV_8UC3, image_buf_888.data());
    cv::Mat frame;
    cv::cvtColor(img, frame, cv::COLOR_RGB2BGR);
    return frame;
}

void InferenceThread::setupPcieInferPath(int fd) {
    pcie_fd_ = fd;
    use_fpga_rgb888_ = envFlagEnabled("PCIE_FPGA_RGB888");
    infer_map_ready_ = false;

    if (!use_fpga_rgb888_) {
        return;
    }

    infer_buf_888_.resize(PCIE_RGB888_FRAME_BYTES);
    FRAME_CAPTURE infer_cap{};
    infer_cap.width = static_cast<unsigned int>(kPcieWidth);
    infer_cap.height = static_cast<unsigned int>(kPcieHeight);
    infer_cap.row_bytes = PCIE_RGB888_ROW_BYTES;
    infer_cap.user_buf = 0;

    if (ioctl(fd, PCI_MAP_INFER_FRAME_CMD, &infer_cap) < 0) {
        std::cerr << "[PCIe] PCI_MAP_INFER_FRAME_CMD failed, fallback CPU rgb565->bgr\n";
        use_fpga_rgb888_ = false;
        return;
    }

    infer_map_ready_ = true;
    std::cout << "[PCIe] infer RGB888 path mapped (PCIE_FPGA_RGB888=1)\n";
}

cv::Mat InferenceThread::captureInferBgrFrame(int fd, const uint8_t* src565, int src_stride_bytes) {
    const int64 t0 = cv::getTickCount();
    const double tick_hz = static_cast<double>(cv::getTickFrequency());
    profile_rgb_ms_ = 0.0;
    const char* force_cpu = std::getenv("PCIE_FPGA_RGB888_FORCE_CPU");

    if (use_fpga_rgb888_ && infer_map_ready_ && fd >= 0 &&
        !(force_cpu && force_cpu[0] == '1') && !infer_buf_888_.empty()) {
        FRAME_CAPTURE infer_cap{};
        infer_cap.width = static_cast<unsigned int>(kPcieWidth);
        infer_cap.height = static_cast<unsigned int>(kPcieHeight);
        infer_cap.row_bytes = PCIE_RGB888_ROW_BYTES;
        infer_cap.user_buf = reinterpret_cast<unsigned long>(infer_buf_888_.data());

        if (ioctl(fd, PCI_READ_INFER_FRAME_CMD, &infer_cap) == 0) {
            cv::Mat rgb(kPcieHeight, kPcieWidth, CV_8UC3, infer_buf_888_.data());
            cv::Mat bgr;
            cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
            profile_rgb_ms_ = (cv::getTickCount() - t0) * 1000.0 / tick_hz;
            return bgr;
        }
        std::cerr << "[PCIe] PCI_READ_INFER_FRAME_CMD failed, fallback CPU\n";
    }

    cv::Mat bgr = rgb565BufferToBgr(src565, kPcieWidth, kPcieHeight, src_stride_bytes);
    profile_rgb_ms_ = (cv::getTickCount() - t0) * 1000.0 / tick_hz;
    return bgr;
}

void InferenceThread::setupPcieRoiPreview() {
    fpga_roi_preview_ = envFlagEnabled("PCIE_FPGA_ROI_PREVIEW");

    const char* margin_env = std::getenv("PCIE_ROI_MARGIN");
    roi_margin_ = (margin_env && margin_env[0]) ? std::max(0, std::atoi(margin_env)) : 32;

    if (!fpga_roi_preview_) {
        return;
    }

    const char* log_path = std::getenv("PCIE_ROI_LOG");
    if (log_path && log_path[0]) {
        roi_log_fp_ = std::fopen(log_path, "w");
        if (roi_log_fp_) {
            std::fprintf(roi_log_fp_, "frame,x,y,w,h,valid,frame_id\n");
            std::fflush(roi_log_fp_);
        }
    }

    std::cout << "[PCIe] FPGA ROI preview (PCIE_FPGA_ROI_PREVIEW=1, margin=" << roi_margin_
              << ")\n";
}

bool InferenceThread::readPlateRoi(int fd, PLATE_ROI_INFO& roi) const {
    if (fd < 0) return false;
    std::memset(&roi, 0, sizeof(roi));
    return ioctl(fd, PCI_GET_PLATE_ROI_CMD, &roi) == 0;
}

void InferenceThread::drawRoiOverlay(QImage& image, const PLATE_ROI_INFO& roi) const {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor box_color = roi.valid ? QColor(0, 255, 0) : QColor(255, 64, 64);
    QPen pen(box_color, 3);
    if (!roi.valid) pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (roi.valid && roi.w > 0 && roi.h > 0) {
        painter.drawRect(static_cast<int>(roi.x), static_cast<int>(roi.y),
                         static_cast<int>(roi.w), static_cast<int>(roi.h));
        if (roi_margin_ > 0) {
            const int mx = std::max(0, static_cast<int>(roi.x) - roi_margin_);
            const int my = std::max(0, static_cast<int>(roi.y) - roi_margin_);
            const int mw = std::min(image.width() - mx, static_cast<int>(roi.w) + roi_margin_ * 2);
            const int mh = std::min(image.height() - my, static_cast<int>(roi.h) + roi_margin_ * 2);
            painter.setPen(QPen(QColor(255, 255, 0), 2, Qt::DashLine));
            painter.drawRect(mx, my, mw, mh);
        }
    } else {
        painter.drawRect(20, 20, image.width() - 40, image.height() - 40);
    }

    QFont font("Microsoft YaHei", 14, QFont::Bold);
    painter.setFont(font);
    painter.setPen(box_color);
    const QString label = roi.valid
        ? QString("ROI x=%1 y=%2 w=%3 h=%4 fid=%5")
              .arg(roi.x).arg(roi.y).arg(roi.w).arg(roi.h).arg(roi.frame_id)
        : QString("ROI invalid fid=%1").arg(roi.frame_id);
    painter.drawText(24, 48, label);
    painter.end();
}

void InferenceThread::emitPreviewWithRoiOverlay(const uint8_t* src565, int width, int height,
                                                  int src_stride_bytes, const PLATE_ROI_INFO& roi) {
    const int compact_stride = width * 2;
    QImage base;
    if (src_stride_bytes > 0 && src_stride_bytes != compact_stride) {
        base = QImage(src565, width, height, src_stride_bytes, QImage::Format_RGB16).copy();
    } else {
        base = rgb565ToQImage(src565, width, height);
    }
    drawRoiOverlay(base, roi);
    emit frameReady(base);
}

void InferenceThread::processCapturedFrame(const uint8_t* src565, int frame_cnt,
                                             int src_stride_bytes, int64 capture_e2e_t0,
                                             double pcie_dma_ms) {
    // Restored from QT?????7.6 (stable imaging path).
    if (fpga_roi_preview_) {
        PLATE_ROI_INFO roi{};
        readPlateRoi(pcie_fd_, roi);

        if (roi_log_fp_) {
            std::fprintf(roi_log_fp_, "%d,%u,%u,%u,%u,%u,%u\n", frame_cnt, roi.x, roi.y, roi.w, roi.h,
                         static_cast<unsigned>(roi.valid), static_cast<unsigned>(roi.frame_id));
            if (frame_cnt % 30 == 0) std::fflush(roi_log_fp_);
        }
        if (frame_cnt % 30 == 0) {
            std::cout << "\n[PCIe][ROI] frame=" << frame_cnt << " x=" << roi.x << " y=" << roi.y
                      << " w=" << roi.w << " h=" << roi.h
                      << " valid=" << static_cast<int>(roi.valid)
                      << " fid=" << static_cast<int>(roi.frame_id) << "\n";
        }
        emitPreviewWithRoiOverlay(src565, kPcieWidth, kPcieHeight, src_stride_bytes, roi);
        return;
    }

    const int64 e2e_t0 = capture_e2e_t0 > 0 ? capture_e2e_t0 : cv::getTickCount();
    cv::Mat frame = captureInferBgrFrame(pcie_fd_, src565, src_stride_bytes);
    if (frame.empty()) return;

    if (frame_cnt == 1) {
        emit showMessage(QString::fromUtf8("PCIe \u5df2\u8fde\u63a5\uff0c\u6b63\u5728\u663e\u793a\u89c6\u9891"));
    }
    processAndEmit(frame, QString(), QString::fromUtf8("PCIe"), e2e_t0, pcie_dma_ms, 0.0,
                   frame_cnt);
}

void InferenceThread::printFrameProfileIfEnabled(const QString& tag, int frame_index,
                                                 const plate_rknn::PlateTimings& pipe,
                                                 double pcie_dma_ms, double rgb_ms,
                                                 double read_ms, double qimage_ms, double e2e_ms) {
    if (!profile_enabled_) return;
    profile_frame_cnt_++;
    if (profile_every_ > 1 && (profile_frame_cnt_ % profile_every_) != 0) return;

    plate_rknn::HostFrameTimings host{};
    host.pcie_dma = pcie_dma_ms;
    host.rgb_convert = rgb_ms;
    host.read_imread = read_ms;
    host.qimage = qimage_ms;
    host.e2e_total = e2e_ms;

    const std::string prefix = tag.isEmpty() ? "" : (tag.toStdString() + " ");
    plate_rknn::printFrameLatencyReport(prefix, pipe, host, frame_index);
}

bool InferenceThread::processAndEmit(cv::Mat& frame, const QString& status_hint,
                                     const QString& image_name, int64 e2e_t0, double pcie_dma_ms,
                                     double read_ms, int frame_index, bool sync_present) {
    pipeline_->setLabelScale(g_label_scale.load(std::memory_order_relaxed));
    cv::Mat out;
    std::vector<plate_rknn::PlateItem> items;
    int code = pipeline_->process(frame, out, items);
    if (!out.empty()) frame = out;

    const bool stream_mode =
        image_name == QString::fromUtf8("PCIe") || status_hint == QString::fromUtf8("PCIe");

    if (code == 0 && !items.empty()) {
        QStringList plate_numbers;
        for (size_t i = 0; i < items.size(); i++) {
            plate_numbers.append(QString::fromStdString(items[i].plate_no));
        }
        emitPlateResultsIfChanged(plate_numbers);

        if (!stream_mode) {
            QString msg = QString::fromUtf8("\u8bc6\u522b: ");
            for (size_t i = 0; i < items.size(); i++) {
                if (i) msg += "; ";
                msg += QString::fromStdString(items[i].plate_no) + " " +
                       QString::fromStdString(items[i].plate_color);
            }
            emit showMessage(msg);
        }
    } else if (code == 2) {
        emitPlateResultsIfChanged(QStringList());
        if (!status_hint.isEmpty()) emit showMessage(status_hint);
    } else if (code == 1) {
        emit showMessage(QString::fromUtf8("\u63a8\u7406\u9519\u8bef"));
        return false;
    }

    const int64 t_qimg0 = cv::getTickCount();
    QImage qimg = matToQImage(frame);
    const double tick_hz = static_cast<double>(cv::getTickFrequency());
    const double qimage_ms = (cv::getTickCount() - t_qimg0) * 1000.0 / tick_hz;

    double e2e_ms = pipeline_->lastTimings().process_wall;
    if (e2e_t0 > 0) {
        e2e_ms = (cv::getTickCount() - e2e_t0) * 1000.0 / tick_hz;
    }

    const plate_rknn::PlateTimings& timings = pipeline_->lastTimings();
    printFrameProfileIfEnabled(image_name.isEmpty() ? status_hint : image_name, frame_index,
                               timings, pcie_dma_ms, profile_rgb_ms_, read_ms, qimage_ms, e2e_ms);

    // Match 7.6: always emit metrics + frame (UI coalesces via updateFrame/flushPendingFrame).
    if (stream_mode) logProcessMemoryIfEnabled(frame_index);
    emit imageProcessTime(image_name.isEmpty() ? status_hint : image_name, timings.process_wall,
                          e2e_ms);
    if (sync_present && parent()) {
        // Folder batch: paint on UI thread and wait (avoids coalesce race / ghost frames).
        QMetaObject::invokeMethod(parent(), "presentFrameNow", Qt::BlockingQueuedConnection,
                                  Q_ARG(QImage, qimg));
    } else {
        emit frameReady(qimg);
    }
    return true;
}

void InferenceThread::run() {
    stop_requested_.store(false, std::memory_order_relaxed);
    if (!initPipeline()) return;
    if (stop_requested_.load(std::memory_order_relaxed)) return;

    keep_running.store(true, std::memory_order_relaxed);
    ensureOutputDirectory();

    if (current_input_type == INPUT_MEDIA)
        processMediaInput();
    else if (current_input_type == INPUT_PCIE)
        processPCIeStream();

    emit showMessage(QString::fromUtf8("\u5c31\u7eea"));
}

void InferenceThread::processFrameBatch(const QFileInfoList& files, const QString& out_root,
                                        const QString& source_label) {
    const int total = files.size();
    QDir().mkpath(out_root);

    emit batchProgressChanged(0, total);
    emit showMessage(QString::fromUtf8("\u5f00\u59cb\u5904\u7406 %1\uff0c\u5171 %2 \u5f20")
                         .arg(source_label)
                         .arg(total));

    QStringList saved_paths;
    int ok_cnt = 0;
    for (int i = 0; i < total && keep_running.load(std::memory_order_relaxed); i++) {
        const QFileInfo& fi = files.at(i);
        const QString path = fi.absoluteFilePath();

        emit showMessage(QString::fromUtf8("\u5904\u7406 %1/%2: %3")
                             .arg(i + 1)
                             .arg(total)
                             .arg(fi.fileName()));

        const int64 t_read0 = cv::getTickCount();
        cv::Mat frame = cv::imread(path.toStdString());
        const double read_ms =
            (cv::getTickCount() - t_read0) * 1000.0 / cv::getTickFrequency();
        if (frame.empty()) {
            emit showMessage(QString::fromUtf8("\u8bfb\u53d6\u5931\u8d25: ") + fi.fileName());
            continue;
        }

        cv::Mat work = frame.clone();
        pipeline_->resetVizSmoothState();
        const bool ok = processAndEmit(work, QString::fromUtf8("\u672a\u68c0\u6d4b\u5230\u8f66\u724c"),
                                       fi.fileName(), 0, 0.0, read_ms, i + 1, true);
        const QString save_path = out_root + "/" + fi.fileName();
        cv::imwrite(save_path.toStdString(), work);
        saved_paths.append(save_path);

        if (ok) ok_cnt++;
        emit batchProgressChanged(i + 1, total);
        msleep(20);
    }

    emit batchProgressEnd();

    if (keep_running.load(std::memory_order_relaxed)) {
        emit showMessage(QString::fromUtf8("\u5904\u7406\u5b8c\u6210: %1/%2 \u5f20\u5df2\u4fdd\u5b58\u81f3 %3")
                             .arg(ok_cnt)
                             .arg(total)
                             .arg(out_root));
        if (!saved_paths.isEmpty()) emit batchGalleryReady(saved_paths);
    }
}

void InferenceThread::processVideoFile() {
    cv::VideoCapture cap(file_path.toStdString());
    if (!cap.isOpened()) {
        emit showMessage(QString::fromUtf8("\u65e0\u6cd5\u6253\u5f00\u89c6\u9891\u6587\u4ef6"));
        emit batchProgressEnd();
        return;
    }

    int total = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    if (total < 0) total = 0;

    double fps = cap.get(cv::CAP_PROP_FPS);
    if (fps < 1.0 || fps > 120.0) fps = 25.0;
    const double frame_period_ms = 1000.0 / fps;

    // Infer every (skip+1) frames. Default 0 => every frame.
    const int skip_frames = std::max(0, envIntOr("VIDEO_SKIP_FRAMES", 0));
    const int stride = skip_frames + 1;
    const bool save_frames = envFlagEnabled("VIDEO_SAVE_FRAMES");
    const int max_side = std::max(0, envIntOr("VIDEO_MAX_SIDE", 1280));
    const bool realtime = envDefaultTrue("VIDEO_REALTIME");

    const QString out_root =
        QString("output/video_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    if (save_frames) QDir().mkpath(out_root);

    const QString video_name = QFileInfo(file_path).fileName();
    emit batchProgressChanged(0, total > 0 ? total : 1);
    emit showMessage(QString::fromUtf8("\u64ad\u653e\u89c6\u9891 %1  (%2 fps)")
                         .arg(video_name)
                         .arg(fps, 0, 'f', 1));
    std::cout << "[video] realtime playback fps=" << fps << " skip=" << skip_frames
              << " box=detect_only period_ms=" << frame_period_ms
              << " save=" << (save_frames ? 1 : 0) << "\n";

    QStringList saved_paths;
    int ok_cnt = 0;
    int frame_idx = 0;
    int infer_cnt = 0;

    // No hold / EMA: draw box only on frames that actually detect a plate.
    pipeline_->setTemporalVizSmooth(false);
    pipeline_->setVizHoldFrames(0);
    pipeline_->resetVizSmoothState();

    const int64 tick0 = cv::getTickCount();
    const double tick_hz = cv::getTickFrequency();

    cv::Mat frame;
    while (keep_running.load(std::memory_order_relaxed) && cap.read(frame)) {
        if (frame.empty()) continue;
        frame_idx++;

        if (max_side > 0) {
            const int side = std::max(frame.cols, frame.rows);
            if (side > max_side) {
                const double scale = static_cast<double>(max_side) / side;
                cv::resize(frame, frame, cv::Size(), scale, scale, cv::INTER_AREA);
            }
        }

        const bool do_infer = ((frame_idx - 1) % stride) == 0;
        cv::Mat display = frame;  // default: raw frame, no box

        if (do_infer) {
            infer_cnt++;
            const QString frame_name =
                QString("frame_%1.jpg").arg(frame_idx, 6, 10, QChar('0'));
            if (infer_cnt == 1 || infer_cnt % 10 == 0) {
                emit showMessage(QString::fromUtf8("\u64ad\u653e %1/%2  \u63a8\u7406#%3")
                                     .arg(frame_idx)
                                     .arg(total > 0 ? total : frame_idx)
                                     .arg(infer_cnt));
            }

            pipeline_->setLabelScale(g_label_scale.load(std::memory_order_relaxed));
            cv::Mat out;
            std::vector<plate_rknn::PlateItem> items;
            const int64 e2e_t0 = cv::getTickCount();
            const int code = pipeline_->process(frame, out, items);

            // Draw only when this frame detected plates; miss => keep raw frame.
            if (code == 0 && !items.empty() && !out.empty()) {
                display = out;
                ok_cnt++;
                QStringList plate_numbers;
                for (const auto& it : items)
                    plate_numbers.append(QString::fromStdString(it.plate_no));
                emitPlateResultsIfChanged(plate_numbers);
            } else {
                emitPlateResultsIfChanged(QStringList());
            }

            const double e2e_ms = (cv::getTickCount() - e2e_t0) * 1000.0 / tick_hz;
            emit imageProcessTime(frame_name, pipeline_->lastTimings().process_wall, e2e_ms);
            printFrameProfileIfEnabled(frame_name, frame_idx, pipeline_->lastTimings(), 0.0, 0.0,
                                       0.0, 0.0, e2e_ms);

            if (save_frames) {
                const QString save_path = out_root + "/" + frame_name;
                cv::imwrite(save_path.toStdString(), display);
                saved_paths.append(save_path);
            }
        }
        // Skip frames: play video only, never reuse previous boxes.

        emit frameReady(matToQImage(display));

        if (frame_idx % 30 == 0 || (total > 0 && frame_idx == total)) {
            emit batchProgressChanged(frame_idx, total > 0 ? total : frame_idx);
        }

        if (!realtime) continue;

        const double target_ms = frame_idx * frame_period_ms;
        const double elapsed_ms = (cv::getTickCount() - tick0) * 1000.0 / tick_hz;
        const double lag_ms = elapsed_ms - target_ms;
        if (lag_ms < -2.0) {
            msleep(std::min(static_cast<int>(-lag_ms), 80));
        } else if (lag_ms > frame_period_ms * 2.0) {
            int drop = static_cast<int>(lag_ms / frame_period_ms) - 1;
            drop = std::min(std::max(drop, 1), 30);
            for (int d = 0; d < drop && keep_running.load(std::memory_order_relaxed); ++d) {
                if (!cap.grab()) break;
                frame_idx++;
            }
        }
    }

    pipeline_->setTemporalVizSmooth(false);
    pipeline_->resetVizSmoothState();

    emit batchProgressEnd();

    if (frame_idx == 0) {
        emit showMessage(QString::fromUtf8("\u89c6\u9891\u65e0\u6709\u6548\u5e27"));
        return;
    }

    if (keep_running.load(std::memory_order_relaxed)) {
        QString msg = QString::fromUtf8(
                          "\u89c6\u9891\u5b8c\u6210: \u64ad\u653e %1 \u5e27, \u63a8\u7406 %2 \u5e27, \u547d\u4e2d %3")
                          .arg(frame_idx)
                          .arg(infer_cnt)
                          .arg(ok_cnt);
        if (save_frames) msg += QString::fromUtf8(", \u5df2\u4fdd\u5b58 ") + out_root;
        emit showMessage(msg);
        if (!saved_paths.isEmpty()) emit batchGalleryReady(saved_paths);
    }
}

void InferenceThread::processMediaInput() {
    const QFileInfo fi(file_path);
    if (!fi.exists()) {
        emit showMessage(QString::fromUtf8("\u8def\u5f84\u4e0d\u5b58\u5728"));
        emit batchProgressEnd();
        return;
    }

    if (fi.isDir()) {
        QDir dir(file_path);
        const QFileInfoList files =
            dir.entryInfoList(imageNameFilters(), QDir::Files, QDir::Name | QDir::IgnoreCase);
        if (files.isEmpty()) {
            emit showMessage(QString::fromUtf8("\u6587\u4ef6\u5939\u5185\u6ca1\u6709\u56fe\u7247"));
            emit batchProgressEnd();
            return;
        }

        const QString out_root =
            QString("output/batch_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        processFrameBatch(files, out_root, QString::fromUtf8("\u6587\u4ef6\u5939"));
        return;
    }

    if (isImageFilePath(file_path)) {
        const QString out_root =
            QString("output/image_%1").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        processFrameBatch({fi}, out_root, QString::fromUtf8("\u56fe\u7247"));
        return;
    }

    if (isVideoFilePath(file_path)) {
        processVideoFile();
        return;
    }

    emit showMessage(QString::fromUtf8("\u4e0d\u652f\u6301\u7684\u6587\u4ef6\u7c7b\u578b\uff0c\u8bf7\u9009\u62e9\u56fe\u7247\u3001\u89c6\u9891\u6216\u56fe\u7247\u6587\u4ef6\u5939"));
    emit batchProgressEnd();
}

void InferenceThread::processPCIeStream() {
    emit showMessage(QString::fromUtf8("\u6b63\u5728\u6253\u5f00 PCIe..."));
    std::cout << "\n[PCIe] opening " << PCIE_DRIVER_FILE_PATH << std::endl;

    int fd = open(PCIE_DRIVER_FILE_PATH, O_RDWR);
    if (fd < 0) {
        std::cerr << "[PCIe] open failed, errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        emit showMessage(QString::fromUtf8("\u65e0\u6cd5\u6253\u5f00 PCIe \u8bbe\u5907"));
        return;
    }

    if (!checkPcieLink(fd)) {
        emit showMessage(QString::fromUtf8("PCIe \u94fe\u8def\u5f02\u5e38"));
        ::close(fd);
        return;
    }

    setupPcieRoiPreview();
    if (!fpga_roi_preview_) {
        setupPcieInferPath(fd);
    } else {
        pcie_fd_ = fd;
    }

    emit showMessage(fpga_roi_preview_
        ? QString::fromUtf8("PCIe HDMI 1280x720 FPGA ROI \u9884\u89c8")
        : QString::fromUtf8("PCIe HDMI 1280x720 \u8bc6\u522b"));

    const size_t frame_bytes = static_cast<size_t>(kPcieWidth) * kPcieHeight * 2;
    std::vector<uint8_t> image_buf_565(frame_bytes);

    int frame_cnt = 0;
    int64 fps_start_time = cv::getTickCount();
    const char* dump_path = std::getenv("PCIE_CAPTURE_FRAME");
    const bool enable_frame_dump = dump_path && dump_path[0];
    bool frame_dumped = false;

    // Match QT?????7.6: row-by-row DMA only unless explicitly PCIE_USE_FRAME=1.
    // Never enable double-buffer (removed; caused upper/lower wrap).
    const char* frame_env = std::getenv("PCIE_USE_FRAME");
    const bool use_frame_mode = frame_env && frame_env[0] == '1';
    std::cout << "[PCIe] imaging=7.6-compatible"
              << " mode=" << (use_frame_mode ? "frame_dma" : "row_dma")
              << " double_buf=0\n";

    if (use_frame_mode) {
        std::cout << "[PCIe] frame stride mmap (PCIE_USE_FRAME=1, stride=" << PCIE_ROW_STRIDE
                  << ")\n";
        FRAME_CAPTURE frame_cap{};
        frame_cap.width = static_cast<unsigned int>(kPcieWidth);
        frame_cap.height = static_cast<unsigned int>(kPcieHeight);
        frame_cap.row_bytes = static_cast<unsigned int>(kPcieRowBytes);
        frame_cap.user_buf = 0;

        if (ioctl(fd, PCI_MAP_FRAME_CMD, &frame_cap) < 0) {
            std::cerr << "[PCIe] PCI_MAP_FRAME_CMD failed\n";
            emit showMessage(QString::fromUtf8("PCIe \u6574\u5e27 DMA \u6620\u5c04\u5931\u8d25"));
            ::close(fd);
            return;
        }

        const long page_size = sysconf(_SC_PAGESIZE);
        const size_t frame_map_bytes = page_size > 0
            ? ((PCIE_FRAME_MAP_BYTES + static_cast<size_t>(page_size) - 1) /
               static_cast<size_t>(page_size)) *
                  static_cast<size_t>(page_size)
            : static_cast<size_t>(PCIE_FRAME_MAP_BYTES);

        void* dma_frame_buf = mmap(nullptr, frame_map_bytes, PROT_READ, MAP_SHARED, fd, 0);
        if (dma_frame_buf == MAP_FAILED) {
            emit showMessage(QString::fromUtf8("PCIe \u6574\u5e27 mmap \u5931\u8d25"));
            ioctl(fd, PCI_UMAP_ADDR_CMD, &frame_cap);
            ::close(fd);
            return;
        }

        // Whole-frame path: single-buffer serial (stable imaging)
        while (keep_running.load(std::memory_order_relaxed)) {
            const int64 t_dma0 = cv::getTickCount();
            if (ioctl(fd, PCI_READ_FRAME_CMD, &frame_cap) < 0) {
                usleep(1000);
                continue;
            }
            const double pcie_dma_ms =
                (cv::getTickCount() - t_dma0) * 1000.0 / cv::getTickFrequency();

            const int64 e2e_t0 = cv::getTickCount();
            const auto* frame_base = static_cast<const uint8_t*>(dma_frame_buf);

            if (enable_frame_dump && !frame_dumped) {
                FILE* fp = std::fopen(dump_path, "wb");
                if (fp) {
                    for (int row = 0; row < kPcieHeight; ++row) {
                        const uint8_t* row_src =
                            frame_base + static_cast<size_t>(row) * PCIE_ROW_STRIDE;
                        std::fwrite(row_src, 1, static_cast<size_t>(kPcieRowBytes), fp);
                    }
                    std::fclose(fp);
                    frame_dumped = true;
                }
            }

            if (frame_cnt > 0 && frame_cnt % 10 == 0) {
                const double fps =
                    10.0 * cv::getTickFrequency() / (cv::getTickCount() - fps_start_time);
                fps_start_time = cv::getTickCount();
                std::cout << "[PCIe] capture FPS: " << fps << " (frame " << frame_cnt << ")\r"
                          << std::flush;
            }
            frame_cnt++;
            if (frame_cnt == 1) {
                std::cout << "\n[PCIe] first frame (frame_dma=1, snapshot=1)\n";
            }

            // Snapshot compact RGB565 before process/infer. Direct mmap use can tear
            // (upper/lower wrap) if the next PCI_READ_FRAME_CMD overwrites the mapping.
            for (int row = 0; row < kPcieHeight; ++row) {
                const uint8_t* row_src =
                    frame_base + static_cast<size_t>(row) * PCIE_ROW_STRIDE;
                std::memcpy(image_buf_565.data() + static_cast<size_t>(row) * kPcieRowBytes,
                            row_src, static_cast<size_t>(kPcieRowBytes));
            }
            processCapturedFrame(image_buf_565.data(), frame_cnt, 0, e2e_t0, pcie_dma_ms);
        }

        munmap(dma_frame_buf, frame_map_bytes);
        ioctl(fd, PCI_UMAP_ADDR_CMD, &frame_cap);
        ::close(fd);
        return;
    }

    std::cout << "[PCIe] default row mmap capture 1280x720\n";

    DMA_OPERATION dma_operation{};
    dma_operation.current_len = kPcieRowBytes / 4;
    dma_operation.offset_addr = 0;

    if (ioctl(fd, PCI_MAP_ADDR_CMD, &dma_operation) < 0) {
        emit showMessage(QString::fromUtf8("PCIe DMA \u6620\u5c04\u5931\u8d25"));
        ::close(fd);
        return;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    const size_t row_map_bytes = page_size > 0
        ? ((static_cast<size_t>(kPcieRowBytes) + static_cast<size_t>(page_size) - 1) /
           static_cast<size_t>(page_size)) *
              static_cast<size_t>(page_size)
        : static_cast<size_t>(DMA_MAX_PACKET_SIZE);

    void* dma_row_buf = mmap(nullptr, row_map_bytes, PROT_READ, MAP_SHARED, fd, 0);
    const bool use_row_mmap = dma_row_buf != MAP_FAILED;

    while (keep_running.load(std::memory_order_relaxed)) {
        const int64 t_dma0 = cv::getTickCount();
        bool row_ok = true;
        for (int row = 0; row < kPcieHeight; ++row) {
            if (ioctl(fd, PCI_DMA_WRITE_CMD, &dma_operation) < 0) {
                row_ok = false;
                break;
            }
            uint8_t* row_dst = image_buf_565.data() + static_cast<size_t>(row) * kPcieRowBytes;
            if (use_row_mmap) {
                std::memcpy(row_dst, dma_row_buf, kPcieRowBytes);
            } else if (ioctl(fd, PCI_READ_FROM_KERNEL_CMD, &dma_operation) < 0) {
                row_ok = false;
                break;
            } else {
                std::memcpy(row_dst, dma_operation.data.read_buf, kPcieRowBytes);
            }
        }
        if (!row_ok) {
            usleep(1000);
            continue;
        }
        const double pcie_dma_ms =
            (cv::getTickCount() - t_dma0) * 1000.0 / cv::getTickFrequency();

        const int64 e2e_t0 = cv::getTickCount();

        if (enable_frame_dump && !frame_dumped) {
            FILE* fp = std::fopen(dump_path ? dump_path : "output/pcie_frame.bin", "wb");
            if (fp) {
                std::fwrite(image_buf_565.data(), 1, frame_bytes, fp);
                std::fclose(fp);
                frame_dumped = true;
            }
        }

        if (frame_cnt > 0 && frame_cnt % 10 == 0) {
            const double fps =
                10.0 * cv::getTickFrequency() / (cv::getTickCount() - fps_start_time);
            fps_start_time = cv::getTickCount();
            std::cout << "[PCIe] capture FPS: " << fps << " (frame " << frame_cnt << ")\r"
                      << std::flush;
        }
        frame_cnt++;
        if (frame_cnt == 1) {
            std::cout << "\n[PCIe] first frame (row_dma=1, mmap=" << (use_row_mmap ? "1" : "0")
                      << ")\n";
        }

        processCapturedFrame(image_buf_565.data(), frame_cnt, 0, e2e_t0, pcie_dma_ms);
    }

    if (use_row_mmap) munmap(dma_row_buf, row_map_bytes);
    ioctl(fd, PCI_UMAP_ADDR_CMD, &dma_operation);
    ::close(fd);
    std::cout << "\n[PCIe] stream closed\n";
}

bool InferenceThread::checkPcieLink(int fd) {
    COMMAND_OPERATION command_operation{};
    command_operation.delay = 0;
    if (read(fd, &command_operation, sizeof(COMMAND_OPERATION)) < 0) {
        std::cerr << "[PCIe] read device info failed, errno=" << errno << " ("
                  << std::strerror(errno) << ")" << std::endl;
        return false;
    }
    if (command_operation.cap_info.cap_status != 1) {
        std::cerr << "[PCIe] link failure (cap_status="
                  << (int)command_operation.cap_info.cap_status << ")" << std::endl;
        return false;
    }
    std::cout << "[PCIe] link OK, vendor=0x" << std::hex
              << command_operation.get_pci_dev_info.vendor_id << " device=0x"
              << command_operation.get_pci_dev_info.device_id << std::dec << " Gen"
              << command_operation.get_pci_dev_info.link_speed << " x"
              << command_operation.get_pci_dev_info.link_width << std::endl;
    return true;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      galleryIndex_(0),
      collectProcessTimes_(false),
      auto_pcie_started_(false),
      galleryBrowseMode_(false),
      frameFlushScheduled_(false) {
    setupUI();
    inferenceThread = new InferenceThread(this);
    connect(btnInit, &QPushButton::clicked, this, &MainWindow::initializeUi);
    connect(btnFolder, &QPushButton::clicked, this, &MainWindow::openFolder);
    connect(btnMedia, &QPushButton::clicked, this, &MainWindow::openMediaFile);
    connect(btnPCIe, &QPushButton::clicked, this, &MainWindow::openPCIe);
    connect(btnStopPCIe, &QPushButton::clicked, this, &MainWindow::stopPCIe);
    connect(inferenceThread, &InferenceThread::frameReady, this, &MainWindow::updateFrame,
            Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::showMessage, this, &MainWindow::updateStatus,
            Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::batchGalleryReady, this, &MainWindow::onBatchGalleryReady,
            Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::batchProgressChanged, this,
            &MainWindow::onBatchProgressChanged, Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::batchProgressEnd, this, &MainWindow::onBatchProgressEnd,
            Qt::QueuedConnection);
    connect(btnPrev, &QPushButton::clicked, this, &MainWindow::showGalleryPrev);
    connect(btnNext, &QPushButton::clicked, this, &MainWindow::showGalleryNext);
    connect(sliderLabelScale, &QSlider::valueChanged, this, &MainWindow::onLabelScaleChanged);
    connect(inferenceThread, &InferenceThread::imageProcessTime, this, &MainWindow::onImageProcessTime,
            Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::plateResultsUpdated, this,
            &MainWindow::onPlateResultsUpdated, Qt::QueuedConnection);
    connect(inferenceThread, &QThread::finished, this, [this]() {
        setPcieStreamingUi(false);
        if (btnStopPCIe) btnStopPCIe->setEnabled(false);
    });
    inferenceThread->setLabelScale(0.52f);
}

MainWindow::~MainWindow() {}

void MainWindow::loadIndustrialStyleSheet() {
    QFile f(":/ui_industrial.qss");
    if (f.open(QIODevice::ReadOnly)) setStyleSheet(QString::fromUtf8(f.readAll()));
}

void MainWindow::setRunState(const QString& text, const char* state) {
    labelTopStatus->setText(text);
    labelStatusLed->setProperty("state", state);
    labelStatusLed->style()->unpolish(labelStatusLed);
    labelStatusLed->style()->polish(labelStatusLed);
}

void MainWindow::setupUI() {
    setWindowTitle(QString::fromUtf8("\u8f66\u724c\u8bc6\u522b\u7cfb\u7edf"));
    resize(1280, 800);
    loadIndustrialStyleSheet();

    QWidget* root = new QWidget(this);
    root->setObjectName("RootWidget");

    QWidget* nav = new QWidget(root);
    nav->setObjectName("NavSidebar");
    nav->setFixedWidth(240);

    QLabel* navBrand = new QLabel(QString::fromUtf8("Plate Vision"), nav);
    navBrand->setObjectName("NavBrand");
    QLabel* navSub = new QLabel(QString::fromUtf8("RK3568 HMI"), nav);
    navSub->setObjectName("NavSub");

    btnFolder = new QPushButton(QString::fromUtf8("\u9009\u62e9\u6587\u4ef6\u5939"), nav);
    btnFolder->setObjectName("NavButton");
    btnMedia = new QPushButton(QString::fromUtf8("\u9009\u62e9\u56fe\u7247\u6216\u89c6\u9891"), nav);
    btnMedia->setObjectName("NavButton");
    btnPCIe = new QPushButton(QString::fromUtf8("PCIe \u5b9e\u65f6\u91c7\u96c6"), nav);
    btnPCIe->setObjectName("NavButton");
    btnStopPCIe = new QPushButton(QString::fromUtf8("\u505c\u6b62\u91c7\u96c6"), nav);
    btnStopPCIe->setObjectName("NavButton");
    btnStopPCIe->setEnabled(false);
    btnInit = new QPushButton(QString::fromUtf8("\u754c\u9762\u521d\u59cb\u5316"), nav);
    btnInit->setObjectName("NavButton");

    QVBoxLayout* navLay = new QVBoxLayout(nav);
    navLay->setContentsMargins(16, 20, 16, 20);
    navLay->setSpacing(8);
    navLay->addWidget(navBrand);
    navLay->addWidget(navSub);
    navLay->addSpacing(12);
    navLay->addWidget(btnInit);
    navLay->addWidget(btnPCIe);
    navLay->addWidget(btnStopPCIe);
    navLay->addWidget(btnFolder);
    navLay->addWidget(btnMedia);
    navLay->addSpacing(16);

    QLabel* plateCaption = new QLabel(QString::fromUtf8("\u8bc6\u522b\u7ed3\u679c"), nav);
    plateCaption->setObjectName("PlateResultCaption");
    plateCaption->setAlignment(Qt::AlignCenter);

    QFrame* platePanel = new QFrame(nav);
    platePanel->setObjectName("PlateResultPanel");

    labelPlateNumber = new QLabel(QString::fromUtf8("--"), platePanel);
    labelPlateNumber->setObjectName("PlateNumberDisplay");
    labelPlateNumber->setAlignment(Qt::AlignCenter);
    labelPlateNumber->setWordWrap(true);
    labelPlateNumber->setProperty("state", "empty");

    labelPlateCount = new QLabel(platePanel);
    labelPlateCount->setObjectName("PlateCountDisplay");
    labelPlateCount->setAlignment(Qt::AlignCenter);
    labelPlateCount->setVisible(false);

    QVBoxLayout* platePanelLay = new QVBoxLayout(platePanel);
    platePanelLay->setContentsMargins(14, 16, 14, 16);
    platePanelLay->setSpacing(8);
    platePanelLay->addWidget(labelPlateNumber);
    platePanelLay->addWidget(labelPlateCount);

    navLay->addWidget(plateCaption);
    navLay->addWidget(platePanel);
    navLay->addSpacing(20);

    QLabel* scaleCaption = new QLabel(QString::fromUtf8("\u6807\u7b7e\u5b57\u53f7"), nav);
    scaleCaption->setObjectName("NavSub");
    sliderLabelScale = new QSlider(Qt::Horizontal, nav);
    sliderLabelScale->setObjectName("LabelScaleSlider");
    sliderLabelScale->setRange(20, 80);
    sliderLabelScale->setValue(52);
    sliderLabelScale->setTickPosition(QSlider::TicksBelow);
    sliderLabelScale->setTickInterval(10);
    labelScaleValue = new QLabel(QString::fromUtf8("0.52"), nav);
    labelScaleValue->setObjectName("NavBrand");

    navLay->addWidget(scaleCaption);
    navLay->addWidget(sliderLabelScale);
    navLay->addWidget(labelScaleValue);
    navLay->addStretch();

    QWidget* topBar = new QWidget(root);
    topBar->setObjectName("TopBar");
    topBar->setFixedHeight(64);

    labelTitle = new QLabel(QString::fromUtf8("\u8f66\u724c\u68c0\u6d4b\u4e0e\u8bc6\u522b"), topBar);
    labelTitle->setObjectName("TopTitle");
    labelSubtitle = new QLabel(QString::fromUtf8("plate_detect + plate_rec (INT8/FP auto)"), topBar);
    labelSubtitle->setObjectName("TopSubtitle");

    labelStatusLed = new QLabel(topBar);
    labelStatusLed->setObjectName("StatusLed");
    labelTopStatus = new QLabel(QString::fromUtf8("\u5c31\u7eea"), topBar);
    labelTopStatus->setObjectName("TopStatusText");
    setRunState(QString::fromUtf8("\u5c31\u7eea"), "ready");

    QLabel* metricCaption = new QLabel(QString::fromUtf8("\u6279\u91cf\u8fdb\u5ea6"), topBar);
    metricCaption->setObjectName("MetricLabel");
    labelMetricValue = new QLabel(QString::fromUtf8("--"), topBar);
    labelMetricValue->setObjectName("MetricValue");
    labelMetricValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QLabel* timeCaption = new QLabel(QString::fromUtf8("process_wall"), topBar);
    timeCaption->setObjectName("MetricLabel");
    labelProcessTime = new QLabel(QString::fromUtf8("--"), topBar);
    labelProcessTime->setObjectName("MetricValue");
    labelProcessTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QLabel* e2eCaption = new QLabel(QString::fromUtf8("\u7aef\u5230\u7aef"), topBar);
    e2eCaption->setObjectName("MetricLabel");
    labelE2eTime = new QLabel(QString::fromUtf8("--"), topBar);
    labelE2eTime->setObjectName("MetricValue");
    labelE2eTime->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QVBoxLayout* titleCol = new QVBoxLayout();
    titleCol->setSpacing(2);
    titleCol->addWidget(labelTitle);
    titleCol->addWidget(labelSubtitle);

    QHBoxLayout* statusRow = new QHBoxLayout();
    statusRow->setSpacing(8);
    statusRow->addWidget(labelStatusLed);
    statusRow->addWidget(labelTopStatus);
    statusRow->addStretch();

    QHBoxLayout* topHBox = new QHBoxLayout(topBar);
    topHBox->setContentsMargins(24, 12, 24, 12);
    topHBox->addLayout(titleCol);
    topHBox->addStretch();
    topHBox->addLayout(statusRow);
    topHBox->addSpacing(24);
    QVBoxLayout* metricCol = new QVBoxLayout();
    metricCol->setSpacing(0);
    metricCol->addWidget(metricCaption);
    metricCol->addWidget(labelMetricValue);
    topHBox->addLayout(metricCol);
    topHBox->addSpacing(16);
    QVBoxLayout* timeCol = new QVBoxLayout();
    timeCol->setSpacing(0);
    timeCol->addWidget(timeCaption);
    timeCol->addWidget(labelProcessTime);
    topHBox->addLayout(timeCol);
    topHBox->addSpacing(16);
    QVBoxLayout* e2eCol = new QVBoxLayout();
    e2eCol->setSpacing(0);
    e2eCol->addWidget(e2eCaption);
    e2eCol->addWidget(labelE2eTime);
    topHBox->addLayout(e2eCol);

    QFrame* viewport = new QFrame(root);
    viewport->setObjectName("ViewportFrame");
    viewport->setFrameShape(QFrame::StyledPanel);

    displayLabel = new FrameView(viewport);

    QVBoxLayout* vpLay = new QVBoxLayout(viewport);
    vpLay->setContentsMargins(8, 8, 8, 8);
    vpLay->addWidget(displayLabel);

    progressBar = new QProgressBar(root);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setFormat(QString::fromUtf8("\u5c31\u7eea"));
    progressBar->setVisible(false);

    btnPrev = new QPushButton(QString::fromUtf8("\u25c0 \u4e0a\u4e00\u5f20"), root);
    btnPrev->setObjectName("GalleryNavButton");
    btnNext = new QPushButton(QString::fromUtf8("\u4e0b\u4e00\u5f20 \u25b6"), root);
    btnNext->setObjectName("GalleryNavButton");
    labelGalleryPage = new QLabel(QString::fromUtf8("0 / 0"), root);
    labelGalleryPage->setObjectName("PageIndicator");
    labelGalleryPage->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    btnPrev->setEnabled(false);
    btnNext->setEnabled(false);

    QHBoxLayout* galleryBtnGroup = new QHBoxLayout();
    galleryBtnGroup->setSpacing(6);
    galleryBtnGroup->setContentsMargins(0, 0, 0, 0);
    galleryBtnGroup->addWidget(btnPrev);
    galleryBtnGroup->addWidget(btnNext);

    QHBoxLayout* navRow = new QHBoxLayout();
    navRow->setSpacing(16);
    navRow->addWidget(labelGalleryPage, 1);
    navRow->addLayout(galleryBtnGroup);

    statusLabel = new QLabel(QString::fromUtf8("\u7cfb\u7edf\u5c31\u7eea"), root);
    statusLabel->setObjectName("FooterStatus");
    statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QHBoxLayout* footerRow = new QHBoxLayout();
    footerRow->addStretch();
    footerRow->addWidget(statusLabel);

    QVBoxLayout* contentLay = new QVBoxLayout();
    contentLay->setContentsMargins(20, 0, 20, 16);
    contentLay->setSpacing(12);
    contentLay->addWidget(topBar);
    contentLay->addWidget(viewport, 1);
    contentLay->addWidget(progressBar);
    contentLay->addLayout(navRow);
    contentLay->addLayout(footerRow);

    QHBoxLayout* rootLay = new QHBoxLayout(root);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);
    rootLay->addWidget(nav);
    rootLay->addLayout(contentLay, 1);

    setCentralWidget(root);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // PCIe ioctl/DMA can block; never wait forever or the UI/Ctrl+C looks frozen.
    stopInferenceIfRunning();
    event->accept();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (!galleryPaths_.isEmpty()) showGalleryAt(galleryIndex_);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (auto_pcie_started_) return;
    auto_pcie_started_ = true;

    const char* env = std::getenv("PLATE_AUTO_PCIE");
    if (env && env[0] == '0') return;

    QTimer::singleShot(300, this, [this]() {
        if (!pcieDeviceAccessible()) {
            loadPcieDriver(false);
        }
        startPCIeStream();
    });
}

QString MainWindow::defaultDialogDir() const {
    if (QDir("/userdata/5.19_v1_computer").exists()) return "/userdata/5.19_v1_computer";
    if (QDir("/userdata").exists()) return "/userdata";
    return QDir::homePath();
}

void MainWindow::stopInferenceIfRunning() {
    if (!inferenceThread || !inferenceThread->isRunning()) return;
    // Match 7.6: soft stop + wait (no terminate mid-DMA, which can leave a torn frame).
    inferenceThread->stop();
    inferenceThread->wait();
}

void MainWindow::startMediaProcessing(const QString& path) {
    if (path.isEmpty()) return;

    stopInferenceIfRunning();
    setPcieStreamingUi(false);
    galleryPaths_.clear();
    galleryProcessMs_.clear();
    galleryIndex_ = 0;
    updateGalleryNavUi();
    labelMetricValue->setText(QString::fromUtf8("--"));
    labelProcessTime->setText(QString::fromUtf8("--"));
    labelE2eTime->setText(QString::fromUtf8("--"));
    progressBar->setVisible(false);
    collectProcessTimes_ = true;
    galleryBrowseMode_ = false;
    pendingFrame_ = QImage();
    frameFlushScheduled_ = false;
    lastDisplayedPlates_.clear();
    displayLabel->setPlaceholder(QString::fromUtf8("\u52a0\u8f7d\u4e2d\u2026"));
    setRunState(QString::fromUtf8("\u51c6\u5907\u5904\u7406"), "running");
    updateStatus(QString::fromUtf8("\u52a0\u8f7d\u6a21\u578b\u4e0e\u5a92\u4f53\u2026"));
    inferenceThread->setInputPath(path);
    inferenceThread->start();
}

void MainWindow::openFolder() {
    const QString path = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("\u9009\u62e9\u56fe\u7247\u6587\u4ef6\u5939"), defaultDialogDir(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty()) return;
    startMediaProcessing(path);
}

void MainWindow::openMediaFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("\u9009\u62e9\u56fe\u7247\u6216\u89c6\u9891"), defaultDialogDir(),
        QString::fromUtf8(
            "\u56fe\u7247\u548c\u89c6\u9891 (*.jpg *.jpeg *.png *.bmp *.webp *.mp4 *.avi *.mkv *.mov "
            "*.mpeg *.mpg *.wmv *.flv);;"
            "\u56fe\u7247 (*.jpg *.jpeg *.png *.bmp *.webp);;"
            "\u89c6\u9891 (*.mp4 *.avi *.mkv *.mov *.mpeg *.mpg *.wmv *.flv);;"
            "\u6240\u6709\u6587\u4ef6 (*)"));
    if (path.isEmpty()) return;
    startMediaProcessing(path);
}

void MainWindow::openPCIe() { startPCIeStream(); }

void MainWindow::setPcieStreamingUi(bool streaming) {
    btnPCIe->setEnabled(!streaming);
    btnStopPCIe->setEnabled(streaming);
    btnFolder->setEnabled(!streaming);
    btnMedia->setEnabled(!streaming);
    btnInit->setEnabled(!streaming);
}

void MainWindow::stopPCIe() {
    if (!inferenceThread->isRunning()) {
        setPcieStreamingUi(false);
        return;
    }
    updateStatus(QString::fromUtf8("\u6b63\u5728\u505c\u6b62 PCIe \u91c7\u96c6\u2026"));
    setRunState(QString::fromUtf8("\u505c\u6b62\u4e2d"), "running");
    btnStopPCIe->setEnabled(false);
    stopInferenceIfRunning();
    setPcieStreamingUi(false);
    setRunState(QString::fromUtf8("\u5df2\u505c\u6b62"), "ready");
    updateStatus(QString::fromUtf8("PCIe \u91c7\u96c6\u5df2\u505c\u6b62"));
}

void MainWindow::startPCIeStream() {
    stopInferenceIfRunning();
    galleryBrowseMode_ = false;
    pendingFrame_ = QImage();
    frameFlushScheduled_ = false;
    lastDisplayedPlates_.clear();
    collectProcessTimes_ = false;
    galleryPaths_.clear();
    galleryProcessMs_.clear();
    labelMetricValue->setText(QString::fromUtf8("PCIe"));
    labelProcessTime->setText(QString::fromUtf8("--"));
    labelE2eTime->setText(QString::fromUtf8("--"));
    progressBar->setVisible(false);
    setRunState(QString::fromUtf8("PCIe \u91c7\u96c6\u4e2d"), "running");
    updateStatus(QString::fromUtf8("PCIe \u6d41\u542f\u52a8\u2026"));
    setPcieStreamingUi(true);
    inferenceThread->setInputPCIe();
    inferenceThread->start();
}

bool MainWindow::loadPcieDriver(bool force_reload) {
    const QString root = resolveAppRoot();
    const QString driver_dir = root + "/driver";
    const QString ko_path = driver_dir + "/pango_pci_driver.ko";

    auto fixPerm = []() {
        std::system("chmod 666 /dev/pango_pci_driver 2>/dev/null");
        std::system("sudo chmod 666 /dev/pango_pci_driver 2>/dev/null");
    };

    if (!QDir(driver_dir).exists()) {
        updateStatus(QString::fromUtf8("\u9a71\u52a8\u76ee\u5f55\u4e0d\u5b58\u5728: ") + driver_dir);
        std::cerr << "[Init] missing driver dir: " << driver_dir.toStdString() << "\n";
        return false;
    }

    if (!force_reload && isPcieDriverLoaded() && pcieDeviceAccessible()) {
        fixPerm();
        updateStatus(QString::fromUtf8("PCIe \u9a71\u52a8\u5df2\u5c31\u7eea"));
        return true;
    }

    updateStatus(QString::fromUtf8("\u6b63\u5728\u52a0\u8f7d PCIe \u9a71\u52a8\u2026"));
    QApplication::processEvents();

    if (!QFile::exists(ko_path)) {
        const QString make_cmd =
            QString("cd \"%1\" && make clean >/dev/null 2>&1; make").arg(driver_dir);
        std::cerr << "[Init] building driver in " << driver_dir.toStdString() << "\n";
        if (std::system(make_cmd.toLocal8Bit().constData()) != 0) {
            updateStatus(QString::fromUtf8("PCIe \u9a71\u52a8\u7f16\u8bd1\u5931\u8d25"));
            return false;
        }
    }

    if (!QFile::exists(ko_path)) {
        updateStatus(QString::fromUtf8("\u672a\u627e\u5230 pango_pci_driver.ko"));
        return false;
    }

    if (force_reload && isPcieDriverLoaded()) {
        std::system("sudo rmmod pango_pci_driver 2>/dev/null");
        std::system("rmmod pango_pci_driver 2>/dev/null");
        usleep(300000);
    }

    if (!isPcieDriverLoaded()) {
        const QString ko = ko_path;
        const QString insmod_direct = QString("insmod \"%1\" 2>&1").arg(ko);
        const QString insmod_sudo = QString("sudo insmod \"%1\" 2>&1").arg(ko);
        int ret = std::system(insmod_direct.toLocal8Bit().constData());
        if (ret != 0) {
            ret = std::system(insmod_sudo.toLocal8Bit().constData());
        }
        if (ret != 0 && !isPcieDriverLoaded()) {
            std::cerr << "[Init] insmod failed, ret=" << ret << "\n";
            updateStatus(QString::fromUtf8(
                "PCIe \u9a71\u52a8\u52a0\u8f7d\u5931\u8d25\uff0c\u8bf7\u5728\u7ec8\u7aef\u6267\u884c: "
                "cd driver && sudo insmod pango_pci_driver.ko && sudo chmod 666 /dev/pango_pci_driver"));
            return false;
        }
    }

    fixPerm();
    if (pcieDeviceAccessible()) {
        updateStatus(QString::fromUtf8("PCIe \u9a71\u52a8\u5df2\u52a0\u8f7d"));
        return true;
    }
    if (QFile::exists("/dev/pango_pci_driver")) {
        updateStatus(QString::fromUtf8(
            "PCIe \u8bbe\u5907\u65e0\u8bfb\u5199\u6743\u9650\uff0c\u8bf7\u6267\u884c: sudo chmod 666 /dev/pango_pci_driver"));
        return false;
    }
    updateStatus(QString::fromUtf8("PCIe \u8bbe\u5907\u8282\u70b9\u672a\u521b\u5efa"));
    return false;
}

QString MainWindow::resolveAppRoot() {
    QDir d(QCoreApplication::applicationDirPath());
    if (d.dirName() == "build") d.cdUp();
    if (QDir(d.filePath("driver")).exists() || QDir(d.filePath("weights")).exists()) {
        return d.absolutePath();
    }
    const QDir cwd(QDir::currentPath());
    if (QDir(cwd.filePath("driver")).exists()) return cwd.absolutePath();
    return d.absolutePath();
}

bool MainWindow::isPcieDriverLoaded() {
    QFile f("/proc/modules");
    if (!f.open(QIODevice::ReadOnly)) return false;
    return QString::fromUtf8(f.readAll()).contains("pango_pci_driver");
}

bool MainWindow::pcieDeviceAccessible() {
    int fd = open("/dev/pango_pci_driver", O_RDWR);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

void MainWindow::initializeUi() {
    galleryBrowseMode_ = false;
    galleryPaths_.clear();
    galleryProcessMs_.clear();
    galleryIndex_ = 0;
    collectProcessTimes_ = false;

    displayLabel->clearToPlaceholder();

    labelMetricValue->setText(QString::fromUtf8("--"));
    labelProcessTime->setText(QString::fromUtf8("--"));
    labelE2eTime->setText(QString::fromUtf8("--"));
    labelGalleryPage->setText(QString::fromUtf8("0 / 0"));

    progressBar->setVisible(false);
    progressBar->setValue(0);
    progressBar->setFormat(QString::fromUtf8("\u5c31\u7eea"));

    btnPrev->setEnabled(false);
    btnNext->setEnabled(false);

    sliderLabelScale->setValue(52);
    labelScaleValue->setText(QString::fromUtf8("0.52"));
    inferenceThread->setLabelScale(0.52f);

    pendingFrame_ = QImage();
    frameFlushScheduled_ = false;
    lastDisplayedPlates_.clear();
    inferenceThread->resetPlateResultCache();
    updatePlateResultDisplay(QStringList());

    setRunState(QString::fromUtf8("\u5c31\u7eea"), "ready");
    statusLabel->setText(QString::fromUtf8("\u754c\u9762\u5df2\u521d\u59cb\u5316"));
}

void MainWindow::updatePlateResultDisplay(const QStringList& plate_numbers) {
    if (plate_numbers == lastDisplayedPlates_) return;
    lastDisplayedPlates_ = plate_numbers;

    auto refreshStyle = [](QLabel* label) {
        label->style()->unpolish(label);
        label->style()->polish(label);
        label->update();
    };

    if (plate_numbers.isEmpty()) {
        labelPlateNumber->setText(QString::fromUtf8("\u672a\u68c0\u6d4b\u5230\u8f66\u724c"));
        labelPlateCount->setVisible(false);
        labelPlateNumber->setProperty("state", "empty");
    } else {
        labelPlateNumber->setText(plate_numbers.join("\n"));
        labelPlateNumber->setProperty("state", "hit");

        if (plate_numbers.size() > 1) {
            labelPlateCount->setText(
                QString::fromUtf8("\u5171 %1 \u4e2a\u8f66\u724c").arg(plate_numbers.size()));
            labelPlateCount->setVisible(true);
        } else {
            labelPlateCount->setVisible(false);
        }
    }

    refreshStyle(labelPlateNumber);
}

void MainWindow::onPlateResultsUpdated(QStringList plate_numbers) {
    if (galleryBrowseMode_) return;
    updatePlateResultDisplay(plate_numbers);
}

void MainWindow::updateFrame(QImage image) {
    if (galleryBrowseMode_) return;
    pendingFrame_ = std::move(image);
    if (frameFlushScheduled_) return;
    frameFlushScheduled_ = true;
    QMetaObject::invokeMethod(this, "flushPendingFrame", Qt::QueuedConnection);
}

void MainWindow::flushPendingFrame() {
    frameFlushScheduled_ = false;
    if (galleryBrowseMode_ || pendingFrame_.isNull()) return;
    presentFrameNow(pendingFrame_);
    pendingFrame_ = QImage();
}

void MainWindow::presentFrameNow(QImage image) {
    if (image.isNull() || !displayLabel) return;
    // Drop any coalesced pending frame so it cannot overwrite this present.
    pendingFrame_ = QImage();
    frameFlushScheduled_ = false;
    displayLabel->setFrameImage(std::move(image));
}

void MainWindow::updateStatus(QString msg) {
    statusLabel->setText(msg);
    labelTopStatus->setText(msg);
    if (msg.contains(QString::fromUtf8("\u5931\u8d25")) ||
        msg.contains(QString::fromUtf8("\u9519\u8bef")) ||
        msg.contains(QString::fromUtf8("\u65e0\u6cd5")) ||
        msg.contains(QString::fromUtf8("\u672a\u627e\u5230"))) {
        setRunState(msg, "err");
    } else if (msg.contains(QString::fromUtf8("\u5b8c\u6210")) ||
               msg.contains(QString::fromUtf8("\u5c31\u7eea"))) {
        setRunState(msg, "ok");
    }
}

void MainWindow::setGalleryResults(const QStringList& paths) {
    galleryPaths_ = paths;
    galleryIndex_ = 0;
    updateGalleryNavUi();
    if (!galleryPaths_.isEmpty()) showGalleryAt(0);
}

void MainWindow::showGalleryAt(int index) {
    if (galleryPaths_.isEmpty()) return;
    galleryBrowseMode_ = true;
    galleryIndex_ = qBound(0, index, galleryPaths_.size() - 1);

    QImage img(galleryPaths_.at(galleryIndex_));
    if (!img.isNull()) displayLabel->setFrameImage(img);

    QFileInfo fi(galleryPaths_.at(galleryIndex_));
    QString page = QString::fromUtf8("%1 / %2  %3")
                       .arg(galleryIndex_ + 1)
                       .arg(galleryPaths_.size())
                       .arg(fi.fileName());
    if (galleryIndex_ < galleryProcessMs_.size()) {
        const double ms = galleryProcessMs_.at(galleryIndex_);
        page += QString::fromUtf8("  \u2502  %1 ms").arg(ms, 0, 'f', 1);
        labelProcessTime->setText(QString::fromUtf8("%1 ms").arg(ms, 0, 'f', 1));
    }
    labelGalleryPage->setText(page);
    updateGalleryNavUi();
}

void MainWindow::updateGalleryNavUi() {
    const bool has = !galleryPaths_.isEmpty();
    btnPrev->setEnabled(has && galleryIndex_ > 0);
    btnNext->setEnabled(has && galleryIndex_ < galleryPaths_.size() - 1);
    if (!has) labelGalleryPage->setText(QString::fromUtf8("0 / 0"));
}

void MainWindow::setBatchUiBusy(bool busy) {
    btnInit->setEnabled(!busy);
    btnFolder->setEnabled(!busy);
    btnMedia->setEnabled(!busy);
    btnPCIe->setEnabled(!busy);
    btnStopPCIe->setEnabled(false);
    if (busy) {
        btnPrev->setEnabled(false);
        btnNext->setEnabled(false);
        setRunState(QString::fromUtf8("\u6279\u91cf\u5904\u7406\u4e2d"), "running");
    } else {
        updateGalleryNavUi();
        setRunState(QString::fromUtf8("\u5c31\u7eea"), "ready");
    }
}

void MainWindow::onBatchProgressChanged(int current, int total) {
    if (total <= 0) return;
    progressBar->setVisible(true);
    progressBar->setRange(0, total);
    progressBar->setValue(current);
    const int pct = (current * 100) / total;
    progressBar->setFormat(QString::fromUtf8("%1%  (%2 / %3)")
                             .arg(pct)
                             .arg(current)
                             .arg(total));
    labelMetricValue->setText(QString::fromUtf8("%1 / %2").arg(current).arg(total));
    setBatchUiBusy(true);
}

void MainWindow::onBatchProgressEnd() {
    collectProcessTimes_ = false;
    progressBar->setValue(progressBar->maximum());
    progressBar->setFormat(QString::fromUtf8("\u5b8c\u6210"));
    labelMetricValue->setText(QString::fromUtf8("\u5b8c\u6210"));
    setRunState(QString::fromUtf8("\u5b8c\u6210"), "ok");
    setBatchUiBusy(false);
}

void MainWindow::onBatchGalleryReady(QStringList saved_paths) {
    galleryBrowseMode_ = true;
    setGalleryResults(saved_paths);
    updateStatus(QString::fromUtf8("\u53ef\u7528\u4e0a\u4e00\u5f20/\u4e0b\u4e00\u5f20\u67e5\u770b\u7ed3\u679c"));
    setRunState(QString::fromUtf8("\u5b8c\u6210"), "ok");
}

void MainWindow::showGalleryPrev() {
    if (galleryIndex_ > 0) showGalleryAt(galleryIndex_ - 1);
}

void MainWindow::showGalleryNext() {
    if (galleryIndex_ + 1 < galleryPaths_.size()) showGalleryAt(galleryIndex_ + 1);
}

void MainWindow::onLabelScaleChanged(int value) {
    const float scale = value / 100.0f;
    labelScaleValue->setText(QString::number(scale, 'f', 2));
    inferenceThread->setLabelScale(scale);
}

void MainWindow::onImageProcessTime(QString /*name*/, double process_wall_ms, double e2e_ms) {
    labelProcessTime->setText(QString::fromUtf8("%1 ms").arg(process_wall_ms, 0, 'f', 1));
    labelE2eTime->setText(QString::fromUtf8("%1 ms").arg(e2e_ms, 0, 'f', 1));
    if (collectProcessTimes_) galleryProcessMs_.append(process_wall_ms);
}
