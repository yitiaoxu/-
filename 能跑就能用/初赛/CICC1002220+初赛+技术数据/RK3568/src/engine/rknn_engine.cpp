// rknn_engine.h的实现

#include "rknn_engine.h"

#include <string.h>
#include <vector>

#include "utils/engine_helper.h"
#include "utils/logging.h"

static const int g_max_io_num = 10;

// 👇 修改：增加 int core_id 接收外层传来的核心编号 👇
nn_error_e RKEngine::LoadModelFile(const char *model_file, int core_id)
{
    int model_len = 0;
    auto model = load_model(model_file, &model_len);
    if (model == nullptr)
    {
        NN_LOG_ERROR("load model file %s fail!", model_file);
        return NN_LOAD_MODEL_FAIL; 
    }
    int ret = rknn_init(&rknn_ctx_, model, model_len, 0, NULL);
    if (ret < 0)
    {
        NN_LOG_ERROR("rknn_init fail! ret=%d", ret);
        return NN_RKNN_INIT_FAIL; 
    }
    NN_LOG_INFO("rknn_init success!");
    ctx_created_ = true;

    // ==========================================================
    // 🚀🚀🚀 终极绑核大招：在这里直接操纵底层的 rknn_ctx_ 🚀🚀🚀
    // ==========================================================
    rknn_core_mask preferred_mask = RKNN_NPU_CORE_0;
    int normalized_core_id = core_id % 3;
    if (normalized_core_id < 0)
    {
        normalized_core_id += 3;
    }
    if (normalized_core_id == 1)
    {
        preferred_mask = RKNN_NPU_CORE_1;
    }
    else if (normalized_core_id == 2)
    {
        preferred_mask = RKNN_NPU_CORE_2;
    }

    // 部分平台(如 RK3568)不支持特定核心掩码，失败后自动降级，避免初始化误判失败。
    std::vector<rknn_core_mask> fallback_masks = {
        preferred_mask, RKNN_NPU_CORE_0, RKNN_NPU_CORE_AUTO
    };

    bool bind_ok = false;
    int bind_ret = RKNN_SUCC;
    rknn_core_mask applied_mask = RKNN_NPU_CORE_UNDEFINED;
    for (auto mask : fallback_masks)
    {
        if (applied_mask == mask)
        {
            continue;
        }
        applied_mask = mask;
        bind_ret = rknn_set_core_mask(rknn_ctx_, mask);
        if (bind_ret == RKNN_SUCC)
        {
            bind_ok = true;
            break;
        }
    }

    if (bind_ok)
    {
        if (applied_mask == preferred_mask)
        {
            NN_LOG_INFO("NPU 绑核成功，使用请求核心掩码=%d", (int)applied_mask);
        }
        else
        {
            NN_LOG_INFO("NPU 绑核降级成功，requested=%d, applied=%d", (int)preferred_mask, (int)applied_mask);
        }
    }
    else
    {
        // 某些平台/驱动组合不支持显式绑核(常见 ret=-13)，此时由驱动自动调度即可。
        if (bind_ret == -13)
        {
            NN_LOG_INFO("NPU 不支持显式绑核，已使用驱动自动调度。requested=%d, ret=%d", (int)preferred_mask, bind_ret);
        }
        else
        {
            NN_LOG_ERROR("NPU 绑核全部尝试失败！requested=%d, ret=%d", (int)preferred_mask, bind_ret);
        }
    }
    // ==========================================================

    rknn_sdk_version version;
    ret = rknn_query(rknn_ctx_, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        NN_LOG_ERROR("rknn_query fail! ret=%d", ret);
        return NN_RKNN_QUERY_FAIL;
    }
    NN_LOG_INFO("RKNN API version: %s", version.api_version);
    NN_LOG_INFO("RKNN Driver version: %s", version.drv_version);

    rknn_input_output_num io_num;
    ret = rknn_query(rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        NN_LOG_ERROR("rknn_query fail! ret=%d", ret);
        return NN_RKNN_QUERY_FAIL;
    }
    NN_LOG_INFO("model input num: %d, output num: %d", io_num.n_input, io_num.n_output);

    input_num_ = io_num.n_input;
    output_num_ = io_num.n_output;

    NN_LOG_INFO("input tensors:");
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            NN_LOG_ERROR("rknn_query fail! ret=%d", ret);
            return NN_RKNN_QUERY_FAIL;
        }
        print_tensor_attr(&(input_attrs[i]));
        in_shapes_.push_back(rknn_tensor_attr_convert(input_attrs[i]));
    }

    NN_LOG_INFO("output tensors:");
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            NN_LOG_ERROR("rknn_query fail! ret=%d", ret);
            return NN_RKNN_QUERY_FAIL;
        }
        print_tensor_attr(&(output_attrs[i]));
        out_shapes_.push_back(rknn_tensor_attr_convert(output_attrs[i]));
    }

    return NN_SUCCESS;
}

