#ifndef PLATE_RKNN_PIPELINE_H
#define PLATE_RKNN_PIPELINE_H

#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace plate_rknn {

/** Per-stage timings (ms). Filled when PlateRknnConfig::profile is true. */
struct PlateTimings {
    double letterbox = 0;
    double det_input = 0;
    double det_npu = 0;
    double det_output = 0;
    double det_decode = 0;
    double det_post = 0;
    double detect_total = 0; /**< pickDetectInput end-to-end */
    double rec_roi = 0;
    double rec_npu = 0;
    double rec_total = 0;
    double draw = 0;
    double infer_wall = 0;   /**< letterbox + detect_total + rec_total (no draw, no IO) */
    double process_wall = 0; /**< letterbox + detect + rec + draw (no IO) */
};

/** Host-side IO timings (ms), outside plate_rknn pipeline. */
struct HostFrameTimings {
    double pcie_dma = 0;
    double rgb_convert = 0;
    double qimage = 0;
    double read_imread = 0;
    double write_imwrite = 0;
    double e2e_total = 0;
};

/** Full per-image timing report (includes read/write when provided). */
void printPlateTimingReport(const std::string& prefix, const PlateTimings& t, double read_ms,
                            double write_ms, double total_ms);

/** Unified terminal report: host IO + pipeline stages. */
void printFrameLatencyReport(const std::string& prefix, const PlateTimings& pipe,
                             const HostFrameTimings& host, int frame_index = -1);

struct PlateRknnConfig {
    int img_size = 640;
    float conf = 0.3f;
    float iou = 0.5f;
    std::string input_mode = "uint8_nhwc";
    std::string rec_input_mode = "auto";
    bool debug = false;
    bool profile = false; /**< print [latency] per-stage ms (Qt side throttles) */
    /** Print plate=/[OK] at most every N process() calls; 0=every frame. Default 60. */
    int result_log_every = 60;
    int topk = 300;
    float left_pad = 0.f;
    float viz_margin = 0.05f;
    float label_scale = 0.52f;
    float rec_left_pad = 0.f;
    std::string font;
    int core_mask = 0;
    /** Temporal EMA on viz box (PCIe stream); does not affect detect/rec ROI. */
    bool viz_temporal_smooth = true;
    float viz_smooth_alpha = 0.7f; /**< new-frame weight 0..1; higher = faster follow */
    int viz_hold_frames = 0;        /**< 0=无检测立即消框；>0 仅 PCIe 丢检时短暂保留 */
    bool viz_stable_rect = true;    /**< draw axis-aligned rect instead of jittery quad */
    bool viz_draw_labels = true;    /**< true=box + plate_no above box (no color text) */
};

struct PlateItem {
    std::vector<float> landmarks;
    std::string plate_no;
    std::string plate_color;
};

void printCnRenderStatus(const std::string& user_font);

/** plate_detect_fp + plate_rec_fp pipeline (same logic as test_rknn_infer/rknn_infer_one). */
class PlateRknnPipeline {
public:
    PlateRknnPipeline();
    ~PlateRknnPipeline();

    int init(const std::string& detect_rknn, const std::string& rec_rknn,
             const PlateRknnConfig& cfg = PlateRknnConfig());

    bool ready() const { return ready_; }
    const PlateRknnConfig& config() const { return cfg_; }
    void setLabelScale(float scale);

    /** Clear temporal viz smoother (e.g. batch still images). */
    void resetVizSmoothState();

    /** Enable/disable cross-frame box smoothing (PCIe / video). */
    void setTemporalVizSmooth(bool on) { cfg_.viz_temporal_smooth = on; }
    void setVizSmoothAlpha(float alpha);
    void setVizHoldFrames(int frames);

    /**
     * Redraw current smoothed boxes onto img (video skip frames).
     * Does not age-out tracks. Returns true if anything was drawn.
     */
    bool drawVizOverlay(cv::Mat& img_bgr);

    /** @return 0 ok, 2 no valid plates, 1 error */
    int process(const cv::Mat& img_bgr, cv::Mat& out_bgr, std::vector<PlateItem>& items,
                const std::string& log_tag = "");

    /** Last frame timings when config().profile was true; otherwise all zeros. */
    const PlateTimings& lastTimings() const { return last_timings_; }

private:
    struct Impl;
    Impl* impl_;
    PlateRknnConfig cfg_;
    bool ready_;
    PlateTimings last_timings_;
};

} // namespace plate_rknn

#endif
