#include "mpp_encoder.h"
#include <stdio.h>
#include <cstring>
#include <opencv2/opencv.hpp>

inline int ALIGN_MPP_SIZE(int size) { return (size + 15) & ~15; }

MPPEncoder::MPPEncoder()
    : mpp_ctx_(nullptr), mpp_mpi_(nullptr), mpp_buf_grp_(nullptr), mpp_frame_(nullptr), width_(0), height_(0), fps_(0) {}

MPPEncoder::~MPPEncoder() { deinit(); }

void MPPEncoder::setEncodeCallback(EncodeFrameCallback callback) { encode_callback_ = std::move(callback); }

void MPPEncoder::configureEncoder() {
    MPP_RET ret;
    MppEncCfg cfg;
    mpp_enc_cfg_init(&cfg);
    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_CFG, cfg);

    mpp_enc_cfg_set_s32(cfg, "prep:width", width_);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height_);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", align_width_);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", align_height_);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420P); // 注意这里配的是 I420 格式

    int bps = width_ * height_ * fps_ * 0.1;                    
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);  
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 3 / 2);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps / 2);

    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);     
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_);   
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);   
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);    
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps_);  
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);  

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 26);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 36);  
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 20);  
    mpp_enc_cfg_set_s32(cfg, "rc:qp_step", 4);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);  
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);     
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps_);  

    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_SET_CFG, cfg);
    if (ret != MPP_OK) printf("encoder config set failed! ret: %d\n", ret);
    
    mpp_mpi_->reset(mpp_ctx_);  
    mpp_enc_cfg_deinit(cfg);
}

void MPPEncoder::init(int width, int height, int fps) {
    width_ = width; height_ = height; fps_ = fps;
    align_width_ = ALIGN_MPP_SIZE(width);
    align_height_ = ALIGN_MPP_SIZE(height);
    
    mpp_create(&mpp_ctx_, &mpp_mpi_);
    mpp_init(mpp_ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    configureEncoder();
    mpp_buffer_group_get_internal(&mpp_buf_grp_, MPP_BUFFER_TYPE_DRM);
    mpp_frame_init(&mpp_frame_);
}

void MPPEncoder::deinit() {
    if (mpp_frame_) mpp_frame_deinit(&mpp_frame_);
    if (mpp_mpi_) mpp_mpi_->reset(mpp_ctx_);
    if (mpp_ctx_) mpp_destroy(mpp_ctx_);
    if (mpp_buf_grp_) mpp_buffer_group_put(mpp_buf_grp_);
    mpp_ctx_ = nullptr; mpp_mpi_ = nullptr; mpp_buf_grp_ = nullptr;
}

void MPPEncoder::encodeFrame(const cv::Mat& bgr_frame, int frame_count) {
    if (bgr_frame.empty() || !mpp_ctx_) return;

    MppBuffer buffer;
    size_t align_size = align_width_ * align_height_;
    
    MPP_RET ret = mpp_buffer_get(mpp_buf_grp_, &buffer, align_size + align_size / 2);
    if (ret != MPP_OK) return;

    // 【核心修改：放弃挑食的 RGA，使用 CPU 极速转换并安全拷贝】
    cv::Mat i420_frame;
    cv::cvtColor(bgr_frame, i420_frame, cv::COLOR_BGR2YUV_I420);

    uint8_t* dst_ptr = (uint8_t*)mpp_buffer_get_ptr(buffer);
    uint8_t* src_ptr = i420_frame.data;

    // 安全的按行拷贝：完美解决 MPP 硬件要求的 16 字节边界对齐问题
    uint8_t* dst_y = dst_ptr;
    uint8_t* src_y = src_ptr;
    for (int i = 0; i < height_; i++) {
        memcpy(dst_y + i * align_width_, src_y + i * width_, width_);
    }

    int uv_width = width_ / 2;
    int uv_height = height_ / 2;
    int align_uv_width = align_width_ / 2;
    int align_uv_height = align_height_ / 2;

    uint8_t* dst_u = dst_ptr + align_width_ * align_height_;
    uint8_t* src_u = src_ptr + width_ * height_;
    for (int i = 0; i < uv_height; i++) {
        memcpy(dst_u + i * align_uv_width, src_u + i * uv_width, uv_width);
    }

    uint8_t* dst_v = dst_ptr + align_width_ * align_height_ + align_uv_width * align_uv_height;
    uint8_t* src_v = src_ptr + width_ * height_ + uv_width * uv_height;
    for (int i = 0; i < uv_height; i++) {
        memcpy(dst_v + i * align_uv_width, src_v + i * uv_width, uv_width);
    }

    // 将拷贝好的对齐数据送给硬件编码器
    mpp_frame_set_buffer(mpp_frame_, buffer);
    mpp_frame_set_width(mpp_frame_, width_);
    mpp_frame_set_height(mpp_frame_, height_);
    mpp_frame_set_hor_stride(mpp_frame_, align_width_);
    mpp_frame_set_ver_stride(mpp_frame_, align_height_);
    mpp_frame_set_fmt(mpp_frame_, MPP_FMT_YUV420P);
    
    int64_t pts_counter = (int64_t)frame_count * (90000 / fps_);
    mpp_frame_set_pts(mpp_frame_, pts_counter);
    mpp_frame_set_eos(mpp_frame_, 0);

    ret = mpp_mpi_->encode_put_frame(mpp_ctx_, mpp_frame_);
    if (ret != MPP_OK) {
        mpp_buffer_put(buffer);
        return;
    }

    MppPacket pkt;
    mpp_packet_init(&pkt, nullptr, 0);
    ret = mpp_mpi_->encode_get_packet(mpp_ctx_, &pkt);
    if (ret == MPP_OK && pkt != nullptr) {
        unsigned char* data = (unsigned char*)mpp_packet_get_data(pkt);
        size_t size = mpp_packet_get_length(pkt);
        MppMeta meta = mpp_packet_get_meta(pkt);
        RK_S32 is_intra = 0;
        mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra);
        
        if (encode_callback_ && size > 0) {
            encode_callback_(data, size, frame_count, is_intra > 0);
        }
    }
    
    if (pkt) mpp_packet_deinit(&pkt);
    mpp_buffer_put(buffer);
}