const std::vector<tensor_attr_s> &RKEngine::GetInputShapes()
{
    return in_shapes_;
}

const std::vector<tensor_attr_s> &RKEngine::GetOutputShapes()
{
    return out_shapes_;
}

nn_error_e RKEngine::Run(std::vector<tensor_data_s> &inputs, std::vector<tensor_data_s> &outputs, bool want_float)
{
    if (inputs.size() != input_num_)
    {
        NN_LOG_ERROR("inputs num not match! inputs.size()=%ld, input_num_=%d", inputs.size(), input_num_);
        return NN_IO_NUM_NOT_MATCH;
    }
    if (outputs.size() != output_num_)
    {
        NN_LOG_ERROR("outputs num not match! outputs.size()=%ld, output_num_=%d", outputs.size(), output_num_);
        return NN_IO_NUM_NOT_MATCH;
    }

    rknn_input rknn_inputs[g_max_io_num];
    for (int i = 0; i < inputs.size(); i++)
    {
        rknn_inputs[i] = tensor_data_to_rknn_input(inputs[i]);
    }
    int ret = rknn_inputs_set(rknn_ctx_, (uint32_t)inputs.size(), rknn_inputs);
    if (ret < 0)
    {
        NN_LOG_ERROR("rknn_inputs_set fail! ret=%d", ret);
        return NN_RKNN_INPUT_SET_FAIL;
    }

    ret = rknn_run(rknn_ctx_, nullptr);
    if (ret < 0)
    {
        NN_LOG_ERROR("rknn_run fail! ret=%d", ret);
        return NN_RKNN_RUNTIME_ERROR;
    }

    rknn_output rknn_outputs[g_max_io_num];
    memset(rknn_outputs, 0, sizeof(rknn_outputs));
    for (int i = 0; i < output_num_; ++i)
    {
        rknn_outputs[i].want_float = want_float ? 1 : 0;
    }
    ret = rknn_outputs_get(rknn_ctx_, output_num_, rknn_outputs, NULL);
    if (ret < 0)
    {
        printf("rknn_outputs_get fail! ret=%d\n", ret);
        NN_LOG_ERROR("rknn_outputs_get fail! ret=%d", ret);
        return NN_RKNN_OUTPUT_GET_FAIL;
    }

    for (int i = 0; i < output_num_; ++i)
    {
        // 仅仅拷贝数据，绝对不要在这里 free！
        rknn_output_to_tensor_data(rknn_outputs[i], outputs[i]);
    }
    
    // 🚀 调用官方 API，一口气安全释放 NPU 驱动层分配的所有内存和句柄！
    rknn_outputs_release(rknn_ctx_, output_num_, rknn_outputs);

    return NN_SUCCESS;
}

RKEngine::~RKEngine()
{
    if (ctx_created_)
    {
        rknn_destroy(rknn_ctx_);
        NN_LOG_INFO("rknn context destroyed!");
    }
}

std::shared_ptr<NNEngine> CreateRKNNEngine()
{
    return std::make_shared<RKEngine>();
}