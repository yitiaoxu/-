#include "lprnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// 替代官方的 file_utils，用标准 C 库自己写个 10 行的读取函数
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

    rknn_input_output_num io_num;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
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

// 极简版推理逻辑：直接塞入 cv::Mat
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

    // 第一步：每列取 argmax，得到时序最优类别序列
    int prebs[OUT_COLS];
    for (int x = 0; x < OUT_COLS; x++) {
        float *ptr = (float *)outputs[0].buf;
        float preb[OUT_ROWS];
        for (int y = 0; y < OUT_ROWS; y++) {
            preb[y] = ptr[x];
            ptr += OUT_COLS;
        }
        prebs[x] = std::max_element(preb, preb + OUT_ROWS) - preb;
    }

    // 第二步：标准 CTC 解码（修复连号车牌中间数字被吞问题）
    // 关键：遇到 blank 时也要更新 pre_c，否则 "8 blank 8" 会因 pre_c 仍为 8 而被丢弃成单个 "8"
    const int BLANK_IDX = OUT_ROWS - 1;
    std::vector<int> decoded;
    int pre_c = BLANK_IDX;
    for (int x = 0; x < OUT_COLS; x++) {
        int value = prebs[x];
        if (value != pre_c && value != BLANK_IDX) {
            decoded.push_back(value);
        }
        pre_c = value;
    }

    // 第三步：按国标车牌规则裁剪多余字符（修复 ROI 过大带入额外数字问题）
    // plate_code 索引：0~30 省份汉字；31~40 数字 0-9；41~66 字母（含 I/O）；67 为 "-"（blank）
    // 规则：普通/特种/涉外 7 位；新能源 8 位（小型 D/F 在第 3 位，大型 D/F 在第 8 位）
    auto is_province = [](int idx) { return idx >= 0 && idx <= 30; };
    const int IDX_D = 44; // plate_code 中 "D" 的下标
    const int IDX_F = 46; // plate_code 中 "F" 的下标

    int start = -1;
    for (int i = 0; i < (int)decoded.size(); i++) {
        if (is_province(decoded[i])) { start = i; break; }
    }

    std::vector<int> final_label;
    if (start >= 0) {
        int avail = (int)decoded.size() - start;
        int plate_len = 7;
        if (avail >= 3 && (decoded[start + 2] == IDX_D || decoded[start + 2] == IDX_F)) {
            plate_len = 8; // 小型新能源：粤A·D12345
        } else if (avail >= 8 && (decoded[start + 7] == IDX_D || decoded[start + 7] == IDX_F)) {
            plate_len = 8; // 大型新能源：川A·12345D
        }
        int take = std::min(plate_len, avail);
        for (int i = 0; i < take; i++) final_label.push_back(decoded[start + i]);
    } else {
        final_label = decoded; // 兜底：未找到省份字符则原样输出
    }

    out_result->plate_name.clear();
    for (int hh : final_label) {
        out_result->plate_name += plate_code[hh];
    }

    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
    return 0;
}