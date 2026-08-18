#include <opencv2/opencv.hpp>
#include <unistd.h>
#include <pthread.h>
#include <fstream>
#include <sstream>
#include <map>
#include <signal.h>
#include <cstring>

// FFmpeg
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// RK_MPI
#include "rk_mpi.h"

// MPP - 仅使用标准头文件
#include "mpp_log.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_task.h"
#include "mpp_meta.h"
#include "mpp_err.h"
#include "rk_mpi_cmd.h"

// RGA
#include "im2d.hpp"
#include "rga.h"
#include "RgaUtils.h"
#include "im2d_type.h"

// DMA
#include "../../dma_alloc/dma_alloc.h"

// YOLO
#include "task/yolov8_custom.h"
#include "task/yolov8_thread_pool.h"
#include "draw/cv_draw.h"
#include "task/border_cross.h"
#include "../streamer/streamer.hpp"

#define MPI_DEC_LOOP_COUNT          4
#define MPI_DEC_STREAM_SIZE         (1024 * 1024)  // 增加到1MB以支持更大的H.264数据包
#define MPI_DEC_MAX_PACKET_SIZE     (2 * 1024 * 1024)  // 最大支持2MB数据包
#define MAX_FILE_NAME_LENGTH        256
static AVFormatContext* format_ctx = nullptr;
static int video_stream_index = -1;
// MPP解码数据结构
typedef struct {
    MppCtx          ctx;
    MppApi          *mpi;
    RK_U32          eos;
    void            *buf;
    MppBufferGroup  frm_grp;
    MppBufferGroup  pkt_grp;        // 数据包缓冲区组
    MppPacket       packet;
    size_t          packet_size;
    int             src_width, src_height;
    int             resize_width, resize_height;
    RK_S32          frame_count;
    RK_S32          frame_num;
    RK_S32          ret;
    RK_S32          max_usage;
    
    // DMA缓冲区
    int             dst_dma_fd;
    void            *dst_buf;
    size_t          dst_buf_size;
    int             resize_dma_fd;
    void            *resize_buf;
    size_t          resize_buf_size;
    
    // RGA相关
    rga_buffer_handle_t dst_handle;
    rga_buffer_handle_t resize_handle;
    rga_buffer_t        dst_img;
    rga_buffer_t        resize_img;
    int                 dst_format;
    
    // 统计信息
    int             total_packets;
    int             large_packets;
    int             failed_packets;
} DECLoopData;

// 错误处理宏
#define CHECK_MPP_RET(ret, msg) \
    if (ret != MPP_OK) { \
        printf("[ERROR] %s failed with ret=%d\n", msg, ret); \
        return -1; \
    }

// 全局变量
static DECLoopData data;
static Yolov8Custom* yolo_model = nullptr;
static Yolov8ThreadPool* thread_pool = nullptr;
static streamer::Streamer* rtmp_streamer = nullptr;
static bool running = true;

// // FFmpeg相关全局变量
// static AVFormatContext* format_ctx = nullptr;
// static int video_stream_index = -1;

// 函数声明
int init_rtsp_client(const char* rtsp_url);

