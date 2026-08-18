#include "mainwindow.h"
#include <QDebug>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <chrono>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QFontMetrics>
#include <QPen>
#include <QColor>
#include <queue>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <algorithm>

#define TYPE 'S'
#define PCI_MAP_ADDR_CMD _IOWR(TYPE, 2, int)
#define PCI_DMA_WRITE_CMD _IOWR(TYPE, 5, int)
#define PCI_READ_FROM_KERNEL_CMD _IOWR(TYPE, 6, int)
#define PCI_UMAP_ADDR_CMD _IOWR(TYPE, 7, int)

#define DMA_MAX_PACKET_SIZE 4096

typedef struct _DMA_DATA_ {
    unsigned char read_buf[DMA_MAX_PACKET_SIZE];
    unsigned char write_buf[DMA_MAX_PACKET_SIZE];
} DMA_DATA;

typedef struct _DMA_OPERATION_ {
    unsigned int current_len;
    unsigned int offset_addr;
    unsigned int cmd;
    DMA_DATA data;
} DMA_OPERATION;

namespace {
// #region agent log
static void dbg_f60c3c(const char* hypothesisId, const char* location, const char* message,
                       const std::string& dataJson) {
    const char* logPath = std::getenv("CURSOR_DEBUG_LOG");
    if (!logPath || !logPath[0])
        logPath = "/tmp/debug-f60c3c.log";
    std::ofstream ofs(logPath, std::ios::app);
    if (!ofs)
        return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    ofs << "{\"sessionId\":\"f60c3c\",\"hypothesisId\":\"" << hypothesisId
        << "\",\"location\":\"" << location << "\",\"message\":\"" << message
        << "\",\"data\":" << dataJson << ",\"timestamp\":" << ts << "}\n";
}

static std::string dbg_hex_prefix(const unsigned char* buf, size_t len, size_t maxBytes) {
    std::ostringstream oss;
    oss << "\"hex\":\"";
    const size_t n = std::min(len, maxBytes);
    for (size_t i = 0; i < n; ++i) {
        if (i)
            oss << ' ';
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(buf[i]);
    }
    oss << std::dec << "\"";
    return oss.str();
}

static size_t dbg_count_nonzero(const unsigned char* buf, size_t len) {
    size_t c = 0;
    for (size_t i = 0; i < len; ++i)
        if (buf[i] != 0)
            ++c;
    return c;
}

static int dbg_find_magic_le(const unsigned char* buf, size_t len, uint32_t magic) {
    if (len < 4)
        return -1;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = 0;
        std::memcpy(&v, buf + i, 4);
        if (v == magic)
            return static_cast<int>(i);
    }
    return -1;
}
// #endregion

constexpr int          kPcieImgW         = 1280;
constexpr int          kPcieImgH         = 720;
constexpr size_t       kPcieMetaBytes    = 224u;
constexpr size_t       kPcieImgBytes     = static_cast<size_t>(kPcieImgW) * kPcieImgH * 2u;
constexpr size_t       kPcieFrameBytes   = kPcieMetaBytes + kPcieImgBytes;
constexpr uint32_t     kFpgaRoiMagic     = 0x524B3031u;
constexpr unsigned     kDmaChunkBytes    = 2560u;
constexpr unsigned     kDmaChunkDw       = kDmaChunkBytes / 4u;
constexpr unsigned     kDmaTailDw        = static_cast<unsigned>(kPcieMetaBytes / 4u);

struct __attribute__((packed)) FpgaMetaHdr {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t width;
    uint32_t height;
    uint32_t roi_count;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
};

struct __attribute__((packed)) FpgaRoi {
    uint32_t x1;
    uint32_t y1;
    uint32_t x2;
    uint32_t y2;
    uint32_t color;
    uint32_t confidence;
};

