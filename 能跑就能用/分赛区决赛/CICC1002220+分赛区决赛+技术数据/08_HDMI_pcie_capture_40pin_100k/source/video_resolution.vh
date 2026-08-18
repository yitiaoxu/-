//////////////////////////////////////////////////////////////////////////////////
// Company: 能跑就能用
// Engineer:
//
// Create Date: 07/13/2026
// Design Name:
// Module Name: video_resolution
// Project Name: HDMI PCIe Image Capture
// Target Devices: PG2L100H
// Tool Versions: Pango Design Suite
// Description: 视频分辨率宏定义（RGB565 SDR），切换 1080p 请修改本文件后重新综合
//
// Dependencies:
//
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
//
//////////////////////////////////////////////////////////////////////////////////

`define VIDEO_WIDTH           1280
`define VIDEO_HEIGHT          720
`define VIDEO_ROW_BYTES       (`VIDEO_WIDTH * 2)
`define VIDEO_FRAME_BYTES     (`VIDEO_WIDTH * `VIDEO_HEIGHT * 2)
// 128-bit DMA beats per line = ROW_BYTES / 16
`define VIDEO_LINE_LAST_BEAT  12'hA0
`define VIDEO_LINE_PREV_BEAT  12'h9F
`define VIDEO_FRAME_LINES     12'd720
