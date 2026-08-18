#ifndef _RKNN_DEMO_LPRNET_H_
#define _RKNN_DEMO_LPRNET_H_

#include "rknn_api.h"
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <opencv2/opencv.hpp> // 直接用 OpenCV

// 多帧车牌字符串众数投票（滑动窗口），减轻单帧解码抖动
struct LprnetMultiFrameVote {
    int window_size = 5;
    std::deque<std::string> history;

    void reset();
    void set_window_size(int w);
    // 推入本帧原始识别串，返回当前窗口内出现次数最多的串（并列时取时间上更近的一帧）
    std::string push(const std::string& frame_plate);
};

#define MODEL_HEIGHT 24
#define MODEL_WIDTH 94
#define OUT_ROWS 70
#define OUT_COLS 18

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;
} rknn_app_context_t;

typedef struct {
    std::string plate_name;
} lprnet_result;

// 初始化模型
int init_lprnet_model(const char *model_path, rknn_app_context_t *app_ctx);
// 释放模型
int release_lprnet_model(rknn_app_context_t *app_ctx);
// 极简版推理接口：直接传 OpenCV 的 cv::Mat 进来！
// force_black_mode: 上游已判定黑牌时强制使用黑牌解码约束
// prefer_new_energy_mode: 上游颜色已判绿牌时，优先按新能源 8 位结构解码
int inference_lprnet_model(rknn_app_context_t *app_ctx, const cv::Mat& src_img, lprnet_result *out_result,
                           bool force_black_mode = false, bool prefer_new_energy_mode = false);

#endif //_RKNN_DEMO_LPRNET_H_