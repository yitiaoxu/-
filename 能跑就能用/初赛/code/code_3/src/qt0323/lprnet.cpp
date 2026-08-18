#include "lprnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <unordered_map>
#include <cmath>

// 车牌字典
const std::vector<std::string> plate_code{
    "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
    "苏", "浙", "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
    "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
    "新",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
    "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "I", "O", "领", "使", "-"};

// 替代官方的 file_utils,用标准 C 库自己写个 10 行的读取函数
static unsigned char* load_model_data(const char* filename, int* model_size) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char* data = (unsigned char*)malloc(size);
    if (data) fread(data, 1, size, fp);
    fclose(fp);
    *model_size = size;
    return data;
}

// HSV 粗判黄牌: 黄色背景占比达到阈值认为是黄牌
static bool is_yellow_plate(const cv::Mat& bgr_img) {
    if (bgr_img.empty() || bgr_img.channels() != 3) return false;
    cv::Mat hsv, mask;
    cv::cvtColor(bgr_img, hsv, cv::COLOR_BGR2HSV);
    // 黄色范围(经验值): H[15,40], S/V 不能太低
    cv::inRange(hsv, cv::Scalar(15, 60, 60), cv::Scalar(40, 255, 255), mask);
    float ratio = (float)cv::countNonZero(mask) / (float)(mask.rows * mask.cols);
    return ratio > 0.22f;
}

// HSV 粗判黑牌: 低亮度+低饱和占比达到阈值认为是黑牌
static bool is_black_plate(const cv::Mat& bgr_img) {
    if (bgr_img.empty() || bgr_img.channels() != 3) return false;
    cv::Mat hsv, mask;
    cv::cvtColor(bgr_img, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(0, 0, 0), cv::Scalar(180, 80, 70), mask);
    float ratio = (float)cv::countNonZero(mask) / (float)(mask.rows * mask.cols);
    return ratio > 0.25f;
}

// 双层黄牌常见宽高比显著小于单层牌
static bool is_double_layer_plate_shape(const cv::Mat& img) {
    if (img.empty() || img.rows <= 0) return false;
    float wh = (float)img.cols / (float)img.rows;
    return wh < 2.8f;
}

// 双层牌展开: 上排字符放左半,下排字符放右半,输出尺寸与输入一致
static cv::Mat unfold_double_layer_plate(const cv::Mat& src_img) {
    int h = src_img.rows;
    int w = src_img.cols;
    if (h < 8 || w < 16) return src_img.clone();

    cv::Mat gray, bin_inv;
    cv::cvtColor(src_img, gray, cv::COLOR_BGR2GRAY);
    cv::threshold(gray, bin_inv, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);

    int y0 = h / 4, y1 = (h * 3) / 4;
    int best_y = h / 2;
    int best_score = 1e9;
    for (int y = y0; y <= y1; ++y) {
        int score = cv::countNonZero(bin_inv.row(y));
        if (score < best_score) {
            best_score = score;
            best_y = y;
        }
    }

    // 避免切分线过于靠近边界
    best_y = std::max(2, std::min(h - 2, best_y));

    cv::Rect top_roi(0, 0, w, best_y);
    cv::Rect bot_roi(0, best_y, w, h - best_y);
    cv::Mat top = src_img(top_roi);
    cv::Mat bot = src_img(bot_roi);

    cv::Mat top_rs, bot_rs;
    cv::resize(top, top_rs, cv::Size(w / 2, h), 0, 0, cv::INTER_LINEAR);
    cv::resize(bot, bot_rs, cv::Size(w - w / 2, h), 0, 0, cv::INTER_LINEAR);

    cv::Mat merged(h, w, src_img.type());
    top_rs.copyTo(merged(cv::Rect(0, 0, top_rs.cols, h)));
    bot_rs.copyTo(merged(cv::Rect(top_rs.cols, 0, bot_rs.cols, h)));
    return merged;
}

