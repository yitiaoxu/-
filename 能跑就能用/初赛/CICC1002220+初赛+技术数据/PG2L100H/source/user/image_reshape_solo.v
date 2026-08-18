`timescale 1ns / 1ps
// 1280x720 RGB565：1 拍对齐 VS/DE/数据，供 DDR 写口
module image_reshape_solo (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        img_vs,
    input  wire        img_data_valid,
    input  wire [15:0] img_data,
    output reg         img_vs_out,
    output reg         img_data_valid_out,
    output reg  [15:0] img_data_out
);
    // VS/DE/像素同拍延迟 1 周期
    always @(posedge clk) begin
        if (!rst_n) begin
            img_vs_out         <= 1'b0;
            img_data_valid_out <= 1'b0;
            img_data_out       <= 16'd0;
        end else begin
            img_vs_out         <= img_vs;
            img_data_valid_out <= img_data_valid;
            img_data_out       <= img_data;
        end
    end
endmodule
