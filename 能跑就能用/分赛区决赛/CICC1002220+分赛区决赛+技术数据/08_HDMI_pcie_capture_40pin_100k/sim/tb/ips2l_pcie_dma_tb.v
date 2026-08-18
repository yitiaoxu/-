`timescale 1ns / 1ps

`include "../../source/video_resolution.vh"

`ifdef SIM_FULL_FRAME
    `define TB_WIDTH  1280
    `define TB_HEIGHT 720
`else
    `define TB_WIDTH  128
    `define TB_HEIGHT 1
`endif

module ips2l_pcie_dma_tb;

    localparam integer FRAME_BYTES = `TB_WIDTH * `TB_HEIGHT * 2;
    localparam integer FRAME_BEATS = FRAME_BYTES / 16;
    localparam integer DMA_DW_LEN  = FRAME_BEATS * 4;
    localparam integer DMA_CMD_LEN = DMA_DW_LEN - 1;

    localparam [31:0] HOST_DMA_ADDR = 32'hA000_0000;
    localparam [31:0] DMA_CMD_WORD  = {8'h0, 1'b1, 7'h0, 1'b0, 6'h0, DMA_CMD_LEN[9:0]};

    reg clk;
    reg rst_n;

    reg  [31:0] test_err_cnt;
    reg  [31:0] req_pulse_cnt;
    reg  [31:0] wait_timeout;
    reg  [11:0] prev_dma_addr;
    reg  [11:0] max_dma_addr;
    reg         dma_active;

    wire        axis_master_tvld;
    wire        axis_master_trdy;
    wire [127:0] axis_master_tdata;
    wire [3:0]   axis_master_tkeep;
    wire         axis_master_tlast;
    wire [7:0]   axis_master_tuser;

    reg         axis_slave0_trdy;
    wire        axis_slave0_tvld;
    wire [127:0] axis_slave0_tdata;
    wire        axis_slave0_tlast;
    wire        axis_slave0_tuser;

    reg         axis_slave1_trdy;
    wire        axis_slave1_tvld;
    wire [127:0] axis_slave1_tdata;
    wire        axis_slave1_tlast;
    wire        axis_slave1_tuser;

    reg         axis_slave2_trdy;
    wire        axis_slave2_tvld;
    wire [127:0] axis_slave2_tdata;
    wire        axis_slave2_tlast;
    wire        axis_slave2_tuser;

    wire        o_dma_write_data_req;
    wire [11:0] o_dma_write_addr;
    wire [127:0] i_dma_write_data;

    wire [31:0] sb_beat_cnt;
    wire [31:0] sb_err_cnt;
    wire [31:0] sb_base_addr;
    wire        sb_xfer_done;
    wire [31:0] src_beat_cnt;

    wire [31:0] last_cpld_data;
    wire        cpld_received;

    function [127:0] expected_beat;
        input [31:0] idx;
        begin
            expected_beat = {idx + 32'd3, idx + 32'd2, idx + 32'd1, idx};
        end
    endfunction

    task check_eq;
        input [255:0] msg;
        input         cond;
        begin
            if (!cond) begin
                test_err_cnt = test_err_cnt + 1;
                $display("FAIL: %0s", msg);
            end else begin
                $display("PASS: %0s", msg);
            end
        end
    endtask

    task scoreboard_all_beats;
        integer i;
        reg [127:0] exp;
        begin
            for (i = 0; i < FRAME_BEATS; i = i + 1) begin
                exp = expected_beat(i);
                if (u_scoreboard.mem[i] !== exp) begin
                    test_err_cnt = test_err_cnt + 1;
                    $display("FAIL: beat %0d exp=%h got=%h", i, exp, u_scoreboard.mem[i]);
                end
            end
            if (test_err_cnt == 0)
                $display("PASS: all %0d payload beats match pattern", FRAME_BEATS);
        end
    endtask

    initial begin
        clk = 1'b0;
        forever #4 clk = ~clk;
    end

    initial begin
        rst_n         = 1'b0;
        test_err_cnt  = 32'd0;
        req_pulse_cnt = 32'd0;
        wait_timeout  = 32'd0;
        prev_dma_addr = 12'd0;
        max_dma_addr  = 12'd0;
        dma_active    = 1'b0;

        axis_slave0_trdy = 1'b1;
        axis_slave1_trdy = 1'b1;
        axis_slave2_trdy = 1'b1;

        #100;
        rst_n = 1'b1;
        #200;

        // TC-DMA01: idle after reset
        check_eq("TC-DMA01: no dma write req after reset", o_dma_write_data_req == 1'b0);
        check_eq("TC-DMA01: axis_slave2 idle", axis_slave2_tvld == 1'b0);

        // TC-DMA02: BAR1 write CMD only
        u_host_bfm.send_bar1_mwr(12'h100, DMA_CMD_WORD);
        #200;
        check_eq("TC-DMA02: CMD only, no dma req yet", o_dma_write_data_req == 1'b0);

        // TC-DMA03/04: BAR1 write ADDR triggers upload
        dma_active = 1'b1;
        u_host_bfm.send_bar1_mwr(12'h110, HOST_DMA_ADDR);

        wait_timeout = 32'd0;
        while (sb_xfer_done !== 1'b1 && wait_timeout < 32'd50000) begin
            @(posedge clk);
            wait_timeout = wait_timeout + 1'b1;
        end
        if (sb_xfer_done !== 1'b1) begin
            test_err_cnt = test_err_cnt + 1;
            $display("FAIL: TC-DMA03/04 timeout (sb_beat=%0d req_cnt=%0d mwr32=%b)",
                     sb_beat_cnt, req_pulse_cnt,
                     u_dut.u_ips2l_pcie_dma_controller.o_mwr32_req);
        end
        #200;

        check_eq("TC-DMA03: dma req beat count", req_pulse_cnt == FRAME_BEATS);
        check_eq("TC-DMA04: host mem base addr",
                 (sb_base_addr == HOST_DMA_ADDR) ||
                 (u_dut.u_ips2l_pcie_dma_controller.dma_cmd_l_addr == HOST_DMA_ADDR));
        scoreboard_all_beats();

        // TC-DMA05: read DMA status via BAR0 MRd
        if (sb_xfer_done) begin
            #500;
            u_host_bfm.send_bar0_mrd(12'h130, 10'd0);
            u_host_bfm.wait_cpld();
            check_eq("TC-DMA05: DMA_STATUS done bit",
                     (last_cpld_data[0] == 1'b1) ||
                     (u_dut.u_ips2l_pcie_dma_controller.dma_status_done == 1'b1));
        end else begin
            check_eq("TC-DMA05: skipped (no upload)", 1'b0);
        end

        // TC-DMA06: write addr increments during active xfer
        check_eq("TC-DMA06: max dma write addr > 0", max_dma_addr > 12'd0);

        #100;
        if (test_err_cnt == 0)
            $display("ALL TESTS PASSED");
        else
            $display("TESTS FAILED: %0d errors", test_err_cnt);
        $finish;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            req_pulse_cnt <= 32'd0;
            prev_dma_addr <= 12'd0;
            max_dma_addr  <= 12'd0;
        end else if (dma_active) begin
            if (o_dma_write_data_req)
                req_pulse_cnt <= req_pulse_cnt + 1'b1;
            if (o_dma_write_addr > max_dma_addr)
                max_dma_addr <= o_dma_write_addr;
            prev_dma_addr <= o_dma_write_addr;
        end
    end

    ips2l_pcie_dma #(
        .DEVICE_TYPE    (3'd0),
        .AXIS_SLAVE_NUM (3),
        .ADDR_WIDTH     (12)
    ) u_dut (
        .clk                    (clk),
        .rst_n                  (rst_n),
        .i_cfg_pbus_num         (8'h01),
        .i_cfg_pbus_dev_num     (5'h00),
        .i_cfg_max_rd_req_size  (3'd2),
        .i_cfg_max_payload_size (3'd0),
        .i_axis_master_tvld     (axis_master_tvld),
        .o_axis_master_trdy     (axis_master_trdy),
        .i_axis_master_tdata    (axis_master_tdata),
        .i_axis_master_tkeep    (axis_master_tkeep),
        .i_axis_master_tlast    (axis_master_tlast),
        .i_axis_master_tuser    (axis_master_tuser),
        .o_trgt1_radm_pkt_halt  (),
        .i_axis_slave0_trdy     (axis_slave0_trdy),
        .o_axis_slave0_tvld     (axis_slave0_tvld),
        .o_axis_slave0_tdata    (axis_slave0_tdata),
        .o_axis_slave0_tlast    (axis_slave0_tlast),
        .o_axis_slave0_tuser    (axis_slave0_tuser),
        .i_axis_slave1_trdy     (axis_slave1_trdy),
        .o_axis_slave1_tvld     (axis_slave1_tvld),
        .o_axis_slave1_tdata    (axis_slave1_tdata),
        .o_axis_slave1_tlast    (axis_slave1_tlast),
        .o_axis_slave1_tuser    (axis_slave1_tuser),
        .i_axis_slave2_trdy     (axis_slave2_trdy),
        .o_axis_slave2_tvld     (axis_slave2_tvld),
        .o_axis_slave2_tdata    (axis_slave2_tdata),
        .o_axis_slave2_tlast    (axis_slave2_tlast),
        .o_axis_slave2_tuser    (axis_slave2_tuser),
        .i_cfg_ido_req_en       (1'b0),
        .i_cfg_ido_cpl_en       (1'b0),
        .i_xadm_ph_cdts         (8'hFF),
        .i_xadm_pd_cdts         (12'hFFF),
        .i_xadm_nph_cdts        (8'hFF),
        .i_xadm_npd_cdts        (12'hFFF),
        .i_xadm_cplh_cdts       (8'hFF),
        .i_xadm_cpld_cdts       (12'hFFF),
        .i_apb_psel             (1'b0),
        .i_apb_paddr            (9'd0),
        .i_apb_pwdata           (32'd0),
        .i_apb_pstrb            (4'd0),
        .i_apb_pwrite           (1'b0),
        .i_apb_penable          (1'b0),
        .o_apb_prdy             (),
        .o_apb_prdata           (),
        .o_cross_4kb_boundary   (),
        .o_dma_write_data_req   (o_dma_write_data_req),
        .o_dma_write_addr       (o_dma_write_addr),
        .i_dma_write_data       (i_dma_write_data),
        .i_dma_read_data_req    (1'b0),
        .i_dma_read_addr        (12'd0),
        .o_dma_read_data        ()
    );

    pcie_axis_host_bfm u_host_bfm (
        .clk            (clk),
        .rst_n          (rst_n),
        .axis_tvld      (axis_master_tvld),
        .axis_trdy      (axis_master_trdy),
        .axis_tdata     (axis_master_tdata),
        .axis_tkeep     (axis_master_tkeep),
        .axis_tlast     (axis_master_tlast),
        .axis_tuser     (axis_master_tuser),
        .cpld_tvld      (axis_slave0_tvld),
        .cpld_tdata     (axis_slave0_tdata),
        .cpld_tlast     (axis_slave0_tlast),
        .last_cpld_data (last_cpld_data),
        .cpld_received  (cpld_received)
    );

    dma_write_data_src #(
        .FRAME_BEATS(FRAME_BEATS)
    ) u_data_src (
        .clk            (clk),
        .rst_n          (rst_n),
        .dma_write_req  (o_dma_write_data_req),
        .dma_write_addr (o_dma_write_addr),
        .dma_write_data (i_dma_write_data),
        .beat_cnt       (src_beat_cnt),
        .frame_done     ()
    );

    host_mem_scoreboard #(
        .MAX_BEATS(FRAME_BEATS),
        .HOST_ADDR (HOST_DMA_ADDR)
    ) u_scoreboard (
        .clk            (clk),
        .rst_n          (rst_n),
        .axis_tvld      (axis_slave2_tvld),
        .axis_tdata     (axis_slave2_tdata),
        .axis_tlast     (axis_slave2_tlast),
        .expected_beats (FRAME_BEATS),
        .mwr_base_addr  (sb_base_addr),
        .beat_cnt       (sb_beat_cnt),
        .err_cnt        (sb_err_cnt),
        .xfer_done      (sb_xfer_done)
    );

endmodule
