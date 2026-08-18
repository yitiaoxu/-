//////////////////////////////////////////////////////////////////////////////////
// Company: 能跑就能用
// Engineer:
//
// Create Date: 07/13/2026
// Design Name:
// Module Name: image_pcie_capture
// Project Name: HDMI PCIe Image Capture
// Target Devices: PG2L100H
// Tool Versions: Pango Design Suite
// Description: RK3568+PG2L100H PCIe 图像采集顶层
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

`define UD #1
//cmos1、cmos2二选一，作为视频源输入
//`define CMOS_1      //cmos1作为视频输入；
`define CMOS_2      //cmos2作为视频输入；
module image_pcie_capture #(

   parameter MEM_ROW_WIDTH        = 15         ,

   parameter MEM_COLUMN_WIDTH     = 10         ,

   parameter MEM_BANK_WIDTH       = 3          ,

  parameter MEM_DQ_WIDTH          = 16         ,

  parameter MEM_DQS_WIDTH         = 2

)(
    input                                free_clk                  ,
    input                                board_rst_n               ,

    //DDR3 interface
    output                               mem_cs_n                  ,  
    output                               mem_rst_n                 ,
    output                               mem_ck                    ,
    output                               mem_ck_n                  ,
    output                               mem_cke                   ,
    output                               mem_ras_n                 ,
    output                               mem_cas_n                 ,
    output                               mem_we_n                  ,
    output                               mem_odt                   ,
    output      [MEM_ROW_WIDTH-1:0]      mem_a                     ,
    output      [MEM_BANK_WIDTH-1:0]     mem_ba                    ,
    inout       [MEM_DQ_WIDTH/8-1:0]     mem_dqs                   ,
    inout       [MEM_DQ_WIDTH/8-1:0]     mem_dqs_n                 ,
    inout       [MEM_DQ_WIDTH-1:0]       mem_dq                    ,
    output      [MEM_DQ_WIDTH/8-1:0]     mem_dm                    ,

    // PCIe interface
    input					             ref_clk_p                 ,		    // 输入参考时钟 
	input					             ref_clk_n                 ,		    // 
	input					             perst_n                   ,		    //pcie复位
	input		[1:0]		             rxn                       ,			//pcie接收
	input		[1:0]		             rxp                       ,			//
	output wire	[1:0]		             txn                       ,			//pcie发送
	output wire	[1:0]		             txp                       ,			// 

    //LED signals
    output reg                           heart_beat_led            ,
    output reg                           pclk_led                  ,
	output reg                           ref_led                   ,

//OV5647
    output  [1:0]                        cmos_init_done             ,//OV5640寄存器初始化完成
    //coms1	        
    inout                                cmos1_scl                  ,//cmos1 i2c 
    inout                                cmos1_sda                  ,//cmos1 i2c 
    input                                cmos1_vsync                ,//cmos1 vsync
    input                                cmos1_href                 ,//cmos1 hsync refrence,data valid
    input                                cmos1_pclk                 ,//cmos1 pxiel clock
    input   [7:0]                        cmos1_data                 ,//cmos1 data
    output                               cmos1_reset                ,//cmos1 reset
    //coms2     
    inout                                cmos2_scl                  ,//cmos2 i2c 
    inout                                cmos2_sda                  ,//cmos2 i2c 
    input                                cmos2_vsync                ,//cmos2 vsync
    input                                cmos2_href                 ,//cmos2 hsync refrence,data valid
    input                                cmos2_pclk                 ,//cmos2 pxiel clock
    input   [7:0]                        cmos2_data                 ,//cmos2 data
    output                               cmos2_reset                , //cmos2 reset

   //HDMI_IN
    output                              hd_scl                     ,
    inout                               hd_sda                     ,
    output                              rx_rst_n                   ,
    input                               pixclk_in /*synthesis PAP_MARK_DEBUG="true"*/                 ,
    input                               vs_in     /*synthesis PAP_MARK_DEBUG="true"*/                 ,
    input                               hs_in     /*synthesis PAP_MARK_DEBUG="true"*/                 ,
    input                               de_in     /*synthesis PAP_MARK_DEBUG="true"*/                 ,
    input                [   7: 0]      r_in      /*synthesis PAP_MARK_DEBUG="true"*/                 ,
    input                [   7: 0]      g_in      /*synthesis PAP_MARK_DEBUG="true"*/                 ,
    input                [   7: 0]      b_in      /*synthesis PAP_MARK_DEBUG="true"*/            
);

parameter CTRL_ADDR_WIDTH = MEM_ROW_WIDTH + MEM_BANK_WIDTH + MEM_COLUMN_WIDTH;
parameter TH_1S = 27'd33000000;
parameter REM_DQS_WIDTH = 9 - MEM_DQS_WIDTH;


wire                        ddrphy_cpd_lock            ;
wire                        ddr_init_done              ;
wire                        pll_lock                   ;
wire                        core_clk                   ;
wire [CTRL_ADDR_WIDTH-1:0]  axi_awaddr                 ;
wire                        axi_awuser_ap              ;
wire [3:0]                  axi_awuser_id              ;
wire [3:0]                  axi_awlen                  ;
wire                        axi_awready                ;
wire                        axi_awvalid                ;
wire [MEM_DQ_WIDTH*8-1:0]   axi_wdata                  ;
wire [MEM_DQ_WIDTH*8/8-1:0] axi_wstrb                  ;
wire                        axi_wready                 ;
wire [3:0]                  axi_wusero_id              ;
wire                        axi_wusero_last            ;
wire [CTRL_ADDR_WIDTH-1:0]  axi_araddr                 ;
wire                        axi_aruser_ap              ;
wire [3:0]                  axi_aruser_id              ;
wire [3:0]                  axi_arlen                  ;
wire                        axi_arready                ;
wire                        axi_arvalid                ;
wire [MEM_DQ_WIDTH*8-1:0]   axi_rdata  /* synthesis syn_keep = 1 */;
wire                        axi_rvalid /* synthesis syn_keep = 1 */;
wire [3:0]                  axi_rid                    ;
wire                        axi_rlast                  ;
wire                        resetn                     ;
reg  [26:0]                 cnt                        ;
wire [7:0]                  err_cnt                    ;
wire                        free_clk_g                 ;

//cmos
reg  [15:0]                 rstn_1ms                   ;
wire                        cmos_scl                   ;//cmos i2c clock
wire                        cmos_sda                   ;//cmos i2c data
wire                        cmos_vsync                 ;//cmos vsync
wire                        cmos_href                  ;//cmos hsync refrence,data valid
wire                        cmos_pclk                  ;//cmos pxiel clock
wire   [7:0]                cmos_data                  ;//cmos data
wire                        cmos_reset                 ;//cmos reset
wire                        initial_en                 ;
wire[15:0]                  cmos1_d_16bit              /*synthesis PAP_MARK_DEBUG="1"*/;
wire                        cmos1_href_16bit           /*synthesis PAP_MARK_DEBUG="1"*/;
reg [7:0]                   cmos1_d_d0                 /*synthesis PAP_MARK_DEBUG="1"*/;
reg                         cmos1_href_d0              /*synthesis PAP_MARK_DEBUG="1"*/;
reg                         cmos1_vsync_d0             /*synthesis PAP_MARK_DEBUG="1"*/;
wire                        cmos1_pclk_16bit           /*synthesis PAP_MARK_DEBUG="1"*/;
wire[15:0]                  cmos2_d_16bit              /*synthesis PAP_MARK_DEBUG="1"*/;
wire                        cmos2_href_16bit           /*synthesis PAP_MARK_DEBUG="1"*/;
reg [7:0]                   cmos2_d_d0                 /*synthesis PAP_MARK_DEBUG="1"*/;
reg                         cmos2_href_d0              /*synthesis PAP_MARK_DEBUG="1"*/;
reg                         cmos2_vsync_d0             /*synthesis PAP_MARK_DEBUG="1"*/;
wire                        cmos2_pclk_16bit           /*synthesis PAP_MARK_DEBUG="1"*/;
wire[15:0]                  o_rgb565                   ;
wire                        pclk_in_test               ;    
wire                        vs_in_test                 ;
wire                        de_in_test                 ;
wire[15:0]                  i_rgb565                   ;
wire                        hdmi_vsync_eof             ;
reg                         de_in_d0                   /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [7:0]                  r_in_d0                    /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [7:0]                  g_in_d0                    /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [7:0]                  b_in_d0                    /*synthesis PAP_MARK_DEBUG="1"*/;
reg                         de_in_d1                   /*synthesis PAP_MARK_DEBUG="1"*/;
reg                         vs_in_d0                   /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [12:0]                 hdmi_line_pix_cnt          /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [12:0]                 hdmi_line_pix_cnt_latch    /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [12:0]                 hdmi_frame_line_cnt        /*synthesis PAP_MARK_DEBUG="1"*/;
reg  [12:0]                 hdmi_frame_line_cnt_latch  /*synthesis PAP_MARK_DEBUG="1"*/;

//pll
wire                        lock                       ;
wire                        clk_10m                    ;
wire                        clk_25m                    ;
wire                        clk_50m                    ;




//pcie 相关信号定义
localparam DEVICE_TYPE = 3'b000;			// @IPC enum 3'b000, 3'b001, 3'b100
localparam AXIS_SLAVE_NUM = 3;				// @IPC enum 1 2 3

//reg             ref_led;

// Test unit mode signals
wire			pcie_cfg_ctrl_en;			
wire			axis_master_tready_cfg;		

wire			cfg_axis_slave0_tvalid;		
wire	[127:0]	cfg_axis_slave0_tdata;		
wire			cfg_axis_slave0_tlast;		
wire			cfg_axis_slave0_tuser;		

// For mux
wire			axis_master_tready_mem;		
wire			axis_master_tvalid_mem;		
wire	[127:0]	axis_master_tdata_mem;		
wire	[3:0]	axis_master_tkeep_mem;		
											
wire			axis_master_tlast_mem;		
wire	[7:0]	axis_master_tuser_mem;		

wire			cross_4kb_boundary;			

wire			dma_axis_slave0_tvalid;		
wire	[127:0]	dma_axis_slave0_tdata;		
wire			dma_axis_slave0_tlast;		
wire			dma_axis_slave0_tuser;		

// Reset debounce and sync
wire			sync_button_rst_n; 			
wire			ref_core_rst_n;	
wire            sync_perst_n;			
wire			s_pclk_rstn;				

// Internal signal
wire			pclk_div2/*synthesis PAP_MARK_DEBUG="1"*/;  	// 用户时钟，x2 5gt/s时，为125MHZ 2.5gt/s时为62.5
wire			pclk/*synthesis PAP_MARK_DEBUG="1"*/;			// 用户时钟，x2 5gt/s时，为125MHZ 2.5gt/s时为62.5			
wire			ref_clk; 					
wire			core_rst_n;					

wire			axis_master_tvalid;
wire			axis_master_tready;
wire	[127:0]	axis_master_tdata;
wire	[3:0]	axis_master_tkeep;
wire			axis_master_tlast;
wire	[7:0]	axis_master_tuser;

// AXI4-Stream slave 0 interface
wire			axis_slave0_tready;
wire			axis_slave0_tvalid;
wire	[127:0]	axis_slave0_tdata;
wire			axis_slave0_tlast;
wire			axis_slave0_tuser;
// AXI4-Stream slave 1 interface
wire			axis_slave1_tready;
wire			axis_slave1_tvalid;
wire	[127:0]	axis_slave1_tdata;
wire			axis_slave1_tlast;
wire			axis_slave1_tuser;
// AXI4-Stream slave 2 interface
wire			axis_slave2_tready;
wire			axis_slave2_tvalid;
wire	[127:0]	axis_slave2_tdata;
wire			axis_slave2_tlast;
wire			axis_slave2_tuser;

wire	[7:0]	cfg_pbus_num;			
wire	[4:0]	cfg_pbus_dev_num; 		
wire	[2:0]	cfg_max_rd_req_size;	
wire	[2:0]	cfg_max_payload_size;	
wire			cfg_rcb;				

wire			cfg_ido_req_en;			
wire			cfg_ido_cpl_en;			
wire	[7:0]	xadm_ph_cdts;			
wire	[11:0]	xadm_pd_cdts;			
wire	[7:0]	xadm_nph_cdts;			
wire	[11:0]	xadm_npd_cdts;			
wire	[7:0]	xadm_cplh_cdts;			
wire	[11:0]	xadm_cpld_cdts;			

wire	[4:0]	smlh_ltssm_state/*synthesis PAP_MARK_DEBUG="1"*/;//link状态机

// Led lights up signal
reg		[22:0]	ref_led_cnt;		
reg		[26:0]	pclk_led_cnt;		
wire			smlh_link_up; 	
wire			rdlh_link_up/*synthesis PAP_MARK_DEBUG="1"*/; 	

// Uart to APB 32bits
wire			uart_p_sel;			
wire	[3:0]	uart_p_strb;		
wire	[15:0]	uart_p_addr;		
wire	[31:0]	uart_p_wdata;		
wire			uart_p_ce;			
wire			uart_p_we;			
wire			uart_p_rdy;			
wire	[31:0]	uart_p_rdata;		

// APB signal
wire	[3:0]	p_strb; 			
wire	[15:0]	p_addr; 			
wire	[31:0]	p_wdata; 			
wire			p_ce; 				
wire			p_we; 				

// APB MUX signal
// 0~5: HSSTLP 6: Reserved 7: PCIe
// 8: config
// 9: DMA
wire			p_sel_pcie;			
wire			p_sel_cfg;			
wire			p_sel_dma;			

wire	[31:0]	p_rdata_pcie;		
wire	[31:0]	p_rdata_cfg;		
wire	[31:0]	p_rdata_dma;		

wire			p_rdy_pcie;			
wire			p_rdy_cfg;			
wire			p_rdy_dma;			

assign cfg_ido_req_en	=	1'b0;	
assign cfg_ido_cpl_en	=	1'b0;	
assign xadm_ph_cdts		=	8'b0;	
assign xadm_pd_cdts		=	12'b0;	
assign xadm_nph_cdts	=	8'b0;	
assign xadm_npd_cdts	=	12'b0;	
assign xadm_cplh_cdts	=	8'b0;	
assign xadm_cpld_cdts	=	12'b0;	





//axi
reg           ch0_rframe_req;
wire          ch0_rframe_req_ack;
wire          ch0_rframe_data_en;
wire  [127:0] ch0_rframe_data;
wire          ch0_rframe_data_valid;

//dma
// DMA CTRL      BASE ADDR = 0x8000
wire            o_dma_write_data_req;
wire [11:0]     o_dma_write_addr ;
wire [127:0]    i_dma_write_data;

wire            cmos2_pclk_bufg;


reg             cmos2_vsync_d1;
reg             cmos2_vsync_d2;
reg             cmos2_href_d1;
reg             cmos2_href_d2;
reg   [7:0]     cmos2_data_d1;
reg   [7:0]     cmos2_data_d2;



always @(posedge cmos2_pclk)begin
    cmos2_vsync_d1 <= cmos2_vsync;
    cmos2_vsync_d2 <= cmos2_vsync_d1;
    cmos2_href_d1 <= cmos2_href;
    cmos2_href_d2  <= cmos2_href_d1;
    cmos2_data_d1 <= cmos2_data;
    cmos2_data_d2 <= cmos2_data_d1;
end

//*==============================================================================
//PLL
//*==============================================================================
pll pll_inst (
  .clkout0(clk_10m),    // output
  .clkout1(clk_25m),    // output
  .clkout2(clk_50m),    // output
  .lock(lock),          // output
  .clkin1(free_clk)       // input
);

// GTP_CLKBUFR U_CLKBUFR ( 
// .CLKOUT     (cmos2_pclk_bufg), 
// .CLKIN      (cmos2_pclk)
// ); 

//*==============================================================================
// 配置HDMI
//*==============================================================================
wire rstn_out;

ms72xx_ctl #(
    .MS7210_EN                          (1'b0                      )
) ms72xx_ctl(
    .clk                                (clk_10m                   ),//input       clk,
    .rst_n                              (rstn_out                  ),//input       rstn,
           
    .init_over_rx                       (rx_init_done              ),
    .init_over                          (init_over                 ),//output      init_over,
    .iic_scl                            (hd_scl                    ),//output      iic_scl,
    .iic_sda                            (hd_sda                    ) //inout       iic_sda
);


always @(posedge clk_10m)
begin
	if(!lock)
	    rstn_1ms <= 16'd0;
	else
	begin
		if(rstn_1ms == 16'h2710)
		    rstn_1ms <= rstn_1ms;
		else
		    rstn_1ms <= rstn_1ms + 1'b1;
	end
end
assign rstn_out = (rstn_1ms == 16'h2710);
assign rx_rst_n = rstn_out;
assign hdmi_vsync_eof = ~vs_in;

always @(posedge pixclk_in or negedge rstn_out) begin
    if (!rstn_out) begin
        de_in_d0 <= 1'b0;
        r_in_d0 <= 8'd0;
        g_in_d0 <= 8'd0;
        b_in_d0 <= 8'd0;
        de_in_d1 <= 1'b0;
        vs_in_d0 <= 1'b0;
        hdmi_line_pix_cnt <= 13'd0;
        hdmi_line_pix_cnt_latch <= 13'd0;
        hdmi_frame_line_cnt <= 13'd0;
        hdmi_frame_line_cnt_latch <= 13'd0;
    end else begin
        de_in_d0 <= de_in;
        r_in_d0 <= r_in;
        g_in_d0 <= g_in;
        b_in_d0 <= b_in;
        de_in_d1 <= de_in_d0;
        vs_in_d0 <= vs_in;

        if (!vs_in_d0 && vs_in) begin
            hdmi_frame_line_cnt_latch <= hdmi_frame_line_cnt;
        end

        if (vs_in) begin
            hdmi_line_pix_cnt <= 13'd0;
            hdmi_frame_line_cnt <= 13'd0;
        end else begin
            if (de_in) begin
                hdmi_line_pix_cnt <= hdmi_line_pix_cnt + 1'b1;
            end else begin
                hdmi_line_pix_cnt <= 13'd0;
            end

            if (de_in_d1 && !de_in_d0) begin
                hdmi_line_pix_cnt_latch <= hdmi_line_pix_cnt;
                hdmi_frame_line_cnt <= hdmi_frame_line_cnt + 1'b1;
            end
        end
    end
end
//*==============================================================================
//配置cmos
//*==============================================================================
// ddr_init_done: ddrphy_sysclk -> clk_50m CDC
reg [1:0] ddr_init_done_sync_50m_ff;
always @(posedge clk_50m) begin
    ddr_init_done_sync_50m_ff <= {ddr_init_done_sync_50m_ff[0], ddr_init_done};
end
wire ddr_init_done_sync_50m = ddr_init_done_sync_50m_ff[1];

//OV5640 register configure enable    
power_on_delay	power_on_delay_inst(
    .clk_50M                 (clk_50m        ),//input
    .reset_n                 (ddr_init_done_sync_50m),//input	
    .camera1_rstn            (cmos1_reset    ),//output
    .camera2_rstn            (cmos2_reset    ),//output	
    .camera_pwnd             (               ),//output
    .initial_en              (initial_en     ) //output		
);
//CMOS1 Camera 
reg_config	coms1_reg_config(
    .clk_25M                 (clk_50m            ),//input
    .camera_rstn             (cmos1_reset        ),//input
    .initial_en              (initial_en         ),//input		
    .i2c_sclk                (cmos1_scl          ),//output
    .i2c_sdat                (cmos1_sda          ),//inout
    .reg_conf_done           (cmos_init_done[0]  ),//output config_finished
    .reg_index               (                   ),//output reg [8:0]
    .clock_20k               (                   ) //output reg
);

//CMOS2 Camera 
reg_config	coms2_reg_config(
    .clk_25M                 (clk_50m            ),//input
    .camera_rstn             (cmos2_reset        ),//input
    .initial_en              (initial_en         ),//input		
    .i2c_sclk                (cmos2_scl          ),//output
    .i2c_sdat                (cmos2_sda          ),//inout
    .reg_conf_done           (cmos_init_done[1]  ),//output config_finished
    .reg_index               (                   ),//output reg [8:0]
    .clock_20k               (                   ) //output reg
);


//===============================================================================
//CMOS 8bit转16bit
//===============================================================================
//CMOS1
// always@(posedge cmos1_pclk)
//     begin
//         cmos1_d_d0        <= cmos1_data    ;
//         cmos1_href_d0     <= cmos1_href    ;
//         cmos1_vsync_d0    <= cmos1_vsync   ;
//     end

// cmos_8_16bit cmos1_8_16bit(
// .pclk           (cmos1_pclk       ),//input
// .rst_n          (cmos_init_done[0]),//input
// .pdata_i        (cmos1_d_d0       ),//input[7:0]
// .de_i           (cmos1_href_d0    ),//input
// .vs_i           (cmos1_vsync_d0    ),//input

// .pixel_clk      (cmos1_pclk_16bit ),//output
// .pdata_o        (cmos1_d_16bit    ),//output[15:0]
// .de_o           (cmos1_href_16bit ) //output
// );

//CMOS2
always@(posedge cmos2_pclk)
    begin
        cmos2_d_d0        <= cmos2_data_d2    ;
        cmos2_href_d0     <= cmos2_href_d2    ;
        cmos2_vsync_d0    <= cmos2_vsync_d2   ;
    end

cmos_8_16bit cmos2_8_16bit(
.pclk           (cmos2_pclk       ),//input
.rst_n          (cmos_init_done[1]),//input
.pdata_i        (cmos2_d_d0       ),//input[7:0]
.de_i           (cmos2_href_d0    ),//input
.vs_i           (cmos2_vsync_d0    ),//input

.pixel_clk      (cmos2_pclk_16bit ),//output
.pdata_o        (cmos2_d_16bit    ),//output[15:0]
.de_o           (cmos2_href_16bit ) //output
);


//===============================================================================
// 输出视频选择
//===============================================================================
`ifdef CMOS_1
assign     pclk_in_test    =    cmos1_pclk_16bit    ;
assign     vs_in_test      =    cmos1_vsync_d0      ;
assign     de_in_test      =    cmos1_href_16bit    ;
assign     i_rgb565        =    {cmos1_d_16bit[4:0],cmos1_d_16bit[10:5],cmos1_d_16bit[15:11]};//{r,g,b}
`elsif CMOS_2
assign     pclk_in_test    =    cmos2_pclk_16bit    ;
assign     vs_in_test      =    cmos2_vsync_d0      ;
assign     de_in_test      =    cmos2_href_16bit    ;
assign     i_rgb565        =    {cmos2_d_16bit[4:0],cmos2_d_16bit[10:5],cmos2_d_16bit[15:11]};//{r,g,b}
`endif

