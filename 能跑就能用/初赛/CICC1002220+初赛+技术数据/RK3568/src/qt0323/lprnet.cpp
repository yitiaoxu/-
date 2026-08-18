#include "lprnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>

// 车牌字典
const std::vector<std::string> plate_code{
    "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
    "苏", "浙", "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
    "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
    "新",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
    "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "I", "O", "-"};

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

int init_lprnet_model(const char *model_path, rknn_app_context_t *app_ctx) {
    int ret, model_len = 0;
    unsigned char *model_data = load_model_data(model_path, &model_len);
    if (model_data == NULL) return -1;

    rknn_context ctx = 0;
    ret = rknn_init(&ctx, model_data, model_len, 0, NULL);
    free(model_data);
    if (ret < 0) return -1;

    rknn_sdk_version version;
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret == RKNN_SUCC) {
        std::cout << "[LPRNet] RKNN API version: " << version.api_version << std::endl;
        std::cout << "[LPRNet] RKNN Driver version: " << version.drv_version << std::endl;
    } else {
        std::cout << "[LPRNet] RKNN_QUERY_SDK_VERSION failed, ret=" << ret << std::endl;
    }

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
int inference_lprnet_model(rknn_app_context_t *app_ctx, const cv::Mat& src_img, lprnet_result *out_result) {
    int ret;
    rknn_input inputs[1];
    rknn_output outputs[1];
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf = (void*)src_img.data;

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
    const int   BLANK_IDX = OUT_ROWS - 1;            // 67
    float *raw_out = (float *)outputs[0].buf;        // 布局: [OUT_ROWS][OUT_COLS] row-major

    // 类别区间:plate_code 索引 0~30 省份汉字, 31~40 数字, 41~66 字母(含 I/O), 67 blank
    auto is_province = [](int idx) { return idx >= 0  && idx <= 30; };
    const int IDX_D = 44;
    const int IDX_F = 46;

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

    // ---- 第三步: 估计车牌长度(7 位 / 8 位新能源) ----
    int plate_len = 7;
    int ctc_start = -1;
    for (int i = 0; i < (int)ctc_decoded.size(); i++) {
        if (is_province(ctc_decoded[i])) { ctc_start = i; break; }
    }
    if (ctc_start >= 0) {
        int avail = (int)ctc_decoded.size() - ctc_start;
        if (avail >= 3 && (ctc_decoded[ctc_start + 2] == IDX_D || ctc_decoded[ctc_start + 2] == IDX_F)) {
            plate_len = 8; // 小型新能源:粤A·D12345
        } else if (avail >= 8 && (ctc_decoded[ctc_start + 7] == IDX_D || ctc_decoded[ctc_start + 7] == IDX_F)) {
            plate_len = 8; // 大型新能源:川A·12345D
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

    // ---- 第五步: 等分 plate_len 段,每段在合法类别集合内取最大概率 ----
    //   段 0     : 仅允许省份汉字 (0..30)   —— 保证只有 1 个汉字
    //   段 1     : 仅允许字母 (41..66)
    //   段 2~N-1 : 仅允许数字或字母 (31..66)
    //   每段独立挑选,即使相邻两段都是 9,也都会各自输出 9 —— 修复连号问题
    std::vector<int> final_label;
    final_label.reserve(plate_len);

    double seg_w = (double)(seg_end - seg_start) / plate_len;
    for (int s = 0; s < plate_len; s++) {
        int s_begin = seg_start + (int)(s * seg_w);
        int s_end   = (s == plate_len - 1) ? seg_end
                                           : seg_start + (int)((s + 1) * seg_w);
        if (s_end <= s_begin) s_end = s_begin + 1;
        if (s_end > OUT_COLS)   s_end = OUT_COLS;
        if (s_begin >= OUT_COLS) s_begin = OUT_COLS - 1;

        int valid_lo, valid_hi;
        if (s == 0)      { valid_lo = 0;  valid_hi = 30; }   // 省份汉字
        else if (s == 1) { valid_lo = 41; valid_hi = 66; }   // 字母
        else             { valid_lo = 31; valid_hi = 66; }   // 数字或字母

        float best = -1e30f;
        int   best_idx = (s == 0) ? 17 : (s == 1 ? 41 : 31); // 兜底: 鄂 / A / 0
        for (int x = s_begin; x < s_end; x++) {
            for (int y = valid_lo; y <= valid_hi; y++) {
                float v = raw_out[y * OUT_COLS + x];
                if (v > best) { best = v; best_idx = y; }
            }
        }
        final_label.push_back(best_idx);
    }

    // ---- 第六步: 拼接输出 ----
    out_result->plate_name.clear();
    for (int hh : final_label) {
        out_result->plate_name += plate_code[hh];
    }

    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
    return 0;
}

