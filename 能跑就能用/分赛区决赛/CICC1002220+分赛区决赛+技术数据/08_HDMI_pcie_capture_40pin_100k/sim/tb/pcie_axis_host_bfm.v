`timescale 1ns / 1ps

// Host-side PCIe TLP BFM: drives axis_master into ips2l_pcie_dma (EP).
module pcie_axis_host_bfm #(
    parameter integer REQ_ID = 16'h0100
)(
    input  wire         clk,
    input  wire         rst_n,

    output reg          axis_tvld,
    input  wire         axis_trdy,
    output reg  [127:0] axis_tdata,
    output reg  [3:0]   axis_tkeep,
    output reg          axis_tlast,
    output reg  [7:0]   axis_tuser,

    input  wire         cpld_tvld,
    input  wire [127:0] cpld_tdata,
    input  wire         cpld_tlast,

    output reg  [31:0]  last_cpld_data,
    output reg          cpld_received
);

    localparam [7:0] MWR_32 = 8'h40;
    localparam [7:0] MRD_32 = 8'h00;

    function [31:0] le_dword;
        input [31:0] val;
        begin
            le_dword = {val[7:0], val[15:8], val[23:16], val[31:24]};
        end
    endfunction

    task axis_send_beat;
        input [127:0] data;
        input [3:0]   keep;
        input         last;
        input [7:0]   user;
        integer       hold;
        begin
            axis_tdata  = data;
            axis_tkeep  = keep;
            axis_tlast  = 1'b0;
            axis_tuser  = user;
            axis_tvld   = 1'b1;
            // ips2l_pcie_dma_tlp_rcv pipelines axis input by 2 stages; each beat
            // must be held for 2 cycles so HEAD_RCV/DATA_RCV see tdata_ff aligned.
            for (hold = 0; hold < 2; hold = hold + 1) begin
                axis_tlast = last && (hold == 1);
                @(posedge clk);
                while (!axis_trdy) @(posedge clk);
            end
            axis_tvld   = 1'b0;
            axis_tlast  = 1'b0;
            @(posedge clk);
        end
    endtask

    task send_bar1_mwr;
        input [11:0]  bar_offset;
        input [31:0]  wr_data;
        reg   [127:0] hdr;
        reg   [127:0] payload;
        reg   [7:0]   user_bar1;
        begin
            user_bar1 = 8'h10;
            hdr       = 128'b0;
            hdr[31:0]   = {MWR_32, 14'h0, 10'd0};
            hdr[39:32]  = 8'hff;
            hdr[63:48]  = REQ_ID;
            hdr[95:64]  = {20'b0, bar_offset[11:0]};

            payload = 128'b0;
            payload[31:0] = le_dword(wr_data);

            axis_send_beat(hdr, 4'h0, 1'b0, user_bar1);
            axis_send_beat(payload, 4'h1, 1'b1, user_bar1);
        end
    endtask

    task send_bar0_mrd;
        input [11:0]  bar_offset;
        input [9:0]   length_dw;
        reg   [127:0] hdr;
        reg   [7:0]   user_bar0;
        begin
            user_bar0 = 8'h00;
            hdr       = 128'b0;
            hdr[31:0]   = {MRD_32, 14'h0, length_dw[9:0]};
            hdr[63:48]  = REQ_ID;
            hdr[95:64]  = {20'b0, bar_offset[11:0]};

            cpld_received = 1'b0;
            axis_send_beat(hdr, 4'h0, 1'b1, user_bar0);
        end
    endtask

    reg         cpld_tvld_d;
    reg [127:0] cpld_tdata_d;
    reg         cpld_tlast_d;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cpld_tvld_d  <= 1'b0;
            cpld_tdata_d <= 128'b0;
            cpld_tlast_d <= 1'b0;
        end else begin
            cpld_tvld_d  <= cpld_tvld;
            cpld_tdata_d <= cpld_tdata;
            cpld_tlast_d <= cpld_tlast;
        end
    end

    task wait_cpld;
        integer timeout;
        reg     saw_header;
        begin
            timeout     = 0;
            saw_header  = 1'b0;
            while (!cpld_received && timeout < 10000) begin
                @(posedge clk);
                if (cpld_tvld_d) begin
                    if (!saw_header && cpld_tdata_d[31:24] == 8'h4A)
                        saw_header = 1'b1;
                    else if (saw_header && cpld_tdata_d[31:24] != 8'h4A) begin
                        last_cpld_data = {cpld_tdata_d[7:0], cpld_tdata_d[15:8],
                                          cpld_tdata_d[23:16], cpld_tdata_d[31:24]};
                        cpld_received  = 1'b1;
                    end
                end
                timeout = timeout + 1;
            end
        end
    endtask

    initial begin
        axis_tvld       = 1'b0;
        axis_tdata      = 128'b0;
        axis_tkeep      = 4'b0;
        axis_tlast      = 1'b0;
        axis_tuser      = 8'b0;
        last_cpld_data  = 32'b0;
        cpld_received   = 1'b0;
    end

endmodule
