`timescale 1ns / 1ps

// Captures outbound MWR TLP payload beats on axis_slave2.
module host_mem_scoreboard #(
    parameter integer MAX_BEATS = 115200,
    parameter [15:0]  REQ_ID    = 16'h0100,
    parameter [31:0]  HOST_ADDR = 32'hA000_0000
)(
    input  wire         clk,
    input  wire         rst_n,

    input  wire         axis_tvld,
    input  wire [127:0] axis_tdata,
    input  wire         axis_tlast,

    input  wire [31:0]  expected_beats,

    output reg  [31:0]  mwr_base_addr,
    output reg  [31:0]  beat_cnt,
    output reg  [31:0]  err_cnt,
    output reg          xfer_done
);

    localparam ST_IDLE = 2'd0;
    localparam ST_DATA = 2'd1;

    reg [1:0] state;
    reg       base_captured;
    reg [127:0] mem [0:MAX_BEATS-1];

    reg         axis_tvld_d;
    reg [127:0] axis_tdata_d;
    reg         axis_tlast_d;

    wire is_mwr_pkt = (axis_tdata_d[31:24] == 8'h40) &&
                      !axis_tlast_d &&
                      (axis_tdata_d[39:32] == 8'hff) &&
                      (axis_tdata_d[63:48] == REQ_ID);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            axis_tvld_d  <= 1'b0;
            axis_tdata_d <= 128'b0;
            axis_tlast_d <= 1'b0;
        end else begin
            axis_tvld_d  <= axis_tvld;
            axis_tdata_d <= axis_tdata;
            axis_tlast_d <= axis_tlast;
        end
    end

    function [127:0] endian_convert;
        input [127:0] data_in;
        begin
            endian_convert[32*0+31:32*0+0] = {data_in[32*0+7:32*0+0], data_in[32*0+15:32*0+8], data_in[32*0+23:32*0+16], data_in[32*0+31:32*0+24]};
            endian_convert[32*1+31:32*1+0] = {data_in[32*1+7:32*1+0], data_in[32*1+15:32*1+8], data_in[32*1+23:32*1+16], data_in[32*1+31:32*1+24]};
            endian_convert[32*2+31:32*2+0] = {data_in[32*2+7:32*2+0], data_in[32*2+15:32*2+8], data_in[32*2+23:32*2+16], data_in[32*2+31:32*2+24]};
            endian_convert[32*3+31:32*3+0] = {data_in[32*3+7:32*3+0], data_in[32*3+15:32*3+8], data_in[32*3+23:32*3+16], data_in[32*3+31:32*3+24]};
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state         <= ST_IDLE;
            mwr_base_addr <= 32'd0;
            beat_cnt      <= 32'd0;
            err_cnt       <= 32'd0;
            xfer_done     <= 1'b0;
            base_captured <= 1'b0;
        end else begin
            if (beat_cnt >= expected_beats)
                xfer_done <= 1'b1;

            if (axis_tvld_d) begin
                if (state == ST_IDLE && is_mwr_pkt) begin
                    if (!base_captured && axis_tdata_d[95:64] == HOST_ADDR) begin
                        mwr_base_addr <= axis_tdata_d[95:64];
                        base_captured <= 1'b1;
                    end
                    state <= ST_DATA;
                end else if (state == ST_DATA) begin
                    mem[beat_cnt] <= endian_convert(axis_tdata_d);
                    beat_cnt      <= beat_cnt + 1'b1;

                    if (axis_tlast_d) begin
                        state <= ST_IDLE;
                    end
                end
            end
        end
    end

endmodule
