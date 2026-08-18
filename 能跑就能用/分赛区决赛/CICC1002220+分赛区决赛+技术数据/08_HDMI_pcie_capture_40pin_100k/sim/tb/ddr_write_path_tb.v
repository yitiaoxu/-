`timescale 1ns / 1ps

`include "../../source/video_resolution.vh"

`ifdef SIM_FULL_FRAME
    `define TB_WIDTH  1280
    `define TB_HEIGHT 720
`else
    `define TB_WIDTH  128
    `define TB_HEIGHT 8
`endif

module ddr_write_path_tb;

    localparam integer FRAME_BYTES = `TB_WIDTH * `TB_HEIGHT * 2;
    localparam integer FRAME_BEATS = FRAME_BYTES / 16;
    localparam integer FRAME_BURSTS = FRAME_BYTES / 256;

    reg         pixclk_in;
    reg         axi_clk;
    reg         rst_n;
    reg         axi_reset;

    reg         wr_start;
    reg  [3:0]  frame_id;

    wire        de_in;
    wire        vs_in;
    wire [7:0]  r_in;
    wire [7:0]  g_in;
    wire [7:0]  b_in;
    wire        frame_done;

    wire        de_in_d0;
    wire [7:0]  r_in_d0;
    wire [7:0]  g_in_d0;
    wire [7:0]  b_in_d0;
    wire        hdmi_vsync_eof;
    wire [15:0] wframe_data;
    wire        wframe_data_en;
    wire        wframe_vsync;

    wire [3:0]  axi_awid;
    wire [27:0] axi_awaddr;
    wire [3:0]  axi_awlen;
    wire [2:0]  axi_awsize;
    wire [1:0]  axi_awburst;
    wire        axi_awlock;
    wire [3:0]  axi_awcache;
    wire [2:0]  axi_awprot;
    wire [3:0]  axi_awqos;
    wire        axi_awvalid;
    wire        axi_awready;
    wire [127:0] axi_wdata;
    wire [15:0] axi_wstrb;
    wire        axi_wlast;
    wire        axi_wvalid;
    wire        axi_wready;

    wire [3:0]  axi_arid;
    wire [31:0] axi_araddr;
    wire [3:0]  axi_arlen;
    wire        axi_arvalid;
    wire        axi_arready;
    wire [127:0] axi_rdata;
    wire        axi_rvalid;
    wire        axi_rlast;

    wire [31:0] slot_write_bytes_0;
    wire [31:0] slot_write_bytes_1;
    wire [31:0] slot_write_bytes_2;
    wire [31:0] slot_write_bytes_3;
    wire [31:0] write_burst_cnt;

    wire [1:0]  rc_wframe_index;
    wire [1:0]  rc_rframe_index;

    integer     test_pass_cnt;
    integer     test_err_cnt;
    integer     cvt_err_cnt;
    integer     reg_err_cnt;

    reg  [7:0]  tcv01_r_prev;
    reg  [7:0]  tcv01_g_prev;
    reg  [7:0]  tcv01_b_prev;
    reg         tcv01_de_prev;
    reg         tcv01_checked;

    reg [31:0] burst_cnt_at_frame_start;
    reg [31:0] slot0_bytes_at_frame_start;
    reg [31:0] burst_cnt_at_frame_end;
    reg [31:0] slot0_bytes_at_frame_end;
    reg [1:0]  wframe_index_at_frame_end;

    function [15:0] rgb888_to_rgb565;
        input [7:0] r;
        input [7:0] g;
        input [7:0] b;
        begin
            rgb888_to_rgb565 = {r[7:3], g[7:2], b[7:3]};
        end
    endfunction

    function [15:0] logical_pixel565;
        input [3:0] fid;
        input [3:0] row4;
        input [7:0] col8;
        begin
            logical_pixel565 = {fid, row4, col8};
        end
    endfunction

    function [127:0] read_slot0_beat0;
        input dummy;
        integer j;
        integer base;
        begin
            base = 0;
            read_slot0_beat0 = 128'd0;
            for (j = 0; j < 16; j = j + 1)
                read_slot0_beat0[j*8 +: 8] = u_mem.mem[base + j];
        end
    endfunction

    function [127:0] pack_beat565;
        input [15:0] base_pixel;
        integer k;
        reg [15:0] px;
        begin
            pack_beat565 = 128'd0;
            for (k = 0; k < 8; k = k + 1) begin
                px = base_pixel + k;
                pack_beat565[k*16 +: 16] = px;
            end
        end
    endfunction

    task automatic check_eq;
        input [255:0] name;
        input [31:0]  got;
        input [31:0]  exp;
        begin
            if (got !== exp) begin
                test_err_cnt = test_err_cnt + 1;
                $display("[FAIL] %0s: got=%0d exp=%0d", name, got, exp);
            end else begin
                test_pass_cnt = test_pass_cnt + 1;
                $display("[PASS] %0s: %0d", name, got);
            end
        end
    endtask

    task automatic check_eq128;
        input [255:0] name;
        input [127:0] got;
        input [127:0] exp;
        begin
            if (got !== exp) begin
                test_err_cnt = test_err_cnt + 1;
                $display("[FAIL] %0s: got=%h exp=%h", name, got, exp);
            end else begin
                test_pass_cnt = test_pass_cnt + 1;
                $display("[PASS] %0s: %h", name, got);
            end
        end
    endtask

    initial begin
        pixclk_in = 1'b0;
        forever #6.75 pixclk_in = ~pixclk_in;
    end

    initial begin
        axi_clk = 1'b0;
        forever #1.9 axi_clk = ~axi_clk;
    end

    initial begin
        rst_n      = 1'b0;
        axi_reset  = 1'b1;
        wr_start   = 1'b0;
        frame_id   = 4'd0;
        test_pass_cnt = 0;
        test_err_cnt  = 0;
        cvt_err_cnt   = 0;
        reg_err_cnt   = 0;
        tcv01_checked = 1'b0;
        burst_cnt_at_frame_end = 32'd0;
        slot0_bytes_at_frame_end = 32'd0;
        burst_cnt_at_frame_start = 32'd0;
        slot0_bytes_at_frame_start = 32'd0;

        #100;
        rst_n = 1'b1;
        #100;

        // TC-V01/V02: spot check before DDR/AXI path is active (avoid side effects).
        $display("=== TC-V01/V02 register + convert spot check ===");
        drive_manual_pixel(8'hFF, 8'h80, 8'h00);
        @(posedge pixclk_in);
        @(posedge pixclk_in);
        check_eq("TC-V01 de_in_d0 delayed", de_in_d0, 1'b1);
        check_eq("TC-V01 r_in_d0", r_in_d0, 8'hFF);
        check_eq("TC-V01 g_in_d0", g_in_d0, 8'h80);
        check_eq("TC-V02 wframe_data", wframe_data,
                 rgb888_to_rgb565(r_in_d0, g_in_d0, b_in_d0));
        drive_manual_pixel(8'd0, 8'd0, 8'd0);
        @(posedge pixclk_in);
        release u_dut.de_in;
        release u_dut.r_in;
        release u_dut.g_in;
        release u_dut.b_in;

        axi_reset = 1'b0;

        // TC-W01: default frame indices after reset release
        #50;
        $display("=== TC-W01 reset defaults ===");
        check_eq("rc_wframe_index", rc_wframe_index, 0);
        check_eq("rc_rframe_index", rc_rframe_index, 2);

        // TC-V03 + TC-W02/W03: full frame through RGB888 BFM
        $display("=== TC-V03/TC-W02 write Frame0 ===");
        write_one_frame(4'd0);

        check_eq("TC-W02 slot0_has_frame",
                 ((slot0_bytes_at_frame_end - slot0_bytes_at_frame_start) >= FRAME_BYTES), 1);
        check_eq("TC-W02 write_burst_activity",
                 ((burst_cnt_at_frame_end - burst_cnt_at_frame_start) >= FRAME_BURSTS), 1);
        check_eq("TC-W02 eof_flush_done",
                 ((wframe_index_at_frame_end == 2'd1) ||
                  ((slot0_bytes_at_frame_end - slot0_bytes_at_frame_start) >= FRAME_BYTES)) ? 1 : 0,
                 1);
        check_eq("TC-V03 cvt_err_cnt", cvt_err_cnt, 0);
        check_eq("TC-W03 pixel0_lo", u_mem.mem[0], 8'h00);
        check_eq("TC-W03 pixel0_hi", u_mem.mem[1], 8'h00);

        #500;
        #500;
        $display("=== SUMMARY pass=%0d err=%0d reg_err=%0d cvt_err=%0d ===",
                 test_pass_cnt, test_err_cnt, reg_err_cnt, cvt_err_cnt);
        if (test_err_cnt == 0)
            $display("ALL TESTS PASSED");
        else
            $display("TESTS FAILED");
        #100;
        $finish;
    end

    task drive_manual_pixel;
        input [7:0] rr;
        input [7:0] gg;
        input [7:0] bb;
        begin
            force u_dut.de_in = 1'b1;
            force u_dut.r_in  = rr;
            force u_dut.g_in  = gg;
            force u_dut.b_in  = bb;
        end
    endtask

    task write_one_frame;
        input [3:0] fid;
        begin
            burst_cnt_at_frame_start = write_burst_cnt;
            slot0_bytes_at_frame_start = slot_write_bytes_0;
            frame_id = fid;
            wr_start = 1'b1;
            @(posedge pixclk_in);
            wr_start = 1'b0;
            wait (frame_done == 1'b1);
            @(posedge pixclk_in);
            repeat (6000) @(posedge axi_clk);
            burst_cnt_at_frame_end = write_burst_cnt;
            slot0_bytes_at_frame_end = slot_write_bytes_0;
            wframe_index_at_frame_end = rc_wframe_index;
        end
    endtask

    always @(posedge pixclk_in) begin
        if (rst_n && de_in && !tcv01_checked) begin
            tcv01_r_prev <= r_in;
            tcv01_g_prev <= g_in;
            tcv01_b_prev <= b_in;
            tcv01_de_prev <= de_in;
        end

        if (rst_n && de_in_d0) begin
            if (r_in_d0 !== tcv01_r_prev && tcv01_de_prev)
                reg_err_cnt <= reg_err_cnt + 1;
        end

        if (rst_n && wframe_data_en) begin
            if (wframe_data !== rgb888_to_rgb565(r_in_d0, g_in_d0, b_in_d0))
                cvt_err_cnt <= cvt_err_cnt + 1;
        end
    end

    rgb888_frame_gen #(
        .SIM_WIDTH  (`TB_WIDTH),
        .SIM_HEIGHT (`TB_HEIGHT)
    ) u_gen (
        .pclk       (pixclk_in),
        .rst_n      (rst_n),
        .start      (wr_start),
        .frame_id   (frame_id),
        .vs_in      (vs_in),
        .de_in      (de_in),
        .r_in       (r_in),
        .g_in       (g_in),
        .b_in       (b_in),
        .frame_done (frame_done)
    );

    hdmi_ddr_write_path #(
        .C_RD_END_ADDR (0)
    ) u_dut (
        .pixclk_in   (pixclk_in),
        .rst_n       (rst_n),
        .de_in       (de_in),
        .vs_in       (vs_in),
        .r_in        (r_in),
        .g_in        (g_in),
        .b_in        (b_in),
        .axi_clk     (axi_clk),
        .axi_reset   (axi_reset),

        .axi_awid    (axi_awid),
        .axi_awaddr  (axi_awaddr),
        .axi_awlen   (axi_awlen),
        .axi_awsize  (axi_awsize),
        .axi_awburst (axi_awburst),
        .axi_awlock  (axi_awlock),
        .axi_awcache (axi_awcache),
        .axi_awprot  (axi_awprot),
        .axi_awqos   (axi_awqos),
        .axi_awvalid (axi_awvalid),
        .axi_awready (axi_awready),
        .axi_wdata   (axi_wdata),
        .axi_wstrb   (axi_wstrb),
        .axi_wlast   (axi_wlast),
        .axi_wvalid  (axi_wvalid),
        .axi_wready  (axi_wready),
        .axi_bid     (4'd0),
        .axi_bresp   (2'b00),
        .axi_bvalid  (1'b1),
        .axi_bready  (),

        .axi_arid    (axi_arid),
        .axi_araddr  (axi_araddr),
        .axi_arlen   (axi_arlen),
        .axi_arsize  (),
        .axi_arburst (),
        .axi_arlock  (),
        .axi_arcache (),
        .axi_arprot  (),
        .axi_arqos   (),
        .axi_arvalid (axi_arvalid),
        .axi_arready (axi_arready),
        .axi_rid     (4'd0),
        .axi_rdata   (128'd0),
        .axi_rresp   (2'b00),
        .axi_rlast   (1'b0),
        .axi_rvalid  (1'b0),
        .axi_rready  (),

        .de_in_d0        (de_in_d0),
        .r_in_d0         (r_in_d0),
        .g_in_d0         (g_in_d0),
        .b_in_d0         (b_in_d0),
        .hdmi_vsync_eof  (hdmi_vsync_eof),
        .wframe_data     (wframe_data),
        .wframe_data_en  (wframe_data_en),
        .wframe_vsync    (wframe_vsync),
        .tp_o            ()
    );

    assign rc_wframe_index = u_dut.u_axi4_ctrl.rc_wframe_index;
    assign rc_rframe_index = u_dut.u_axi4_ctrl.rc_rframe_index;

    axi4_mem_model u_mem (
        .aclk            (axi_clk),
        .aresetn         (~axi_reset),
        .s_axi_awid      (axi_awid),
        .s_axi_awaddr    ({4'd0, axi_awaddr}),
        .s_axi_awlen     (axi_awlen),
        .s_axi_awsize    (axi_awsize),
        .s_axi_awburst   (axi_awburst),
        .s_axi_awvalid   (axi_awvalid),
        .s_axi_awready   (axi_awready),
        .s_axi_wdata     (axi_wdata),
        .s_axi_wstrb     (axi_wstrb),
        .s_axi_wlast     (axi_wlast),
        .s_axi_wvalid    (axi_wvalid),
        .s_axi_wready    (axi_wready),
        .s_axi_bid       (),
        .s_axi_bresp     (),
        .s_axi_bvalid    (),
        .s_axi_bready    (1'b1),
        .s_axi_arid      (axi_arid),
        .s_axi_araddr    (axi_araddr),
        .s_axi_arlen     (axi_arlen),
        .s_axi_arsize    (3'b100),
        .s_axi_arburst   (2'b01),
        .s_axi_arvalid   (axi_arvalid),
        .s_axi_arready   (axi_arready),
        .s_axi_rid       (),
        .s_axi_rdata     (axi_rdata),
        .s_axi_rresp     (),
        .s_axi_rlast     (axi_rlast),
        .s_axi_rvalid    (axi_rvalid),
        .s_axi_rready    (1'b0),
        .slot0_write_bytes (slot_write_bytes_0),
        .slot1_write_bytes (slot_write_bytes_1),
        .slot2_write_bytes (slot_write_bytes_2),
        .slot3_write_bytes (slot_write_bytes_3),
        .write_burst_cnt   (write_burst_cnt),
        .read_burst_cnt  ()
    );

endmodule