// FFmpeg错误信息转换函数 - 使用线程安全的方式
static const char* get_av_error_string(int errnum) {
    static __thread char error_buffer[AV_ERROR_MAX_STRING_SIZE];
    memset(error_buffer, 0, sizeof(error_buffer));
    av_strerror(errnum, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

// 初始化RTSP客户端
int init_rtsp_client(const char* rtsp_url) {
    int ret = 0;
    AVDictionary* options = nullptr;
    
    printf("[INFO] Initializing RTSP client for URL: %s\n", rtsp_url);
    
    // 初始化FFmpeg网络
    avformat_network_init();
    
    // 分配格式上下文
    format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        printf("[ERROR] Failed to allocate format context\n");
        return -1;
    }
    
    // 设置RTSP选项
    av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP传输
    av_dict_set(&options, "stimeout", "10000000", 0);   // 10秒超时
    av_dict_set(&options, "buffer_size", "4194304", 0); // 4MB缓冲区
    av_dict_set(&options, "max_delay", "1000000", 0);   // 最大延迟1秒
    av_dict_set(&options, "reorder_queue_size", "500", 0); // 重排序队列大小
    av_dict_set(&options, "fflags", "nobuffer", 0);     // 禁用额外缓冲
    
    // 打开输入流
    ret = avformat_open_input(&format_ctx, rtsp_url, nullptr, &options);
    if (ret < 0) {
        printf("[ERROR] Failed to open RTSP stream: %s\n", get_av_error_string(ret));
        av_dict_free(&options);
        avformat_free_context(format_ctx);
        format_ctx = nullptr;
        return -1;
    }
    
    av_dict_free(&options);
    
    // 查找流信息
    ret = avformat_find_stream_info(format_ctx, nullptr);
    if (ret < 0) {
        printf("[ERROR] Failed to find stream info: %s\n", get_av_error_string(ret));
        avformat_close_input(&format_ctx);
        return -1;
    }
    
    // 查找视频流
    video_stream_index = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    
    if (video_stream_index == -1) {
        printf("[ERROR] No video stream found\n");
        avformat_close_input(&format_ctx);
        return -1;
    }
    
    // 获取视频流参数
    AVCodecParameters* codecpar = format_ctx->streams[video_stream_index]->codecpar;
    printf("[INFO] Video stream found: %dx%d, codec: %s\n", 
           codecpar->width, codecpar->height, 
           avcodec_get_name(codecpar->codec_id));
    
    // 更新解码器参数
    if (codecpar->width > 0 && codecpar->height > 0) {
        data.src_width = codecpar->width;
        data.src_height = codecpar->height;
        printf("[INFO] Updated decoder resolution to %dx%d\n", 
               data.src_width, data.src_height);
        
        // 重新计算DMA缓冲区大小
        data.dst_buf_size = data.src_width * data.src_height * 3; // RGB888
        printf("[INFO] Updated DMA buffer size to %zu bytes\n", data.dst_buf_size);
    }
    
    printf("[INFO] RTSP client initialized successfully\n");
    return 0;
}

// 初始化MPP解码器
int init_mpp_decoder() {
    MPP_RET ret = MPP_OK;
    
    memset(&data, 0, sizeof(data));
    
    // 设置解码参数
    data.packet_size = MPI_DEC_STREAM_SIZE;
    data.src_width = 1920;
    data.src_height = 1080;
    data.resize_width = 640;
    data.resize_height = 640;
    data.dst_format = RK_FORMAT_RGB_888;
    
    // 创建MPP上下文
    ret = mpp_create(&data.ctx, &data.mpi);
    CHECK_MPP_RET(ret, "mpp_create");
    
    // 初始化解码器
    ret = mpp_init(data.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    CHECK_MPP_RET(ret, "mpp_init");
    
    // 配置解码器参数
    MppDecCfg cfg = NULL;
    ret = data.mpi->control(data.ctx, MPP_DEC_GET_CFG, &cfg);
    if (ret == MPP_OK && cfg) {
        // 设置解码模式
        ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
        if (ret != MPP_OK) {
            printf("[WARN] Failed to set split_parse: %d\n", ret);
        }
        
        // 应用配置
        ret = data.mpi->control(data.ctx, MPP_DEC_SET_CFG, cfg);
        if (ret != MPP_OK) {
            printf("[WARN] Failed to set decoder config: %d\n", ret);
        } else {
            printf("[INFO] MPP decoder configuration applied\n");
        }
    } else {
        printf("[WARN] Failed to get decoder config: %d\n", ret);
    }
    
    // 创建数据包缓冲区组
    ret = mpp_buffer_group_get_internal(&data.pkt_grp, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK) {
        printf("[WARN] Failed to get internal buffer group, using external: %d\n", ret);
        ret = mpp_buffer_group_get_external(&data.pkt_grp, MPP_BUFFER_TYPE_ION);
        CHECK_MPP_RET(ret, "mpp_buffer_group_get_external");
    }
    printf("[INFO] MPP packet buffer group created\n");
    
    // 设置缓冲区组限制
    ret = mpp_buffer_group_limit_config(data.pkt_grp, 0, MPI_DEC_MAX_PACKET_SIZE);
    if (ret != MPP_OK) {
        printf("[WARN] Failed to set buffer group limit: %d\n", ret);
    }
    
    // 分配数据包缓冲区（作为备用）- 使用安全的内存分配
    if (data.packet_size == 0 || data.packet_size > MPI_DEC_MAX_PACKET_SIZE) {
        printf("[ERROR] Invalid packet size: %zu\n", data.packet_size);
        return -1;
    }
    
    data.buf = calloc(1, data.packet_size);  // 使用calloc初始化为0
    if (!data.buf) {
        printf("[ERROR] Failed to allocate backup packet buffer\n");
        return -1;
    }
    printf("[INFO] Allocated backup packet buffer: %p, size: %zu\n", data.buf, data.packet_size);
    
    // 创建数据包（使用备用缓冲区）
    ret = mpp_packet_init(&data.packet, data.buf, data.packet_size);
    CHECK_MPP_RET(ret, "mpp_packet_init");
    printf("[INFO] MPP packet initialized with backup buffer: %p\n", data.buf);
    
    printf("[INFO] MPP decoder initialized successfully\n");
    return 0;
}

// 初始化DMA缓冲区
int init_dma_buffer() {
    int ret = 0;
    
    // 计算缓冲区大小
    data.dst_buf_size = data.src_width * data.src_height * 3; // RGB888
    data.resize_buf_size = data.resize_width * data.resize_height * 3;
    
    printf("[INFO] Allocating DMA buffers: dst=%zu bytes, resize=%zu bytes\n", 
           data.dst_buf_size, data.resize_buf_size);
    
    // 分配目标缓冲区
    ret = dma_buf_alloc(DMA_HEAP_PATH, data.dst_buf_size, &data.dst_dma_fd, (void **)&data.dst_buf);
    if (ret < 0) {
        printf("[ERROR] Failed to allocate dst dma_buf, ret=%d\n", ret);
        return -1;
    }
    
    // 分配调整大小缓冲区
    ret = dma_buf_alloc(DMA_HEAP_PATH, data.resize_buf_size, &data.resize_dma_fd, (void **)&data.resize_buf);
    if (ret < 0) {
        printf("[ERROR] Failed to allocate resize dma_buf, ret=%d\n", ret);
        // 清理已分配的dst缓冲区
        if (data.dst_dma_fd > 0) {
            dma_buf_free(data.dst_buf_size, &data.dst_dma_fd, data.dst_buf);
        }
        return -1;
    }
    
    printf("[INFO] DMA buffers allocated successfully\n");
    return 0;
}

// 处理解码帧
int process_decoded_frame(MppFrame frame) {
    if (!frame) {
        printf("[ERROR] Frame is null\n");
        return -1;
    }
    
    // 获取帧信息
    RK_U32 width = mpp_frame_get_width(frame);
    RK_U32 height = mpp_frame_get_height(frame);
    RK_U32 h_stride = mpp_frame_get_hor_stride(frame);
    RK_U32 v_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    
    if (!buffer) {
        printf("[ERROR] Frame buffer is null\n");
        return -1;
    }
    
    // 获取帧数据指针和文件描述符
    void* frame_ptr = mpp_buffer_get_ptr(buffer);
    int frame_fd = mpp_buffer_get_fd(buffer);
    
    // 输出帧信息（仅在调试模式下） - 使用全局计数器
    static int frame_debug_count = 0;
    frame_debug_count++;
    if (frame_debug_count % 30 == 0) { // 每30帧输出一次
        printf("[DEBUG] Frame #%d: %dx%d, stride: %dx%d, format: %d, fd: %d\n", 
               data.frame_count, width, height, h_stride, v_stride, fmt, frame_fd);
    }
    
    // TODO: 添加RGA转换和YOLO检测逻辑
    // 1. 使用RGA进行格式转换和缩放
    // 2. 调用YOLO模型进行目标检测
    // 3. 绘制检测结果
    // 4. 推流到RTMP服务器
    
    return 0;
}

// MPP解码循环
int mpp_decode_loop() {
    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    AVPacket* pkt = av_packet_alloc();
    int no_frame_count = 0;
    
    if (!pkt) {
        printf("[ERROR] Failed to allocate AVPacket\n");
        return -1;
    }
    
    printf("[INFO] Starting MPP decode loop...\n");
    
    // 验证关键变量初始化状态 - 增强安全检查
    printf("[DEBUG] Verification - data.buf: %p, data.packet: %p, data.packet_size: %zu\n", 
           data.buf, data.packet, data.packet_size);
    printf("[DEBUG] Verification - format_ctx: %p, video_stream_index: %d\n", 
           format_ctx, video_stream_index);
    
    if (!data.buf || data.packet_size == 0) {
        printf("[ERROR] data.buf is NULL or packet_size is 0 at loop start!\n");
        return -1;
    }
    if (!data.packet) {
        printf("[ERROR] data.packet is NULL at loop start!\n");
        return -1;
    }
    if (!format_ctx || video_stream_index < 0) {
        printf("[ERROR] Invalid format context or video stream index!\n");
        return -1;
    }
    if (!data.ctx || !data.mpi) {
        printf("[ERROR] MPP context or API is NULL!\n");
        return -1;
    }

    while (running) {
        // 从RTSP流读取数据包
        int read_ret = av_read_frame(format_ctx, pkt);
        if (read_ret < 0) {
            if (read_ret == AVERROR_EOF) {
                printf("[INFO] End of stream\n");
                break;
            } else if (read_ret == AVERROR(EAGAIN)) {
                // 暂时无数据可读，短暂等待后继续
                usleep(1000); // 1ms
                continue;
            } else {
                static int consecutive_errors = 0;
                consecutive_errors++;
                
                // 使用安全的错误信息获取
                const char* err_str = get_av_error_string(read_ret);
                
                if (consecutive_errors % 10 == 1) {
                    printf("[ERROR] Failed to read frame from RTSP stream: %s (ret=%d, count=%d)\n", 
                           err_str, read_ret, consecutive_errors);
                }
                
                // 如果连续错误过多，尝试重新连接
                if (consecutive_errors > 100) {
                    printf("[WARN] Too many consecutive errors (%d), attempting reconnection...\n", consecutive_errors);
                    // TODO: 实现重连逻辑
                    consecutive_errors = 0;
                }
                
                usleep(10000); // 10ms，适度延迟避免过度重试
                continue;
            }
        } else {
            // 成功读取，重置错误计数 - 移除不必要的static变量
            // consecutive_errors已在上面的else分支中处理
        }
        
        // 只处理视频流
        if (pkt->stream_index != video_stream_index) {
            av_packet_unref(pkt);
            continue;
        }
        
        // 调试信息：每100个数据包输出一次统计 - 使用全局计数器避免栈问题
        static int packet_count = 0;
        static int keyframe_count = 0;
        packet_count++;
        
        // 检测关键帧
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            keyframe_count++;
        }
        
        if (packet_count % 100 == 0) {
            printf("[DEBUG] Processed %d packets (%d keyframes, %d large, %d failed)\n", 
                   data.total_packets, keyframe_count, data.large_packets, data.failed_packets);
        }
        
        // 将FFmpeg数据包转换为MPP数据包
        if (pkt->size > 0) {
            data.total_packets++;
            MppPacket mpp_pkt = NULL;
            MppBuffer mpp_buf = NULL;
            bool use_buffer_group = false;
            
            // 检查数据包大小并选择处理策略
            if (pkt->size <= data.packet_size) {
                // 使用预分配的缓冲区
                if (data.buf && pkt->data && pkt->size > 0) {
                    // 安全的内存复制，添加边界检查
                    size_t copy_size = (pkt->size <= data.packet_size) ? pkt->size : data.packet_size;
                    memcpy(data.buf, pkt->data, copy_size);
                    mpp_pkt = data.packet;
                    
                    // 重置MPP数据包并设置属性
                    mpp_packet_set_data(mpp_pkt, data.buf);
                    mpp_packet_set_size(mpp_pkt, pkt->size);
                    mpp_packet_set_pos(mpp_pkt, data.buf);
                    mpp_packet_set_length(mpp_pkt, pkt->size);
                } else {
                    printf("[ERROR] Backup buffer is NULL\n");
                    data.failed_packets++;
                    continue;
                }
            } else if (pkt->size <= MPI_DEC_MAX_PACKET_SIZE) {
                // 使用MPP缓冲区组动态分配
                data.large_packets++;
                use_buffer_group = true;
                
                if (data.large_packets % 10 == 1) {
                    printf("[INFO] Large packet %d bytes, using buffer group (count: %d)\n", 
                           pkt->size, data.large_packets);
                }
                
                // 从缓冲区组分配内存
                ret = mpp_buffer_get(data.pkt_grp, &mpp_buf, pkt->size);
                if (ret != MPP_OK) {
                    printf("[ERROR] Failed to get buffer from group, ret=%d\n", ret);
                    data.failed_packets++;
                    continue;
                }
                
                // 复制数据到MPP缓冲区
                void* buf_ptr = mpp_buffer_get_ptr(mpp_buf);
                if (buf_ptr && pkt->data && pkt->size > 0) {
                    // 安全的内存复制，添加边界检查
                    size_t buf_size = mpp_buffer_get_size(mpp_buf);
                    size_t copy_size = (pkt->size <= buf_size) ? pkt->size : buf_size;
                    memcpy(buf_ptr, pkt->data, copy_size);
                    
                    // 创建MPP数据包
                    ret = mpp_packet_init_with_buffer(&mpp_pkt, mpp_buf);
                    if (ret != MPP_OK) {
                        printf("[ERROR] Failed to init packet with buffer, ret=%d\n", ret);
                        mpp_buffer_put(mpp_buf);
                        data.failed_packets++;
                        continue;
                    }
                    
                    mpp_packet_set_length(mpp_pkt, pkt->size);
                } else {
                    printf("[ERROR] Failed to get buffer pointer\n");
                    mpp_buffer_put(mpp_buf);
                    data.failed_packets++;
                    continue;
                }
            } else {
                // 数据包过大，跳过
                data.failed_packets++;
                if (data.failed_packets % 10 == 1) {
                    printf("[WARN] Packet too large %d > %d, skipping (count: %d)\n", 
                           pkt->size, MPI_DEC_MAX_PACKET_SIZE, data.failed_packets);
                }
                continue;
            }
            
            // 设置时间戳
            if (mpp_pkt) {
                mpp_packet_set_pts(mpp_pkt, pkt->pts);
                mpp_packet_set_dts(mpp_pkt, pkt->dts);
                
                // 送入解码器
                ret = data.mpi->decode_put_packet(data.ctx, mpp_pkt);
                if (ret != MPP_OK) {
                    printf("[ERROR] decode_put_packet failed with ret=%d\n", ret);
                    data.failed_packets++;
                }
                
                // 清理动态分配的资源
                if (use_buffer_group && mpp_pkt != data.packet) {
                    mpp_packet_deinit(&mpp_pkt);
                }
                if (mpp_buf) {
                    mpp_buffer_put(mpp_buf);
                }
            }
        }
        
        av_packet_unref(pkt);
        
        // 获取解码帧
        ret = data.mpi->decode_get_frame(data.ctx, &frame);
        if (ret != MPP_OK) {
            if (ret != MPP_ERR_TIMEOUT) {
                printf("[ERROR] decode_get_frame failed with ret=%d\n", ret);
            }
            // 每1000次超时输出一次调试信息 - 使用全局计数器
            static int timeout_count = 0;
            if (ret == MPP_ERR_TIMEOUT) {
                timeout_count++;
                if (timeout_count % 1000 == 0) {
                    printf("[DEBUG] decode_get_frame timeout count: %d\n", timeout_count);
                }
            }
            usleep(1000); // 1ms
            continue;
        }
        
        if (!frame) {
            no_frame_count++;
            if (no_frame_count > 1000) { // 1秒无帧时输出警告
                printf("[WARN] No frame received for 1 second (processed %d packets)\n", data.total_packets);
                
                // 如果长时间无帧且处理的数据包很少，可能是连接问题
                if (data.total_packets < 10) {
                    printf("[WARN] Very few packets processed, possible connection issue\n");
                }
                
                no_frame_count = 0;
            }
            usleep(1000); // 1ms
            continue;
        }
        
        no_frame_count = 0;
        
        // 检查帧的有效性
        if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
            printf("[WARN] Frame has error or should be discarded\n");
            mpp_frame_deinit(&frame);
            frame = NULL;
            continue;
        }
        
        // 处理解码帧
        if (process_decoded_frame(frame) != 0) {
            printf("[ERROR] Failed to process decoded frame\n");
        }
        
        // 释放帧资源
        if (frame) {
            mpp_frame_deinit(&frame);
            frame = NULL;
        }
        
        data.frame_count++;
        
        // 性能统计（每100帧输出一次）
        if (data.frame_count % 100 == 0) {
            printf("[INFO] Decoded %d frames (total: %d, large: %d, failed: %d packets)\n", 
                   data.frame_count, data.total_packets, data.large_packets, data.failed_packets);
        }
    }
    
    // 清理AVPacket
    if (pkt) {
        av_packet_free(&pkt);
    }
    
    printf("[INFO] MPP decode loop finished, total frames: %d\n", data.frame_count);
    return 0;
}

