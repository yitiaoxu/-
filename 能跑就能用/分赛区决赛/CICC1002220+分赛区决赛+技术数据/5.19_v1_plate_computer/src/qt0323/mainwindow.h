#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QSlider>
#include <QVector>
#include <QThread>
#include <QImage>
#include <QPixmap>
#include <QPainter>
#include <QPaintEvent>
#include <QFileDialog>
#include <QDateTime>
#include <QDir>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QColor>
#include <QSizePolicy>

#include <opencv2/opencv.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

#include "plate/plate_rknn_pipeline.h"
#include "pcie_dma_read_test.h"

/** Self-painting viewport: always clears full area before drawing (no QLabel pixmap ghost). */
class FrameView : public QWidget {
public:
    explicit FrameView(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("DisplayLabel");
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setMinimumSize(640, 420);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        placeholder_ = QString::fromUtf8("\u8bf7\u5728\u5de6\u4fa7\u9009\u62e9\u8f93\u5165\u6e90");
    }

    void clearToPlaceholder() {
        image_ = QImage();
        placeholder_ = QString::fromUtf8("\u8bf7\u5728\u5de6\u4fa7\u9009\u62e9\u8f93\u5165\u6e90");
        update();
        repaint();
    }

    void setPlaceholder(const QString& text) {
        image_ = QImage();
        placeholder_ = text;
        update();
        repaint();
    }

    void setFrameImage(QImage image) {
        image_ = std::move(image);
        placeholder_.clear();
        update();
        repaint();  // force immediate paint on embedded boards
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0xFA, 0xFB, 0xFC));
        if (!image_.isNull()) {
            const QImage scaled =
                image_.scaled(size(), Qt::KeepAspectRatio, Qt::FastTransformation);
            const int x = (width() - scaled.width()) / 2;
            const int y = (height() - scaled.height()) / 2;
            p.drawImage(x, y, scaled);
        } else if (!placeholder_.isEmpty()) {
            p.setPen(QColor(0x9C, 0xA3, 0xAF));
            QFont f = font();
            f.setPointSize(14);
            p.setFont(f);
            p.drawText(rect(), Qt::AlignCenter, placeholder_);
        }
    }

private:
    QImage image_;
    QString placeholder_;
};

enum InputType {
    INPUT_PCIE,
    INPUT_MEDIA
};

class InferenceThread : public QThread {
    Q_OBJECT
public:
    explicit InferenceThread(QObject* parent = nullptr);
    ~InferenceThread() override;

    void setInputPath(const QString& path);
    void setInputPCIe();
    void stop();
    void setLabelScale(float scale);
    void resetPlateResultCache();

signals:
    void frameReady(QImage image);
    void showMessage(QString msg);
    void imageProcessTime(QString name, double process_wall_ms, double e2e_ms);
    void batchGalleryReady(QStringList saved_paths);
    void batchProgressChanged(int current, int total);
    void batchProgressEnd();
    void plateResultsUpdated(QStringList plate_numbers);

protected:
    void run() override;

private:
    std::atomic<bool> keep_running;
    std::atomic<bool> stop_requested_;
    InputType current_input_type;
    QString file_path;

    std::unique_ptr<plate_rknn::PlateRknnPipeline> pipeline_;

    int pcie_fd_;
    bool use_fpga_rgb888_;
    bool infer_map_ready_;
    bool profile_enabled_;
    int profile_every_;
    int profile_frame_cnt_;
    bool fpga_roi_preview_;
    int roi_margin_;
    FILE* roi_log_fp_;
    double profile_rgb_ms_;
    std::vector<uint8_t> infer_buf_888_;
    QStringList last_plate_numbers_;

    bool initPipeline();
    void processMediaInput();
    void processFrameBatch(const QFileInfoList& files, const QString& out_root,
                           const QString& source_label);
    void processVideoFile();
    void processPCIeStream();
    bool checkPcieLink(int fd);