static bool pcie_read_one_frame_pango(int fd, std::vector<uint8_t>& frame, DMA_OPERATION& dma_op) {
    frame.resize(kPcieFrameBytes);
    size_t off = 0;
    static int s_dbg_try = 0;
    const bool dbg_this = (s_dbg_try < 3);
    if (dbg_this)
        ++s_dbg_try;

    // 与 code_3/文档一致：单次 MAP，按 2560B 块连续读满整帧（224B meta + 1843200B 像素）
    dma_op.offset_addr = 0;
    dma_op.current_len = kDmaChunkDw;
    memset(dma_op.data.write_buf, 0, DMA_MAX_PACKET_SIZE);
    memset(dma_op.data.read_buf, 0, DMA_MAX_PACKET_SIZE);

    int r_map = ioctl(fd, PCI_MAP_ADDR_CMD, &dma_op);
    if (r_map < 0)
        return false;

    // #region agent log
    if (dbg_this) {
        uint32_t wb0 = 0, wb1 = 0;
        std::memcpy(&wb0, dma_op.data.write_buf, 4);
        std::memcpy(&wb1, dma_op.data.write_buf + 4, 4);
        std::ostringstream dj;
        dj << "{\"phase\":\"after_map\",\"try\":" << (s_dbg_try - 1) << ",\"map_ret\":" << r_map
           << ",\"write_buf_u32_0\":" << wb0 << ",\"write_buf_u32_1\":" << wb1 << "}";
        dbg_f60c3c("F", "mainwindow.cpp:after_map", "write_buf after PCI_MAP_ADDR", dj.str());
    }
    // #endregion

    int chunk_idx = 0;
    while (off < kPcieFrameBytes) {
        const size_t chunk_bytes =
            std::min(static_cast<size_t>(kDmaChunkBytes), kPcieFrameBytes - off);
        memset(dma_op.data.read_buf, 0, DMA_MAX_PACKET_SIZE);
        int r_dma = ioctl(fd, PCI_DMA_WRITE_CMD, &dma_op);
        if (r_dma < 0) {
            ioctl(fd, PCI_UMAP_ADDR_CMD, &dma_op);
            return false;
        }
        for (volatile int k = 0; k < 2500; ++k) { }
        int r_read = ioctl(fd, PCI_READ_FROM_KERNEL_CMD, &dma_op);
        if (r_read < 0) {
            ioctl(fd, PCI_UMAP_ADDR_CMD, &dma_op);
            return false;
        }

        // #region agent log
        if (dbg_this && (chunk_idx == 0 || chunk_idx == 1)) {
            const size_t nz = dbg_count_nonzero(dma_op.data.read_buf, DMA_MAX_PACKET_SIZE);
            const int magic_off = dbg_find_magic_le(dma_op.data.read_buf, DMA_MAX_PACKET_SIZE, kFpgaRoiMagic);
            uint32_t le0 = 0;
            std::memcpy(&le0, dma_op.data.read_buf, 4);
            std::ostringstream dj;
            dj << "{\"phase\":\"chunk\",\"try\":" << (s_dbg_try - 1) << ",\"chunk_idx\":" << chunk_idx
               << ",\"frame_off\":" << off << ",\"chunk_bytes\":" << chunk_bytes
               << ",\"dma_ret\":" << r_dma << ",\"read_ret\":" << r_read
               << ",\"current_len_dw\":" << dma_op.current_len
               << ",\"offset_addr\":" << dma_op.offset_addr << ",\"nonzero_in_4k\":" << nz
               << ",\"magic_le_offset\":" << magic_off << ",\"first_u32\":" << le0 << ","
               << dbg_hex_prefix(dma_op.data.read_buf, DMA_MAX_PACKET_SIZE, 32) << "}";
            dbg_f60c3c(chunk_idx == 0 ? "A" : "B", "mainwindow.cpp:chunk_read",
                       chunk_idx == 0 ? "first chunk after read" : "second chunk after read", dj.str());
            std::cerr << "[DBG-f60c3c][" << (chunk_idx == 0 ? "A" : "B") << "] chunk" << chunk_idx
                      << " nonzero=" << nz << " magic_off=" << magic_off << " first_u32=0x" << std::hex
                      << le0 << std::dec << std::endl;
        }
        // #endregion

        std::memcpy(frame.data() + off, dma_op.data.read_buf, chunk_bytes);
        off += chunk_bytes;
        ++chunk_idx;
    }

    if (ioctl(fd, PCI_UMAP_ADDR_CMD, &dma_op) < 0)
        return false;

    // #region agent log
    if (dbg_this) {
        const size_t nz_frame = dbg_count_nonzero(frame.data(), std::min(kPcieFrameBytes, size_t(4096)));
        const int magic_frame = dbg_find_magic_le(frame.data(), kPcieFrameBytes, kFpgaRoiMagic);
        std::ostringstream dj;
        dj << "{\"phase\":\"assembled\",\"try\":" << (s_dbg_try - 1) << ",\"off\":" << off
           << ",\"nonzero_first_4k\":" << nz_frame << ",\"magic_le_offset\":" << magic_frame
           << ",\"hdr_magic\":" << reinterpret_cast<const FpgaMetaHdr*>(frame.data())->magic << ","
           << dbg_hex_prefix(frame.data(), kPcieFrameBytes, 32) << "}";
        dbg_f60c3c("C", "mainwindow.cpp:frame_done", "assembled frame header", dj.str());
        std::cerr << "[DBG-f60c3c][C] frame nz(first4k)=" << nz_frame << " magic_off=" << magic_frame
                  << std::endl;
    }
    // #endregion

    return off == kPcieFrameBytes;
}

struct PlateCandidate {
    cv::RotatedRect rect;
    double score = -1.0;
};

static double clamp01(double v) {
    return std::max(0.0, std::min(v, 1.0));
}

static double calcTextureScore(const cv::Mat& gray_roi) {
    if (gray_roi.empty()) return 0.0;
    cv::Mat gx, abs_gx;
    cv::Sobel(gray_roi, gx, CV_16S, 1, 0, 3);
    cv::convertScaleAbs(gx, abs_gx);
    return clamp01((cv::mean(abs_gx)[0] - 10.0) / 45.0);
}

