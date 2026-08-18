`timescale 1ns / 1ps

// Behavioral simulation model (same module name as IP core).
module R_FIFO_128i_128o (
    input              wr_clk,
    input              wr_rst,
    input              wr_en,
    input       [127:0] wr_data,
    output             wr_full,
    output      [15:0] wr_water_level,
    input              rd_clk,
    input              rd_rst,
    input              rd_en,
    output reg  [127:0] rd_data,
    output             almost_full,
    output             rd_empty,
    output      [15:0] rd_water_level,
    output             almost_empty
);

    localparam integer DEPTH = 512;
    reg [127:0] mem [0:DEPTH-1];
    reg [15:0]  wr_ptr;
    reg [15:0]  rd_ptr;
    reg [16:0]  count;

    assign wr_full        = (count >= DEPTH);
    assign almost_full    = (count >= DEPTH - 4);
    assign rd_empty       = (count == 0);
    assign almost_empty   = (count < 4);
    assign wr_water_level = count[15:0];
    assign rd_water_level = count[15:0];

    always @(posedge wr_clk or posedge wr_rst) begin
        if (wr_rst) begin
            wr_ptr <= 16'd0;
        end else if (wr_en && !wr_full) begin
            mem[wr_ptr[8:0]] <= wr_data;
            wr_ptr <= wr_ptr + 1'b1;
        end
    end

    always @(posedge rd_clk or posedge rd_rst) begin
        if (rd_rst) begin
            rd_ptr <= 16'd0;
            rd_data <= 128'd0;
        end else if (rd_en && !rd_empty) begin
            rd_data <= mem[rd_ptr[8:0]];
            rd_ptr <= rd_ptr + 1'b1;
        end
    end

    always @(posedge wr_clk or posedge wr_rst) begin
        if (wr_rst)
            count <= 17'd0;
        else
            count <= count + (wr_en && !wr_full) - (rd_en && !rd_empty);
    end

endmodule
