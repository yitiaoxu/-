`timescale 1ns / 1ps

`include "../../source/video_resolution.vh"

`ifdef SIM_FULL_FRAME
    `define TB_WIDTH  1280
    `define TB_HEIGHT 720
`else
    `define TB_WIDTH  128
    `define TB_HEIGHT 1
`endif

module axi4_ctrl_tb;

    localparam integer FRAME_BYTES  = `TB_WIDTH * `TB_HEIGHT * 2;
    localparam integer FRAME_BEATS  = FRAME_BYTES / 16;
    localparam integer FRAME_BURSTS = FRAME_BYTES / 256;
    localparam integer C_RD_END     = FRAME_BYTES;
    localparam integer SLOT_BYTES   = 22;
    localparam integer SLOT_SIZE    = (1 << SLOT_BYTES);

    reg         wframe_pclk;
    reg         rframe_pclk;
    reg         axi_clk;
    reg         axi_reset;

    reg         wr_start;
    reg  [3:0]  wr_frame_id;

    wire        wframe_vsync;
    wire        wframe_data_en;
    wire [15:0] wframe_data;
    wire        wr_frame_done;

    wire        rframe_vsync;
    wire        rframe_data_en;
    wire        rframe_data_valid;
    wire [127:0] rframe_data;
    wire [31:0] rd_beat_cnt;
    wire        rd_active;
    wire        rd_read_done;

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

    wire [3:0]  axi_bid;
    wire [1:0]  axi_bresp;
    wire        axi_bvalid;
    wire        axi_bready;

    wire [3:0]  axi_arid;
    wire [31:0] axi_araddr;
    wire [3:0]  axi_arlen;
    wire [2:0]  axi_arsize;
    wire [1:0]  axi_arburst;
    wire        axi_arlock;
    wire [3:0]  axi_arcache;
    wire [2:0]  axi_arprot;
    wire [3:0]  axi_arqos;
    wire        axi_arvalid;
    wire        axi_arready;
    wire [3:0]  axi_rid;
    wire [127:0] axi_rdata;
    wire [1:0]  axi_rresp;
    wire        axi_rlast;
    wire        axi_rvalid;
    wire        axi_rready;

    wire [31:0] slot_write_bytes_0;
    wire [31:0] slot_write_bytes_1;
    wire [31:0] slot_write_bytes_2;
    wire [31:0] slot_write_bytes_3;
    wire [31:0] write_burst_cnt;
    wire [31:0] read_burst_cnt;

    wire [1:0]  rc_wframe_index;
    wire [1:0]  rc_rframe_index;

    integer     test_pass_cnt;
    integer     test_err_cnt;
    reg  [31:0] score_err_cnt;
    reg  [31:0] sb_slot_base;

    reg [31:0] burst_cnt_at_frame_start;
    reg [31:0] slot0_bytes_at_frame_start;
    reg [31:0] slot1_bytes_at_frame_start;
    reg [31:0] burst_cnt_at_frame_end;
    reg [31:0] slot0_bytes_at_frame_end;
    reg [31:0] slot1_bytes_at_frame_end;
    reg [1:0]  wframe_index_at_frame_end;
    reg [1:0]  rframe_index_before_read;
    reg [1:0]  rframe_index_after_read;

    function automatic [15:0] logical_pixel565;
        input [3:0] fid;
        input integer pix_idx;
        reg [3:0] row4;
        reg [7:0] col8;
        integer row_i;
        integer col_i;
        begin
            row_i = pix_idx / `TB_WIDTH;
            col_i = pix_idx % `TB_WIDTH;
            row4  = row_i[3:0];
            col8  = col_i[7:0];
            logical_pixel565 = {fid, row4, col8};
        end
    endfunction

    function automatic [127:0] mem_expected_beat;
        input integer slot_base;
        input [15:0]  beat_idx;
        integer j;
        integer addr;
        begin
            mem_expected_beat = 128'd0;
            for (j = 0; j < 16; j = j + 1) begin
                addr = slot_base + beat_idx * 16 + j;
                mem_expected_beat[j*8 +: 8] = u_mem.mem[addr];
            end
        end
    endfunction

    function automatic integer slot_byte_base;
        input [1:0] slot_idx;
        begin
            slot_byte_base = slot_idx * SLOT_SIZE;
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

    task automatic check_ge;
        input [255:0] name;
        input [31:0]  got;
        input [31:0]  exp;
        begin
            if (got < exp) begin
                test_err_cnt = test_err_cnt + 1;
                $display("[FAIL] %0s: got=%0d exp>=%0d", name, got, exp);
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

    task automatic read_one_frame;
        input [1:0]   slot_idx;
        input [255:0] tag;
        reg [31:0] err_before;
        integer wait_i;
        integer beat_i;
        reg [127:0] exp_beat;
        begin
            err_before = score_err_cnt;
            sb_slot_base = slot_byte_base(slot_idx);

            $display("[%0s] t=%0t: frame switch pulse, slot=%0d",
                     tag, $time, slot_idx);
            u_bfm.pulse_frame_switch();
            repeat (800) @(posedge axi_clk);
            $display("[%0s] t=%0t: prefetch wait start (12000 rframe_pclk)",
                     tag, $time);
            for (wait_i = 0; wait_i < 12000; wait_i = wait_i + 1)
                @(posedge rframe_pclk);
            $display("[%0s] t=%0t: prefetch done, assert rframe_data_en",
                     tag, $time);

            beat_i = 0;
            u_bfm.rframe_data_en = 1'b1;
            @(posedge rframe_pclk);
            for (beat_i = 0; beat_i < FRAME_BEATS; beat_i = beat_i + 1) begin
                @(posedge rframe_pclk);
                exp_beat = mem_expected_beat(sb_slot_base, beat_i[15:0]);
                if (beat_i == 0)
                    $display("[%0s] t=%0t: beat0 valid=%b data=%h exp=%h",
                             tag, $time, rframe_data_valid, rframe_data, exp_beat);
                if (!rframe_data_valid || (rframe_data !== exp_beat))
                    score_err_cnt = score_err_cnt + 1;
            end
            $display("[%0s] t=%0t: read done, score_err_delta=%0d",
                     tag, $time, score_err_cnt - err_before);
            @(posedge rframe_pclk);
            u_bfm.rframe_data_en = 1'b0;

            check_eq({tag, "_score_err"}, score_err_cnt - err_before, 0);
            check_eq({tag, "_beat_cnt"}, beat_i, FRAME_BEATS);
        end
    endtask

    initial begin
        wframe_pclk = 1'b0;
        forever #6.75 wframe_pclk = ~wframe_pclk;
    end

    initial begin
        rframe_pclk = 1'b0;
        forever #8.0 rframe_pclk = ~rframe_pclk;
    end

    initial begin
        axi_clk = 1'b0;
        forever #1.9 axi_clk = ~axi_clk;
    end

    initial begin
        axi_reset     = 1'b1;
        wr_start      = 1'b0;
        wr_frame_id   = 4'd0;
        test_pass_cnt = 0;
        test_err_cnt  = 0;
        score_err_cnt = 0;

        #100;
        axi_reset = 1'b0;

        repeat (64) @(posedge axi_clk);
        $display("=== TC01 reset defaults ===");
        check_eq("TC01 rc_wframe_index", rc_wframe_index, 0);
        check_eq("TC01 rc_rframe_index", rc_rframe_index, 2);

        $display("=== TC02 write Frame0 ===");
        write_one_frame(4'd0, slot0_bytes_at_frame_start, burst_cnt_at_frame_start);
        check_ge("TC02 slot0_write_delta",
                 slot0_bytes_at_frame_end - slot0_bytes_at_frame_start, FRAME_BYTES);
        check_ge("TC02 write_burst_delta",
                 burst_cnt_at_frame_end - burst_cnt_at_frame_start, FRAME_BURSTS);
        check_eq("TC02 rc_wframe_index_after_f0", wframe_index_at_frame_end, 1);
        check_eq("TC02 pixel0_lo", u_mem.mem[0], 8'h00);
        check_eq("TC02 pixel0_hi", u_mem.mem[1], 8'h00);
        check_eq128("TC02 mem_beat0",
                    mem_expected_beat(0, 16'd0),
                    128'h0007_0006_0005_0004_0003_0002_0001_0000);

        $display("=== TC03 read Frame0 ===");
        read_one_frame(2'd0, "TC03");
        check_eq("TC03 rc_rframe_index_after", rc_rframe_index, 0);

        $display("=== TC04 write Frame1 ===");
        slot1_bytes_at_frame_start = slot_write_bytes_1;
        write_one_frame(4'd1, slot0_bytes_at_frame_start, burst_cnt_at_frame_start);
        check_ge("TC04 slot1_write_delta",
                 slot1_bytes_at_frame_end - slot1_bytes_at_frame_start, FRAME_BYTES);
        check_eq("TC04 rc_wframe_index_after_f1", wframe_index_at_frame_end, 2);
        if (rc_wframe_index == rc_rframe_index) begin
            test_err_cnt = test_err_cnt + 1;
            $display("[FAIL] TC04 pingpong_overlap: windex==rindex");
        end else begin
            test_pass_cnt = test_pass_cnt + 1;
            $display("[PASS] TC04 pingpong_no_overlap");
        end

        $display("=== TC05 read Frame1 + write Frame2 ===");
        fork
            begin
                read_one_frame(2'd1, "TC05_read_f1");
            end
            begin
                #8000;
                write_one_frame(4'd2, slot0_bytes_at_frame_start, burst_cnt_at_frame_start);
            end
        join
        check_eq("TC05 rc_wframe_index_after_f2", wframe_index_at_frame_end, 3);

        read_one_frame(2'd2, "TC05_read_f2");
        check_eq("TC05 rc_rframe_index_after_f2_read", rc_rframe_index, 2);

        #500;
        $display("=== SUMMARY pass=%0d err=%0d score_err=%0d ===",
                 test_pass_cnt, test_err_cnt, score_err_cnt);
        if (test_err_cnt == 0 && score_err_cnt == 0)
            $display("ALL TESTS PASSED");
        else
            $display("TESTS FAILED");
        #100;
        $finish;
    end

    task write_one_frame;
        input [3:0]  fid;
        output [31:0] slot0_start;
        output [31:0] burst_start;
        begin
            burst_start      = write_burst_cnt;
            slot0_start      = slot_write_bytes_0;
            slot1_bytes_at_frame_start = slot_write_bytes_1;
            wr_frame_id      = fid;
            wr_start         = 1'b1;
            @(posedge wframe_pclk);
            wr_start         = 1'b0;
            wait (wr_frame_done == 1'b1);
            @(posedge wframe_pclk);
            repeat (8000) @(posedge axi_clk);
            burst_cnt_at_frame_end     = write_burst_cnt;
            slot0_bytes_at_frame_end   = slot_write_bytes_0;
            slot1_bytes_at_frame_end   = slot_write_bytes_1;
            wframe_index_at_frame_end  = rc_wframe_index;
        end
    endtask

    rgb565_frame_gen #(
        .SIM_WIDTH  (`TB_WIDTH),
        .SIM_HEIGHT (`TB_HEIGHT)
    ) u_gen (
        .pclk           (wframe_pclk),
        .rst_n          (~axi_reset),
        .start          (wr_start),
        .frame_id       (wr_frame_id),
        .wframe_vsync   (wframe_vsync),
        .wframe_data_en (wframe_data_en),
        .wframe_data    (wframe_data),
        .frame_done     (wr_frame_done)
    );

    axi4_ctrl #(
        .C_RD_END_ADDR (C_RD_END),
        .C_W_WIDTH     (16),
        .C_R_WIDTH     (128),
        .C_ID_LEN      (4)
    ) u_dut (
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

        .wframe_pclk     (wframe_pclk),
        .wframe_vsync    (wframe_vsync),
        .wframe_data_en  (wframe_data_en),
        .wframe_data     (wframe_data),

        .rframe_pclk     (rframe_pclk),
        .rframe_vsync    (rframe_vsync),
        .rframe_data_en  (rframe_data_en),
        .rframe_data_valid (rframe_data_valid),
        .rframe_data     (rframe_data),

        .tp_o            ()
    );

    assign rc_wframe_index = u_dut.rc_wframe_index;
    assign rc_rframe_index = u_dut.rc_rframe_index;

    dma_read_bfm #(
        .SIM_WIDTH (`TB_WIDTH)
    ) u_bfm (
        .rframe_pclk       (rframe_pclk),
        .rst_n             (~axi_reset),
        .rframe_vsync      (rframe_vsync),
        .rframe_data_en    (rframe_data_en),
        .rframe_data_valid (rframe_data_valid),
        .rframe_data       (rframe_data),
        .rd_beat_cnt       (rd_beat_cnt),
        .rd_active         (rd_active),
        .rd_read_done      (rd_read_done)
    );

    axi4_mem_model u_mem (
        .aclk              (axi_clk),
        .aresetn           (~axi_reset),
        .s_axi_awid        (axi_awid),
        .s_axi_awaddr      ({4'd0, axi_awaddr}),
        .s_axi_awlen       (axi_awlen),
        .s_axi_awsize      (axi_awsize),
        .s_axi_awburst     (axi_awburst),
        .s_axi_awvalid     (axi_awvalid),
        .s_axi_awready     (axi_awready),
        .s_axi_wdata       (axi_wdata),
        .s_axi_wstrb       (axi_wstrb),
        .s_axi_wlast       (axi_wlast),
        .s_axi_wvalid      (axi_wvalid),
        .s_axi_wready      (axi_wready),
        .s_axi_bid         (axi_bid),
        .s_axi_bresp       (axi_bresp),
        .s_axi_bvalid      (axi_bvalid),
        .s_axi_bready      (1'b1),
        .s_axi_arid        (axi_arid),
        .s_axi_araddr      (axi_araddr),
        .s_axi_arlen       (axi_arlen),
        .s_axi_arsize      (axi_arsize),
        .s_axi_arburst     (axi_arburst),
        .s_axi_arvalid     (axi_arvalid),
        .s_axi_arready     (axi_arready),
        .s_axi_rid         (axi_rid),
        .s_axi_rdata       (axi_rdata),
        .s_axi_rresp       (axi_rresp),
        .s_axi_rlast       (axi_rlast),
        .s_axi_rvalid      (axi_rvalid),
        .s_axi_rready      (1'b1),
        .slot0_write_bytes (slot_write_bytes_0),
        .slot1_write_bytes (slot_write_bytes_1),
        .slot2_write_bytes (slot_write_bytes_2),
        .slot3_write_bytes (slot_write_bytes_3),
        .write_burst_cnt   (write_burst_cnt),
        .read_burst_cnt    (read_burst_cnt)
    );

endmodule