    void setupPcieRoiPreview();
    void setupPcieInferPath(int fd);
    cv::Mat captureInferBgrFrame(int fd, const uint8_t* src565, int src_stride_bytes);
    bool readPlateRoi(int fd, PLATE_ROI_INFO& roi) const;
    void processCapturedFrame(const uint8_t* src565, int frame_cnt, int src_stride_bytes,
                              int64 capture_e2e_t0 = 0, double pcie_dma_ms = 0);

    void ensureOutputDirectory();
    QImage matToQImage(const cv::Mat& mat);
    QImage rgb565ToQImage(const uint8_t* data565, int width, int height);
    cv::Mat rgb565BufferToBgr(const uint8_t* src565, int width, int height, int src_stride_bytes);
    void drawRoiOverlay(QImage& image, const PLATE_ROI_INFO& roi) const;
    void emitPreviewWithRoiOverlay(const uint8_t* src565, int width, int height,
                                   int src_stride_bytes, const PLATE_ROI_INFO& roi);
    bool processAndEmit(cv::Mat& frame, const QString& status_hint, const QString& image_name,
                        int64 e2e_t0 = 0, double pcie_dma_ms = 0, double read_ms = 0,
                        int frame_index = -1, bool sync_present = false);
    void printFrameProfileIfEnabled(const QString& tag, int frame_index,
                                    const plate_rknn::PlateTimings& pipe,
                                    double pcie_dma_ms, double rgb_ms, double read_ms,
                                    double qimage_ms, double e2e_ms);
    void emitPlateResultsIfChanged(const QStringList& plate_numbers);
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void openFolder();
    void openMediaFile();
    void openPCIe();
    void stopPCIe();
    void initializeUi();
    void updateFrame(QImage image);
    void updateStatus(QString msg);
    void onBatchGalleryReady(QStringList saved_paths);
    void onBatchProgressChanged(int current, int total);
    void onBatchProgressEnd();
    void showGalleryPrev();
    void showGalleryNext();
    void setBatchUiBusy(bool busy);
    void onLabelScaleChanged(int value);
    void onImageProcessTime(QString name, double process_wall_ms, double e2e_ms);
    void onPlateResultsUpdated(QStringList plate_numbers);
    void flushPendingFrame();
    void presentFrameNow(QImage image);

private:
    void setupUI();
    QString defaultDialogDir() const;
    void stopInferenceIfRunning();
    void startMediaProcessing(const QString& path);
    void setGalleryResults(const QStringList& paths);
    void showGalleryAt(int index);
    void updateGalleryNavUi();
    void setRunState(const QString& text, const char* state);
    void updatePlateResultDisplay(const QStringList& plate_numbers);
    void loadIndustrialStyleSheet();
    void startPCIeStream();
    void setPcieStreamingUi(bool streaming);
    bool loadPcieDriver(bool force_reload = false);
    static QString resolveAppRoot();
    static bool isPcieDriverLoaded();
    static bool pcieDeviceAccessible();

    FrameView* displayLabel;
    QLabel* statusLabel;
    QLabel* labelGalleryPage;
    QLabel* labelTitle;
    QLabel* labelSubtitle;
    QLabel* labelStatusLed;
    QLabel* labelTopStatus;
    QLabel* labelMetricValue;
    QLabel* labelProcessTime;
    QLabel* labelE2eTime;
    QLabel* labelScaleValue;
    QLabel* labelPlateNumber;
    QLabel* labelPlateCount;
    QSlider* sliderLabelScale;
    QProgressBar* progressBar;
    QPushButton* btnFolder;
    QPushButton* btnMedia;
    QPushButton* btnPCIe;
    QPushButton* btnStopPCIe;
    QPushButton* btnInit;
    QPushButton* btnPrev;
    QPushButton* btnNext;

    QStringList galleryPaths_;
    QVector<double> galleryProcessMs_;
    int galleryIndex_;
    bool collectProcessTimes_;
    bool auto_pcie_started_;
    bool galleryBrowseMode_;
    bool frameFlushScheduled_;
    QImage pendingFrame_;
    QStringList lastDisplayedPlates_;

    InferenceThread* inferenceThread;
};

#endif