// 清理资源
void cleanup() {
    printf("[INFO] Starting cleanup...\n");
    
    // 停止运行标志
    running = false;
    
    // 清理MPP资源
    if (data.packet) {
        mpp_packet_deinit(&data.packet);
        data.packet = NULL;
        printf("[INFO] MPP packet cleaned up\n");
    }
    
    // 清理数据包缓冲区组
    if (data.pkt_grp) {
        mpp_buffer_group_put(data.pkt_grp);
        data.pkt_grp = NULL;
        printf("[INFO] MPP packet buffer group cleaned up\n");
    }
    
    // 释放备用数据包缓冲区
    if (data.buf) {
        free(data.buf);
        data.buf = NULL;
        printf("[INFO] Backup packet buffer freed\n");
    }
    
    if (data.ctx) {
        mpp_destroy(data.ctx);
        data.ctx = NULL;
        printf("[INFO] MPP context destroyed\n");
    }
    
    // 输出最终统计信息
    printf("[INFO] Final stats - Total: %d, Large: %d, Failed: %d packets\n", 
           data.total_packets, data.large_packets, data.failed_packets);
    
    // 清理DMA缓冲区
    if (data.dst_dma_fd > 0) {
        dma_buf_free(data.dst_buf_size, &data.dst_dma_fd, data.dst_buf);
        data.dst_dma_fd = 0;
        data.dst_buf = nullptr;
        printf("[INFO] Destination DMA buffer freed\n");
    }
    
    if (data.resize_dma_fd > 0) {
        dma_buf_free(data.resize_buf_size, &data.resize_dma_fd, data.resize_buf);
        data.resize_dma_fd = 0;
        data.resize_buf = nullptr;
        printf("[INFO] Resize DMA buffer freed\n");
    }
    
    // 清理FFmpeg资源
    if (format_ctx) {
        avformat_close_input(&format_ctx);
        format_ctx = nullptr;
        printf("[INFO] FFmpeg format context cleaned up\n");
    }
    
    // 清理YOLO相关资源
    if (yolo_model) {
        delete yolo_model;
        yolo_model = nullptr;
        printf("[INFO] YOLO model cleaned up\n");
    }
    
    if (thread_pool) {
        delete thread_pool;
        thread_pool = nullptr;
        printf("[INFO] Thread pool cleaned up\n");
    }
    
    // 清理推流器
    if (rtmp_streamer) {
        delete rtmp_streamer;
        rtmp_streamer = nullptr;
        printf("[INFO] Streamer cleaned up\n");
    }
    
    printf("[INFO] Cleanup completed successfully\n");
}

