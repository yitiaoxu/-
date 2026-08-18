`timescale 1ns / 1ps

// PCIe DMA read-side BFM: emulates o_dma_write_data_req / ch0_rframe_req.
// rframe_vsync: one-cycle HIGH pulse (falling edge triggers axi4_ctrl read index switch).
module dma_read_bfm #(
    parameter SIM_WIDTH = 128
)(
    input  wire                 rframe_pclk,
    input  wire                 rst_n,

    output reg                  rframe_vsync,
    output reg                  rframe_data_en,
    input  wire                 rframe_data_valid,
    input  wire       [127:0]   rframe_data,

    output reg        [31:0]    rd_beat_cnt,
    output reg                  rd_active,
    output reg                  rd_read_done
);

    task automatic pulse_frame_switch;
        begin
            rframe_vsync   = 1'b0;
            rframe_data_en = 1'b0;
            @(posedge rframe_pclk);
            rframe_vsync   = 1'b1;
            @(posedge rframe_pclk);
            rframe_vsync   = 1'b0;
        end
    endtask

    task automatic dma_read_beats;
        input [15:0] beats;
        input integer prefetch_wait;
        integer i;
        begin
            rd_beat_cnt    = 32'd0;
            rd_active      = 1'b1;
            rframe_data_en = 1'b0;

            for (i = 0; i < prefetch_wait; i = i + 1)
                @(posedge rframe_pclk);

            rframe_data_en = 1'b1;
            while (rd_beat_cnt < beats) begin
                @(posedge rframe_pclk);
            end
            @(posedge rframe_pclk);
            rframe_data_en = 1'b0;
            rd_active      = 1'b0;
            rd_read_done   = 1'b1;
            @(posedge rframe_pclk);
            rd_read_done   = 1'b0;
        end
    endtask

    always @(posedge rframe_pclk or negedge rst_n) begin
        if (!rst_n) begin
            rd_beat_cnt <= 32'd0;
        end else if (rframe_data_valid && rd_active) begin
            rd_beat_cnt <= rd_beat_cnt + 1;
        end
    end

    initial begin
        rframe_vsync   = 1'b0;
        rframe_data_en = 1'b0;
        rd_active      = 1'b0;
        rd_read_done   = 1'b0;
        rd_beat_cnt    = 32'd0;
    end

endmodule
