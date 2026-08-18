`timescale 1ns / 1ps
// 帧内非零像素最小外接矩形；输出打包格式同 ch1_roi_array_apb_solo。
module ch1_roi_bbox_simple #(
    parameter integer VS_INVERT      = 0,
    parameter integer FG_USE_DE_ONLY = 0,
    parameter integer FG_FORCE_ON    = 0
) (
    input  wire        clk_pix,
    input  wire        rst_n_pix,
    input  wire        de_in,
    input  wire        vs_in,
    input  wire [15:0] din,
    input  wire        clk_div2,
    input  wire        rst_n_div2,
    output wire [32*6*8-1:0] o_roi_flat,
    output wire [3:0]  o_roi_count,
    output wire [31:0] o_frame_id
);

    wire vs_eff = (VS_INVERT != 0) ? ~vs_in : vs_in;

    reg vs_d, de_d;
    always @(posedge clk_pix or negedge rst_n_pix) begin
        if (!rst_n_pix) begin vs_d <= 1'b0; de_d <= 1'b0; end
        else            begin vs_d <= vs_eff; de_d <= de_in; end
    end
    wire vs_rise = vs_eff & ~vs_d;
    wire vs_fall = ~vs_eff & vs_d;
    wire de_fall = ~de_in & de_d;

    reg [11:0] x_cnt, y_cnt;
    always @(posedge clk_pix or negedge rst_n_pix) begin
        if (!rst_n_pix) begin
            x_cnt <= 12'd0;
            y_cnt <= 12'd0;
        end else if (vs_rise) begin
            x_cnt <= 12'd0;
            y_cnt <= 12'd0;
        end else if (de_in) begin
            x_cnt <= x_cnt + 1'b1;
        end else if (de_fall) begin
            x_cnt <= 12'd0;
            y_cnt <= y_cnt + 1'b1;
        end
    end

    wire fg = (FG_FORCE_ON != 0) ? 1'b1
             : (FG_USE_DE_ONLY != 0) ? de_in : (de_in & (|din));

    reg [11:0] minx, maxx, miny, maxy;
    reg [19:0] total_fg_cnt;

    always @(posedge clk_pix or negedge rst_n_pix) begin
        if (!rst_n_pix) begin
            minx         <= 12'hFFF;
            miny         <= 12'hFFF;
            maxx         <= 12'd0;
            maxy         <= 12'd0;
            total_fg_cnt <= 20'd0;
        end else if (vs_rise) begin
            minx         <= 12'hFFF;
            miny         <= 12'hFFF;
            maxx         <= 12'd0;
            maxy         <= 12'd0;
            total_fg_cnt <= 20'd0;
        end else if (fg) begin
            total_fg_cnt <= total_fg_cnt + 20'd1;
            if (x_cnt < minx) minx <= x_cnt;
            if (x_cnt > maxx) maxx <= x_cnt;
            if (y_cnt < miny) miny <= y_cnt;
            if (y_cnt > maxy) maxy <= y_cnt;
        end
    end

    reg [11:0] lat_x1, lat_y1, lat_x2, lat_y2;
    reg        lat_valid;
    reg [6:0]  lat_conf;
    reg        ftgl;

    always @(posedge clk_pix or negedge rst_n_pix) begin
        if (!rst_n_pix) begin
            lat_x1    <= 12'd0;
            lat_y1    <= 12'd0;
            lat_x2    <= 12'd0;
            lat_y2    <= 12'd0;
            lat_valid <= 1'b0;
            lat_conf  <= 7'd0;
            ftgl      <= 1'b0;
        end else if (vs_fall) begin
            if (total_fg_cnt > 20'd0) begin
                lat_x1    <= minx;
                lat_y1    <= miny;
                lat_x2    <= maxx;
                lat_y2    <= maxy;
                lat_valid <= 1'b1;
                lat_conf  <= 7'd60;
            end else begin
                lat_x1    <= 12'd0;
                lat_y1    <= 12'd0;
                lat_x2    <= 12'd0;
                lat_y2    <= 12'd0;
                lat_valid <= 1'b0;
                lat_conf  <= 7'd0;
            end
            ftgl <= ~ftgl;
        end
    end

    reg [2:0] ftgl_sync;
    always @(posedge clk_div2 or negedge rst_n_div2) begin
        if (!rst_n_div2) ftgl_sync <= 3'd0;
        else             ftgl_sync <= {ftgl_sync[1:0], ftgl};
    end
    wire frame_done = ftgl_sync[2] ^ ftgl_sync[1];

    reg [11:0] sx1, sy1, sx2, sy2;
    reg        sval;
    reg [6:0]  sconf;
    reg [31:0] frame_id;

    always @(posedge clk_div2 or negedge rst_n_div2) begin
        if (!rst_n_div2) begin
            sx1      <= 12'd0;
            sy1      <= 12'd0;
            sx2      <= 12'd0;
            sy2      <= 12'd0;
            sval     <= 1'b0;
            sconf    <= 7'd0;
            frame_id <= 32'd0;
        end else if (frame_done) begin
            sx1      <= lat_x1;
            sy1      <= lat_y1;
            sx2      <= lat_x2;
            sy2      <= lat_y2;
            sval     <= lat_valid;
            sconf    <= lat_conf;
            frame_id <= frame_id + 32'd1;
        end
    end

    assign o_roi_flat[31:0]      = {20'd0, sx1};
    assign o_roi_flat[63:32]     = {20'd0, sy1};
    assign o_roi_flat[95:64]     = {20'd0, sx2};
    assign o_roi_flat[127:96]    = {20'd0, sy2};
    assign o_roi_flat[159:128]   = 32'd0;
    assign o_roi_flat[191:160]   = {25'd0, sconf};
    assign o_roi_flat[1535:192]  = 1344'd0;

    assign o_roi_count = sval ? 4'd1 : 4'd0;
    assign o_frame_id  = frame_id;

endmodule
