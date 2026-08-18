`timescale 1ns / 1ps
// 多车牌 ROI: APB 配置, p_addr 为 [8:0] 相对 0xA000
// 每项 6x uint32: x1, y1, x2, y2, color, confidence(0-100)
// color: 0=未定义 1=蓝 2=绿 3=黄 4=白
// 坐标: 图像像素 (左上原点), (x1,y1)-(x2,y2) 为矩形
module ch1_roi_array_apb_solo #(
    parameter integer MAX_ROI = 8
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         i_apb_psel,
    input  wire [8:0]   i_apb_paddr,
    input  wire [31:0]  i_apb_pwdata,
    input  wire [3:0]   i_apb_pstrb,
    input  wire         i_apb_pwrite,
    input  wire         i_apb_penable,
    output reg          o_apb_prdy,
    output reg  [31:0]  o_apb_prdata,
    input  wire         i_ch1_frame_inc,
    // flat: ROI i 的 6 字为 o_roi_flat[i*6*32 +: 6*32] (i=0..MAX_ROI-1)
    output wire [32*6*8-1:0] o_roi_flat,
    output wire [31:0]  o_frame_id,
    output wire [3:0]   o_roi_count
);
    localparam A_FRM  = 9'h000;
    localparam A_RCNT = 9'h004;
    localparam BASE0  = 9'h010;
    localparam WIDX_W = 6;

    reg [31:0] frame_id;
    reg [3:0]  roi_count;
    reg [31:0] r [0:6*8-1];

    wire [8:0]  off9;
    wire        in_tab;
    wire [WIDX_W-1:0] widx;

    wire apb_write = i_apb_psel && i_apb_penable &&  i_apb_pwrite;
    wire apb_read  = i_apb_psel && i_apb_penable && !i_apb_pwrite;

    assign off9  = (i_apb_paddr >= BASE0) ? (i_apb_paddr - BASE0) : 9'd0;
    assign in_tab = (i_apb_paddr >= BASE0) && (i_apb_paddr < (BASE0 + 9'd24 * 9'd8));
    assign widx  = in_tab ? off9[8:2] : {WIDX_W{1'b0}};

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            o_apb_prdy <= 1'b0;
        else if (i_apb_psel && !i_apb_penable && !o_apb_prdy)
            o_apb_prdy <= 1'b1;
        else
            o_apb_prdy <= 1'b0;
    end

    integer n;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            frame_id  <= 32'd0;
            roi_count <= 4'd2;
            for (n = 0; n < 6*8; n = n + 1)
                r[n] <= 32'd0;
            // 文档“两车牌”缺省(可 APB 改)
            r[0]  <= 32'd234;  r[1]  <= 32'd105;  r[2]  <= 32'd567;  r[3]  <= 32'd198;
            r[4]  <= 32'd1;     r[5]  <= 32'd95;
            r[6]  <= 32'd801;  r[7]  <= 32'd234;  r[8]  <= 32'd1102; r[9]  <= 32'd312;
            r[10] <= 32'd2;    r[11] <= 32'd87;
        end
        else begin
            if (apb_write && &i_apb_pstrb) begin
                if (i_apb_paddr == A_FRM)
                    frame_id <= i_apb_pwdata;
                else if (i_apb_paddr == A_RCNT)
                    roi_count <= (i_apb_pwdata[3:0] > 4'd8) ? 4'd8 : i_apb_pwdata[3:0];
                else if (in_tab && (widx < 6*8))
                    r[widx] <= i_apb_pwdata;
            end
            else if (i_ch1_frame_inc)
                frame_id <= frame_id + 32'd1;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            o_apb_prdata <= 32'd0;
        else if (apb_read) begin
            if (i_apb_paddr == A_FRM)
                o_apb_prdata <= frame_id;
            else if (i_apb_paddr == A_RCNT)
                o_apb_prdata <= { 28'd0, roi_count };
            else if (in_tab && (widx < 6*8))
                o_apb_prdata <= r[widx];
            else
                o_apb_prdata <= 32'd0;
        end
    end

    genvar g;
    generate
        for (g = 0; g < 6*8; g = g + 1) begin : gflat
            assign o_roi_flat[32*g+31:32*g] = r[g];
        end
    endgenerate

    assign o_frame_id  = frame_id;
    assign o_roi_count = roi_count;
endmodule