static double calcCharacterLikeScore(const cv::Mat& gray_roi) {
    if (gray_roi.empty() || gray_roi.cols < 20 || gray_roi.rows < 10) return 0.0;

    cv::Mat resized;
    const int w = 180;
    const int h = std::max(1, static_cast<int>(std::round(static_cast<double>(gray_roi.rows) * w / gray_roi.cols)));
    cv::resize(gray_roi, resized, cv::Size(w, h));

    cv::Mat blur, binary;
    cv::GaussianBlur(resized, blur, cv::Size(3, 3), 0.0);
    cv::adaptiveThreshold(blur, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY_INV, 21, 5);
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2)));

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    int valid_count = 0;
    for (const auto& contour : contours) {
        cv::Rect r = cv::boundingRect(contour);
        if (r.width < 3 || r.height < 8) continue;
        const double hr = static_cast<double>(r.height) / binary.rows;
        const double wr = static_cast<double>(r.width) / binary.cols;
        const double ar = static_cast<double>(r.width) / r.height;
        if (hr > 0.28 && hr < 0.95 && wr > 0.01 && wr < 0.20 && ar > 0.08 && ar < 1.2) {
            ++valid_count;
        }
    }

    if (valid_count <= 1) return 0.0;
    if (valid_count <= 9) return clamp01(valid_count / 7.0);
    return clamp01(1.0 - (valid_count - 9) / 8.0);
}

static double calcMaskCoverageScore(const cv::RotatedRect& rr, const cv::Mat& color_mask, const cv::Size& sz) {
    cv::Rect br = rr.boundingRect() & cv::Rect(0, 0, sz.width, sz.height);
    if (br.empty()) return 0.0;
    const int nz = cv::countNonZero(color_mask(br));
    const int total = br.area();
    if (total <= 0) return 0.0;
    return clamp01((static_cast<double>(nz) / total - 0.15) / 0.60);
}

static PlateCandidate selectBestFromMask(const cv::Mat& bgr, const cv::Mat& candidate_mask) {
    cv::Mat hsv, gray;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    cv::Mat blue, green, color;
    cv::inRange(hsv, cv::Scalar(90, 35, 35), cv::Scalar(140, 255, 255), blue);
    cv::inRange(hsv, cv::Scalar(35, 25, 25), cv::Scalar(95, 255, 255), green);
    color = blue | green;

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(candidate_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    PlateCandidate best;
    const double img_area = static_cast<double>(bgr.cols * bgr.rows);
    for (const auto& contour : contours) {
        if (contour.size() < 5) continue;
        cv::RotatedRect rr = cv::minAreaRect(contour);
        const double w = rr.size.width;
        const double h = rr.size.height;
        if (w <= 1.0 || h <= 1.0) continue;

        const double ratio = std::max(w, h) / std::min(w, h);
        const double area_ratio = (w * h) / img_area;
        if (ratio < 2.1 || ratio > 6.2 || area_ratio < 0.0015 || area_ratio > 0.35) continue;

        cv::Rect br = rr.boundingRect() & cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (br.empty()) continue;

        const double ratio_score = 1.0 - std::min(std::abs(ratio - 3.2) / 3.2, 1.0);
        const double area_score = std::min(area_ratio / 0.08, 1.0);
        const double color_score = calcMaskCoverageScore(rr, color, bgr.size());
        const double texture_score = calcTextureScore(gray(br));
        const double char_score = calcCharacterLikeScore(gray(br));

        const double y = rr.center.y / std::max(1.0, static_cast<double>(bgr.rows));
        const double position_score = (y < 0.20) ? 0.20 : ((y < 0.30) ? 0.60 : 1.0);
        if (color_score < 0.06 && char_score < 0.12) continue;

        const double score = 0.24 * ratio_score + 0.14 * area_score + 0.20 * color_score +
                             0.16 * texture_score + 0.18 * char_score + 0.08 * position_score;
        if (score > best.score) {
            best.rect = rr;
            best.score = score;
        }
    }
    return best;
}

static PlateCandidate findBestPlate(const cv::Mat& bgr) {
    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

    cv::Mat blue, green, color;
    cv::inRange(hsv, cv::Scalar(90, 35, 35), cv::Scalar(140, 255, 255), blue);
    cv::inRange(hsv, cv::Scalar(35, 25, 25), cv::Scalar(95, 255, 255), green);
    color = blue | green;

    cv::morphologyEx(color, color, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(11, 3)));
    cv::morphologyEx(color, color, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
    return selectBestFromMask(bgr, color);
}

static PlateCandidate findBestPlateByEdge(const cv::Mat& bgr) {
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

    cv::Mat edge;
    cv::Canny(gray, edge, 60, 180);
    cv::morphologyEx(edge, edge, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(11, 3)));
    cv::morphologyEx(edge, edge, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
    return selectBestFromMask(bgr, edge);
}

static cv::Rect detectPlateRectByV2(const cv::Mat& frame_bgr) {
    PlateCandidate best = findBestPlate(frame_bgr);
    if (best.score < 0.0) {
        best = findBestPlateByEdge(frame_bgr);
    }
    if (best.score < 0.0) return cv::Rect();

    cv::Rect plate_rect = best.rect.boundingRect() & cv::Rect(0, 0, frame_bgr.cols, frame_bgr.rows);
    if (plate_rect.area() <= 0) return cv::Rect();

    const int pad_x = std::max(2, plate_rect.width / 30);
    const int pad_y = std::max(2, plate_rect.height / 12);
    int x = std::max(0, plate_rect.x - pad_x);
    int y = std::max(0, plate_rect.y - pad_y);
    int w = std::min(frame_bgr.cols - x, plate_rect.width + 2 * pad_x);
    int h = std::min(frame_bgr.rows - y, plate_rect.height + 2 * pad_y);
    return cv::Rect(x, y, w, h);
}

} // namespace

