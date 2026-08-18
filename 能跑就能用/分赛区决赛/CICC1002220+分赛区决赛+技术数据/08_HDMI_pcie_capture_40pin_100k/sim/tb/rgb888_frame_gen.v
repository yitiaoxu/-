`timescale 1ns / 1ps

`ifdef SIM_FULL_FRAME
    `define SIM_GEN_WIDTH  1280
    `define SIM_GEN_HEIGHT 720
`else
    `define SIM_GEN_WIDTH  128
    `define SIM_GEN_HEIGHT 8
`endif

// MS7200-style parallel RGB888 frame generator for write-path simulation.
// vs_in is active-LOW during active video (matches hdmi_vsync_eof = ~vs_in).
module rgb888_frame_gen #(
    parameter SIM_WIDTH  = `SIM_GEN_WIDTH,
    parameter SIM_HEIGHT = `SIM_GEN_HEIGHT,
    parameter H_BLANK    = 16,
    parameter V_BLANK    = 4
)(
    input  wire                 pclk,
    input  wire                 rst_n,

    input  wire                 start,
    input  wire       [ 3:0]    frame_id,

    output reg                  vs_in,
    output reg                  de_in,
    output reg        [ 7:0]    r_in,
    output reg        [ 7:0]    g_in,
    output reg        [ 7:0]    b_in,
    output reg                  frame_done
);

    localparam integer LINE_CYCLES = SIM_WIDTH + H_BLANK;
    localparam integer FRAME_LINES = SIM_HEIGHT + V_BLANK;

    reg        running;
    integer    line_idx;
    integer    pix_idx;
    reg [7:0]  row;
    reg [7:0]  col;

    function automatic [23:0] rgb888_for_logical_pixel;
        input [3:0] fid;
        input [3:0] row4;
        input [7:0] col8;
        reg [15:0] target565;
        reg [7:0]  rr;
        reg [7:0]  gg;
        reg [7:0]  bb;
        begin
            target565 = {fid, row4, col8};
            rr = {target565[15:11], 3'b000};
            gg = {target565[10:5],  2'b00};
            bb = {target565[4:0],   3'b000};
            rgb888_for_logical_pixel = {rr, gg, bb};
        end
    endfunction

    always @(posedge pclk or negedge rst_n) begin
        if (!rst_n) begin
            running    <= 1'b0;
            vs_in      <= 1'b1;
            de_in      <= 1'b0;
            r_in       <= 8'd0;
            g_in       <= 8'd0;
            b_in       <= 8'd0;
            frame_done <= 1'b0;
            line_idx   <= 0;
            pix_idx    <= 0;
            row        <= 8'd0;
            col        <= 8'd0;
        end else begin
            frame_done <= 1'b0;

            if (start && !running) begin
                running  <= 1'b1;
                line_idx <= 0;
                pix_idx  <= 0;
                vs_in    <= 1'b0;
            end

            if (running) begin
                if (line_idx < SIM_HEIGHT) begin
                    vs_in <= 1'b0;
                    row   <= line_idx[7:0];
                    if (pix_idx < SIM_WIDTH) begin
                        de_in <= 1'b1;
                        col   <= pix_idx[7:0];
                        {r_in, g_in, b_in} <= rgb888_for_logical_pixel(
                            frame_id,
                            line_idx[3:0],
                            pix_idx[7:0]
                        );
                        pix_idx <= pix_idx + 1;
                    end else if (pix_idx < LINE_CYCLES) begin
                        de_in <= 1'b0;
                        r_in  <= 8'd0;
                        g_in  <= 8'd0;
                        b_in  <= 8'd0;
                        pix_idx <= pix_idx + 1;
                    end else begin
                        pix_idx  <= 0;
                        line_idx <= line_idx + 1;
                    end
                end else if (line_idx < FRAME_LINES) begin
                    vs_in   <= 1'b1;
                    de_in   <= 1'b0;
                    r_in    <= 8'd0;
                    g_in    <= 8'd0;
                    b_in    <= 8'd0;
                    pix_idx <= 0;
                    line_idx <= line_idx + 1;
                end else begin
                    vs_in      <= 1'b1;
                    de_in      <= 1'b0;
                    running    <= 1'b0;
                    frame_done <= 1'b1;
                end
            end else begin
                vs_in <= 1'b1;
                de_in <= 1'b0;
            end
        end
    end

endmodule