//*==============================================================================
//ddr3 IP例化
//*==============================================================================
// Register ddr_init_done on core_clk to reduce fanout and hold pressure
reg ddr_init_done_core;
always @(posedge core_clk) begin
    ddr_init_done_core <= ddr_init_done;
end

always@(posedge core_clk)
begin
   if (!ddr_init_done_core)
      cnt <= 27'd0;
   else if ( cnt >= TH_1S )
      cnt <= 27'd0;
   else
      cnt <= cnt + 27'd1;
end

always @(posedge core_clk)
begin
   if (!ddr_init_done_core)
      heart_beat_led <= 1'd1;
   else if ( cnt >= TH_1S )
      heart_beat_led <= ~heart_beat_led;
end
ddr3 #(
    .MEM_ROW_WIDTH              (MEM_ROW_WIDTH                ),
    .MEM_COLUMN_WIDTH           (MEM_COLUMN_WIDTH             ),
    .MEM_BANK_WIDTH             (MEM_BANK_WIDTH               ),
    .MEM_DQ_WIDTH               (MEM_DQ_WIDTH                 ),
    .MEM_DM_WIDTH               (MEM_DQS_WIDTH                ),
    .MEM_DQS_WIDTH              (MEM_DQS_WIDTH                ),
    .CTRL_ADDR_WIDTH            (CTRL_ADDR_WIDTH              )
  )I_ips_ddr_top(
    .ref_clk                    (free_clk                     ),
    .resetn                     (board_rst_n                  ),
    .core_clk                   (core_clk                     ),
    .pll_lock                   (pll_lock                     ),
    .phy_pll_lock               (phy_pll_lock                 ),
    .gpll_lock                  (gpll_lock                    ),
    .rst_gpll_lock              (rst_gpll_lock                ),
    .ddrphy_cpd_lock            (ddrphy_cpd_lock              ),
    .ddr_init_done              (ddr_init_done                ),

    .axi_awaddr                 (axi_awaddr                   ),
    .axi_awuser_ap              (axi_awuser_ap                ),
    .axi_awuser_id              (axi_awuser_id                ),
    .axi_awlen                  (axi_awlen                    ),
    .axi_awready                (axi_awready                  ),
    .axi_awvalid                (axi_awvalid                  ),

    .axi_wdata                  (axi_wdata                    ),
    .axi_wstrb                  (axi_wstrb                    ),
    .axi_wready                 (axi_wready                   ),
    .axi_wusero_id              (axi_wusero_id                ),
    .axi_wusero_last            (axi_wusero_last              ),

    .axi_araddr                 (axi_araddr                   ),
    .axi_aruser_ap              (axi_aruser_ap                ),
    .axi_aruser_id              (axi_aruser_id                ),
    .axi_arlen                  (axi_arlen                    ),
    .axi_arready                (axi_arready                  ),
    .axi_arvalid                (axi_arvalid                  ),

    .axi_rdata                  (axi_rdata                    ),
    .axi_rid                    (axi_rid                      ),
    .axi_rlast                  (axi_rlast                    ),
    .axi_rvalid                 (axi_rvalid                   ),

    .apb_clk                    (1'b0                         ),
    .apb_rst_n                  (1'b0                         ),
    .apb_sel                    (1'b0                         ),
    .apb_enable                 (1'b0                         ),
    .apb_addr                   (8'd0                         ),
    .apb_write                  (1'b0                         ),
    .apb_ready                  (                             ),
    .apb_wdata                  (16'd0                        ),
    .apb_rdata                  (                             ),


    .mem_cs_n                   (mem_cs_n                     ),

    .mem_rst_n                  (mem_rst_n                    ),
    .mem_ck                     (mem_ck                       ),
    .mem_ck_n                   (mem_ck_n                     ),
    .mem_cke                    (mem_cke                      ),
    .mem_ras_n                  (mem_ras_n                    ),
    .mem_cas_n                  (mem_cas_n                    ),
    .mem_we_n                   (mem_we_n                     ),
    .mem_odt                    (mem_odt                      ),
    .mem_a                      (mem_a                        ),
    .mem_ba                     (mem_ba                       ),
    .mem_dqs                    (mem_dqs                      ),
    .mem_dqs_n                  (mem_dqs_n                    ),
    .mem_dq                     (mem_dq                       ),
    .mem_dm                     (mem_dm                       ),

    //debug
    .dbg_gate_start             (1'b0                         ),
    .dbg_cpd_start              (1'b0                         ),
    .dbg_ddrphy_rst_n           (1'b1                         ),
    .dbg_gpll_scan_rst          (1'b0                         ),

    .samp_position_dyn_adj      (1'b0                         ),
    .init_samp_position_even    (16'd0                        ),
    .init_samp_position_odd     (16'd0                        ),

    .wrcal_position_dyn_adj     (1'b0                         ),
    .init_wrcal_position        (16'd0                        ),

    .force_read_clk_ctrl        (1'b0                         ),
    .init_slip_step             (8'd0                         ),
    .init_read_clk_ctrl         (6'd0                         ),

    .debug_calib_ctrl           (                             ),
    .dbg_dll_upd_state          (                             ),
    .dbg_slice_status           (                             ),
    .dbg_slice_state            (                             ),
    .debug_data                 (                             ),
    .debug_gpll_dps_phase       (                             ),

    .dbg_rst_dps_state          (                             ),
    .dbg_tran_err_rst_cnt       (                             ),
    .dbg_ddrphy_init_fail       (                             ),

    .debug_cpd_offset_adj       (1'b0                         ),
    .debug_cpd_offset_dir       (1'b0                         ),
    .debug_cpd_offset           (10'd0                        ),
    .debug_dps_cnt_dir0         (                             ),
    .debug_dps_cnt_dir1         (                             ),

    .ck_dly_en                  (1'b0                         ),
    .init_ck_dly_step           (8'd0                         ),
    .ck_dly_set_bin             (                             ),

    .align_error                (                             ),
    .debug_rst_state            (                             ),
    .debug_cpd_state            (                             )

  );



//*==============================================================================
//axi控制器例化
//*==============================================================================
axi4_ctrl #(.C_RD_END_ADDR(`VIDEO_FRAME_BYTES), .C_W_WIDTH(16), .C_R_WIDTH(128), .C_ID_LEN(4)) u_axi4_ctrl (   // RGB565: C_RD_END_ADDR = width*height*2

    .axi_clk                    (core_clk                                 ),  	//DdrCtrl
    .axi_reset                  (~ddr_init_done_core                      ),	//DdrCtrl

    .axi_awaddr                 (axi_awaddr                               ),	//AXI4_AWARMux
    .axi_awlen                  (axi_awlen                                ),	//AXI4_AWARMux
    .axi_awvalid                (axi_awvalid                              ),	//AXI4_AWARMux
    .axi_awready                (axi_awready                              ),	//AXI4_AWARMux
                        
    .axi_wdata                  (axi_wdata                                ),	//DdrCtrl
    .axi_wstrb                  (axi_wstrb                                ),	//DdrCtrl
    .axi_wlast                  (axi_wusero_last                          ),	//DdrCtrl
    .axi_wvalid                 (                                         ),	//DdrCtrl
    .axi_wready                 (axi_wready                               ),	//DdrCtrl

    .axi_bid                    (0                                        ),
    .axi_bresp                  (0                                        ),
    .axi_bvalid                 (1                                        ),

    .axi_arid                   (axi_aruser_id                            ),	//AXI4_ARARMux
    .axi_araddr                 (axi_araddr                               ),	//AXI4_ARARMux
    .axi_arlen                  (axi_arlen                                ),	//AXI4_ARARMux
    .axi_arvalid                (axi_arvalid                              ),	//AXI4_ARARMux
    .axi_arready                (axi_arready                              ),	//AXI4_ARARMux
                    
    .axi_rid                    (axi_rid                                  ),	//DdrCtrl
    .axi_rdata                  (axi_rdata                                ),	//DdrCtrl
    .axi_rresp                  (0                                        ),
    .axi_rlast                  (axi_rlast                                ),	//DdrCtrl
    .axi_rvalid                 (axi_rvalid                               ),	//DdrCtrl
    .axi_rready                 (                                         ),	//DdrCtrl

    .wframe_pclk                (pixclk_in                                ),		//	Writter User Pclk.  (像素时钟) ),
    .wframe_vsync               (hdmi_vsync_eof                           ), // axi4_ctrl expects EOF on VS falling edge
    .wframe_data_en             (de_in_d0                                 ),
    .wframe_data                ({r_in_d0[7:3],g_in_d0[7:2],b_in_d0[7:3]}),

    .rframe_pclk                (pclk_div2                                ),
    .rframe_vsync               (ch0_rframe_req                           ),		//	Reader VSync. Flush on rising edge. Connect to ~EOF. 
    .rframe_data_en             (o_dma_write_data_req                     ),
    .rframe_data                (i_dma_write_data                         ),
                    
    .tp_o 		                (                                         )
);


//*==============================================================================
// pcie
//*==============================================================================
// Rst debounce
hsst_rst_cross_sync_v1_0 #(
    `ifdef IPS2L_PCIE_SPEEDUP_SIM
    .RST_CNTR_VALUE     (16'h10             )
    `else
    .RST_CNTR_VALUE     (16'hC000           )
    `endif
)
u_refclk_buttonrstn_debounce(
    .clk                (ref_clk            ),
    .rstn_in            (board_rst_n        ),
    .rstn_out           (sync_button_rst_n  )
);

hsst_rst_cross_sync_v1_0 #(
    `ifdef IPS2L_PCIE_SPEEDUP_SIM
    .RST_CNTR_VALUE     (16'h10             )
    `else
    .RST_CNTR_VALUE     (16'hC000           )
    `endif
)
u_refclk_perstn_debounce(
    .clk                (ref_clk            ),
    .rstn_in            (perst_n            ),
    .rstn_out           (sync_perst_n       )
);

hsst_rst_sync_v1_0  u_ref_core_rstn_sync    (
    .clk                (ref_clk            ),
    .rst_n              (core_rst_n         ),
    .sig_async          (1'b1               ),
    .sig_synced         (ref_core_rst_n     )
);

hsst_rst_sync_v1_0  u_pclk_core_rstn_sync   (
    .clk                (pclk               ),
    .rst_n              (core_rst_n         ),
    .sig_async          (1'b1               ),
    .sig_synced         (s_pclk_rstn        )
);

always @(posedge ref_clk or negedge sync_perst_n) begin
	if (!sync_perst_n) begin
		ref_led_cnt <= 23'd0;
		ref_led <= 1'b1;
	end else if (smlh_link_up & rdlh_link_up) begin
		ref_led_cnt <= ref_led_cnt + 23'd1;
		if(&ref_led_cnt)
			ref_led <= ~ref_led;
	end
end

always @(posedge pclk or negedge s_pclk_rstn) begin
	if (!s_pclk_rstn) begin
		pclk_led_cnt <= 27'd0;
		pclk_led <= 1'b1;
	end else if (smlh_link_up & rdlh_link_up) begin
		pclk_led_cnt <= pclk_led_cnt + 27'd1;
		if(&pclk_led_cnt)
			pclk_led <= ~pclk_led;
	end
end


//===========================================================================
//pcie dma
//===========================================================================
// DMA CTRL      BASE ADDR = 0x8000
ips2l_pcie_dma #(
	.DEVICE_TYPE			(DEVICE_TYPE),
	.AXIS_SLAVE_NUM			(AXIS_SLAVE_NUM)
) u_ips2l_pcie_dma (
	.clk					(pclk_div2),				
	.rst_n					(core_rst_n),				

	// Num
	.i_cfg_pbus_num			(cfg_pbus_num),				
	.i_cfg_pbus_dev_num		(cfg_pbus_dev_num),			
	.i_cfg_max_rd_req_size	(cfg_max_rd_req_size),		
	.i_cfg_max_payload_size	(cfg_max_payload_size),		

	// AXI4-Stream master interface
	.i_axis_master_tvld		(axis_master_tvalid_mem),	
	.o_axis_master_trdy		(axis_master_tready_mem),	
	.i_axis_master_tdata	(axis_master_tdata_mem),	
	.i_axis_master_tkeep	(axis_master_tkeep_mem),	
														
	.i_axis_master_tlast	(axis_master_tlast_mem),	
	.i_axis_master_tuser	(axis_master_tuser_mem),	

	// AXI4-Stream slave0 interface
	.i_axis_slave0_trdy		(axis_slave0_tready),		
	.o_axis_slave0_tvld		(dma_axis_slave0_tvalid),	
	.o_axis_slave0_tdata	(dma_axis_slave0_tdata),	
	.o_axis_slave0_tlast	(dma_axis_slave0_tlast),	
	.o_axis_slave0_tuser	(dma_axis_slave0_tuser),	

	// AXI4-Stream slave1 interface
	.i_axis_slave1_trdy		(axis_slave1_tready),		
	.o_axis_slave1_tvld		(axis_slave1_tvalid),		
	.o_axis_slave1_tdata	(axis_slave1_tdata),		
	.o_axis_slave1_tlast	(axis_slave1_tlast),		
	.o_axis_slave1_tuser	(axis_slave1_tuser),		

	// AXI4-Stream slave2 interface
	.i_axis_slave2_trdy		(axis_slave2_tready),		
	.o_axis_slave2_tvld		(axis_slave2_tvalid),		
	.o_axis_slave2_tdata	(axis_slave2_tdata),		
	.o_axis_slave2_tlast	(axis_slave2_tlast),		
	.o_axis_slave2_tuser	(axis_slave2_tuser),		

	// From pcie
	.i_cfg_ido_req_en		(cfg_ido_req_en),			
	.i_cfg_ido_cpl_en		(cfg_ido_cpl_en),			
	.i_xadm_ph_cdts			(xadm_ph_cdts),				
	.i_xadm_pd_cdts			(xadm_pd_cdts),				
	.i_xadm_nph_cdts		(xadm_nph_cdts),			
	.i_xadm_npd_cdts		(xadm_npd_cdts),			
	.i_xadm_cplh_cdts		(xadm_cplh_cdts),			
	.i_xadm_cpld_cdts		(xadm_cpld_cdts),			

	// APB interface
	.i_apb_psel				(p_sel_dma),				
	.i_apb_paddr			(p_addr[8:0]),				
	.i_apb_pwdata			(p_wdata),					
	.i_apb_pstrb			(p_strb),					
	.i_apb_pwrite			(p_we),						
	.i_apb_penable			(p_ce),						
	.o_apb_prdy				(p_rdy_dma),				
	.o_apb_prdata			(p_rdata_dma),				
	.o_cross_4kb_boundary	(cross_4kb_boundary),		//4k边界
    //**********************************************************************
    // dma write interface
    .o_dma_write_data_req   (o_dma_write_data_req  ),
    .o_dma_write_addr       (o_dma_write_addr      ),
    .i_dma_write_data       (i_dma_write_data      )
);



assign p_rdy_cfg               = 1'b0;
assign p_rdata_cfg             = 32'b0;

assign axis_slave0_tvalid      = dma_axis_slave0_tvalid;
assign axis_slave0_tlast       = dma_axis_slave0_tlast;
assign axis_slave0_tuser       = dma_axis_slave0_tuser;
assign axis_slave0_tdata       = dma_axis_slave0_tdata;

assign axis_master_tvalid_mem  = axis_master_tvalid;
assign axis_master_tdata_mem   = axis_master_tdata;
assign axis_master_tkeep_mem   = axis_master_tkeep;
assign axis_master_tlast_mem   = axis_master_tlast;
assign axis_master_tuser_mem   = axis_master_tuser;

assign axis_master_tready      = axis_master_tready_mem;



// PCIe IP TOP : HSSTLP : 0x0000~6000 PCIe BASE ADDR : 0x7000
pcie_test u_ips2l_pcie_wrap (
	.button_rst_n				(1'b1),	
	.power_up_rst_n				(1'b1),			
	.perst_n					(1'b1),			

	// The clock and reset signals
	.pclk						(pclk),					
	.pclk_div2					(pclk_div2),			
	.ref_clk					(ref_clk),				
	.ref_clk_n					(ref_clk_n),			
	.ref_clk_p					(ref_clk_p),			
	.core_rst_n					(core_rst_n),			

	// APB interface to DBI config
	.p_sel						(p_sel_pcie),			
	.p_strb						(uart_p_strb),			
	.p_addr						(uart_p_addr),			
	.p_wdata					(uart_p_wdata),			
	.p_ce						(uart_p_ce),			
	.p_we						(uart_p_we),			
	.p_rdy						(p_rdy_pcie),			
	.p_rdata					(p_rdata_pcie),			

	// PHY diff signals
	.rxn						(rxn),					
	.rxp						(rxp),					
	.txn						(txn),					
	.txp						(txp),					
	.pcs_nearend_loop			({4{1'b0}}),			
	.pma_nearend_ploop			({4{1'b0}}),			
	.pma_nearend_sloop			({4{1'b0}}),			

	// AXI4-Stream master interface
	.axis_master_tvalid			(axis_master_tvalid),	
	.axis_master_tready			(axis_master_tready),	
	.axis_master_tdata			(axis_master_tdata),	
	.axis_master_tkeep			(axis_master_tkeep),	
														
	.axis_master_tlast			(axis_master_tlast),	
	.axis_master_tuser			(axis_master_tuser),	

	// AXI4-Stream slave 0 interface
	.axis_slave0_tready			(axis_slave0_tready),	
	.axis_slave0_tvalid			(axis_slave0_tvalid),	
	.axis_slave0_tdata			(axis_slave0_tdata),	
	.axis_slave0_tlast			(axis_slave0_tlast),	
	.axis_slave0_tuser			(axis_slave0_tuser),	

	// AXI4-Stream slave 1 interface
	.axis_slave1_tready			(axis_slave1_tready),	
	.axis_slave1_tvalid			(axis_slave1_tvalid),	
	.axis_slave1_tdata			(axis_slave1_tdata),	
	.axis_slave1_tlast			(axis_slave1_tlast),	
	.axis_slave1_tuser			(axis_slave1_tuser),	

	// AXI4-Stream slave 2 interface
	.axis_slave2_tready			(axis_slave2_tready),	
	.axis_slave2_tvalid			(axis_slave2_tvalid),	
	.axis_slave2_tdata			(axis_slave2_tdata),	
	.axis_slave2_tlast			(axis_slave2_tlast),	
	.axis_slave2_tuser			(axis_slave2_tuser),	

	.pm_xtlh_block_tlp			(),						

	.cfg_send_cor_err_mux		(),						
	.cfg_send_nf_err_mux		(),						
	.cfg_send_f_err_mux			(),						
	.cfg_sys_err_rc				(),						
	.cfg_aer_rc_err_mux			(),						

	// The radm timeout
	.radm_cpl_timeout			(),						

	// Configuration signals
	.cfg_max_rd_req_size		(cfg_max_rd_req_size),	
	.cfg_bus_master_en			(),						
	.cfg_max_payload_size		(cfg_max_payload_size),	
	.cfg_ext_tag_en				(),						
	.cfg_rcb					(cfg_rcb),				
	.cfg_mem_space_en			(),						
	.cfg_pm_no_soft_rst			(),						
	.cfg_crs_sw_vis_en			(),						
	.cfg_no_snoop_en			(),						
	.cfg_relax_order_en			(),						
	.cfg_tph_req_en				(),						
	.cfg_pf_tph_st_mode			(),						
	.rbar_ctrl_update			(),						
	.cfg_atomic_req_en			(),						

	.cfg_pbus_num				(cfg_pbus_num),			
	.cfg_pbus_dev_num			(cfg_pbus_dev_num),		

	// Debug signals
	.radm_idle					(),						
	.radm_q_not_empty			(),						
	.radm_qoverflow				(),						
	.diag_ctrl_bus				(2'b0),					
	.cfg_link_auto_bw_mux		(),						
	.cfg_bw_mgt_mux				(),						
	.cfg_pme_mux				(),						
	.app_ras_des_sd_hold_ltssm	(1'b0),					
	.app_ras_des_tba_ctrl		(2'b0),					

	.dyn_debug_info_sel			(4'b0),					
	.debug_info_mux				(),

	// System signal
	.smlh_link_up				(smlh_link_up),			//link状态
	.rdlh_link_up				(rdlh_link_up),			//link状态
	.smlh_ltssm_state			(smlh_ltssm_state)
);



//=======================
reg  [11:0]  o_dma_write_addr_dly1;
reg  [11:0]  o_dma_write_addr_dly2;
reg  [11:0]  dma_write_cnt;  // 计数dma_write的次数

reg [1:0] ddr_init_done_sync_pclk_ff;
always @(posedge pclk_div2) begin
    ddr_init_done_sync_pclk_ff <= {ddr_init_done_sync_pclk_ff[0], ddr_init_done};
end
wire ddr_init_done_pclk = ddr_init_done_sync_pclk_ff[1];

always @(posedge pclk_div2) begin
    if (!ddr_init_done_pclk) begin
        o_dma_write_addr_dly1 <= 12'd0;
        o_dma_write_addr_dly2 <= 12'd0;
    end else begin
        o_dma_write_addr_dly1 <= o_dma_write_addr;
        o_dma_write_addr_dly2 <= o_dma_write_addr_dly1;
    end
end


always @(posedge pclk_div2) begin
    if (!ddr_init_done_pclk) begin
        dma_write_cnt <= 12'd0;
    end else if (o_dma_write_addr_dly1 == `VIDEO_LINE_LAST_BEAT && o_dma_write_addr_dly2 == `VIDEO_LINE_PREV_BEAT) begin
        dma_write_cnt <= dma_write_cnt + 1'b1;
    end
    else if (dma_write_cnt == `VIDEO_FRAME_LINES)begin
        dma_write_cnt <= 12'd0;
    end
    else begin
        dma_write_cnt <= dma_write_cnt;
    end
end

always @(posedge pclk_div2) begin
    if (!ddr_init_done_pclk) begin
        ch0_rframe_req <=1'b0;
    end else if (dma_write_cnt == `VIDEO_FRAME_LINES) begin
       ch0_rframe_req <= 1'b1;
    end
    else begin
        ch0_rframe_req <= 1'b0;
    end
end





endmodule
