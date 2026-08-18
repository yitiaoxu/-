//////////////////////////////////////////////////////////////////////////////////
// Company: 能跑就能用
// Engineer:
//
// Create Date: 07/13/2026
// Design Name:
// Module Name: ms72xx_ctl
// Project Name: HDMI PCIe Image Capture
// Target Devices: PG2L100H
// Tool Versions: Pango Design Suite
// Description: MS72xx HDMI 收发控制
//
// Dependencies:
//
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
//
//////////////////////////////////////////////////////////////////////////////////

`timescale 1ns / 1ps

`define UD #1
module ms72xx_ctl #(
    parameter MS7210_EN = 1'b1
)(
    input       clk,
    input       rst_n,
    
    output      init_over_rx,
    output      init_over,
    output      iic_scl,
    inout       iic_sda
);
    reg rstn_temp1,rstn_temp2;
    reg rstn;
    always @(posedge clk or negedge rst_n)
    begin
    	if(!rst_n)
    	    rstn_temp1 <= 1'b0;
    	else
    	    rstn_temp1 <= rst_n;
    end
    
    always @(posedge clk)
    begin
    	rstn_temp2 <= rstn_temp1;
    	rstn <= rstn_temp2;
    end
    
    wire         init_over_rx;
    wire         init_over_tx;
    wire   [7:0] device_id_rx;
    wire         iic_trig_rx ;
    wire         w_r_rx      ;
    wire  [15:0] addr_rx     ;
    wire  [ 7:0] data_in_rx  ;
    wire         busy_rx     ;
    wire  [ 7:0] data_out_rx ;
    wire         byte_over_rx;
    
    wire   [7:0] device_id_tx;
    wire         iic_trig_tx ;
    wire         w_r_tx      ;
    wire  [15:0] addr_tx     ;
    wire  [ 7:0] data_in_tx  ;
    wire         busy_tx     ;
    wire  [ 7:0] data_out_tx ;
    wire         byte_over_tx;
    
    wire   [7:0] device_id   ;
    wire         iic_trig    ;
    wire         w_r         ;
    wire  [15:0] addr        /*synthesis PAP_MARK_DEBUG="true"*/;
    wire  [ 7:0] data_in     ;
    wire         busy        ;
    wire  [ 7:0] data_out    /*synthesis PAP_MARK_DEBUG="true"*/;
    wire         byte_over   /*synthesis PAP_MARK_DEBUG="true"*/;
    wire         use_ms7210;

    ms7200_ctl U1_ms7200_ctl(
        .clk             (  clk           ),//input               
        .rstn            (  rstn          ),//input               
                              
        .init_over       (  init_over_rx  ),//output reg          
        .device_id       (                ),//output        [7:0] 
        .iic_trig        (  iic_trig_rx   ),//output reg          
        .w_r             (  w_r_rx        ),//output reg          
        .addr            (  addr_rx       ),//output reg   [15:0] 
        .data_in         (  data_in_rx    ),//output reg   [ 7:0] 
        .busy            (  busy_rx       ),//input               
        .data_out        (  data_out_rx   ),//input        [ 7:0] 
        .byte_over       (  byte_over_rx  ) //input               
    );
    
    ms7210_ctl U2_ms7210_ctl(
        .clk             (  clk           ),//input               
        .rstn            (  MS7210_EN ? init_over_rx : 1'b0 ),//input               
                              
        .init_over       (  init_over_tx  ),//output reg          
        .device_id       (                ),//output        [7:0] 
        .iic_trig        (  iic_trig_tx   ),//output reg          
        .w_r             (  w_r_tx        ),//output reg          
        .addr            (  addr_tx       ),//output reg   [15:0] 
        .data_in         (  data_in_tx    ),//output reg   [ 7:0] 
        .busy            (  busy_tx       ),//input               
        .data_out        (  data_out_tx   ),//input        [ 7:0] 
        .byte_over       (  byte_over_tx  ) //input               
    );
    
    assign use_ms7210 = MS7210_EN && init_over_rx && !init_over_tx;
    assign init_over = MS7210_EN ? init_over_tx : init_over_rx;
    assign device_id = use_ms7210 ? 8'hB2 : 8'h56;
    assign iic_trig = use_ms7210 ? iic_trig_tx : iic_trig_rx;
    assign w_r = use_ms7210 ? w_r_tx : w_r_rx;
    assign addr = use_ms7210 ? addr_tx : addr_rx;
    assign data_in = use_ms7210 ? data_in_tx : data_in_rx;
    assign busy_tx = use_ms7210 ? busy : 1'b0;
    assign data_out_tx = use_ms7210 ? data_out : 8'd0;
    assign byte_over_tx = use_ms7210 ? byte_over : 1'b0;
    
    assign busy_rx = use_ms7210 ? 1'b0 : busy;
    assign data_out_rx = use_ms7210 ? 8'd0 : data_out;
    assign byte_over_rx = use_ms7210 ? 1'b0 : byte_over;

    wire         sda_in/*synthesis PAP_MARK_DEBUG="true"*/;
    wire         sda_out/*synthesis PAP_MARK_DEBUG="true"*/;
    wire         sda_out_en/*synthesis PAP_MARK_DEBUG="true"*/;  
    iic_dri #(
        .CLK_FRE        (  27'd10_000_000  ),//parameter            CLK_FRE   = 27'd50_000_000,//system clock frequency
        .IIC_FREQ       (  20'd400_000     ),//parameter            IIC_FREQ  = 20'd400_000,   //I2c clock frequency
        .T_WR           (  10'd1           ),//parameter            T_WR      = 10'd5,         //I2c transmit delay ms
        .ADDR_BYTE      (  2'd2            ),//parameter            ADDR_BYTE = 2'd1,          //I2C addr byte number
        .LEN_WIDTH      (  8'd3            ),//parameter            LEN_WIDTH = 8'd3,          //I2C transmit byte width
        .DATA_BYTE      (  2'd1            ) //parameter            DATA_BYTE = 2'd1           //I2C data byte number
    )iic_dri(                       
        .clk            (  clk             ),//input                clk,
        .rstn           (  rstn            ),//input                rstn,
        .device_id      (  device_id       ),//input                device_id,
        .pluse          (  iic_trig        ),//input                pluse,                     //I2C transmit trigger
        .w_r            (  w_r             ),//input                w_r,                       //I2C transmit direction 1:send  0:receive
        .byte_len       (  4'd1            ),//input  [LEN_WIDTH:0] byte_len,                  //I2C transmit data byte length of once trigger
                   
        .addr           (  addr            ),//input  [7:0]         addr,                      //I2C transmit addr
        .data_in        (  data_in         ),//input  [7:0]         data_in,                   //I2C send data
                     
        .busy           (  busy            ),//output reg           busy=0,                    //I2C bus status
        
        .byte_over      (  byte_over       ),//output reg           byte_over=0,               //I2C byte transmit over flag               
        .data_out       (  data_out        ),//output reg[7:0]      data_out,                  //I2C receive data
                                           
        .scl            (  iic_scl         ),//output               scl,
        .sda_in         (  sda_in          ),//input                sda_in,
        .sda_out        (  sda_out         ),//output   reg         sda_out=1'b1,
        .sda_out_en     (  sda_out_en      ) //output               sda_out_en
    );
    
    assign iic_sda = sda_out_en ? sda_out : 1'bz;
    assign sda_in = iic_sda;
    
endmodule
