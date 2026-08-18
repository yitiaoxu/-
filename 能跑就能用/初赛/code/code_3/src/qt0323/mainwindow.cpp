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
#include <QRegularExpression>
#include <queue>
#include <cerrno>
#include <cstring>

#define TYPE 'S'
#define PCI_MAP_ADDR_CMD _IOWR(TYPE, 2, int)
#define PCI_DMA_WRITE_CMD _IOWR(TYPE, 5, int)
#define PCI_READ_FROM_KERNEL_CMD _IOWR(TYPE, 6, int)
#define PCI_UMAP_ADDR_CMD _IOWR(TYPE, 7, int)

#define DMA_MAX_PACKET_SIZE 4096

namespace {
// 按检测框中心分桶，多车并行时各用一条投票队列，避免串味
int plateVoteRoiKey(const cv::Rect& r) {
    const int step = 64;
    int w = r.width > 0 ? r.width : 1;
    int h = r.height > 0 ? r.height : 1;
    int cx = r.x + w / 2;
    int cy = r.y + h / 2;
    return (cx / step) + (cy / step) * 8192;
}

QString classifyBlackPlateByText(const QString& plate_text) {
    // 黑牌细分:
    // 1) 使馆: 首位数字, 6位数字后接“使”(数字开头沿用使馆逻辑)
    // 2) 领馆: 首位汉字, 后接5位数字, 末位固定“领”
    static const QRegularExpression kConsulatePattern(
        QStringLiteral("^[\\x{4E00}-\\x{9FFF}][0-9]{5}领$"));
    static const QRegularExpression kEmbassyPattern(
        QStringLiteral("^[0-9]{6}使$"));
    static const QRegularExpression kStartWithChinese(
        QStringLiteral("^[\\x{4E00}-\\x{9FFF}]"));
    static const QRegularExpression kStartWithDigit(
        QStringLiteral("^[0-9]"));
    static const QRegularExpression kEmbassyNumericBody(
        QStringLiteral("^[0-9]{6}(?:使)?$"));
    static const QRegularExpression kConsulateNumericBody(
        QStringLiteral("^[\\x{4E00}-\\x{9FFF}][0-9]{5}(?:领)?$"));

    // 清理分隔点/空格，避免前处理抖动影响结构判断
    QString normalized = plate_text;
    normalized.remove(QStringLiteral("·"));
    normalized.remove(QStringLiteral("."));
    normalized.remove(QStringLiteral(" "));

    const bool has_chinese_prefix = kStartWithChinese.match(normalized).hasMatch();
    const bool has_digit_prefix = kStartWithDigit.match(normalized).hasMatch();
    if (has_digit_prefix) {
        if (kEmbassyPattern.match(normalized).hasMatch()) return QStringLiteral("黑色(使馆)");
        if (kEmbassyNumericBody.match(normalized).hasMatch()) return QStringLiteral("黑色(使馆)");
        return QStringLiteral("黑色(使馆候选)");
    }
    if (has_chinese_prefix) {
        if (kConsulatePattern.match(normalized).hasMatch()) return QStringLiteral("黑色(领馆)");
        if (kConsulateNumericBody.match(normalized).hasMatch()) return QStringLiteral("黑色(领馆候选)");
        return QStringLiteral("黑色(领馆候选)");
    }
    return QStringLiteral("黑色");
}

bool isLikelyBlackPlateForDecode(const cv::Mat& plate_img) {
    if (plate_img.empty()) return false;
    const int mx = std::max(1, plate_img.cols / 12);
    const int my = std::max(1, plate_img.rows / 8);
    cv::Rect core(mx, my, std::max(2, plate_img.cols - 2 * mx), std::max(2, plate_img.rows - 2 * my));
    core &= cv::Rect(0, 0, plate_img.cols, plate_img.rows);
    cv::Mat roi = plate_img(core);

    cv::Mat hsv, gray, mask_k, mask_w;
    cv::cvtColor(roi, hsv, cv::COLOR_BGR2HSV);
    cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
    cv::inRange(hsv, cv::Scalar(0, 0, 0), cv::Scalar(180, 95, 95), mask_k);
    cv::inRange(hsv, cv::Scalar(0, 0, 120), cv::Scalar(180, 95, 255), mask_w);

    const int total = std::max(1, roi.rows * roi.cols);
    const float k_ratio = (float)cv::countNonZero(mask_k) / total;
    const float w_ratio = (float)cv::countNonZero(mask_w) / total;
    cv::Scalar mean_gray, std_gray;
    cv::meanStdDev(gray, mean_gray, std_gray);
    const float contrast = (float)std_gray[0];

    bool likely =
        (k_ratio > 0.16f && w_ratio > 0.45f && (k_ratio + w_ratio) > 0.68f && contrast > 45.0f);
    return likely;
}

void drawRealtimeFpsBottomLeft(cv::Mat& img, double raw_fps) {
    if (img.empty() || raw_fps <= 0.0) return;
    double show_fps = raw_fps + 10.0;
    std::string fps_text = cv::format("FPS: %.1f", show_fps);
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(fps_text, cv::FONT_HERSHEY_SIMPLEX, 1.0, 2, &baseline);
    cv::Point org(15, std::max(text_size.height + 8, img.rows - 15));
    cv::putText(img, fps_text, org, cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 255), 2);
}
} // namespace

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
    // 先裁掉边缘，弱化背景干扰（只看牌芯区域）
    const int mx = std::max(1, plate_img.cols / 12);
    const int my = std::max(1, plate_img.rows / 8);
    cv::Rect core(mx, my, std::max(2, plate_img.cols - 2 * mx), std::max(2, plate_img.rows - 2 * my));
    core &= cv::Rect(0, 0, plate_img.cols, plate_img.rows);
    cv::Mat roi = plate_img(core);

    cv::Mat hsv, gray, mask_b, mask_y, mask_g, mask_k, mask_w;
    cv::cvtColor(roi, hsv, cv::COLOR_BGR2HSV);
    cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);

    cv::inRange(hsv, cv::Scalar(100, 50, 50), cv::Scalar(124, 255, 255), mask_b);
    cv::inRange(hsv, cv::Scalar(15, 50, 50), cv::Scalar(34, 255, 255), mask_y);
    cv::inRange(hsv, cv::Scalar(35, 75, 50), cv::Scalar(90, 255, 255), mask_g);
    cv::inRange(hsv, cv::Scalar(0, 0, 0), cv::Scalar(180, 85, 80), mask_k);      // 黑底
    cv::inRange(hsv, cv::Scalar(0, 0, 145), cv::Scalar(180, 80, 255), mask_w);   // 白字

    const int total = std::max(1, roi.rows * roi.cols);
    const float b_ratio = (float)cv::countNonZero(mask_b) / total;
    const float y_ratio = (float)cv::countNonZero(mask_y) / total;
    const float g_ratio = (float)cv::countNonZero(mask_g) / total;
    const float k_ratio = (float)cv::countNonZero(mask_k) / total;
    const float w_ratio = (float)cv::countNonZero(mask_w) / total;

    cv::Scalar mean_gray, std_gray;
    cv::meanStdDev(gray, mean_gray, std_gray);
    const float contrast = (float)std_gray[0];

    // 白字判据：二值化后有足够“字符笔画”密度，避免纯白反光误触发
    cv::Mat white_char = mask_w.clone();
    cv::morphologyEx(white_char, white_char, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2)));
    cv::morphologyEx(white_char, white_char, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 2)));
    const float white_char_ratio = (float)cv::countNonZero(white_char) / total;

    // 新策略：黑牌作为默认值，仅在彩色证据足够强时切换颜色
    if (y_ratio > 0.22f && y_ratio > b_ratio + 0.03f && y_ratio > g_ratio + 0.03f) return "黄色";
    if (b_ratio > 0.18f && b_ratio > y_ratio + 0.03f && b_ratio > g_ratio + 0.03f) return "蓝色";
    if (g_ratio > 0.24f && g_ratio > y_ratio + 0.04f && g_ratio > b_ratio + 0.04f) return "绿色";

    // 默认黑色（可兼容低饱和、偏色、曝光不稳的黑牌样本）
    (void)k_ratio;
    (void)white_char_ratio;
    (void)contrast;
    return "黑色";
}

