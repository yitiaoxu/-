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
    std::string lprnet_model_path = "./weights/lprnet_rk3568.rknn";
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

    int width = 1280, height = 720;
    std::vector<uint8_t> image_buf_temp(width * height * 2);
    std::vector<uint8_t> image_buf_888(width * height * 3);

    DMA_OPERATION dma_operation; 

    int job_cnt = 0, result_cnt = 0, numThreads = 4;
    std::queue<cv::Mat> frame_queue;
    int64 fps_start_time = cv::getTickCount();

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
                QString plate_text = recognizePlateText(plate_crop);

                std::cout << "         [检测结果] 车牌: " << plate_text.toStdString() 
                          << " | 颜色: " << plate_color.toStdString() 
                          << " | 置信度: " << obj.confidence << std::endl;

                cv::Scalar box_color;
                if (plate_color == "蓝色") box_color = cv::Scalar(255, 0, 0); 
                else if (plate_color == "黄色") box_color = cv::Scalar(0, 255, 255);
                else if (plate_color == "绿色") box_color = cv::Scalar(0, 255, 0);
                else box_color = cv::Scalar(255, 255, 255); 

                QString label_str = QString("[%1] %2 (%3)").arg(obj.confidence, 0, 'f', 2).arg(plate_text).arg(plate_color);
                drawChineseTextAndBox(draw_frame, safe_box, label_str, box_color);
            }

            if (result_cnt % 10 == 0) {
                int64 fps_end_time = cv::getTickCount();
                double fps = 10.0 * cv::getTickFrequency() / (fps_end_time - fps_start_time);
                fps_start_time = fps_end_time; 
                std::cout << "[PCIe] 实时整体帧率: " << fps << " FPS\r" << std::flush;
            }

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
    connect(btnCamera, &QPushButton::clicked, this, &MainWindow::openCamera);
    connect(btnImage, &QPushButton::clicked, this, &MainWindow::openImage);
    connect(btnVideo, &QPushButton::clicked, this, &MainWindow::openLocalVideo);
    connect(btnPCIe, &QPushButton::clicked, this, &MainWindow::openPCIe);
    connect(inferenceThread, &InferenceThread::frameReady, this, &MainWindow::updateFrame, Qt::QueuedConnection);
    connect(inferenceThread, &InferenceThread::showMessage, this, &MainWindow::updateStatus, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI() {
    this->setWindowTitle("VisionAI PCIe 测试工具");
    this->resize(1024, 768);
    this->setStyleSheet("QMainWindow { background-color: #1e1e2e; } QPushButton { background-color: #89b4fa; color: #11111b; border: none; border-radius: 6px; padding: 12px 20px; font-weight: bold; font-size: 14px;} QPushButton:hover { background-color: #b4befe; } QPushButton:pressed { background-color: #74c7ec; } QLabel#DisplayLabel { background-color: #181825; color: #a6adc8; border: 2px dashed #45475a; border-radius: 10px; font-size: 18px;} QLabel#StatusLabel { color: #a6adc8; font-size: 13px; padding: 5px;}");

    displayLabel = new QLabel("点击下方按钮选择输入源");
    displayLabel->setObjectName("DisplayLabel");
    displayLabel->setAlignment(Qt::AlignCenter);
    displayLabel->setMinimumSize(800, 500);

    statusLabel = new QLabel("状态: 就绪");
    statusLabel->setObjectName("StatusLabel");

    btnCamera = new QPushButton("打开摄像头");
    btnImage = new QPushButton("打开图片");
    btnVideo = new QPushButton("打开本地视频");
    btnPCIe = new QPushButton("打开 PCIe 裸流");

    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    btnLayout->addWidget(btnCamera);
    btnLayout->addWidget(btnImage);
    btnLayout->addWidget(btnVideo);
    btnLayout->addWidget(btnPCIe);
    btnLayout->addStretch();

    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->setContentsMargins(20, 20, 20, 10);
    mainLayout->addWidget(displayLabel, 1);
    mainLayout->addLayout(btnLayout);
    mainLayout->addWidget(statusLabel);

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

void MainWindow::updateFrame(QImage image) { displayLabel->setPixmap(QPixmap::fromImage(image).scaled(displayLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); }
void MainWindow::updateStatus(QString msg) { statusLabel->setText("状态: " + msg); }