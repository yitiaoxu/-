`timescale 1ns / 1ps
//============================================================================
// TB：morph_close_rgb565_solo
// compile：tb_04_morph_close.v + morph_close_rgb565_solo.v + linebuf3x3_solo.v（子模块）
//============================================================================

module tb_04_morph_close;

    localparam integer H = 40;
    localparam integer W = 48;

    reg         clk   = 0;
    reg         rst_n = 0;
    reg         de_in = 0;
    reg         vs_in = 0;
    reg  [15:0] din   = 0;

    wire  [15:0] m11, m12, m13, m21, m22, m23, m31, m32, m33;

    wire  [15:0] lb_d_unused;

    wire         lb_de;
    wire         lb_vs;

    wire         mc_de;
    wire         mc_vs;
    wire  [15:0] mc_d;

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
        .dout  (lb_d_unused),
        .m_m11(m11), .m_m12(m12), .m_m13(m13),
        .m_m21(m21), .m_m22(m22), .m_m23(m23),
        .m_m31(m31), .m_m32(m32), .m_m33(m33)
    );

    morph_close_rgb565_solo #(
        .W(W)
    ) u_mclose (
        .clk       (clk),
        .rst_n     (rst_n),
        .matrix_de (lb_de),
        .matrix_vs (lb_vs),
        .m11       (m11), .m12 (m12), .m13 (m13),
        .m21       (m21), .m22 (m22), .m23 (m23),
        .m31       (m31), .m32 (m32), .m33 (m33),
        .close_de  (mc_de),
        .close_vs  (mc_vs),
        .close_dout(mc_d)
    );

    initial begin
        rst_n  = 0;
        vs_in  = 0;
        de_in  = 0;
        din    = 16'h0000;
        repeat (10) @(posedge clk);
        rst_n = 1;
        @(posedge clk);
        vs_in = 1'b1;

        // 在二值图上留一条“断开”白条 + 空洞：闭运算后有连通修补趋势
        for (r = 0; r < H; r = r + 1) begin
            for (x = 0; x < W; x = x + 1) begin
                @(posedge clk);
                de_in = 1'b1;
                if (r >= H/4 && r < 3*H/4 && x >= 6 && x < W - 8) begin
                    if (r == H/2 && (x >= W/2 - 2 && x <= W/2 + 2))
                        din = 16'h0000;
                    else
                        din = 16'hFFFF;
                end else
                    din = 16'h0000;
            end
            @(posedge clk);
            de_in = 0;
            @(posedge clk);
        end

        @(posedge clk);
        vs_in = 1'b0;
        repeat (300) @(posedge clk);
        $display("[TB04] DONE. Inspect morph close_dout reconnect vs din.");
        $finish;
    end
endmodule
