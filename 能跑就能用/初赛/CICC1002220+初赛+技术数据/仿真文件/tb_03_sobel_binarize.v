`timescale 1ns / 1ps
//============================================================================
// TB：sobel_vertical_rgb565_solo + binarize_rgb565_solo
// compile：tb_03_sobel_binarize.v +
//          linebuf3x3_solo.v + sobel_vertical_rgb565_solo.v + binarize_rgb565_solo.v
//============================================================================

module tb_03_sobel_binarize;

    localparam integer H = 30;
    localparam integer W = 48;

    reg         clk   = 0;
    reg         rst_n = 0;
    reg         de_in = 0;
    reg         vs_in = 0;
    reg  [15:0] din   = 0;

    wire         lb_de, lb_vs;
    wire  [15:0] lb_d;
    wire  [15:0] m11, m12, m13, m21, m22, m23, m31, m32, m33;

    wire         s_de, s_vs;
    wire  [15:0] s_d;

    wire         b_de, b_vs;
    wire  [15:0] b_d;

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

    sobel_vertical_rgb565_solo #(
        .SOBEL_THRESHOLD(8'd28)
    ) u_sobel (
        .clk       (clk),
        .rst_n     (rst_n),
        .matrix_de (lb_de),
        .matrix_vs (lb_vs),
        .m11       (m11), .m12 (m12), .m13 (m13),
        .m21       (m21), .m22 (m22), .m23 (m23),
        .m31       (m31), .m32 (m32), .m33 (m33),
        .sobel_de  (s_de),
        .sobel_vs  (s_vs),
        .sobel_dout(s_d)
    );

    binarize_rgb565_solo #(
        .Binar_Threshold(8'd128)
    ) u_bin (
        .clk      (clk),
        .rst_n    (rst_n),
        .de_in    (s_de),
        .vs_in    (s_vs),
        .din      (s_d),
        .de_out   (b_de),
        .vs_out   (b_vs),
        .dout     (b_d)
    );

    initial begin
        rst_n  = 0;
        vs_in  = 0;
        de_in  = 0;
        din    = 0;
        repeat (8) @(posedge clk);
        rst_n = 1;
        @(posedge clk);
        vs_in = 1'b1;

        // 左侧黑、右侧白的竖向边缘：利于 Sobel/Gx 响应
        for (r = 0; r < H; r = r + 1) begin
            for (x = 0; x < W; x = x + 1) begin
                @(posedge clk);
                de_in = 1'b1;
                din   = (x < W/2) ? 16'h0000 : 16'hFFFF;
            end
            @(posedge clk);
            de_in = 0;
            @(posedge clk);
        end

        @(posedge clk);
        vs_in = 1'b0;
        repeat (240) @(posedge clk);
        $display("[TB03] DONE. Expect strong edge band on Sobel/binarize.");
        $finish;
    end
endmodule