int init_lprnet_model(const char *model_path, rknn_app_context_t *app_ctx) {
    int ret, model_len = 0;
    unsigned char *model_data = load_model_data(model_path, &model_len);
    if (model_data == NULL) return -1;

    rknn_context ctx = 0;
    ret = rknn_init(&ctx, model_data, model_len, 0, NULL);
    free(model_data);
    if (ret < 0) return -1;

    rknn_input_output_num io_num;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        std::cout << "[LPRNet][Input " << i << "]"
                  << " name=" << input_attrs[i].name
                  << " dims=[" << input_attrs[i].dims[0] << ","
                  << input_attrs[i].dims[1] << ","
                  << input_attrs[i].dims[2] << ","
                  << input_attrs[i].dims[3] << "]"
                  << " n_elems=" << input_attrs[i].n_elems
                  << " size=" << input_attrs[i].size
                  << " type=" << input_attrs[i].type
                  << " fmt=" << input_attrs[i].fmt
                  << std::endl;
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        std::cout << "[LPRNet][Output " << i << "]"
                  << " name=" << output_attrs[i].name
                  << " dims=[" << output_attrs[i].dims[0] << ","
                  << output_attrs[i].dims[1] << ","
                  << output_attrs[i].dims[2] << ","
                  << output_attrs[i].dims[3] << "]"
                  << " n_elems=" << output_attrs[i].n_elems
                  << " size=" << output_attrs[i].size
                  << " type=" << output_attrs[i].type
                  << " fmt=" << output_attrs[i].fmt
                  << std::endl;
    }

    app_ctx->rknn_ctx = ctx;
    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    } else {
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    return 0;
}

int release_lprnet_model(rknn_app_context_t *app_ctx) {
    if (app_ctx->input_attrs) free(app_ctx->input_attrs);
    if (app_ctx->output_attrs) free(app_ctx->output_attrs);
    if (app_ctx->rknn_ctx != 0) rknn_destroy(app_ctx->rknn_ctx);
    return 0;
}