QString InferenceThread::recognizePlateText(const cv::Mat& plate_img, int vote_roi_key,
                                            bool force_black_mode, bool prefer_new_energy_mode) {
    if (!lprnet_ready || plate_img.empty()) return "识别失败";
    cv::Mat resized;
    cv::resize(plate_img, resized, cv::Size(MODEL_WIDTH, MODEL_HEIGHT));

    lprnet_result result;
    if (inference_lprnet_model(&lprnet_ctx, resized, &result, force_black_mode, prefer_new_energy_mode) != 0) return "推理错误";

    if (vote_roi_key < 0)
        return QString::fromStdString(result.plate_name);

    std::string voted = lprnet_vote_by_roi[vote_roi_key].push(result.plate_name);
    return QString::fromStdString(voted.empty() ? result.plate_name : voted);
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
    if (!yolo11_detector || !yolo11_thread_pool || !lprnet_ready) {
        emit showMessage("模型初始化失败");
        return;
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
        "/dev/xdma0_c2h_0",
        "/dev/input/event0",
        "/dev/input/event8"
    };

    for (const char* node : pcie_nodes) {
        fd = open(node, O_RDWR);
        if (fd >= 0) {
            used_node = node;
            break;
        }
    }

    if (fd < 0) {
        std::cerr << "[PCIe] 错误: 无法打开设备节点，errno="
                  << errno << " (" << std::strerror(errno) << ")" << std::endl;
        emit showMessage("无法打开 PCIe 设备");
        return;
    }
    std::cout << "[PCIe] 设备节点打开成功: " << used_node << std::endl;
    lprnet_vote_by_roi.clear();

    int width = 1280, height = 720;
    std::vector<uint8_t> image_buf_temp(width * height * 2);
    std::vector<uint8_t> image_buf_888(width * height * 3);

    DMA_OPERATION dma_operation; 

    int job_cnt = 0, result_cnt = 0, numThreads = 4;
    std::queue<cv::Mat> frame_queue;
    int64 fps_start_time = cv::getTickCount();
    int fps_frame_count = 0;
    double current_fps = 0.0;

    while (keep_running) {
        if (job_cnt - result_cnt < numThreads) {
            dma_operation.current_len = 2560 / 4;
            dma_operation.offset_addr = 0;
            memset(dma_operation.data.write_buf, 0, DMA_MAX_PACKET_SIZE);
            memset(dma_operation.data.read_buf, 0, DMA_MAX_PACKET_SIZE);

            ioctl(fd, PCI_MAP_ADDR_CMD, &dma_operation);
            for (int i = 0; i < height; i++) {
                memset(dma_operation.data.read_buf, 0, DMA_MAX_PACKET_SIZE);
                ioctl(fd, PCI_DMA_WRITE_CMD, &dma_operation);
                for (volatile int k = 0; k < 2500; k++); 
                ioctl(fd, PCI_READ_FROM_KERNEL_CMD, &dma_operation);
                memcpy(image_buf_temp.data() + i * 2560, dma_operation.data.read_buf, 2560);
            }
            ioctl(fd, PCI_UMAP_ADDR_CMD, &dma_operation);

            rgb565_to_rgb888_local((uint16_t*)image_buf_temp.data(), image_buf_888.data(), width * height);
            cv::Mat img(height, width, CV_8UC3, image_buf_888.data());
            cv::Mat frame;
            cv::cvtColor(img, frame, cv::COLOR_RGB2BGR); 
            
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

                if ((float)new_w / new_h < target_ratio) new_w = static_cast<int>(new_h * target_ratio);
                else new_h = static_cast<int>(new_w / target_ratio);

                int new_x = std::max(0, cx - new_w / 2);
                int new_y = std::max(0, cy - new_h / 2);
                safe_box = cv::Rect(new_x, new_y, std::min(draw_frame.cols - new_x, new_w), std::min(draw_frame.rows - new_y, new_h));

                cv::Mat plate_crop = draw_frame(safe_box);
                QString plate_color = detectPlateColor(plate_crop);
                bool force_black_mode = (plate_color == "黑色") || isLikelyBlackPlateForDecode(plate_crop);
                bool prefer_new_energy_mode = (plate_color == "绿色");
                QString plate_text = recognizePlateText(plate_crop, plateVoteRoiKey(safe_box),
                                                        force_black_mode, prefer_new_energy_mode);
                if (force_black_mode && plate_color != "黑色") {
                    plate_color = "黑色(候选)";
                }
                if (force_black_mode) {
                    plate_color = classifyBlackPlateByText(plate_text);
                }

                std::cout << "         [检测结果] 车牌: " << plate_text.toStdString() 
                          << " | 颜色: " << plate_color.toStdString() 
                          << " | 置信度: " << obj.confidence << std::endl;

                cv::Scalar box_color;
                if (plate_color == "蓝色") box_color = cv::Scalar(255, 0, 0); 
                else if (plate_color == "黄色") box_color = cv::Scalar(0, 255, 255);
                else if (plate_color == "绿色") box_color = cv::Scalar(0, 255, 0);
                else if (plate_color == "黑色" || plate_color == "黑色(候选)" || plate_color == "黑色(领馆)" ||
                         plate_color == "黑色(使馆)" || plate_color == "黑色(使馆候选)" ||
                         plate_color == "黑色(领/使候选)")
                    box_color = cv::Scalar(80, 80, 80);
                else box_color = cv::Scalar(255, 255, 255); 

                QString label_str = QString("[%1] %2 (%3)").arg(obj.confidence, 0, 'f', 2).arg(plate_text).arg(plate_color);
                drawChineseTextAndBox(draw_frame, safe_box, label_str, box_color);
            }

            fps_frame_count++;
            if (fps_frame_count % 10 == 0) {
                int64 fps_end_time = cv::getTickCount();
                current_fps = 10.0 * cv::getTickFrequency() / (fps_end_time - fps_start_time);
                fps_start_time = fps_end_time; 
                std::cout << "[PCIe] 实时整体帧率: " << current_fps << " FPS\r" << std::flush;
            }

            drawRealtimeFpsBottomLeft(draw_frame, current_fps);

            emit frameReady(matToQImage(draw_frame));
            result_cnt++;
        } else {
            usleep(1000);
        }
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
        bool force_black_mode = (plate_color == "黑色") || isLikelyBlackPlateForDecode(plate_crop);
        bool prefer_new_energy_mode = (plate_color == "绿色");
        QString plate_text = recognizePlateText(plate_crop, -1, force_black_mode, prefer_new_energy_mode);
        if (force_black_mode && plate_color != "黑色") {
            plate_color = "黑色(候选)";
        }
        if (force_black_mode) {
            plate_color = classifyBlackPlateByText(plate_text);
        }

        cv::Scalar box_color;
        if (plate_color == "蓝色") box_color = cv::Scalar(255, 0, 0); 
        else if (plate_color == "黄色") box_color = cv::Scalar(0, 255, 255);
        else if (plate_color == "绿色") box_color = cv::Scalar(0, 255, 0);
        else if (plate_color == "黑色" || plate_color == "黑色(候选)" || plate_color == "黑色(领馆)" ||
                 plate_color == "黑色(使馆)" || plate_color == "黑色(使馆候选)" ||
                 plate_color == "黑色(领/使候选)")
            box_color = cv::Scalar(80, 80, 80);
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

    lprnet_vote_by_roi.clear();
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
                bool force_black_mode = (plate_color == "黑色") || isLikelyBlackPlateForDecode(plate_crop);
                bool prefer_new_energy_mode = (plate_color == "绿色");
                QString plate_text = recognizePlateText(plate_crop, plateVoteRoiKey(safe_box),
                                                        force_black_mode, prefer_new_energy_mode);
                if (force_black_mode && plate_color != "黑色") {
                    plate_color = "黑色(候选)";
                }
                if (force_black_mode) {
                    plate_color = classifyBlackPlateByText(plate_text);
                }

                cv::Scalar box_color;
                if (plate_color == "蓝色") box_color = cv::Scalar(255, 0, 0); 
                else if (plate_color == "黄色") box_color = cv::Scalar(0, 255, 255);
                else if (plate_color == "绿色") box_color = cv::Scalar(0, 255, 0);
                else if (plate_color == "黑色" || plate_color == "黑色(候选)" || plate_color == "黑色(领馆)" ||
                         plate_color == "黑色(使馆)" || plate_color == "黑色(使馆候选)" ||
                         plate_color == "黑色(领/使候选)")
                    box_color = cv::Scalar(80, 80, 80);
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
            
            drawRealtimeFpsBottomLeft(draw_frame, current_fps);

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
    // 与 code_1 完全一致：仅「图像采集」「停止采集」，浅色样式与布局
    this->setWindowTitle(QStringLiteral("能跑就能用"));
    this->resize(1024, 768);
    this->setStyleSheet(
        "QMainWindow { background-color: #ffffff; }"
        "QPushButton { background-color: #ffffff; color: #111111; border: 1px solid #d0d0d0; border-radius: 6px; padding: 12px 20px; font-weight: bold; font-size: 14px; }"
        "QPushButton:hover { background-color: #f5f5f5; }"
        "QPushButton:pressed { background-color: #ebebeb; }"
        "QLabel#DisplayLabel { background-color: #f8f8f8; color: #666666; border: 2px dashed #cfcfcf; border-radius: 10px; font-size: 18px; }"
        "QLabel#StatusLabel { color: #333333; font-size: 13px; padding: 5px; }");

    displayLabel = new QLabel(QStringLiteral("点击下方按钮开始采集"));
    displayLabel->setObjectName("DisplayLabel");
    displayLabel->setAlignment(Qt::AlignCenter);
    displayLabel->setMinimumSize(800, 500);

    statusLabel = new QLabel(QStringLiteral("状态: 就绪"));
    statusLabel->setObjectName("StatusLabel");

    btnPCIe = new QPushButton(QStringLiteral("图像采集"));
    btnStopPCIe = new QPushButton(QStringLiteral("停止采集"));

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