// 信号处理函数
void signal_handler(int sig) {
    printf("\n[INFO] Received signal %d, stopping...\n", sig);
    running = false;
}

int main(int argc, char* argv[]) {
    // 启用栈保护和安全检查
    setbuf(stdout, NULL);  // 禁用stdout缓冲，避免缓冲区溢出
    setbuf(stderr, NULL);  // 禁用stderr缓冲
    
    // 参数验证
    if (argc < 7) {
        printf("Usage: %s <model_path> <thread_num> <rtsp_url> <rtmp_url> <fps> <bitrate> [polygon_file]\n", argv[0]);
        printf("Example: %s ./weights/yolov8s.rknn 4 rtsp://192.168.1.100:554/stream rtmp://localhost/live/stream 25 2000000\n", argv[0]);
        return -1;
    }
    
    // 安全的参数解析，添加长度检查
    if (!argv[1] || strlen(argv[1]) == 0 || strlen(argv[1]) > MAX_FILE_NAME_LENGTH) {
        printf("[ERROR] Invalid model path\n");
        return -1;
    }
    if (!argv[2] || strlen(argv[2]) == 0) {
        printf("[ERROR] Invalid thread number\n");
        return -1;
    }
    if (!argv[3] || strlen(argv[3]) == 0 || strlen(argv[3]) > 512) {
        printf("[ERROR] Invalid RTSP URL\n");
        return -1;
    }
    if (!argv[4] || strlen(argv[4]) == 0 || strlen(argv[4]) > 512) {
        printf("[ERROR] Invalid RTMP URL\n");
        return -1;
    }
    
    std::string model_path = argv[1];
    int thread_num = std::atoi(argv[2]);
    std::string rtsp_url = argv[3];
    std::string rtmp_url = argv[4];
    int fps = std::atoi(argv[5]);
    int bitrate = std::atoi(argv[6]);
    std::string polygon_file = (argc > 7 && argv[7]) ? argv[7] : "";
    
    // 参数合理性检查
    if (thread_num <= 0 || thread_num > 16) {
        printf("[ERROR] Invalid thread number: %d (should be 1-16)\n", thread_num);
        return -1;
    }
    if (fps <= 0 || fps > 60) {
        printf("[ERROR] Invalid fps: %d (should be 1-60)\n", fps);
        return -1;
    }
    if (bitrate <= 0) {
        printf("[ERROR] Invalid bitrate: %d (should be > 0)\n", bitrate);
        return -1;
    }
    
    // 注册信号处理函数
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("=== MPP Hardware Decoder Demo ===\n");
    printf("Model: %s\n", model_path.c_str());
    printf("Threads: %d\n", thread_num);
    printf("RTSP: %s\n", rtsp_url.c_str());
    printf("RTMP: %s\n", rtmp_url.c_str());
    printf("FPS: %d, Bitrate: %d\n", fps, bitrate);
    if (!polygon_file.empty()) {
        printf("Polygon file: %s\n", polygon_file.c_str());
    }
    printf("================================\n");
    
    // 初始化YOLO模型
    printf("[INFO] Initializing YOLO model...\n");
    yolo_model = new Yolov8Custom();
    if (yolo_model->LoadModel(model_path.c_str()) != NN_SUCCESS) {
        printf("[ERROR] Failed to load YOLO model\n");
        cleanup();
        return -1;
    }
    
    // 初始化线程池
    printf("[INFO] Initializing thread pool...\n");
    thread_pool = new Yolov8ThreadPool();
    if (thread_pool->setUp(model_path, thread_num) != NN_SUCCESS) {
        printf("[ERROR] Failed to setup thread pool\n");
        cleanup();
        return -1;
    }
    
    // 初始化推流器
    printf("[INFO] Initializing streamer...\n");
    rtmp_streamer = new streamer::Streamer();
    try {
        rtmp_streamer->enable_av_debug_log();
        
        streamer::StreamerConfig streamer_config(
            1920, 1080,   // Source width/height
            1920, 1080,   // Output width/height
            fps,          // FPS
            bitrate,      // Bitrate
            "main",       // Profile
            rtmp_url      // RTMP URL
        );
        
        int rtmp_init_result = rtmp_streamer->init(streamer_config);
        if (rtmp_init_result != 0) {
            printf("[ERROR] Failed to initialize RTMP streamer, error code: %d\n", rtmp_init_result);
            cleanup();
            return -1;
        }
        printf("[INFO] RTMP streamer initialized successfully\n");
    } catch (const std::exception& e) {
        printf("[ERROR] Exception during RTMP initialization: %s\n", e.what());
        cleanup();
        return -1;
    }
    
    // 初始化RTSP客户端（先获取正确的分辨率信息）
    printf("[INFO] Initializing RTSP client...\n");
    if (init_rtsp_client(rtsp_url.c_str()) != 0) {
        printf("[ERROR] Failed to initialize RTSP client\n");
        cleanup();
        return -1;
    }
    
    // 初始化MPP解码器（使用正确的分辨率）
    printf("[INFO] Initializing MPP decoder...\n");
    if (init_mpp_decoder() != 0) {
        printf("[ERROR] Failed to initialize MPP decoder\n");
        cleanup();
        return -1;
    }
    
    // 初始化DMA缓冲区（使用正确的分辨率）
    printf("[INFO] Initializing DMA buffers...\n");
    if (init_dma_buffer() != 0) {
        printf("[ERROR] Failed to initialize DMA buffers\n");
        cleanup();
        return -1;
    }
    
    printf("[INFO] All components initialized successfully\n");
    
    // 开始解码循环
    mpp_decode_loop();
    
    // 清理资源
    cleanup();
    
    printf("[INFO] MPP decoder demo finished successfully\n");
    return 0;
}