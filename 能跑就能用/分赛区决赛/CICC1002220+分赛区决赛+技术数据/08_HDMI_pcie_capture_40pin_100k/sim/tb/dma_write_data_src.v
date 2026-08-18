`timescale 1ns / 1ps

// Pattern image-data source for ips2l_pcie_dma user write interface.
module dma_write_data_src #(
    parameter integer FRAME_BEATS = 16
)(
    input  wire         clk,
    input  wire         rst_n,

    input  wire         dma_write_req,
    input  wire [11:0]  dma_write_addr,
    output reg  [127:0] dma_write_data,

    output reg  [31:0]  beat_cnt,
    output reg          frame_done
);

    function automatic [127:0] beat_pattern;
        input [31:0] idx;
        begin
            beat_pattern = {idx + 32'd3, idx + 32'd2, idx + 32'd1, idx};
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            beat_cnt      <= 32'd0;
            dma_write_data <= 128'd0;
            frame_done    <= 1'b0;
        end else begin
            frame_done <= 1'b0;
            dma_write_data <= beat_pattern(beat_cnt);
            if (dma_write_req) begin
                if (beat_cnt + 1 >= FRAME_BEATS)
                    frame_done <= 1'b1;
                beat_cnt <= beat_cnt + 1'b1;
            end
        end
    end

    function automatic [127:0] expected_beat;
        input [31:0] idx;
        begin
            expected_beat = {idx + 32'd3, idx + 32'd2, idx + 32'd1, idx};
        end
    endfunction

endmodule
