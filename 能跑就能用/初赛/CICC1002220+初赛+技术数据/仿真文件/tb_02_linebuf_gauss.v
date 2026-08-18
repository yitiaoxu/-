`timescale 1ns / 1ps
//============================================================================
// TB：linebuf3x3_solo + gauss_filter_rgb565_solo
// compile：tb_02_linebuf_gauss.v +
//          ..\source\user\linebuf3x3_solo.v +
//          ..\source\user\gauss_filter_rgb565_solo.v
// 行宽 W=48（加速）；约 40 行，便于布满 3×3 窗口
//============================================================================

module tb_02_linebuf_gauss;

    localparam integer H = 40;
    localparam integer W = 48;

    reg         clk   = 0;
    reg         rst_n = 0;
    reg         de_in = 0;
    reg         vs_in = 0;
    reg  [15:0] din   = 0;

    wire         lb_de;
    wire         lb_vs;
    wire  [15:0] lb_d;
    wire  [15:0] m11, m12, m13, m21, m22, m23, m31, m32, m33;

    wire         g_de;
    wire         g_vs;
    wire  [15:0] g_out;

    integer r, x;

    always #5 clk = ~clk;

    linebuf3x3_solo #(
        .W(W),
        .BYPASS3x3(1'b0)
    ) u_lb (
        .clk   (clk),
        .rst_n (rst_n),
        .de_in (de_in),
        .vs_in (vs_in),
        .din   (din),
        .de_out(lb_de),
        .vs_out(lb_vs),
        .dout  (lb_d),
        .m_m11(m11), .m_m12(m12), .m_m13(m13),
        .m_m21(m21), .m_m22(m22), .m_m23(m23),
        .m_m31(m31), .m_m32(m32), .m_m33(m33)
    );

    gauss_filter_rgb565_solo u_gauss (
        .clk        (clk),
        .rst_n      (rst_n),
        .matrix_de  (lb_de),
        .matrix_vs  (lb_vs),
        .m11        (m11), .m12 (m12), .m13 (m13),
        .m21        (m21), .m22 (m22), .m23 (m23),
        .m31        (m31), .m32 (m32), .m33 (m33),
        .gauss_de   (g_de),
        .gauss_vs   (g_vs),
        .gauss_dout (g_out)
    );

    task automatic idle_line(integer gap);
        integer k;
        begin
            for (k = 0; k < gap; k = k + 1)
                @(posedge clk);
            de_in = 0;
        end
    endtask

    initial begin
        rst_n  = 0;
        vs_in  = 0;
        de_in  = 0;
        din    = 16'h7CEF;
        repeat (8) @(posedge clk);
        rst_n = 1;
        @(posedge clk);
        vs_in = 1'b1;
        @(posedge clk);

        // 整场 vs=1：按帧结束再给 vs fall，用于 linebuf/y 计数近似真实
        for (r = 0; r < H; r = r + 1) begin
            for (x = 0; x < W; x = x + 1) begin
                @(posedge clk);
                de_in = 1'b1;
                // 从左到右递增（RGB565）
                din = { x[4:0], x[5:0], x[4:0] };
            end
            idle_line(2);
        end

        @(posedge clk);
        vs_in = 1'b0;
        @(posedge clk);
        de_in = 0;

        repeat (200) @(posedge clk);
        $display("[TB02] DONE. Inspect gauss_dout smoothed vs din.");
        $finish;
    end
endmodule
