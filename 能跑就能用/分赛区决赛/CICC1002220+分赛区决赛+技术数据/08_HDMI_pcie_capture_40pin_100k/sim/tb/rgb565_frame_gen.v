`timescale 1ns / 1ps

`ifdef SIM_FULL_FRAME
    `define SIM_GEN_WIDTH  1280
    `define SIM_GEN_HEIGHT 720
`else
    `define SIM_GEN_WIDTH  128
    `define SIM_GEN_HEIGHT 8
`endif

// RGB565 frame generator for axi4_ctrl write-side simulation.
// wframe_vsync is HIGH during active video; EOF on falling edge (matches axi4_ctrl).
module rgb565_frame_gen #(
    parameter SIM_WIDTH  = `SIM_GEN_WIDTH,
    parameter SIM_HEIGHT = `SIM_GEN_HEIGHT,
    parameter H_BLANK    = 16,
    parameter V_BLANK    = 4
)(
    input  wire                 pclk,
    input  wire                 rst_n,

    input  wire                 start,
    input  wire       [ 3:0]    frame_id,

    output reg                  wframe_vsync,
    output reg                  wframe_data_en,
    output reg        [15:0]    wframe_data,
    output reg                  frame_done
);

    localparam integer LINE_CYCLES = SIM_WIDTH + H_BLANK;
    localparam integer FRAME_LINES = SIM_HEIGHT + V_BLANK;

    reg        running;
    integer    line_idx;
    integer    pix_idx;
    reg [7:0]  row;
    reg [7:0]  col;

    function automatic [15:0] logical_pixel565;
        input [3:0] fid;
        input [3:0] row4;
        input [7:0] col8;
        begin
            logical_pixel565 = {fid, row4, col8};
        end
    endfunction

    always @(posedge pclk or negedge rst_n) begin
        if (!rst_n) begin
            running        <= 1'b0;
            wframe_vsync   <= 1'b0;
            wframe_data_en <= 1'b0;
            wframe_data    <= 16'd0;
            frame_done     <= 1'b0;
            line_idx       <= 0;
            pix_idx        <= 0;
            row            <= 8'd0;
            col            <= 8'd0;
        end else begin
            frame_done <= 1'b0;

            if (start && !running) begin
                running  <= 1'b1;
                line_idx <= 0;
                pix_idx  <= 0;
            end

            if (running) begin
                if (line_idx < SIM_HEIGHT) begin
                    wframe_vsync <= 1'b1;
                    row          <= line_idx[7:0];
                    if (pix_idx < SIM_WIDTH) begin
                        wframe_data_en <= 1'b1;
                        col            <= pix_idx[7:0];
                        wframe_data    <= logical_pixel565(
                            frame_id,
                            line_idx[3:0],
                            pix_idx[7:0]
                        );
                        pix_idx <= pix_idx + 1;
                    end else if (pix_idx < LINE_CYCLES) begin
                        wframe_data_en <= 1'b0;
                        wframe_data    <= 16'd0;
                        pix_idx        <= pix_idx + 1;
                    end else begin
                        pix_idx  <= 0;
                        line_idx <= line_idx + 1;
                    end
                end else if (line_idx < FRAME_LINES) begin
                    wframe_vsync   <= 1'b0;
                    wframe_data_en <= 1'b0;
                    wframe_data    <= 16'd0;
                    pix_idx        <= 0;
                    line_idx       <= line_idx + 1;
                end else begin
                    wframe_vsync   <= 1'b0;
                    wframe_data_en <= 1'b0;
                    running        <= 1'b0;
                    frame_done     <= 1'b1;
                end
            end else begin
                wframe_vsync   <= 1'b0;
                wframe_data_en <= 1'b0;
            end
        end
    end

endmodule
