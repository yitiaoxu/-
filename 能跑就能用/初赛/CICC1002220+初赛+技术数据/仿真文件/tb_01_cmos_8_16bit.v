`timescale 1ns / 1ps
//============================================================================
// TB：cmos_8_16bit
// compile：本文件 + 工程源码 ..\source\cmos_8_16bit.v
//
// ◆ 若已链接 Pango 提供的 GTP_IOCLKDIV_E2 仿真模型，请加宏： USE_PANGO_GTP_SIM_MODEL
//    （此时删除或禁用下方桩模块，否则会重复定义）
// ◆ 无厂商库时使用默认：不定义 USE_PANGO_GTP_SIM_MODEL，使用本文件中行为桩。
//============================================================================

`ifndef USE_PANGO_GTP_SIM_MODEL
module GTP_IOCLKDIV_E2 #(
    parameter DIV_FACTOR = "2"
) (
    output reg CLKDIVOUT,
    input  wire CE,
    input  wire CLKIN,
    input  wire RST_N
);
    initial CLKDIVOUT = 1'b0;
    always @(posedge CLKIN or negedge RST_N) begin
        if (!RST_N)
            CLKDIVOUT <= 1'b0;
        else if (CE)
            CLKDIVOUT <= ~CLKDIVOUT;
    end
endmodule
`endif

module tb_01_cmos_8_16bit;

    reg        pclk     = 0;
    reg        rst_n    = 0;
    reg        vs_i     = 0;
    reg        de_i     = 0;
    reg  [7:0] pdata_i  = 0;

    wire        pixel_clk;
    wire        de_o;
    wire [15:0] pdata_o;

    always #5 pclk = ~pclk;

    cmos_8_16bit u_dut (
        .pclk     (pclk),
        .rst_n    (rst_n),
        .de_i     (de_i),
        .pdata_i  (pdata_i),
        .vs_i     (vs_i),
        .pixel_clk(pixel_clk),
        .de_o     (de_o),
        .pdata_o  (pdata_o)
    );

    task automatic burst_pair(input [7:0] a, input [7:0] b);
        begin
            @(posedge pclk);
            de_i    = 1'b1;
            pdata_i = a;
            @(posedge pclk);
            pdata_i = b;
            @(posedge pclk);
            de_i    = 1'b0;
        end
    endtask

    integer px;
    initial begin
        rst_n   = 0;
        vs_i    = 0;
        de_i    = 0;
        pdata_i = 0;
        repeat (8) @(posedge pclk);
        rst_n = 1;
        repeat (10) @(posedge pclk);

        // vs 上升沿：内部 enble=1（见源码），IOCLKDIV 才开始分频输出
        @(posedge pclk);
        vs_i = 1'b1;
        @(posedge pclk);
        vs_i = 1'b0;

        repeat (4) @(posedge pclk);
        burst_pair(8'h11, 8'h22);
        burst_pair(8'h33, 8'h44);

        // 再等若干 pixel_clk（div2）观察 pdata_o/de_o 对齐情况
        for (px = 0; px < 200; px = px + 1)
            @(posedge pixel_clk);

        $display("[TB01] DONE. Check pdata_o aligns to {11,22}{33,44} on pixel_clk in waveform viewer.");
        $finish;
    end
endmodule
