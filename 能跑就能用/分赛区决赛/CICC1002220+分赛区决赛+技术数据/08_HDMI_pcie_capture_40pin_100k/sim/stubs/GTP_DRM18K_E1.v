`timescale 1ns / 1ps

// Behavioral stub for Pango GTP_DRM18K_E1 (simulation only).
module GTP_DRM18K_E1 #(
    parameter [287:0] INIT_00 = 288'h0,
    parameter [287:0] INIT_01 = 288'h0,
    parameter [287:0] INIT_02 = 288'h0,
    parameter [287:0] INIT_03 = 288'h0,
    parameter [287:0] INIT_04 = 288'h0,
    parameter [287:0] INIT_05 = 288'h0,
    parameter [287:0] INIT_06 = 288'h0,
    parameter [287:0] INIT_07 = 288'h0,
    parameter [287:0] INIT_08 = 288'h0,
    parameter [287:0] INIT_09 = 288'h0,
    parameter [287:0] INIT_0A = 288'h0,
    parameter [287:0] INIT_0B = 288'h0,
    parameter [287:0] INIT_0C = 288'h0,
    parameter [287:0] INIT_0D = 288'h0,
    parameter [287:0] INIT_0E = 288'h0,
    parameter [287:0] INIT_0F = 288'h0,
    parameter [287:0] INIT_10 = 288'h0,
    parameter [287:0] INIT_11 = 288'h0,
    parameter [287:0] INIT_12 = 288'h0,
    parameter [287:0] INIT_13 = 288'h0,
    parameter [287:0] INIT_14 = 288'h0,
    parameter [287:0] INIT_15 = 288'h0,
    parameter [287:0] INIT_16 = 288'h0,
    parameter [287:0] INIT_17 = 288'h0,
    parameter [287:0] INIT_18 = 288'h0,
    parameter [287:0] INIT_19 = 288'h0,
    parameter [287:0] INIT_1A = 288'h0,
    parameter [287:0] INIT_1B = 288'h0,
    parameter [287:0] INIT_1C = 288'h0,
    parameter [287:0] INIT_1D = 288'h0,
    parameter [287:0] INIT_1E = 288'h0,
    parameter [287:0] INIT_1F = 288'h0,
    parameter [287:0] INIT_20 = 288'h0,
    parameter [287:0] INIT_21 = 288'h0,
    parameter [287:0] INIT_22 = 288'h0,
    parameter [287:0] INIT_23 = 288'h0,
    parameter [287:0] INIT_24 = 288'h0,
    parameter [287:0] INIT_25 = 288'h0,
    parameter [287:0] INIT_26 = 288'h0,
    parameter [287:0] INIT_27 = 288'h0,
    parameter [287:0] INIT_28 = 288'h0,
    parameter [287:0] INIT_29 = 288'h0,
    parameter [287:0] INIT_2A = 288'h0,
    parameter [287:0] INIT_2B = 288'h0,
    parameter [287:0] INIT_2C = 288'h0,
    parameter [287:0] INIT_2D = 288'h0,
    parameter [287:0] INIT_2E = 288'h0,
    parameter [287:0] INIT_2F = 288'h0,
    parameter [287:0] INIT_30 = 288'h0,
    parameter [287:0] INIT_31 = 288'h0,
    parameter [287:0] INIT_32 = 288'h0,
    parameter [287:0] INIT_33 = 288'h0,
    parameter [287:0] INIT_34 = 288'h0,
    parameter [287:0] INIT_35 = 288'h0,
    parameter [287:0] INIT_36 = 288'h0,
    parameter [287:0] INIT_37 = 288'h0,
    parameter [287:0] INIT_38 = 288'h0,
    parameter [287:0] INIT_39 = 288'h0,
    parameter [287:0] INIT_3A = 288'h0,
    parameter [287:0] INIT_3B = 288'h0,
    parameter [287:0] INIT_3C = 288'h0,
    parameter [287:0] INIT_3D = 288'h0,
    parameter [287:0] INIT_3E = 288'h0,
    parameter [287:0] INIT_3F = 288'h0,
    parameter         GRS_EN = "FALSE",
    parameter integer DATA_WIDTH_A = 18,
    parameter integer DATA_WIDTH_B = 18,
    parameter         WRITE_MODE_A = "NORMAL_WRITE",
    parameter         WRITE_MODE_B = "NORMAL_WRITE",
    parameter         DOA_REG = 0,
    parameter         DOB_REG = 0,
    parameter         DOA_REG_CLKINV = 0,
    parameter         DOB_REG_CLKINV = 0,
    parameter         RST_TYPE = "ASYNC",
    parameter         RAM_MODE = "SIMPLE_DUAL_PORT",
    parameter         INIT_FILE = "NONE",
    parameter         BLOCK_X = 0,
    parameter         BLOCK_Y = 0,
    parameter         RAM_ADDR_WIDTH = 14,
    parameter         RAM_DATA_WIDTH = 18,
    parameter         INIT_FORMAT = "BIN"
)(
    output reg  [17:0] DOA,
    input       [13:0] ADDRA,
    input              ADDRA_HOLD,
    input       [3:0]  BWEA,
    input       [17:0] DIA,
    input              WEA,
    input              CLKA,
    input              CEA,
    input              ORCEA,
    input              RSTA,

    output reg  [17:0] DOB,
    input       [13:0] ADDRB,
    input              ADDRB_HOLD,
    input       [1:0]  BWEB,
    input       [17:0] DIB,
    input              WEB,
    input              CLKB,
    input              CEB,
    input              ORCEB,
    input              RSTB
);

    localparam integer DEPTH = 16384;
    reg [17:0] mem [0:DEPTH-1];

    always @(posedge CLKA) begin
        if (RSTA) begin
            DOA <= 18'd0;
        end else if (CEA) begin
            if (WEA)
                mem[ADDRA] <= DIA;
            if (ORCEA)
                DOA <= mem[ADDRA];
        end
    end

    always @(posedge CLKB) begin
        if (RSTB) begin
            DOB <= 18'd0;
        end else if (CEB && ORCEB) begin
            if (WEB)
                mem[ADDRB] <= DIB;
            DOB <= mem[ADDRB];
        end
    end

endmodule