void rgb565_to_rgb888_local(const uint16_t *image565, uint8_t *image888, size_t num_pixels) {
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

InferenceThread::InferenceThread(QObject *parent)
    : QThread(parent), keep_running(false), current_input_type(INPUT_CAMERA),
      camera_index(0), yolo11_detector(nullptr), yolo11_thread_pool(nullptr), lprnet_ready(false) 
{
    std::string yolo_model_path = "./weights/yolov11_3568.rknn";
    std::string lprnet_model_path = "./weights/lprnet_rk3568_new.rknn";
    int numThreads = 4;

    yolo11_detector = new Yolov11Custom(); 
    if (yolo11_detector->LoadModel(yolo_model_path.c_str()) == NN_SUCCESS) {
        yolo11_thread_pool = new Yolov11ThreadPool();
        yolo11_thread_pool->setUp(yolo_model_path, numThreads);
    }

    memset(&lprnet_ctx, 0, sizeof(rknn_app_context_t));
    if (init_lprnet_model(lprnet_model_path.c_str(), &lprnet_ctx) == 0) {
        lprnet_ready = true;
    }
}

InferenceThread::~InferenceThread() {
    stop();
    wait();
    if (yolo11_detector) { delete yolo11_detector; yolo11_detector = nullptr; }
    if (yolo11_thread_pool) {
        yolo11_thread_pool->stopAll();
        delete yolo11_thread_pool;
        yolo11_thread_pool = nullptr;
    }
    if (lprnet_ready) {
        release_lprnet_model(&lprnet_ctx);
        lprnet_ready = false;
    }
}

void InferenceThread::setInputCamera(int cam_index) { camera_index = cam_index; current_input_type = INPUT_CAMERA; }
void InferenceThread::setInputImage(const QString& path) { file_path = path; current_input_type = INPUT_IMAGE; }
void InferenceThread::setInputVideo(const QString& path) { file_path = path; current_input_type = INPUT_VIDEO; }
void InferenceThread::setInputPCIe() { current_input_type = INPUT_PCIE; }
void InferenceThread::stop() { keep_running = false; }

void InferenceThread::ensureOutputDirectory() {
    QDir dir("output");
    if (!dir.exists()) dir.mkpath(".");
}

QImage InferenceThread::matToQImage(const cv::Mat& mat) {
    return QImage((const unsigned char*)(mat.data), mat.cols, mat.rows, mat.step, QImage::Format_BGR888).copy();
}

QString InferenceThread::detectPlateColor(const cv::Mat& plate_img) {
    if (plate_img.empty()) return "Unknown";
    cv::Mat hsv, mask_b, mask_y, mask_g;
    cv::cvtColor(plate_img, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(100, 50, 50), cv::Scalar(124, 255, 255), mask_b);
    cv::inRange(hsv, cv::Scalar(15, 50, 50), cv::Scalar(34, 255, 255), mask_y);
    cv::inRange(hsv, cv::Scalar(35, 50, 50), cv::Scalar(85, 255, 255), mask_g);

    int b_pts = cv::countNonZero(mask_b);
    int y_pts = cv::countNonZero(mask_y);
    int g_pts = cv::countNonZero(mask_g);
    int max_pts = std::max({b_pts, y_pts, g_pts});
    
    if (max_pts < 50) return "Unknown";
    if (max_pts == b_pts) return "蓝色";
    if (max_pts == y_pts) return "黄色";
    return "绿色";
}

QString InferenceThread::recognizePlateText(const cv::Mat& plate_img) {
    if (!lprnet_ready || plate_img.empty()) return "识别失败";
    cv::Mat resized;
    cv::resize(plate_img, resized, cv::Size(MODEL_WIDTH, MODEL_HEIGHT));
    
    lprnet_result result;
    if (inference_lprnet_model(&lprnet_ctx, resized, &result) != 0) return "推理错误";
    return QString::fromStdString(result.plate_name);
}

void InferenceThread::drawChineseTextAndBox(cv::Mat& img, const cv::Rect& box, const QString& text, const cv::Scalar& color) {
    cv::Mat rgb;
    cv::cvtColor(img, rgb, cv::COLOR_BGR2RGB);
    QImage qimg(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888);
    QPainter painter(&qimg);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor qColor(color[2], color[1], color[0]); 
    
    painter.setPen(QPen(qColor, 3));
    painter.drawRect(box.x, box.y, box.width, box.height);
    QFont font("Microsoft YaHei", 18, QFont::Bold);
    painter.setFont(font);
    QFontMetrics fm(font);
    
    int w = fm.horizontalAdvance(text), h = fm.height();
    painter.setBrush(qColor);
    painter.setPen(Qt::NoPen);
    painter.drawRect(box.x, box.y - h - 8, w + 10, h + 8);
    painter.setPen(QColor(0, 0, 0)); 
    painter.drawText(box.x + 5, box.y - 5, text);
    painter.end();
    cv::cvtColor(rgb, img, cv::COLOR_RGB2BGR);
}

void InferenceThread::run() {
    if (current_input_type == INPUT_PCIE) {
        // PCIe 链路不依赖 YOLO；即使 LPRNet 初始化失败也允许继续跑流显示
        plate_tracker_.reset();
        if (!lprnet_ready) {
            emit showMessage("LPRNet 模型初始化失败，已切换为仅流显示模式");
        }
    } else {
        if (!yolo11_detector || !yolo11_thread_pool || !lprnet_ready) {
            emit showMessage("模型初始化失败");
            return;
        }
    }
    keep_running = true;
    ensureOutputDirectory();

    if (current_input_type == INPUT_IMAGE) processSingleImage();
    else if (current_input_type == INPUT_PCIE) processPCIeStream();
    else processStream();
    
    emit showMessage("就绪");
}

void InferenceThread::processPCIeStream() {
    std::cout << "\n[PCIe] 尝试打开设备节点..." << std::endl;
    int fd = -1;
    const char* used_node = nullptr;
    const char* pcie_nodes[] = {
        "/dev/pango_pci_driver",
        "/dev/xdma0_c2h_0"
    };

    for (const char* node : pcie_nodes) {
        fd = open(node, O_RDWR);
        if (fd >= 0) { used_node = node; break; }
    }
    if (fd < 0) {
        std::cerr << "[PCIe] 错误: 无法打开设备节点，errno="
                  << errno << " (" << std::strerror(errno) << ")" << std::endl;
        emit showMessage("无法打开 PCIe 设备");
        return;
    }
    std::cout << "[PCIe] 设备节点打开成功: " << used_node << std::endl;
    std::cout << "[PCIe] 帧格式: " << kPcieFrameBytes << " B = " << kPcieMetaBytes
              << " B meta(先) + " << kPcieImgBytes << " B RGB565 ("
              << kPcieImgW << "x" << kPcieImgH << ")" << std::endl;

    const int width = kPcieImgW;
    const int height = kPcieImgH;
    std::vector<uint8_t> frame_raw;
    std::vector<uint8_t> image_buf_temp(width * height * 2);
    std::vector<uint8_t> image_buf_888(width * height * 3);
    DMA_OPERATION dma_operation{};

    uint64_t read_fail_cnt = 0;
    uint64_t meta_fail_cnt = 0;
    uint64_t pcie_frames_ok = 0;
    int res_mismatch_log = 0;
    int64 fps_start_time = cv::getTickCount();
    double current_fps = 0.0;

    while (keep_running) {
        if (!pcie_read_one_frame_pango(fd, frame_raw, dma_operation)) {
            ++read_fail_cnt;
            if (read_fail_cnt <= 3u) {
                std::cerr << "[PCIe] 读帧失败 (ioctl)" << std::endl;
            }
            usleep(2000);
            continue;
        }

        if (frame_raw.size() < kPcieMetaBytes + kPcieImgBytes) {
            ++read_fail_cnt;
            usleep(1000);
            continue;
        }

        const auto* hdr = reinterpret_cast<const FpgaMetaHdr*>(frame_raw.data());
        if (hdr->magic != kFpgaRoiMagic) {
            ++meta_fail_cnt;
            if (meta_fail_cnt <= 5u || (meta_fail_cnt % 30u) == 0u) {
                std::cerr << "[PCIe][meta] magic 校验失败, 期望 0x" << std::hex << kFpgaRoiMagic
                          << " 收到 0x" << hdr->magic << std::dec
                          << " (累计 " << meta_fail_cnt << ")" << std::endl;
            }
            // #region agent log
            if (meta_fail_cnt <= 3u) {
                const int magic_any = dbg_find_magic_le(frame_raw.data(), frame_raw.size(), kFpgaRoiMagic);
                std::ostringstream dj;
                dj << "{\"meta_fail_cnt\":" << meta_fail_cnt << ",\"hdr_magic\":" << hdr->magic
                   << ",\"magic_anywhere_offset\":" << magic_any << ",\"frame_nonzero_4k\":"
                   << dbg_count_nonzero(frame_raw.data(), std::min(frame_raw.size(), size_t(4096)))
                   << "," << dbg_hex_prefix(frame_raw.data(), frame_raw.size(), 48) << "}";
                dbg_f60c3c("D", "mainwindow.cpp:magic_fail", "magic check failed on assembled frame", dj.str());
                std::cerr << "[DBG-f60c3c][D] magic_anywhere_offset=" << magic_any << std::endl;
            }
            // #endregion
            continue;
        }
        if (hdr->width != static_cast<uint32_t>(width) || hdr->height != static_cast<uint32_t>(height)) {
            if (res_mismatch_log < 3) {
                ++res_mismatch_log;
                std::cerr << "[PCIe][meta] 分辨率异常 " << hdr->width << "x" << hdr->height << std::endl;
            }
        }

        ++pcie_frames_ok;

        memcpy(image_buf_temp.data(), frame_raw.data() + kPcieMetaBytes, kPcieImgBytes);
        rgb565_to_rgb888_local(reinterpret_cast<uint16_t*>(image_buf_temp.data()), image_buf_888.data(), width * height);

        cv::Mat img(height, width, CV_8UC3, image_buf_888.data());
        cv::Mat draw_frame;
        cv::cvtColor(img, draw_frame, cv::COLOR_RGB2BGR);

        cv::Rect search_roi(0, 0, draw_frame.cols, draw_frame.rows);
        if (plate_tracker_.isVisible()) {
            cv::Rect tr = plate_tracker_.visibleRect();
            int padX = tr.width;
            int padY = std::max(tr.height * 2, 40);
            int x = std::max(0, tr.x - padX);
            int y = std::max(0, tr.y - padY);
            int w = std::min(draw_frame.cols - x, tr.width + 2 * padX);
            int h = std::min(draw_frame.rows - y, tr.height + 2 * padY);
            if (w > 60 && h > 30) search_roi = cv::Rect(x, y, w, h);
        }

        cv::Rect detected_box;
        {
            cv::Mat sub = draw_frame(search_roi);
            cv::Rect local = detectPlateRectByV2(sub);
            if (local.area() > 0) {
                detected_box = local + cv::Point(search_roi.x, search_roi.y);
            }
        }

        std::string raw_text;
        std::string raw_color;
        if (detected_box.area() > 0) {
            cv::Mat plate_crop = draw_frame(detected_box).clone();
            raw_color = detectPlateColor(plate_crop).toStdString();
            raw_text = recognizePlateText(plate_crop).toStdString();
        }

        cv::Rect draw_box = plate_tracker_.update(detected_box, raw_text, raw_color);
        if (draw_box.area() > 0) {
            QString label_str = QString("%1 (%2)")
                                    .arg(plate_tracker_.stableText())
                                    .arg(plate_tracker_.stableColor());
            drawChineseTextAndBox(draw_frame, draw_box, label_str, cv::Scalar(0, 255, 0));
        }

        if (pcie_frames_ok % 10 == 0) {
            int64 fps_end_time = cv::getTickCount();
            double fps = 10.0 * cv::getTickFrequency() / (fps_end_time - fps_start_time);
            fps_start_time = fps_end_time;
            current_fps = fps;
        }
        if (current_fps > 0.0) {
            std::string fps_text = cv::format("FPS: %.1f", current_fps);
            cv::putText(draw_frame,
                        fps_text,
                        cv::Point(15, draw_frame.rows - 20),
                        cv::FONT_HERSHEY_SIMPLEX,
                        1.0,
                        cv::Scalar(0, 255, 0),
                        2);
        }
        emit frameReady(matToQImage(draw_frame));
        usleep(2000);
    }
    close(fd);
    std::cout << "\n[PCIe] 流读取安全关闭" << std::endl;
}

void InferenceThread::processSingleImage() {
    emit showMessage("正在处理图片...");
    cv::Mat frame = cv::imread(file_path.toStdString());
    if (frame.empty()) {
        emit showMessage("无法读取图片！");
        return;
    }

    int64 t_start = cv::getTickCount();
    yolo11_thread_pool->submitTask(frame.clone(), 0);
    
    std::vector<Detection> objects;
    while (yolo11_thread_pool->getTargetResultNonBlock(objects, 0) != NN_SUCCESS && keep_running) {
        usleep(1000);
    }

    if (!keep_running) return;

    for (auto &obj : objects) {
        if (obj.confidence < 0.15) continue; 
        cv::Rect safe_box = obj.box & cv::Rect(0, 0, frame.cols, frame.rows);
        if (safe_box.area() <= 0) continue;

        float target_ratio = (float)MODEL_WIDTH / MODEL_HEIGHT; 
        int cx = safe_box.x + safe_box.width / 2;
        int cy = safe_box.y + safe_box.height / 2;
        int new_h = safe_box.height * 1.1; 
        int new_w = safe_box.width * 1.1;

        if ((float)new_w / new_h < target_ratio) {
            new_w = static_cast<int>(new_h * target_ratio);
        } else {
            new_h = static_cast<int>(new_w / target_ratio);
        }

        int new_x = std::max(0, cx - new_w / 2);
        int new_y = std::max(0, cy - new_h / 2);
        new_w = std::min(frame.cols - new_x, new_w);
        new_h = std::min(frame.rows - new_y, new_h);

        safe_box = cv::Rect(new_x, new_y, new_w, new_h);
        if (safe_box.area() <= 0) continue;

        cv::Mat plate_crop = frame(safe_box);
        QString plate_color = detectPlateColor(plate_crop);
        QString plate_text = recognizePlateText(plate_crop);

        cv::Scalar box_color;
        if (plate_color == "蓝色") box_color = cv::Scalar(255, 0, 0); 
        else if (plate_color == "黄色") box_color = cv::Scalar(0, 255, 255);
        else if (plate_color == "绿色") box_color = cv::Scalar(0, 255, 0);
        else box_color = cv::Scalar(255, 255, 255); 

        QString label_str = QString("[%1] %2 (%3)").arg(obj.confidence, 0, 'f', 2).arg(plate_text).arg(plate_color);
        drawChineseTextAndBox(frame, safe_box, label_str, box_color);
    }

    int64 t_end = cv::getTickCount();
    double time_ms = (t_end - t_start) * 1000.0 / cv::getTickFrequency();
    cv::putText(frame, cv::format("Cost: %.1f ms", time_ms), cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);

    QString out_file = QString("output/image_%1.jpg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    cv::imwrite(out_file.toStdString(), frame);
    
    emit frameReady(matToQImage(frame));
    emit showMessage("图片已保存至: " + out_file);
}

void InferenceThread::processStream() {
    cv::VideoCapture cap;
    if (current_input_type == INPUT_CAMERA) {
        emit showMessage("正在打开摄像头...");
        cap.open(camera_index, cv::CAP_V4L2);
    } else {
        emit showMessage("正在打开本地视频...");
        cap.open(file_path.toStdString());
    }

    if (!cap.isOpened()) {
        emit showMessage("无法打开视频源！");
        return;
    }

    int width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps = cap.get(cv::CAP_PROP_FPS);
    if (fps <= 0 || current_input_type == INPUT_CAMERA) fps = 30.0;

    QString out_file = QString("output/video_%1.mp4").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    cv::VideoWriter writer(out_file.toStdString(), cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, cv::Size(width, height));
    
    emit showMessage("正在录制视频至: " + out_file);

    int job_cnt = 0, result_cnt = 0;
    std::queue<cv::Mat> frame_queue;
    cv::Mat frame;
    int numThreads = 4;

    int64 fps_start_time = cv::getTickCount();
    int fps_frame_count = 0;
    double current_fps = 0.0;

    while (keep_running || result_cnt < job_cnt) {
        if (keep_running && (job_cnt - result_cnt < numThreads)) {
            cap >> frame;
            if (frame.empty()) break; 
            
            yolo11_thread_pool->submitTask(frame, job_cnt++);
            frame_queue.push(frame.clone());
        }

        std::vector<Detection> objects;
        if (result_cnt < job_cnt && yolo11_thread_pool->getTargetResultNonBlock(objects, result_cnt) == NN_SUCCESS) {
            cv::Mat draw_frame = frame_queue.front();
            frame_queue.pop();

            for (auto &obj : objects) {
                if (obj.confidence < 0.15) continue; 
                cv::Rect safe_box = obj.box & cv::Rect(0, 0, draw_frame.cols, draw_frame.rows);
                if (safe_box.area() <= 0) continue;

                float target_ratio = (float)MODEL_WIDTH / MODEL_HEIGHT; 
                int cx = safe_box.x + safe_box.width / 2;
                int cy = safe_box.y + safe_box.height / 2;

                int new_h = safe_box.height * 1.1; 
                int new_w = safe_box.width * 1.1;

                float current_ratio = (float)new_w / new_h;
                if (current_ratio < target_ratio) {
                    new_w = static_cast<int>(new_h * target_ratio);
                } else {
                    new_h = static_cast<int>(new_w / target_ratio);
                }

                int new_x = std::max(0, cx - new_w / 2);
                int new_y = std::max(0, cy - new_h / 2);
                new_w = std::min(draw_frame.cols - new_x, new_w);
                new_h = std::min(draw_frame.rows - new_y, new_h);

                safe_box = cv::Rect(new_x, new_y, new_w, new_h);
                if (safe_box.area() <= 0) continue;

                cv::Mat plate_crop = draw_frame(safe_box);
                QString plate_color = detectPlateColor(plate_crop);
                QString plate_text = recognizePlateText(plate_crop);

                cv::Scalar box_color;
                if (plate_color == "蓝色") box_color = cv::Scalar(255, 0, 0); 
                else if (plate_color == "黄色") box_color = cv::Scalar(0, 255, 255);
                else if (plate_color == "绿色") box_color = cv::Scalar(0, 255, 0);
                else box_color = cv::Scalar(255, 255, 255); 

                QString label_str = QString("[%1] %2 (%3)").arg(obj.confidence, 0, 'f', 2).arg(plate_text).arg(plate_color);
                drawChineseTextAndBox(draw_frame, safe_box, label_str, box_color);
            }

            fps_frame_count++;
            if (fps_frame_count % 10 == 0) {
                int64 fps_end_time = cv::getTickCount();
                current_fps = 10.0 * cv::getTickFrequency() / (fps_end_time - fps_start_time);
                fps_start_time = fps_end_time; 
            }
            
            if (current_fps > 0.0) {
                std::string fps_text = cv::format("FPS: %.1f", current_fps);
                cv::putText(draw_frame, fps_text, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
            }

            if (writer.isOpened()) writer.write(draw_frame); 
            emit frameReady(matToQImage(draw_frame));
            result_cnt++;
        } else {
            usleep(1000);
        }

        if (!keep_running && result_cnt == job_cnt) break;
        if (frame.empty() && result_cnt == job_cnt) break;
    }
    
    if (writer.isOpened()) writer.release(); 
    cap.release();
    emit showMessage("视频保存完成: " + out_file);
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setupUI();
    inferenceThread = new InferenceThread(this);
    connect(btnPCIe, &QPushButton::clicked, this, &MainWindow::openPCIe);
    connect(btnStopPCIe, &QPushButton::clicked, this, &MainWindow::stopPCIe);
    connect(inferenceThread, &InferenceThread::frameReady, this, &MainWindow::updateFrame, Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::showMessage, this, &MainWindow::updateStatus, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI() {
    this->setWindowTitle("能跑就能用");
    this->resize(1024, 768);
    this->setStyleSheet(
        "QMainWindow { background-color: #ffffff; }"
        "QPushButton { background-color: #ffffff; color: #111111; border: 1px solid #d0d0d0; border-radius: 6px; padding: 12px 20px; font-weight: bold; font-size: 14px; }"
        "QPushButton:hover { background-color: #f5f5f5; }"
        "QPushButton:pressed { background-color: #ebebeb; }"
        "QLabel#DisplayLabel { background-color: #f8f8f8; color: #666666; border: 2px dashed #cfcfcf; border-radius: 10px; font-size: 18px; }"
        "QLabel#StatusLabel { color: #333333; font-size: 13px; padding: 5px; }");

    displayLabel = new QLabel("点击下方按钮开始采集");
    displayLabel->setObjectName("DisplayLabel");
    displayLabel->setAlignment(Qt::AlignCenter);
    displayLabel->setMinimumSize(800, 500);

    statusLabel = new QLabel("状态: 就绪");
    statusLabel->setObjectName("StatusLabel");

    btnPCIe = new QPushButton("图像采集");
    btnStopPCIe = new QPushButton("停止采集");

    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    btnLayout->addWidget(btnPCIe);
    btnLayout->addWidget(btnStopPCIe);
    btnLayout->addStretch();

    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->setContentsMargins(20, 20, 20, 10);
    mainLayout->addWidget(statusLabel, 0, Qt::AlignLeft | Qt::AlignTop);
    mainLayout->addWidget(displayLabel, 1);
    mainLayout->addLayout(btnLayout);

    QWidget *centralWidget = new QWidget(this);
    centralWidget->setLayout(mainLayout);
    setCentralWidget(centralWidget);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (inferenceThread->isRunning()) {
        inferenceThread->stop();
        inferenceThread->wait();
    }
    event->accept();
}

void MainWindow::openCamera() { if(inferenceThread->isRunning()){inferenceThread->stop();inferenceThread->wait();} inferenceThread->setInputCamera(0); inferenceThread->start(); }

void MainWindow::openImage() {
    QString fileName = QFileDialog::getOpenFileName(this, "选择测试图片", "", "Images (*.png *.jpg *.jpeg)");
    if (!fileName.isEmpty()) {
        if (inferenceThread->isRunning()) { inferenceThread->stop(); inferenceThread->wait(); }
        inferenceThread->setInputImage(fileName);
        inferenceThread->start();
    }
}

void MainWindow::openLocalVideo() {
    QString fileName = QFileDialog::getOpenFileName(this, "选择本地视频", "", "Videos (*.mp4 *.avi *.mkv)");
    if (!fileName.isEmpty()) {
        if (inferenceThread->isRunning()) { inferenceThread->stop(); inferenceThread->wait(); }
        inferenceThread->setInputVideo(fileName);
        inferenceThread->start();
    }
}

void MainWindow::openPCIe() {
    if (inferenceThread->isRunning()) { 
        inferenceThread->stop(); 
        inferenceThread->wait(); 
    }
    inferenceThread->setInputPCIe();
    inferenceThread->start();
}

void MainWindow::stopPCIe() {
    if (inferenceThread->isRunning()) {
        inferenceThread->stop();
        inferenceThread->wait();
        updateStatus("PCIe 传输已停止");
    } else {
        updateStatus("当前没有进行中的采集任务");
    }
}

void MainWindow::updateFrame(QImage image) { displayLabel->setPixmap(QPixmap::fromImage(image).scaled(displayLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); }
void MainWindow::updateStatus(QString msg) { statusLabel->setText("状态: " + msg); }