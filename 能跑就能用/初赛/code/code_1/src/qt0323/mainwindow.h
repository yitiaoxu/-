#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QThread>
#include <QImage>
#include <QFileDialog>
#include <QDateTime>
#include <QDir>
#include <QCloseEvent>

#include <opencv2/opencv.hpp>
#include "task/yolov11_custom.h"
#include "task/yolov11_thread_pool.h"
#include "lprnet.h"
#include "plate_tracker.h"

enum InputType {
    INPUT_CAMERA,
    INPUT_IMAGE,
    INPUT_VIDEO,
    INPUT_PCIE  
};

// ---------------------------------------------------------
// 推理线程：负责 YOLOv11 车牌检测 -> OpenCV 颜色识别 -> LPRNet 字符识别
// ---------------------------------------------------------
class InferenceThread : public QThread {
    Q_OBJECT
public:
    explicit InferenceThread(QObject *parent = nullptr);
    ~InferenceThread();

    void setInputCamera(int cam_index);
    void setInputImage(const QString& path);
    void setInputVideo(const QString& path);
    void setInputPCIe(); 
    void stop();

signals:
    void frameReady(QImage image);
    void showMessage(QString msg);

protected:
    void run() override;

private:
    bool keep_running;
    InputType current_input_type;
    int camera_index;
    QString file_path;

    Yolov11Custom* yolo11_detector;
    Yolov11ThreadPool* yolo11_thread_pool;
    rknn_app_context_t lprnet_ctx;   
    bool lprnet_ready;               
    PlateTracker plate_tracker_;

    // 处理流程
    void processSingleImage();
    void processStream(); 
    void processPCIeStream(); 
    
    void ensureOutputDirectory();
    QImage matToQImage(const cv::Mat& mat);
    
    QString detectPlateColor(const cv::Mat& plate_img);
    QString recognizePlateText(const cv::Mat& plate_img);
    void drawChineseTextAndBox(cv::Mat& img, const cv::Rect& box, const QString& text, const cv::Scalar& color);
};

// ---------------------------------------------------------
// 主界面
// ---------------------------------------------------------
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openCamera();
    void openImage();
    void openLocalVideo();
    void openPCIe(); // 打开 PCIe 的槽函数
    void stopPCIe();
    void updateFrame(QImage image);
    void updateStatus(QString msg);

private:
    void setupUI();

    QLabel *displayLabel;
    QLabel *statusLabel;
    QPushButton *btnCamera;
    QPushButton *btnImage;
    QPushButton *btnVideo;
    QPushButton *btnPCIe;
    QPushButton *btnStopPCIe;

    InferenceThread *inferenceThread;
};

#endif // MAINWINDOW_H