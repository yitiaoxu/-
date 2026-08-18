#include "mpp_encoder.h"
#include <stdio.h>
#include <cstring>
#include "im2d.h"
#include "rga.h"

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
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420P);

    // =========================================================
    // 【核心修改区：码率控制 (Bitrate Control)】
    // 将系数从 0.1 提升到了 0.15，确保 2400x1080 高清分辨率下无马赛克
    // 2400 * 1080 * 25 * 0.15 ≈ 9,720,000 bps (约 9.7 Mbps)
    // =========================================================
    int bps = width_ * height_ * fps_ * 0.15;                    
    
    // 使用 VBR (动态码率) 替代 CBR (固定码率)，在画面静止时省带宽，画面剧烈运动时分配更多带宽
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);  
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    // VBR 模式下，允许码率瞬间飙升到目标码率的 1.5 倍（约 14.5 Mbps）以应对复杂画面
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 3 / 2);
    // 画面静止时，允许码率最低降到目标码率的 1/4，节省内网网络带宽
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps / 4);

    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);     
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_);   
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);   
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);    
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps_);  
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);  

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    
    // 调低 QP 最小值，进一步放宽压缩质量限制，让画面更清晰 (数值越小越清晰)
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 26);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 40);  
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 16);  
    mpp_enc_cfg_set_s32(cfg, "rc:qp_step", 4);
    
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);  
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);     
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
    
    // 关键帧间隔设为帧率的2倍(2秒一个I帧)，可有效降低推流延迟和带宽
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps_ * 2);  

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

    int fd = mpp_buffer_get_fd(buffer);

    rga_buffer_t src = wrapbuffer_virtualaddr((void*)bgr_frame.data, width_, height_, RK_FORMAT_BGR_888);
    rga_buffer_t dst = wrapbuffer_fd(fd, width_, height_, RK_FORMAT_YCbCr_420_SP, align_width_, align_height_);
    
    // 调用 RGA 硬件引擎干活
    IM_STATUS STATUS = imcvt(src, dst);
    if (STATUS != IM_STATUS_SUCCESS) {
        printf("RGA 颜色转换失败!\n");
        mpp_buffer_put(buffer);
        return;
    }

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