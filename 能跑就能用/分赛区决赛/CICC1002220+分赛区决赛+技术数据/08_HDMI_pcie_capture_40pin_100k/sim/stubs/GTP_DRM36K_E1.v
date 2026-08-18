`timescale 1ns / 1ps

// Behavioral stub for Pango GTP_DRM36K_E1 (simulation only).
module GTP_DRM36K_E1 #(
    parameter integer DATA_WIDTH_A = 36,
    parameter integer DATA_WIDTH_B = 36
)(
    output reg  [DATA_WIDTH_A-1:0] DOA,
    input       [15:0]             ADRA,
    input                          ADRA_HOLD,
    input       [7:0]              BWEA,
    input       [DATA_WIDTH_A-1:0] DIA,
    input       [2:0]              CSA,
    input                          WEA,
    input                          CLKA,
    input                          CEA,
    input                          ORCEA,
    input                          RSTA,
    input                          CINA,
    output                         COUTA,

    output reg  [DATA_WIDTH_B-1:0] DOB,
    input       [15:0]             ADDRB,
    input                          ADDRB_HOLD,
    input       [3:0]              BWEB,
    input       [DATA_WIDTH_B-1:0] DIB,
    input       [2:0]              CSB,
    input                          WEB,
    input                          CLKB,
    input                          CEB,
    input                          ORCEB,
    input                          RSTB,
    input                          CINB,
    output                         COUTB,

    input                          INJECT_SBITERR,
    input                          INJECT_DBITERR,
    output                         ECC_SBITERR,
    output                         ECC_DBITERR,
    output      [15:0]             ECC_RDADDR,
    output      [DATA_WIDTH_B-1:0] ECC_PARITY
);

    localparam integer DEPTH = 1024;
    reg [DATA_WIDTH_A-1:0] mem [0:DEPTH-1];

    assign COUTA = 1'b0;
    assign COUTB = 1'b0;
    assign ECC_SBITERR = 1'b0;
    assign ECC_DBITERR = 1'b0;
    assign ECC_RDADDR  = 16'd0;
    assign ECC_PARITY  = {DATA_WIDTH_B{1'b0}};

    integer i;

    always @(posedge CLKA) begin
        if (RSTA) begin
            DOA <= {DATA_WIDTH_A{1'b0}};
            for (i = 0; i < DEPTH; i = i + 1)
                mem[i] <= {DATA_WIDTH_A{1'b0}};
        end else if (CEA) begin
            if (WEA)
                mem[ADRA[$clog2(DEPTH)-1:0]] <= DIA;
            if (ORCEA)
                DOA <= mem[ADRA[$clog2(DEPTH)-1:0]];
        end
    end

    always @(posedge CLKB) begin
        if (RSTB) begin
            DOB <= {DATA_WIDTH_B{1'b0}};
        end else if (CEB && ORCEB) begin
            if (WEB)
                mem[ADDRB[$clog2(DEPTH)-1:0]] <= DIB;
            DOB <= mem[ADDRB[$clog2(DEPTH)-1:0]];
        end
    end

endmodule
