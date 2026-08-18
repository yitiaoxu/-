`timescale 1ns / 1ps

// Lightweight AXI4 slave memory model: 4 frame slots x 4 MB each.
module axi4_mem_model #(
    parameter C_DATA_LEN   = 128,
    parameter C_STRB_LEN   = 16,
    parameter C_BURST_LEN  = 16,
    parameter C_SLOT_BYTES = 22,
    parameter C_NUM_SLOTS  = 4
)(
    input  wire                 aclk,
    input  wire                 aresetn,

    input  wire       [ 3:0]    s_axi_awid,
    input  wire       [31:0]    s_axi_awaddr,
    input  wire       [ 3:0]    s_axi_awlen,
    input  wire       [ 2:0]    s_axi_awsize,
    input  wire       [ 1:0]    s_axi_awburst,
    input  wire                 s_axi_awvalid,
    output reg                  s_axi_awready,

    input  wire       [127:0]   s_axi_wdata,
    input  wire       [15:0]    s_axi_wstrb,
    input  wire                 s_axi_wlast,
    input  wire                 s_axi_wvalid,
    output reg                  s_axi_wready,

    output reg        [ 3:0]    s_axi_bid,
    output reg        [ 1:0]    s_axi_bresp,
    output reg                  s_axi_bvalid,
    input  wire                 s_axi_bready,

    input  wire       [ 3:0]    s_axi_arid,
    input  wire       [31:0]    s_axi_araddr,
    input  wire       [ 3:0]    s_axi_arlen,
    input  wire       [ 2:0]    s_axi_arsize,
    input  wire       [ 1:0]    s_axi_arburst,
    input  wire                 s_axi_arvalid,
    output reg                  s_axi_arready,

    output reg        [ 3:0]    s_axi_rid,
    output reg        [127:0]   s_axi_rdata,
    output reg        [ 1:0]    s_axi_rresp,
    output reg                  s_axi_rlast,
    output reg                  s_axi_rvalid,
    input  wire                 s_axi_rready,

    output reg        [31:0]    slot0_write_bytes,
    output reg        [31:0]    slot1_write_bytes,
    output reg        [31:0]    slot2_write_bytes,
    output reg        [31:0]    slot3_write_bytes,
    output reg        [31:0]    write_burst_cnt,
    output reg        [31:0]    read_burst_cnt
);

    localparam integer MEM_BYTES = (1 << C_SLOT_BYTES) * C_NUM_SLOTS;
    reg [7:0] mem [0:MEM_BYTES-1];

    reg [31:0] awaddr_latch;
    reg [3:0]  awlen_latch;
    reg [7:0]  wbeat_cnt;
    reg        write_active;

    reg [31:0] araddr_latch;
    reg [3:0]  arlen_latch;
    reg [7:0]  rbeat_cnt;
    reg        read_active;

    integer i;

    function automatic [31:0] flat_byte_addr;
        input [31:0] addr;
        input [7:0]  byte_off;
        reg [1:0] slot;
        reg [31:0] off;
        begin
            slot = addr[23:22];
            off  = {10'd0, addr[21:0]} + byte_off;
            flat_byte_addr = (slot * (1 << C_SLOT_BYTES)) + off;
        end
    endfunction

    always @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bid     <= 4'd0;
            s_axi_bresp   <= 2'b00;
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rid     <= 4'd0;
            s_axi_rdata   <= 128'd0;
            s_axi_rresp   <= 2'b00;
            s_axi_rlast   <= 1'b0;
            write_active  <= 1'b0;
            read_active   <= 1'b0;
            wbeat_cnt     <= 8'd0;
            rbeat_cnt     <= 8'd0;
            write_burst_cnt <= 32'd0;
            read_burst_cnt  <= 32'd0;
            slot0_write_bytes <= 32'd0;
            slot1_write_bytes <= 32'd0;
            slot2_write_bytes <= 32'd0;
            slot3_write_bytes <= 32'd0;
        end else begin
            s_axi_bvalid <= 1'b0;
            s_axi_rvalid <= 1'b0;
            s_axi_rlast  <= 1'b0;

            if (!write_active) begin
                s_axi_wready <= 1'b0;
                if (s_axi_awvalid && !s_axi_awready) begin
                    s_axi_awready <= 1'b1;
                    awaddr_latch  <= s_axi_awaddr;
                    awlen_latch   <= s_axi_awlen;
                end else if (s_axi_awready) begin
                    s_axi_awready <= 1'b0;
                    write_active  <= 1'b1;
                    wbeat_cnt     <= 8'd0;
                    s_axi_wready  <= 1'b1;
                    write_burst_cnt <= write_burst_cnt + 1;
                end
            end else begin
                if (s_axi_wvalid && s_axi_wready) begin
                    for (i = 0; i < C_STRB_LEN; i = i + 1) begin
                        if (s_axi_wstrb[i]) begin
                            mem[flat_byte_addr(awaddr_latch, i[7:0])] <=
                                s_axi_wdata[i*8 +: 8];
                        end
                    end
                    case (awaddr_latch[23:22])
                        2'd0: slot0_write_bytes <= slot0_write_bytes + C_STRB_LEN;
                        2'd1: slot1_write_bytes <= slot1_write_bytes + C_STRB_LEN;
                        2'd2: slot2_write_bytes <= slot2_write_bytes + C_STRB_LEN;
                        default: slot3_write_bytes <= slot3_write_bytes + C_STRB_LEN;
                    endcase

                    awaddr_latch <= awaddr_latch + C_STRB_LEN;
                    wbeat_cnt    <= wbeat_cnt + 1;

                    if (s_axi_wlast || (wbeat_cnt >= awlen_latch)) begin
                        s_axi_wready <= 1'b0;
                        write_active <= 1'b0;
                        s_axi_bvalid <= 1'b1;
                        s_axi_bid    <= s_axi_awid;
                        s_axi_bresp  <= 2'b00;
                    end
                end
            end

            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;

            if (!read_active) begin
                if (s_axi_arvalid && !s_axi_arready) begin
                    s_axi_arready <= 1'b1;
                    araddr_latch  <= s_axi_araddr;
                    arlen_latch   <= s_axi_arlen;
                end else if (s_axi_arready) begin
                    s_axi_arready <= 1'b0;
                    read_active   <= 1'b1;
                    rbeat_cnt     <= 8'd0;
                    read_burst_cnt <= read_burst_cnt + 1;
                end
            end else begin
                s_axi_rvalid <= 1'b1;
                s_axi_rid    <= s_axi_arid;
                s_axi_rresp  <= 2'b00;
                for (i = 0; i < C_STRB_LEN; i = i + 1) begin
                    s_axi_rdata[i*8 +: 8] <=
                        mem[flat_byte_addr(araddr_latch, i[7:0])];
                end
                s_axi_rlast <= (rbeat_cnt >= arlen_latch);

                if (s_axi_rready) begin
                    araddr_latch <= araddr_latch + C_STRB_LEN;
                    rbeat_cnt    <= rbeat_cnt + 1;
                    if (rbeat_cnt >= arlen_latch) begin
                        read_active  <= 1'b0;
                        s_axi_rvalid <= 1'b0;
                    end
                end
            end
        end
    end

endmodule
