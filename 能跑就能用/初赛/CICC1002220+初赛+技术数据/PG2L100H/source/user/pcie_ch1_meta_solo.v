`timescale 1ns / 1ps
// PCIe DMA 每帧：14 拍 metadata(224B) + 115200 拍像素；host 对齐见 need_align / align_pulse。
//
module pcie_ch1_meta_solo (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         dma_wr_data_req,
    input  wire [3:0]   i_roi_count,
    input  wire [32*6*8-1:0] i_roi_flat,
    input  wire [31:0]  i_frame_id,
    output wire [127:0] dma_wr_data,
    output wire         ch1_data_req,
    input  wire [127:0] ch1_data,
    input  wire         ch1_data_valid,
    output wire         o_pre_dma_pulse
);
    localparam integer META_BEATS  = 14;
    localparam integer PIX_BEATS   = (1280*720*2) / 16;
    localparam integer TOTAL_BEATS = META_BEATS + PIX_BEATS;
    localparam [16:0]  TOTAL_LAST  = TOTAL_BEATS[16:0] - 17'd1;
    localparam [16:0]  LINE_BEATS  = 17'd160;

    localparam [23:0]  HOST_FRAME_GAP_CYCLES = 24'd12_500_000;

    reg  [16:0] beat_cnt /*synthesis PAP_MARK_DEBUG="1"*/;
    reg  [23:0] idle_cnt /*synthesis PAP_MARK_DEBUG="1"*/;
    reg           drq_d;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            drq_d <= 1'b0;
        else
            drq_d <= dma_wr_data_req;
    end

    wire dma_rise = dma_wr_data_req && !drq_d;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            idle_cnt <= 24'd0;
        else if (dma_wr_data_req)
            idle_cnt <= 24'd0;
        else if (idle_cnt != 24'hFFFFFF)
            idle_cnt <= idle_cnt + 24'd1;
    end

    wire align_pulse = dma_rise && (idle_cnt >= HOST_FRAME_GAP_CYCLES);

    reg need_align /*synthesis PAP_MARK_DEBUG="1"*/;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            need_align <= 1'b1;
        else if (align_pulse)
            need_align <= 1'b0;
        else if (dma_rise && need_align)
            need_align <= 1'b0;
        else if ((beat_cnt == TOTAL_LAST) && !dma_wr_data_req)
            need_align <= 1'b1;
    end

    wire gap_align /*synthesis PAP_MARK_DEBUG="1"*/ = dma_rise && need_align;
    wire any_align   = align_pulse | gap_align;

    wire in_meta /*synthesis PAP_MARK_DEBUG="1"*/ = (beat_cnt < META_BEATS[16:0]);

    wire idle_prefetch_pulse =
        !dma_wr_data_req && (idle_cnt == HOST_FRAME_GAP_CYCLES);

    wire frame_end_pulse = dma_wr_data_req && (beat_cnt == TOTAL_LAST);

    wire early_line_prefetch = dma_wr_data_req
        && (beat_cnt == (TOTAL_LAST - LINE_BEATS));

    assign o_pre_dma_pulse = idle_prefetch_pulse | frame_end_pulse | early_line_prefetch;

    reg  [3:0]  lat_rc;
    reg  [31:0] lat_fid;
    reg  [31:0] lat_r [0:47];
    integer     ii;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            beat_cnt <= 17'd0;
            lat_rc   <= 4'd0;
            lat_fid  <= 32'd0;
            for (ii = 0; ii < 48; ii = ii + 1)
                lat_r[ii] <= 32'd0;
        end
        else if (any_align) begin
            lat_rc  <= i_roi_count;
            lat_fid <= i_frame_id;
            for (ii = 0; ii < 48; ii = ii + 1)
                lat_r[ii] <= i_roi_flat[32*ii+31:32*ii];
            beat_cnt <= 17'd0;
        end
        else if (dma_wr_data_req) begin
            if (beat_cnt == TOTAL_LAST) begin
                lat_rc  <= i_roi_count;
                lat_fid <= i_frame_id;
                for (ii = 0; ii < 48; ii = ii + 1)
                    lat_r[ii] <= i_roi_flat[32*ii+31:32*ii];
                beat_cnt <= 17'd0;
            end else begin
                beat_cnt <= beat_cnt + 17'd1;
            end
        end
    end

    assign ch1_data_req = (~in_meta) & dma_wr_data_req;

    reg [127:0] ch1_data_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            ch1_data_q <= 128'd0;
        else if (ch1_data_valid)
            ch1_data_q <= ch1_data;
    end

    reg [127:0] mbeat;
    reg [5:0]   base;

    always @(*) begin
        base  = 6'd0;
        mbeat = 128'd0;
        case (beat_cnt[3:0])
            4'd0: begin
                base  = 6'd0;
                mbeat = { 32'd720, 32'd1280, lat_fid, 32'h524B3031 };
            end
            4'd1: begin
                base  = 6'd0;
                mbeat = { 32'd0, 32'd0, 32'd0, {28'd0, lat_rc} };
            end
            4'd2, 4'd3, 4'd4, 4'd5, 4'd6, 4'd7,
            4'd8, 4'd9, 4'd10, 4'd11, 4'd12, 4'd13: begin
                base  = {beat_cnt[3:0] - 4'd2, 2'd0};
                mbeat = { lat_r[base + 6'd3],
                          lat_r[base + 6'd2],
                          lat_r[base + 6'd1],
                          lat_r[base] };
            end
            default: begin
                base  = 6'd0;
                mbeat = 128'd0;
            end
        endcase
    end

    assign dma_wr_data = in_meta ? mbeat
        : (ch1_data_valid ? ch1_data : ch1_data_q);
endmodule
