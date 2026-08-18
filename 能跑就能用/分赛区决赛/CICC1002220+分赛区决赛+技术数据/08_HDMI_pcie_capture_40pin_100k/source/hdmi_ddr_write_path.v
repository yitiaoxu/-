//////////////////////////////////////////////////////////////////////////////////
// Company: 能跑就能用
// Engineer:
//
// Create Date: 07/13/2026
// Design Name:
// Module Name: hdmi_ddr_write_path
// Project Name: HDMI PCIe Image Capture
// Target Devices: PG2L100H
// Tool Versions: Pango Design Suite
// Description: HDMI 写侧前端 + axi4_ctrl DDR 帧缓冲
//
// Dependencies:
//
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
//
//////////////////////////////////////////////////////////////////////////////////

`timescale 1ns / 1ps

`include "video_resolution.vh"

module hdmi_ddr_write_path #(
    parameter C_RD_END_ADDR = `VIDEO_FRAME_BYTES,
    parameter C_W_WIDTH     = 16,
    parameter C_R_WIDTH     = 128,
    parameter C_ID_LEN      = 4
)(
    input  wire                 pixclk_in,
    input  wire                 rst_n,

    input  wire                 de_in,
    input  wire                 vs_in,
    input  wire       [ 7:0]    r_in,
    input  wire       [ 7:0]    g_in,
    input  wire       [ 7:0]    b_in,

    input  wire                 axi_clk,
    input  wire                 axi_reset,

    output wire       [C_ID_LEN-1:0] axi_awid,
    output wire       [27:0]         axi_awaddr,
    output wire       [ 3:0]         axi_awlen,
    output wire       [ 2:0]         axi_awsize,
    output wire       [ 1:0]         axi_awburst,
    output wire                      axi_awlock,
    output wire       [ 3:0]         axi_awcache,
    output wire       [ 2:0]         axi_awprot,
    output wire       [ 3:0]         axi_awqos,
    output wire                      axi_awvalid,
    input  wire                      axi_awready,

    output wire       [127:0]        axi_wdata,
    output wire       [15:0]         axi_wstrb,
    output wire                      axi_wlast,
    output wire                      axi_wvalid,
    input  wire                      axi_wready,

    input  wire       [C_ID_LEN-1:0] axi_bid,
    input  wire       [ 1:0]         axi_bresp,
    input  wire                      axi_bvalid,
    output wire                      axi_bready,

    output wire       [C_ID_LEN-1:0] axi_arid,
    output wire       [31:0]         axi_araddr,
    output wire       [ 3:0]         axi_arlen,
    output wire       [ 2:0]         axi_arsize,
    output wire       [ 1:0]         axi_arburst,
    output wire                      axi_arlock,
    output wire       [ 3:0]         axi_arcache,
    output wire       [ 2:0]         axi_arprot,
    output wire       [ 3:0]         axi_arqos,
    output wire                      axi_arvalid,
    input  wire                      axi_arready,

    input  wire       [C_ID_LEN-1:0] axi_rid,
    input  wire       [127:0]        axi_rdata,
    input  wire       [ 1:0]         axi_rresp,
    input  wire                      axi_rlast,
    input  wire                      axi_rvalid,
    output wire                      axi_rready,

    // Debug / waveform probes (same names as image_pcie_capture.v)
    output wire                      de_in_d0,
    output wire       [ 7:0]         r_in_d0,
    output wire       [ 7:0]         g_in_d0,
    output wire       [ 7:0]         b_in_d0,
    output wire                      hdmi_vsync_eof,
    output wire       [15:0]         wframe_data,
    output wire                      wframe_data_en,
    output wire                      wframe_vsync,

    output wire       [31:0]         tp_o
);

    reg                         de_in_d0_r;
    reg              [ 7:0]     r_in_d0_r;
    reg              [ 7:0]     g_in_d0_r;
    reg              [ 7:0]     b_in_d0_r;
    reg                         de_in_d1;
    reg                         vs_in_d0;

    assign de_in_d0       = de_in_d0_r;
    assign r_in_d0        = r_in_d0_r;
    assign g_in_d0        = g_in_d0_r;
    assign b_in_d0        = b_in_d0_r;
    assign hdmi_vsync_eof = ~vs_in;
    assign wframe_data_en = de_in_d0_r;
    assign wframe_data    = {r_in_d0_r[7:3], g_in_d0_r[7:2], b_in_d0_r[7:3]};

    assign wframe_vsync = hdmi_vsync_eof;

    always @(posedge pixclk_in or negedge rst_n) begin
        if (!rst_n) begin
            de_in_d0_r <= 1'b0;
            r_in_d0_r  <= 8'd0;
            g_in_d0_r  <= 8'd0;
            b_in_d0_r  <= 8'd0;
            de_in_d1   <= 1'b0;
            vs_in_d0   <= 1'b0;
        end else begin
            de_in_d0_r <= de_in;
            r_in_d0_r  <= r_in;
            g_in_d0_r  <= g_in;
            b_in_d0_r  <= b_in;
            de_in_d1   <= de_in_d0_r;
            vs_in_d0   <= vs_in;
        end
    end

    // Read side tied off for write-path-only simulation.
    wire rframe_pclk        = axi_clk;
    wire rframe_vsync       = 1'b0;
    wire rframe_data_en     = 1'b0;
    wire [C_R_WIDTH-1:0]    rframe_data_unused;
    wire                    rframe_data_valid_unused;

    axi4_ctrl #(
        .C_RD_END_ADDR (C_RD_END_ADDR),
        .C_W_WIDTH     (C_W_WIDTH),
        .C_R_WIDTH     (C_R_WIDTH),
        .C_ID_LEN      (C_ID_LEN)
    ) u_axi4_ctrl (
        .axi_clk         (axi_clk),
        .axi_reset       (axi_reset),

        .axi_awid        (axi_awid),
        .axi_awaddr      (axi_awaddr),
        .axi_awlen       (axi_awlen),
        .axi_awsize      (axi_awsize),
        .axi_awburst     (axi_awburst),
        .axi_awlock      (axi_awlock),
        .axi_awcache     (axi_awcache),
        .axi_awprot      (axi_awprot),
        .axi_awqos       (axi_awqos),
        .axi_awvalid     (axi_awvalid),
        .axi_awready     (axi_awready),

        .axi_wdata       (axi_wdata),
        .axi_wstrb       (axi_wstrb),
        .axi_wlast       (axi_wlast),
        .axi_wvalid      (axi_wvalid),
        .axi_wready      (axi_wready),

        .axi_bid         (axi_bid),
        .axi_bresp       (axi_bresp),
        .axi_bvalid      (axi_bvalid),
        .axi_bready      (axi_bready),

        .axi_arid        (axi_arid),
        .axi_araddr      (axi_araddr),
        .axi_arlen       (axi_arlen),
        .axi_arsize      (axi_arsize),
        .axi_arburst     (axi_arburst),
        .axi_arlock      (axi_arlock),
        .axi_arcache     (axi_arcache),
        .axi_arprot      (axi_arprot),
        .axi_arqos       (axi_arqos),
        .axi_arvalid     (axi_arvalid),
        .axi_arready     (axi_arready),

        .axi_rid         (axi_rid),
        .axi_rdata       (axi_rdata),
        .axi_rresp       (axi_rresp),
        .axi_rlast       (axi_rlast),
        .axi_rvalid      (axi_rvalid),
        .axi_rready      (axi_rready),

        .wframe_pclk     (pixclk_in),
        .wframe_vsync    (hdmi_vsync_eof),
        .wframe_data_en  (de_in_d0_r),
        .wframe_data     (wframe_data),

        .rframe_pclk     (rframe_pclk),
        .rframe_vsync    (rframe_vsync),
        .rframe_data_en      (rframe_data_en),
        .rframe_data_valid   (rframe_data_valid_unused),
        .rframe_data         (rframe_data_unused),

        .tp_o                (tp_o)
    );

endmodule
