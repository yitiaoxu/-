`timescale 1ns / 1ps

// Behavioral FWFT async FIFO model (same module name as IP core).
module W_FIFO_16i_128o (
    input              wr_clk,
    input              wr_rst,
    input              wr_en,
    input       [15:0] wr_data,
    output             wr_full,
    output      [15:0] wr_water_level,
    input              rd_clk,
    input              rd_rst,
    input              rd_en,
    output      [127:0] rd_data,
    output             almost_full,
    output             rd_empty,
    output      [15:0] rd_water_level,
    output             almost_empty
);

    localparam integer DEPTH = 2048;
    reg [15:0] mem [0:DEPTH-1];
    reg [10:0] wr_ptr;
    reg [10:0] rd_ptr;
    reg [11:0] wr_words;
    reg [11:0] rd_words;
    reg [11:0] wr_words_rd;
    reg [11:0] rd_words_wr;

    wire [11:0] avail = (wr_words_rd >= rd_words) ? (wr_words_rd - rd_words) : 12'd0;
    wire [11:0] occ   = (wr_words >= rd_words_wr) ? (wr_words - rd_words_wr) : 12'd0;

    assign wr_full        = (occ >= DEPTH);
    assign almost_full    = (occ >= (DEPTH - 8));
    assign rd_empty       = (avail == 0);
    assign almost_empty   = (avail < 8);
    assign wr_water_level = occ[15:0];
    assign rd_water_level = avail[15:0];

    integer i;
    reg [127:0] rd_data_i;

    always @* begin
        rd_data_i = 128'd0;
        if (avail >= 8) begin
            for (i = 0; i < 8; i = i + 1)
                rd_data_i[i*16 +: 16] = mem[(rd_ptr + i[10:0]) % DEPTH];
        end
    end

    assign rd_data = rd_data_i;

    always @(posedge rd_clk or posedge rd_rst) begin
        if (rd_rst)
            wr_words_rd <= 12'd0;
        else
            wr_words_rd <= wr_words;
    end

    always @(posedge wr_clk or posedge wr_rst) begin
        if (wr_rst)
            rd_words_wr <= 12'd0;
        else
            rd_words_wr <= rd_words;
    end

    always @(posedge wr_clk or posedge wr_rst) begin
        if (wr_rst) begin
            wr_ptr   <= 11'd0;
            wr_words <= 12'd0;
        end else if (wr_en && !wr_full) begin
            mem[wr_ptr] <= wr_data;
            wr_ptr <= (wr_ptr == DEPTH - 1) ? 11'd0 : (wr_ptr + 1'b1);
            wr_words <= wr_words + 1'b1;
        end
    end

    always @(posedge rd_clk or posedge rd_rst) begin
        if (rd_rst) begin
            rd_ptr   <= 11'd0;
            rd_words <= 12'd0;
        end else if (rd_en && (avail >= 8)) begin
            rd_ptr <= (rd_ptr + 8) % DEPTH;
            rd_words <= rd_words + 8;
        end
    end

endmodule