// 极简版推理逻辑:直接塞入 cv::Mat
int inference_lprnet_model(rknn_app_context_t *app_ctx, const cv::Mat& src_img, lprnet_result *out_result,
                           bool force_black_mode, bool prefer_new_energy_mode) {
    int ret;
    rknn_input inputs[1];
    rknn_output outputs[1];
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    // 黄牌专用预处理:
    // 1) 先判断是否黄牌
    // 2) 双层黄牌先展开成单行布局,再送入 LPRNet
    bool is_yellow = is_yellow_plate(src_img);
    bool is_black = force_black_mode || is_black_plate(src_img);
    bool is_double_yellow = is_yellow && is_double_layer_plate_shape(src_img);
    cv::Mat infer_img = is_double_yellow ? unfold_double_layer_plate(src_img) : src_img;

    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf = (void*)infer_img.data;

    ret = rknn_inputs_set(app_ctx->rknn_ctx, 1, inputs);
    if (ret < 0) return -1;

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) return -1;

    outputs[0].want_float = 1;
    ret = rknn_outputs_get(app_ctx->rknn_ctx, 1, outputs, NULL);
    if (ret < 0) return ret;

    // ============================================================
    //  解码:车牌结构先验 + 等长分段众数解码
    //  - 修复 1: 车牌正文位置(第 2 位起)禁止再输出汉字,杜绝多汉字现象
    //  - 修复 2: 不依赖"合并相同字符"的 CTC 规则,而是按段独立挑选,
    //           连号 9999 每个 9 落在不同段里,不会被合并吞掉
    // ============================================================
    const int   BLANK_IDX = OUT_ROWS - 1;            // 69
    float *raw_out = (float *)outputs[0].buf;        // 布局: [OUT_ROWS][OUT_COLS] row-major

    // 类别区间:
    // 0~30 省份汉字, 31~40 数字, 41~66 字母(含 I/O), 67~68 领/使, 69 blank
    auto is_province = [](int idx) { return idx >= 0  && idx <= 30; };
    const int IDX_D = 44;
    const int IDX_F = 46;
    const int IDX_LING = 67;
    const int IDX_SHI  = 68;

    // ---- 第一步: 每列 argmax,得到逐时序最大类别序列 ----
    int prebs[OUT_COLS];
    for (int x = 0; x < OUT_COLS; x++) {
        float best_v = -1e30f;
        int   best_y = BLANK_IDX;
        for (int y = 0; y < OUT_ROWS; y++) {
            float v = raw_out[y * OUT_COLS + x];
            if (v > best_v) { best_v = v; best_y = y; }
        }
        prebs[x] = best_y;
    }

    // ---- 第二步: 标准 CTC 解码,只用来估计车牌长度 / 起点参考 ----
    std::vector<int> ctc_decoded;
    {
        int pre_c = BLANK_IDX;
        for (int x = 0; x < OUT_COLS; x++) {
            int value = prebs[x];
            if (value != pre_c && value != BLANK_IDX) ctc_decoded.push_back(value);
            pre_c = value;
        }
    }

    // ---- 第三步: 估计车牌长度(默认 7 位 / 新能源 8 位) ----
    int plate_len = 7;
    int ctc_start = -1;
    for (int i = 0; i < (int)ctc_decoded.size(); i++) {
        if (is_province(ctc_decoded[i])) { ctc_start = i; break; }
    }
    if (!is_yellow && !is_black) {
        if (prefer_new_energy_mode) {
            // 上游颜色已稳定判绿时，优先按新能源 8 位切分，避免模糊场景 D/F 漏检导致退化为 7 位。
            plate_len = 8;
        } else if (ctc_start >= 0) {
            int avail = (int)ctc_decoded.size() - ctc_start;
            bool hit_small_new_energy = (avail >= 3 &&
                                         (ctc_decoded[ctc_start + 2] == IDX_D ||
                                          ctc_decoded[ctc_start + 2] == IDX_F));
            bool hit_large_new_energy = (avail >= 8 &&
                                         (ctc_decoded[ctc_start + 7] == IDX_D ||
                                          ctc_decoded[ctc_start + 7] == IDX_F));
            // 蓝牌默认应为 7 位(汉字+6 位)，只在新能源关键位命中 D/F 时才切到 8 位。
            if (hit_small_new_energy || hit_large_new_energy) {
                plate_len = 8;
            }
        }
    }

    // ---- 第四步: 在时序上定位有效区间 [seg_start, seg_end) ----
    //   seg_start: 第一个被预测为"省份汉字"的列(更稳),否则用第一个非 blank 列
    //   seg_end  : 最后一个非 blank 列 + 1
    //   这样能自动剔除 ROI 左右两侧的空白填充,避免分段错位。
    int col_first = -1, col_last = -1, province_col = -1;
    for (int x = 0; x < OUT_COLS; x++) {
        int idx = prebs[x];
        if (idx == BLANK_IDX) continue;
        if (col_first < 0) col_first = x;
        col_last = x;
        if (province_col < 0 && is_province(idx)) province_col = x;
    }

    if (col_last < 0) {
        // 全部是 blank,无法识别
        out_result->plate_name.clear();
        rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
        return 0;
    }

    int seg_start = (province_col >= 0) ? province_col : col_first;
    int seg_end   = col_last + 1;
    if (seg_end - seg_start < plate_len) {
        // 有效区间过短(模型几乎没输出),退化为整段
        seg_start = 0;
        seg_end   = OUT_COLS;
    }

    // ---- 第五步: 分段解码(抑制第 2 位后的分隔点"·"干扰) ----
    //   段 0     : 仅允许省份汉字 (0..30)   —— 保证只有 1 个汉字
    //   段 1     : 仅允许字母 (41..66)
    //   段 2~N-1 : 仅允许数字或字母 (31..66)
    //   每段独立挑选,即使相邻两段都是 9,也都会各自输出 9 —— 修复连号问题
    std::vector<int> final_label;
    final_label.reserve(plate_len);

    // 车牌第 2 位后通常存在分隔点/小圆点,它不属于任何字符类别。
    // 若直接全段等分,分隔点可能挤占第 3 位字符段,导致漏一位或错位。
    // 这里在理论分隔点附近寻找 "blank 概率最高" 的列作为切分点:
    // 左侧解码前 2 位,右侧解码剩余位数。
    int split_col = -1;
    {
        int span = seg_end - seg_start;
        if (!is_yellow && !is_black && span >= plate_len + 2) {
            int guess = seg_start + (int)(span * 0.25);  // 2/8 附近
            int win = std::max(2, span / 10);
            int l = std::max(seg_start + 1, guess - win);
            int r = std::min(seg_end - 1, guess + win);
            float best_blank = -1e30f;
            for (int x = l; x <= r; ++x) {
                float b = raw_out[BLANK_IDX * OUT_COLS + x];
                if (b > best_blank) {
                    best_blank = b;
                    split_col = x;
                }
            }
            if (split_col <= seg_start + 1 || split_col >= seg_end - 1) {
                split_col = -1;
            }
        }
    }

    bool black_numeric_head = false;
    if (is_black) {
        // 黑牌启发式: 若首段“数字置信度”显著高于“省份汉字置信度”，按使馆数字开头模板解码。
        int first_probe_end = seg_start + std::max(2, (seg_end - seg_start) / plate_len);
        first_probe_end = std::min(first_probe_end, seg_end);
        float best_province = -1e30f;
        float best_digit = -1e30f;
        for (int x = seg_start; x < first_probe_end; ++x) {
            for (int y = 0; y <= 30; ++y) {
                float v = raw_out[y * OUT_COLS + x];
                if (v > best_province) best_province = v;
            }
            for (int y = 31; y <= 40; ++y) {
                float v = raw_out[y * OUT_COLS + x];
                if (v > best_digit) best_digit = v;
            }
        }
        black_numeric_head = (best_digit > best_province + 0.10f);
    }

    for (int s = 0; s < plate_len; s++) {
        int s_begin, s_end;
        if (split_col > 0 && plate_len >= 7) {
            if (s < 2) {
                double lw = (double)(split_col - seg_start) / 2.0;
                s_begin = seg_start + (int)(s * lw);
                s_end   = (s == 1) ? split_col : seg_start + (int)((s + 1) * lw);
            } else {
                int right_len = plate_len - 2;
                double rw = (double)(seg_end - split_col) / right_len;
                int rs = s - 2;
                s_begin = split_col + (int)(rs * rw);
                s_end   = (s == plate_len - 1) ? seg_end
                                               : split_col + (int)((rs + 1) * rw);
            }
        } else {
            double seg_w = (double)(seg_end - seg_start) / plate_len;
            s_begin = seg_start + (int)(s * seg_w);
            s_end   = (s == plate_len - 1) ? seg_end
                                           : seg_start + (int)((s + 1) * seg_w);
        }

        if (s_end <= s_begin) s_end = s_begin + 1;
        if (s_end > OUT_COLS)   s_end = OUT_COLS;
        if (s_begin >= OUT_COLS) s_begin = OUT_COLS - 1;

        int valid_lo, valid_hi;
        if (is_black) {
            // 黑牌细分模板:
            // - 使馆: 6位数字 + 使
            // - 领馆: 汉字 + 5位数字 + 领
            if (s == 0) {
                if (black_numeric_head) valid_lo = 31, valid_hi = 40; // 使馆首位数字
                else valid_lo = 0, valid_hi = 30;                     // 领馆首位汉字
            }
            else if (s == plate_len - 1) {
                if (black_numeric_head) valid_lo = IDX_LING, valid_hi = IDX_SHI; // 使馆末位偏向使
                else valid_lo = IDX_LING, valid_hi = IDX_LING;                    // 领馆末位固定领
            }
            else valid_lo = 31, valid_hi = 40;              // 主体数字
        } else {
            if (s == 0)      { valid_lo = 0;  valid_hi = 30; }   // 省份汉字
            else if (s == 1) { valid_lo = 41; valid_hi = 66; }   // 字母
            else             { valid_lo = 31; valid_hi = 66; }   // 数字或字母
        }

        float best = -1e30f;
        int   best_idx;
        if (is_black) {
            if (s == 0) best_idx = black_numeric_head ? 31 : 17; // 首位兜底: 0 / 鄂
            else if (s == plate_len - 1) best_idx = black_numeric_head ? IDX_SHI : IDX_LING; // 使馆优先使
            else best_idx = 31; // 主体数字
        } else {
            best_idx = (s == 0) ? 17 : (s == 1 ? 41 : 31); // 兜底: 鄂 / A / 0
        }
        for (int x = s_begin; x < s_end; x++) {
            for (int y = valid_lo; y <= valid_hi; y++) {
                float v = raw_out[y * OUT_COLS + x];
                if (v > best) { best = v; best_idx = y; }
            }
        }
        if (is_black && s == plate_len - 1 && black_numeric_head) {
            // 黑牌末位若“领/使”置信度极低，回退到数字，减轻末位乱字。
            float best_num = -1e30f;
            int best_num_idx = 31;
            for (int x = s_begin; x < s_end; x++) {
                for (int y = 31; y <= 40; y++) {
                    float v = raw_out[y * OUT_COLS + x];
                    if (v > best_num) { best_num = v; best_num_idx = y; }
                }
            }
            if (best_num > best + 0.15f) best_idx = best_num_idx;
            // 数字开头模板下，若使与领分数接近，轻微偏置“使”
            if (black_numeric_head && best_idx == IDX_LING) {
                float best_shi = -1e30f, best_ling = -1e30f;
                for (int x = s_begin; x < s_end; ++x) {
                    float vs = raw_out[IDX_SHI * OUT_COLS + x];
                    float vl = raw_out[IDX_LING * OUT_COLS + x];
                    if (vs > best_shi) best_shi = vs;
                    if (vl > best_ling) best_ling = vl;
                }
                if (best_shi >= best_ling - 0.05f) best_idx = IDX_SHI;
            }
        }
        final_label.push_back(best_idx);
    }

    if (is_black && plate_len >= 7 && final_label.size() >= 7) {
        // 使馆格式强约束：当前 7 位里前 6 位均为数字时，末位直接置为“使”
        bool first_six_all_digit = true;
        for (int i = 0; i < 6; ++i) {
            int idx = final_label[i];
            if (!(idx >= 31 && idx <= 40)) {
                first_six_all_digit = false;
                break;
            }
        }
        if (first_six_all_digit) {
            final_label[6] = IDX_SHI;
        } else {
            // 领馆格式强约束：首位汉字 + 后5位数字 => 末位固定“领”
            bool consulate_shape = (final_label[0] >= 0 && final_label[0] <= 30);
            for (int i = 1; i <= 5 && consulate_shape; ++i) {
                int idx = final_label[i];
                if (!(idx >= 31 && idx <= 40)) consulate_shape = false;
            }
            if (consulate_shape) final_label[6] = IDX_LING;
        }
    }

    // ---- 第六步: 拼接输出 ----
    out_result->plate_name.clear();
    for (int hh : final_label) {
        out_result->plate_name += plate_code[hh];
    }

    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
    return 0;
}

void LprnetMultiFrameVote::reset() { history.clear(); }

void LprnetMultiFrameVote::set_window_size(int w) {
    if (w < 1) w = 1;
    window_size = w;
    while ((int)history.size() > window_size)
        history.pop_front();
}

std::string LprnetMultiFrameVote::push(const std::string& frame_plate) {
    history.push_back(frame_plate);
    while ((int)history.size() > window_size)
        history.pop_front();

    std::unordered_map<std::string, int> cnt;
    for (const auto& s : history) {
        if (!s.empty()) ++cnt[s];
    }
    int best = 0;
    for (const auto& p : cnt)
        if (p.second > best) best = p.second;
    if (best == 0) return "";

    for (int i = (int)history.size() - 1; i >= 0; --i) {
        const std::string& s = history[i];
        if (s.empty()) continue;
        if (cnt[s] == best) return s;
    }
    return "";
}

