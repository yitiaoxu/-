`timescale 1ns / 1ps
// 单路：cmos2 -> ch1 -> DDR -> PCIe，1280x720
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

//OV5647
    output  [2:0]                        cmos_init_done             ,//OV5640寄存器初始化完成
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
    output                               cmos2_reset                ,//cmos2 reset
    //cmos3
    inout                                cmos3_scl                  ,//cmos3 i2c 
    inout                                cmos3_sda                  ,//cmos3 i2c 
    input                                cmos3_vsync                ,//cmos3 vsync
    input                                cmos3_href                 ,//cmos3 hsync refrence,data valid
    input                                cmos3_pclk                 ,//cmos3 pxiel clock
    input   [7:0]                        cmos3_data                 ,//cmos3 data
    output                               cmos3_reset                 ,//cmos3 reset
    // UART: 与 pango_pcie_top 同协议，经 uart2apb + expd 访问 0x8000(DMA) / 0xA000(ROI) 等
    output  wire                         uart_txd                    ,
    input   wire                         uart_rxd
);

parameter CTRL_ADDR_WIDTH = MEM_ROW_WIDTH + MEM_BANK_WIDTH + MEM_COLUMN_WIDTH;

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
wire                        free_clk_g                 ;

wire                        initial_en                 ;
wire                        initial_en_1               ;
wire[15:0]                  cmos1_d_16bit              ;
wire                        cmos1_href_16bit           ;
reg [7:0]                   cmos1_d_d0                 ;
reg                         cmos1_href_d0              ;
reg                         cmos1_vsync_d0             ;
wire                        cmos1_pclk_16bit           ;
wire[15:0]                  cmos2_d_16bit              ;
wire                        cmos2_href_16bit           ;
reg [7:0]                   cmos2_d_d0                 ;
reg                         cmos2_href_d0              ;
reg                         cmos2_vsync_d0             ;
wire                        cmos2_pclk_16bit           ;
wire[15:0]                  cmos3_d_16bit              ;
wire                        cmos3_href_16bit           ;
reg [7:0]                   cmos3_d_d0                 ;
reg                         cmos3_href_d0              ;
reg                         cmos3_vsync_d0             ;
wire                        cmos3_pclk_16bit           ;

wire[15:0]                  o_rgb565                   ;
wire                        pclk_in_test               ;    
wire                        vs_in_test                 ;
wire                        de_in_test                 ;
wire[15:0]                  i_rgb565                   ;
wire                        pclk_in_test_2             ;    
wire                        vs_in_test_2               ;
wire                        de_in_test_2               ;
wire[15:0]                  i_rgb565_2                 ;
wire                        pclk_in_test_3             ;    
wire                        vs_in_test_3               ;
wire                        de_in_test_3               ;
wire[15:0]                  i_rgb565_3                 ;

//pll
wire                        lock                       ;
wire                        clk_10m                    ;
wire                        clk_25m                    ;
wire                        clk_50m                    ;
reg  [1:0]                 ddr_init_done_sync_50m     ;
wire                        ddr_init_done_50m          ;




//pcie 相关信号定义
localparam DEVICE_TYPE = 3'b000;			// @IPC enum 3'b000, 3'b001, 3'b100
localparam AXIS_SLAVE_NUM = 3;				// @IPC enum 1 2 3

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
wire			pclk_div2;
wire			pclk;
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

wire	[4:0]	smlh_ltssm_state;
wire			smlh_link_up; 	
wire			rdlh_link_up;

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

// APB MUX signal（经 ips2l_expd_apb_mux 后 pclk_div2 域选通）
// 0~7: PCIe-DBI；0x8xxx: DMA 控制；0x9xxx: cfg；0xAxxx: CH1 ROI
wire			p_sel_pcie;			
wire			p_sel_cfg;			
wire			p_sel_dma;			
wire			p_sel_roi;			

wire	[31:0]	p_rdata_pcie;		
wire	[31:0]	p_rdata_cfg;		
wire	[31:0]	p_rdata_dma;		
wire	[31:0]	p_rdata_roi;		

wire			p_rdy_pcie;			
wire			p_rdy_cfg;			
wire			p_rdy_dma;			
wire			p_rdy_roi;			

assign cfg_ido_req_en	=	1'b0;	
assign cfg_ido_cpl_en	=	1'b0;	
assign xadm_ph_cdts		=	8'b0;	
assign xadm_pd_cdts		=	12'b0;	
assign xadm_nph_cdts	=	8'b0;	
assign xadm_npd_cdts	=	12'b0;	
assign xadm_cplh_cdts	=	8'b0;	
assign xadm_cpld_cdts	=	12'b0;

//axi
wire          ch0_wframe_data_valid      ;
wire [15:0]   ch0_wframe_data            ;
// 单路 cmos2->ch1 工程: DDR 读帧请求（仅驱动 ch1 读口）
reg           ch1_rd_frame_req;
wire          ch0_rframe_data_en;
wire  [127:0] ch0_rframe_data;
wire          ch0_rframe_data_valid;

// CH1：reshape 后直送 DDR；ROI 框取自同源像素流（ch1_roi_bbox_simple）
localparam integer CH1_ROI_FG_DE_ONLY = 1;
localparam integer CH1_ROI_VS_INVERT   = 0;
localparam integer CH1_ROI_FG_FORCE_ON = 1;
wire          ch1_reshape_vs;
wire          ch1_reshape_de;
wire [15:0]   ch1_reshape_data;
wire          ch1_wframe_vsync_dly;
wire          ch1_wframe_data_valid;
wire [15:0]   ch1_wframe_data;
wire          ch1_rframe_req_ack;
wire          ch1_rframe_data_en;
wire  [127:0] ch1_rframe_data;
wire          ch1_rframe_data_valid;
wire  [32*6*8-1:0] ch1_apb_roi_flat;
wire  [3:0]   ch1_apb_roi_count;
wire  [31:0]  ch1_apb_frame_id;
wire  [32*6*8-1:0] ch1_det_roi_flat;
wire  [3:0]        ch1_det_roi_count;
wire  [31:0]       ch1_det_frame_id;
wire  [32*6*8-1:0] dbg_fixed_roi_flat;
wire  [32*6*8-1:0] ch1_pcie_roi_flat;
wire  [3:0]        ch1_pcie_roi_count;

wire          ch2_wframe_data_valid      ;
wire [15:0]   ch2_wframe_data            ;
wire          ch2_rframe_req_ack;
wire          ch2_rframe_data_en;
wire  [127:0] ch2_rframe_data;
wire          ch2_rframe_data_valid;

wire          ch3_wframe_data_valid      ;
wire [15:0]   ch3_wframe_data            ;
wire          ch3_rframe_req_ack;
wire          ch3_rframe_data_en;
wire  [127:0] ch3_rframe_data;
wire          ch3_rframe_data_valid;

//dma
// DMA CTRL      BASE ADDR = 0x8000
wire            o_dma_write_data_req;
wire [11:0]     o_dma_write_addr ;
wire [127:0]    i_dma_write_data;
`ifndef PCIE_DMA_DIRECT
wire            pcie_ch1_pre_dma_pulse;
`endif

reg             cmos1_vsync_d1;
reg             cmos1_vsync_d2;
reg             cmos1_href_d1;
reg             cmos1_href_d2;
reg   [7:0]     cmos1_data_d1;
reg   [7:0]     cmos1_data_d2;

reg             cmos2_vsync_d1;
reg             cmos2_vsync_d2;
reg             cmos2_href_d1;
reg             cmos2_href_d2;
reg   [7:0]     cmos2_data_d1;
reg   [7:0]     cmos2_data_d2;

reg             cmos3_vsync_d1;
reg             cmos3_vsync_d2;
reg             cmos3_href_d1;
reg             cmos3_href_d2;
reg   [7:0]     cmos3_data_d1;
reg   [7:0]     cmos3_data_d2;


wire            line_full_flag_1;
wire            line_full_flag_2;
wire            line_full_flag_3;
wire            line_full_flag_4;

always @(posedge cmos1_pclk)begin
    cmos1_vsync_d1 <= cmos1_vsync;
    cmos1_vsync_d2 <= cmos1_vsync_d1;
    cmos1_href_d1 <= cmos1_href;
    cmos1_href_d2  <= cmos1_href_d1;
    cmos1_data_d1 <= cmos1_data;
    cmos1_data_d2 <= cmos1_data_d1;
end

always @(posedge cmos2_pclk)begin
    cmos2_vsync_d1 <= cmos2_vsync;
    cmos2_vsync_d2 <= cmos2_vsync_d1;
    cmos2_href_d1 <= cmos2_href;
    cmos2_href_d2  <= cmos2_href_d1;
    cmos2_data_d1 <= cmos2_data;
    cmos2_data_d2 <= cmos2_data_d1;
end

always @(posedge cmos3_pclk)begin
    cmos3_vsync_d1 <= cmos3_vsync;
    cmos3_vsync_d2 <= cmos3_vsync_d1;
    cmos3_href_d1 <= cmos3_href;
    cmos3_href_d2  <= cmos3_href_d1;
    cmos3_data_d1 <= cmos3_data;
    cmos3_data_d2 <= cmos3_data_d1;
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

// CDC: synchronize DDR init-done into clk_50m domain before using it
// as control/reset for 50MHz camera init logic.
always @(posedge clk_50m or negedge board_rst_n) begin
    if (!board_rst_n)
        ddr_init_done_sync_50m <= 2'b00;
    else
        ddr_init_done_sync_50m <= {ddr_init_done_sync_50m[0], ddr_init_done};
end

assign ddr_init_done_50m = ddr_init_done_sync_50m[1];

//*==============================================================================
//配置cmos
//*==============================================================================
//OV5640 register configure enable    
power_on_delay	power_on_delay_inst(
    .clk_50M                 (clk_50m        ),//input
    .reset_n                 (ddr_init_done_50m),//input	
    .camera1_rstn            (cmos1_reset    ),//output
    .camera2_rstn            (cmos2_reset    ),//output	
    .camera_pwnd             (               ),//output
    .initial_en              (initial_en     ) //output		
);

power_on_delay_fmc	power_on_delay_1_inst(
    .clk_50M                 (clk_50m        ),//input
    .reset_n                 (ddr_init_done_50m),//input	
    .camera1_rstn            (               ),//output
    .camera2_rstn            (cmos3_reset    ),//output	
    .camera_pwnd             (               ),//output
    .initial_en              (initial_en_1   ) //output		
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

//CMOS3 Camera 
reg_config_fmc	coms3_reg_config(
    .clk_25M                 (clk_50m            ),//input
    .camera_rstn             (cmos3_reset        ),//input
    .initial_en              (initial_en_1       ),//input		
    .i2c_sclk                (cmos3_scl          ),//output
    .i2c_sdat                (cmos3_sda          ),//inout
    .reg_conf_done           (cmos_init_done[2]  ),//output config_finished
    .reg_index               (                   ),//output reg [8:0]
    .clock_20k               (                   ) //output reg
);
//===============================================================================
//CMOS 8bit转16bit
//===============================================================================
//CMOS1
always@(posedge cmos1_pclk)
    begin
        cmos1_d_d0        <= cmos1_data_d2    ;
        cmos1_href_d0     <= cmos1_href_d2    ;
        cmos1_vsync_d0    <= cmos1_vsync_d2   ;
    end

cmos_8_16bit cmos1_8_16bit(
.pclk           (cmos1_pclk       ),//input
.rst_n          (cmos_init_done[0]),//input
.pdata_i        (cmos1_d_d0       ),//input[7:0]
.de_i           (cmos1_href_d0    ),//input
.vs_i           (cmos1_vsync_d0    ),//input

.pixel_clk      (cmos1_pclk_16bit ),//output
.pdata_o        (cmos1_d_16bit    ),//output[15:0]
.de_o           (cmos1_href_16bit ) //output
);

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


//CMOS3
always@(posedge cmos3_pclk)
    begin
        cmos3_d_d0        <= cmos3_data_d2    ;
        cmos3_href_d0     <= cmos3_href_d2    ;
        cmos3_vsync_d0    <= cmos3_vsync_d2   ;
    end

cmos_8_16bit_fmc cmos2_8_16bit_fmc(
.pclk           (cmos3_pclk       ),//input
.rst_n          (cmos_init_done[2]),//input
.pdata_i        (cmos3_d_d0       ),//input[7:0]
.de_i           (cmos3_href_d0    ),//input
.vs_i           (cmos3_vsync_d0    ),//input

.pixel_clk      (cmos3_pclk_16bit ),//output
.pdata_o        (cmos3_d_16bit    ),//output[15:0]
.de_o           (cmos3_href_16bit ) //output
);

//===============================================================================
// 输出视频选择
//===============================================================================
assign     pclk_in_test    =    cmos1_pclk_16bit    ;
assign     vs_in_test      =    cmos1_vsync_d0      ;
assign     de_in_test      =    cmos1_href_16bit    ;
assign     i_rgb565        =    {cmos1_d_16bit[4:0],cmos1_d_16bit[10:5],cmos1_d_16bit[15:11]};//{r,g,b}

assign     pclk_in_test_2  =    cmos2_pclk_16bit    ;
assign     vs_in_test_2    =    cmos2_vsync_d0      ;
assign     de_in_test_2    =    cmos2_href_16bit    ;
assign     i_rgb565_2      =    {cmos2_d_16bit[4:0],cmos2_d_16bit[10:5],cmos2_d_16bit[15:11]};//{r,g,b}

assign     pclk_in_test_3  =    cmos3_pclk_16bit    ;
assign     vs_in_test_3    =    cmos3_vsync_d0      ;
assign     de_in_test_3    =    cmos3_href_16bit    ;
assign     i_rgb565_3      =    {cmos3_d_16bit[4:0],cmos3_d_16bit[10:5],cmos3_d_16bit[15:11]};//{r,g,b}
//*==============================================================================
//ddr3 IP例化
//*==============================================================================
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
// cmos2 单路：ch1 全像素写 DDR；ch0/ch2/ch3 写关断
//*==============================================================================
assign ch0_wframe_data_valid = 1'b0;
assign ch0_wframe_data       = 16'd0;
assign ch2_wframe_data_valid = 1'b0;
assign ch2_wframe_data       = 16'd0;
assign ch3_wframe_data_valid = 1'b0;
assign ch3_wframe_data       = 16'd0;

image_reshape_solo ch1_solo_reshape(
    .clk                 (pclk_in_test_2            ),
    .rst_n               (lock && ddr_init_done     ),
    .img_vs              (vs_in_test_2              ),
    .img_data_valid      (de_in_test_2              ),
    .img_data            (i_rgb565_2                ),
    .img_vs_out          (ch1_reshape_vs            ),
    .img_data_valid_out  (ch1_reshape_de            ),
    .img_data_out        (ch1_reshape_data          )
);

assign ch1_wframe_vsync_dly   = ch1_reshape_vs;
assign ch1_wframe_data_valid  = ch1_reshape_de;
assign ch1_wframe_data        = ch1_reshape_data;

assign ch0_rframe_data_en     = 1'b0;
assign ch2_rframe_data_en     = 1'b0;
assign ch3_rframe_data_en     = 1'b0;

// A/B：若加宏 PCIE_DMA_DIRECT，则 o_dma 直连读 FIFO 使能、i_dma 组合取 ch1（仅用于判条纹是否在 solo/DMA 握手层）
`ifdef PCIE_DMA_DIRECT
assign ch1_rframe_data_en = o_dma_write_data_req;
assign i_dma_write_data   = ch1_rframe_data;
`else
pcie_ch1_meta_solo pcie_ch1_meta_solo_inst(
    .clk                    (pclk_div2                  ),
    .rst_n                  (core_rst_n                 ),
    .dma_wr_data_req        (o_dma_write_data_req       ),
    .i_roi_count            (ch1_pcie_roi_count         ),
    .i_roi_flat             (ch1_pcie_roi_flat           ),
    .i_frame_id             (ch1_det_frame_id           ),
    .dma_wr_data            (i_dma_write_data           ),
    .ch1_data_req           (ch1_rframe_data_en         ),
    .ch1_data               (ch1_rframe_data            ),
    .ch1_data_valid         (ch1_rframe_data_valid      ),
    .o_pre_dma_pulse        (pcie_ch1_pre_dma_pulse     )
);
`endif
//*==============================================================================
//axi控制器例化
//*==============================================================================
axi_ctrl_256_burst axi_ctrl_inst
(
	  .ARESETN                     (ddr_init_done                             ),
	  .ACLK                        (core_clk                                  ),
	  .M_AXI_AWID                  (axi_awuser_id                             ),
	  .M_AXI_AWADDR                (axi_awaddr                                ),
	  .M_AXI_AWLEN                 (axi_awlen                                 ),
	  .M_AXI_AWSIZE                (                                          ),
	  .M_AXI_AWBURST               (                                          ),
	  .M_AXI_AWLOCK                (                                          ),
	  .M_AXI_AWCACHE               (                                          ),
	  .M_AXI_AWPROT                (                                          ),
	  .M_AXI_AWQOS                 (                                          ),
	  .M_AXI_AWUSER                (                                          ),
	  .M_AXI_AWVALID               (axi_awvalid                               ),
	  .M_AXI_AWREADY               (axi_awready                               ),
	  .M_AXI_WDATA                 (axi_wdata                                 ),
	  .M_AXI_WSTRB                 (axi_wstrb                                 ),
	  .M_AXI_WLAST                 (                                          ),
	  .M_AXI_WUSER                 (                                          ),
	  .M_AXI_WVALID                (                                          ),
	  .M_AXI_WREADY                (axi_wready                                ),
	  .M_AXI_BID                   (0                                         ),
	  .M_AXI_BRESP                 (0                                         ),
	  .M_AXI_BUSER                 (0                                         ),
      .M_AXI_BVALID                (1'b1                                      ),

	  .M_AXI_BREADY                (                                          ),
	  .M_AXI_ARID                  (axi_aruser_id                             ),
	  .M_AXI_ARADDR                (axi_araddr                                ),
	  .M_AXI_ARLEN                 (axi_arlen                                 ),
	  .M_AXI_ARSIZE                (                                          ),
	  .M_AXI_ARBURST               (                                          ),
	  .M_AXI_ARLOCK                (                                          ),
	  .M_AXI_ARCACHE               (                                          ),
	  .M_AXI_ARPROT                (                                          ),
	  .M_AXI_ARQOS                 (                                          ),
	  .M_AXI_ARUSER                (                                          ),
	  .M_AXI_ARVALID               (axi_arvalid                               ),
	  .M_AXI_ARREADY               (axi_arready                               ),
	  .M_AXI_RID                   (axi_rid                                   ),
	  .M_AXI_RDATA                 (axi_rdata                                 ),
	  .M_AXI_RRESP                 (0                                         ),
	  .M_AXI_RLAST                 (axi_rlast                                 ),
	  .M_AXI_RUSER                 (0                                         ),
	  .M_AXI_RVALID                (axi_rvalid                                ),
	  .M_AXI_RREADY                (                                          ),  

      // key
      .key                         ({1'b1,3'b111,4'b0000}                     ),

      // 通道0
      .ch0_wframe_pclk             (pclk_in_test                              ),
      .ch0_wframe_rst_n            (lock && ddr_init_done                     ),
      .ch0_wframe_vsync            (vs_in_test                                ),
      .ch0_wframe_data_valid       (ch0_wframe_data_valid                     ),         
      .ch0_wframe_data             (ch0_wframe_data                           ),

      .ch0_rframe_pclk             (pclk_div2                                 ),   
      .ch0_rframe_rst_n            (ddr_init_done                             ), 
      .ch0_rframe_vsync            (1'b0                                      ),
      .ch0_rframe_req              (1'b0                                      ),
      .ch0_rframe_req_ack          (                                          ),
      .ch0_rframe_data_en          (ch0_rframe_data_en                        ),
      .ch0_rframe_data             (ch0_rframe_data                           ),      
      .ch0_rframe_data_valid       (                                          ),
      .ch0_read_line_full          (line_full_flag_1                          ),

        // 通道1
      .ch1_wframe_pclk             (pclk_in_test_2                            ),
      .ch1_wframe_rst_n            (lock && ddr_init_done                     ),
      .ch1_wframe_vsync            (ch1_wframe_vsync_dly                      ),
      .ch1_wframe_data_valid       (ch1_wframe_data_valid                     ),          
      .ch1_wframe_data             (ch1_wframe_data                           ),      

      .ch1_rframe_pclk             (pclk_div2                                 ),   
      .ch1_rframe_rst_n            (ddr_init_done                             ), 
      .ch1_rframe_vsync            (ch1_rd_frame_req                          ),
      .ch1_rframe_req              (ch1_rd_frame_req                          ),
      .ch1_rframe_req_ack          (ch1_rframe_req_ack                        ),
      .ch1_rframe_data_en          (ch1_rframe_data_en                        ),
      .ch1_rframe_data             (ch1_rframe_data                           ),
      .ch1_rframe_data_valid       (ch1_rframe_data_valid                      ),
      .ch1_read_line_full          (line_full_flag_2                          ),

        // 通道2
      .ch2_wframe_pclk             (pclk_in_test_3                            ),
      .ch2_wframe_rst_n            (lock && ddr_init_done                     ),
      .ch2_wframe_vsync            (vs_in_test_3                              ),
      .ch2_wframe_data_valid       (ch2_wframe_data_valid                     ),
      .ch2_wframe_data             (ch2_wframe_data                           ),

      .ch2_rframe_pclk             (pclk_div2                                 ),   
      .ch2_rframe_rst_n            (ddr_init_done                             ), 
      .ch2_rframe_vsync            (1'b0                                      ),
      .ch2_rframe_req              (1'b0                                      ),
      .ch2_rframe_req_ack          (                                          ),
      .ch2_rframe_data_en          (ch2_rframe_data_en                        ),
      .ch2_rframe_data             (ch2_rframe_data                           ),      
      .ch2_rframe_data_valid       (                                          ),
      .ch2_read_line_full          (line_full_flag_3                          ),

      // 通道3
      .ch3_wframe_pclk             (pclk_in_test_3                            ),  
      .ch3_wframe_rst_n            (lock && ddr_init_done                     ),
      .ch3_wframe_vsync            (vs_in_test_3                              ),
      .ch3_wframe_data_valid       (ch3_wframe_data_valid                     ),
      .ch3_wframe_data             (ch3_wframe_data                           ),

      .ch3_rframe_pclk             (pclk_div2                                 ),   
      .ch3_rframe_rst_n            (ddr_init_done                             ), 
      .ch3_rframe_vsync            (1'b0                                      ),
      .ch3_rframe_req              (1'b0                                      ),
      .ch3_rframe_req_ack          (                                          ),
      .ch3_rframe_data_en          (ch3_rframe_data_en                        ),
      .ch3_rframe_data             (ch3_rframe_data                           ),      
      .ch3_rframe_data_valid       (                                          ),
      .ch3_read_line_full          (line_full_flag_4                          )
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

//===========================================================================
// PCIe -> uart2apb(ref) + expd(CDC) -> pclk_div2 域 APB：DMA(0x8) / cfg(0x9) / ROI(0xA)
//===========================================================================
// PCIe IP TOP
pcie_test u_ips2l_pcie_wrap (
	.button_rst_n				(sync_button_rst_n	),
	.power_up_rst_n				(sync_perst_n		),
	.perst_n					(sync_perst_n		),

	// The clock and reset signals
	.pclk						(pclk					),
	.pclk_div2					(pclk_div2				),
	.ref_clk					(ref_clk				),
	.ref_clk_n					(ref_clk_n				),
	.ref_clk_p					(ref_clk_p				),
	.core_rst_n					(core_rst_n				),

	// APB interface to DBI
	.p_sel						(p_sel_pcie				),
	.p_strb						(uart_p_strb			),
	.p_addr						(uart_p_addr			),
	.p_wdata					(uart_p_wdata			),
	.p_ce						(uart_p_ce				),
	.p_we						(uart_p_we				),
	.p_rdy						(p_rdy_pcie				),
	.p_rdata					(p_rdata_pcie			),

	// PHY diff signals
	.rxn						(rxn					),
	.rxp						(rxp					),
	.txn						(txn					),
	.txp						(txp					),
	.pcs_nearend_loop			({4{1'b0}}				),
	.pma_nearend_ploop			({4{1'b0}}				),
	.pma_nearend_sloop			({4{1'b0}}				),

	// AXI4-Stream master interface
	.axis_master_tvalid			(axis_master_tvalid		),
	.axis_master_tready			(axis_master_tready		),
	.axis_master_tdata			(axis_master_tdata		),
	.axis_master_tkeep			(axis_master_tkeep		),
	.axis_master_tlast			(axis_master_tlast		),
	.axis_master_tuser			(axis_master_tuser		),

	// AXI4-Stream slave 0 interface
	.axis_slave0_tready			(axis_slave0_tready		),
	.axis_slave0_tvalid			(axis_slave0_tvalid		),
	.axis_slave0_tdata			(axis_slave0_tdata		),
	.axis_slave0_tlast			(axis_slave0_tlast		),
	.axis_slave0_tuser			(axis_slave0_tuser		),

	// AXI4-Stream slave 1 interface
	.axis_slave1_tready			(axis_slave1_tready		),
	.axis_slave1_tvalid			(axis_slave1_tvalid		),
	.axis_slave1_tdata			(axis_slave1_tdata		),
	.axis_slave1_tlast			(axis_slave1_tlast		),
	.axis_slave1_tuser			(axis_slave1_tuser		),

	// AXI4-Stream slave 2 interface
	.axis_slave2_tready			(axis_slave2_tready		),
	.axis_slave2_tvalid			(axis_slave2_tvalid		),
	.axis_slave2_tdata			(axis_slave2_tdata		),
	.axis_slave2_tlast			(axis_slave2_tlast		),
	.axis_slave2_tuser			(axis_slave2_tuser		),

	.pm_xtlh_block_tlp			(						),

	.cfg_send_cor_err_mux		(						),
	.cfg_send_nf_err_mux		(						),
	.cfg_send_f_err_mux			(						),
	.cfg_sys_err_rc				(						),
	.cfg_aer_rc_err_mux			(						),

	// The radm timeout
	.radm_cpl_timeout			(						),

	// Configuration signals
	.cfg_max_rd_req_size		(cfg_max_rd_req_size	),
	.cfg_bus_master_en			(						),
	.cfg_max_payload_size		(cfg_max_payload_size	),
	.cfg_ext_tag_en				(						),
	.cfg_rcb					(cfg_rcb				),
	.cfg_mem_space_en			(						),
	.cfg_pm_no_soft_rst			(						),
	.cfg_crs_sw_vis_en			(						),
	.cfg_no_snoop_en			(						),
	.cfg_relax_order_en			(						),
	.cfg_tph_req_en				(						),
	.cfg_pf_tph_st_mode			(						),
	.rbar_ctrl_update			(						),
	.cfg_atomic_req_en			(						),

	.cfg_pbus_num				(cfg_pbus_num			),
	.cfg_pbus_dev_num			(cfg_pbus_dev_num		),

	// Debug signals
	.radm_idle					(						),
	.radm_q_not_empty			(						),
	.radm_qoverflow				(						),
	.diag_ctrl_bus				(2'b0					),
	.cfg_link_auto_bw_mux		(						),
	.cfg_bw_mgt_mux				(						),
	.cfg_pme_mux				(						),
	.app_ras_des_sd_hold_ltssm	(1'b0					),
	.app_ras_des_tba_ctrl		(2'b0					),

	.dyn_debug_info_sel			(4'b0					),
	.debug_info_mux				(						),

	// System signal
	.smlh_link_up				(smlh_link_up			),
	.rdlh_link_up				(rdlh_link_up			),
	.smlh_ltssm_state			(smlh_ltssm_state		)
);

pgr_uart2apb_top_32bit #(
	.CLK_DIV_P					(16'd145)
) u_pgr_uart2apb (
	.clk						(ref_clk				),
	.rst_n						(ref_core_rst_n			),
	.p_sel						(uart_p_sel				),
	.p_strb						(uart_p_strb			),
	.p_addr						(uart_p_addr			),
	.p_wdata					(uart_p_wdata			),
	.p_ce						(uart_p_ce				),
	.p_we						(uart_p_we				),
	.p_rdy						(uart_p_rdy				),
	.p_rdata					(uart_p_rdata			),
	.txd						(uart_txd				),
	.rxd						(uart_rxd				)
);

ips2l_expd_apb_mux u_ips2l_pcie_expd_apb_mux (
	.i_uart_clk					(ref_clk				),
	.i_uart_rst_n				(ref_core_rst_n			),
	.i_uart_p_sel				(uart_p_sel				),
	.i_uart_p_strb				(uart_p_strb			),
	.i_uart_p_addr				(uart_p_addr			),
	.i_uart_p_wdata				(uart_p_wdata			),
	.i_uart_p_ce				(uart_p_ce				),
	.i_uart_p_we				(uart_p_we				),
	.o_uart_p_rdy				(uart_p_rdy				),
	.o_uart_p_rdata				(uart_p_rdata			),
	.i_pclk_div2_clk			(pclk_div2				),
	.i_pclk_div2_rst_n			(core_rst_n				),
	.o_pclk_div2_p_strb			(p_strb					),
	.o_pclk_div2_p_addr			(p_addr					),
	.o_pclk_div2_p_wdata		(p_wdata				),
	.o_pclk_div2_p_ce			(p_ce					),
	.o_pclk_div2_p_we			(p_we					),
	.o_pcie_p_sel				(p_sel_pcie				),
	.i_pcie_p_rdy				(p_rdy_pcie				),
	.i_pcie_p_rdata				(p_rdata_pcie			),
	.o_dma_p_sel				(p_sel_dma				),
	.i_dma_p_rdy				(p_rdy_dma				),
	.i_dma_p_rdata				(p_rdata_dma			),
	.o_cfg_p_sel				(p_sel_cfg				),
	.i_cfg_p_rdy				(p_rdy_cfg				),
	.i_cfg_p_rdata				(p_rdata_cfg			),
	.o_roi_p_sel				(p_sel_roi				),
	.i_roi_p_rdy				(p_rdy_roi				),
	.i_roi_p_rdata				(p_rdata_roi			)
);

// ch1 写完成后的 vsync 属于 cmos2 像素时钟域，此处 3 级同步到 pclk_div2 后做上升沿=帧
reg [2:0] ch1_wvs_sync;
wire      ch1_roi_frame_inc;
always @(posedge pclk_div2 or negedge core_rst_n) begin
	if (!core_rst_n)
		ch1_wvs_sync <= 3'd0;
	else
		ch1_wvs_sync <= {ch1_wvs_sync[1:0], ch1_wframe_vsync_dly};
end
assign ch1_roi_frame_inc = ch1_wvs_sync[1] & ~ch1_wvs_sync[2];

ch1_roi_array_apb_solo u_ch1_roi_apb (
	.clk				(pclk_div2			),
	.rst_n				(core_rst_n			),
	.i_apb_psel			(p_sel_roi			),
	.i_apb_paddr		(p_addr[8:0]		),
	.i_apb_pwdata		(p_wdata			),
	.i_apb_pstrb		(p_strb				),
	.i_apb_pwrite		(p_we				),
	.i_apb_penable		(p_ce				),
	.o_apb_prdy			(p_rdy_roi			),
	.o_apb_prdata		(p_rdata_roi		),
	.i_ch1_frame_inc	(ch1_roi_frame_inc	),
	.o_roi_flat			(ch1_apb_roi_flat		),
	.o_frame_id			(ch1_apb_frame_id		),
	.o_roi_count		(ch1_apb_roi_count		)
);

ch1_roi_bbox_simple #(
    .VS_INVERT      (CH1_ROI_VS_INVERT ),
    .FG_USE_DE_ONLY (CH1_ROI_FG_DE_ONLY),
    .FG_FORCE_ON    (CH1_ROI_FG_FORCE_ON)
) u_ch1_roi_bbox_simple (
    .clk_pix        (pclk_in_test_2           ),
    .rst_n_pix      (lock && ddr_init_done    ),
    .de_in          (ch1_reshape_de            ),
    .vs_in          (ch1_reshape_vs            ),
    .din            (ch1_reshape_data          ),
    .clk_div2       (pclk_div2                 ),
    .rst_n_div2     (core_rst_n                ),
    .o_roi_flat     (ch1_det_roi_flat          ),
    .o_roi_count    (ch1_det_roi_count         ),
    .o_frame_id     (ch1_det_frame_id          )
);

// -------------------------------------------------------------------------
// FIXED_ROI_DEBUG=1 或 ROI_PCIE_FALLBACK_BOX=1 时使用下方固定矩形经 PCIe 送出
localparam FIXED_ROI_DEBUG       = 0;
localparam ROI_PCIE_FALLBACK_BOX = 0;

localparam [11:0] FIX_X1 = 12'd20;
localparam [11:0] FIX_Y1 = 12'd540;
localparam [11:0] FIX_X2 = 12'd179;
localparam [11:0] FIX_Y2 = 12'd699;
assign dbg_fixed_roi_flat[31:0]      = {20'd0, FIX_X1};
assign dbg_fixed_roi_flat[63:32]      = {20'd0, FIX_Y1};
assign dbg_fixed_roi_flat[95:64]      = {20'd0, FIX_X2};
assign dbg_fixed_roi_flat[127:96]     = {20'd0, FIX_Y2};
assign dbg_fixed_roi_flat[159:128]     = 32'd0;
assign dbg_fixed_roi_flat[191:160]     = {25'd0, 7'd80};
assign dbg_fixed_roi_flat[1535:192]   = 1344'd0;

wire roi_meas_ok = (ch1_det_roi_count != 4'd0);
wire use_pcie_fixed = (FIXED_ROI_DEBUG != 0)
    || ((!roi_meas_ok) && (ROI_PCIE_FALLBACK_BOX != 0));

assign ch1_pcie_roi_flat  = (FIXED_ROI_DEBUG != 0) ? dbg_fixed_roi_flat
                         : roi_meas_ok            ? ch1_det_roi_flat
                         : use_pcie_fixed         ? dbg_fixed_roi_flat
                         : ch1_det_roi_flat;
assign ch1_pcie_roi_count = use_pcie_fixed ? 4'd1 : ch1_det_roi_count;
// DMA CTRL: BASE 0x8000（[15:12]==8）
ips2l_pcie_dma #(
	.DEVICE_TYPE			(DEVICE_TYPE),
	.AXIS_SLAVE_NUM			(AXIS_SLAVE_NUM)
) u_ips2l_pcie_dma (
	.clk					(pclk_div2			),				
	.rst_n					(core_rst_n		),				

	// Num
	.i_cfg_pbus_num			(cfg_pbus_num		),				
	.i_cfg_pbus_dev_num		(cfg_pbus_dev_num		),			
	.i_cfg_max_rd_req_size	(cfg_max_rd_req_size		),		
	.i_cfg_max_payload_size	(cfg_max_payload_size		),		

	// AXI4-Stream master interface
	.i_axis_master_tvld		(axis_master_tvalid_mem		),	
	.o_axis_master_trdy		(axis_master_tready_mem		),	
	.i_axis_master_tdata	(axis_master_tdata_mem		),	
	.i_axis_master_tkeep	(axis_master_tkeep_mem		),	
	.i_axis_master_tlast	(axis_master_tlast_mem		),	
	.i_axis_master_tuser	(axis_master_tuser_mem		),	

	// AXI4-Stream slave0 interface
	.i_axis_slave0_trdy		(axis_slave0_tready		),		
	.o_axis_slave0_tvld		(dma_axis_slave0_tvalid		),	
	.o_axis_slave0_tdata	(dma_axis_slave0_tdata		),	
	.o_axis_slave0_tlast	(dma_axis_slave0_tlast		),	
	.o_axis_slave0_tuser	(dma_axis_slave0_tuser		),	

	// AXI4-Stream slave1 interface
	.i_axis_slave1_trdy		(axis_slave1_tready		),		
	.o_axis_slave1_tvld		(axis_slave1_tvalid		),		
	.o_axis_slave1_tdata	(axis_slave1_tdata		),		
	.o_axis_slave1_tlast	(axis_slave1_tlast		),		
	.o_axis_slave1_tuser	(axis_slave1_tuser		),		

	// AXI4-Stream slave2 interface
	.i_axis_slave2_trdy		(axis_slave2_tready		),		
	.o_axis_slave2_tvld		(axis_slave2_tvalid		),		
	.o_axis_slave2_tdata	(axis_slave2_tdata		),		
	.o_axis_slave2_tlast	(axis_slave2_tlast		),		
	.o_axis_slave2_tuser	(axis_slave2_tuser		),		

	// From pcie
	.i_cfg_ido_req_en		(cfg_ido_req_en		),			
	.i_cfg_ido_cpl_en		(cfg_ido_cpl_en		),			
	.i_xadm_ph_cdts			(xadm_ph_cdts		),				
	.i_xadm_pd_cdts			(xadm_pd_cdts		),				
	.i_xadm_nph_cdts		(xadm_nph_cdts		),			
	.i_xadm_npd_cdts		(xadm_npd_cdts		),			
	.i_xadm_cplh_cdts		(xadm_cplh_cdts		),			
	.i_xadm_cpld_cdts		(xadm_cpld_cdts		),			

	// APB interface
	.i_apb_psel				(p_sel_dma		),				
	.i_apb_paddr			(p_addr[8:0]		),				
	.i_apb_pwdata			(p_wdata		),					
	.i_apb_pstrb			(p_strb		),					
	.i_apb_pwrite			(p_we		),						
	.i_apb_penable			(p_ce		),						
	.o_apb_prdy				(p_rdy_dma		),				
	.o_apb_prdata			(p_rdata_dma		),				
	.o_cross_4kb_boundary	(cross_4kb_boundary		),
    // dma write
    .o_dma_write_data_req   (o_dma_write_data_req  ),
    .o_dma_write_addr       (o_dma_write_addr      ),
    .i_dma_write_data       (i_dma_write_data      )
);

assign p_rdy_cfg             = 1'b0;
assign p_rdata_cfg           = 32'b0;

assign axis_slave0_tvalid    = dma_axis_slave0_tvalid;
assign axis_slave0_tlast     = dma_axis_slave0_tlast;
assign axis_slave0_tuser     = dma_axis_slave0_tuser;
assign axis_slave0_tdata     = dma_axis_slave0_tdata;

assign axis_master_tvalid_mem  = axis_master_tvalid;
assign axis_master_tdata_mem   = axis_master_tdata;
assign axis_master_tkeep_mem   = axis_master_tkeep;
assign axis_master_tlast_mem   = axis_master_tlast;
assign axis_master_tuser_mem   = axis_master_tuser;

assign axis_master_tready      = axis_master_tready_mem;

// ch1 DDR 读请求 ACK 超时 (~1s@125MHz): 避免 DDR 偶发忙时误释放 read_req 导致欠载碎块
localparam [26:0] CH1_RD_REQ_ACK_TIMEOUT = 27'd125_000_000;
reg  [26:0]       ch1_rd_req_to_cnt;
wire              ch1_rd_req_timeout;

always @(posedge pclk_div2 or negedge core_rst_n) begin
    if (!core_rst_n)
        ch1_rd_req_to_cnt <= 27'd0;
    else if (!ch1_rd_frame_req)
        ch1_rd_req_to_cnt <= 27'd0;
    else if (ch1_rframe_req_ack)
        ch1_rd_req_to_cnt <= 27'd0;
    else if (ch1_rd_req_to_cnt != 27'h7FF_FFFF)
        ch1_rd_req_to_cnt <= ch1_rd_req_to_cnt + 27'd1;
end

assign ch1_rd_req_timeout = ch1_rd_frame_req && (ch1_rd_req_to_cnt >= CH1_RD_REQ_ACK_TIMEOUT);

// ch1 DDR 读下一帧:
//   带 224B metadata 时由 pcie_ch1_meta_solo.o_pre_dma_pulse 触发 (帧尾 + 长 idle 预取).
//   PCIE_DMA_DIRECT 时仍为纯图 720×160 拍/帧, 保留原 dma_write_cnt==720.
`ifdef PCIE_DMA_DIRECT
reg  [11:0]  o_dma_write_addr_dly1;
reg  [11:0]  o_dma_write_addr_dly2;
reg  [11:0]  dma_write_cnt;

always @(posedge pclk_div2) begin
    if (!core_rst_n) begin
        o_dma_write_addr_dly1 <= 12'd0;
        o_dma_write_addr_dly2 <= 12'd0;
    end else begin
        o_dma_write_addr_dly1 <= o_dma_write_addr;
        o_dma_write_addr_dly2 <= o_dma_write_addr_dly1;
    end
end

always @(posedge pclk_div2) begin
    if (!core_rst_n) begin
        dma_write_cnt <= 12'd0;
    end else if (o_dma_write_addr_dly1 == 12'ha0 && o_dma_write_addr_dly2 == 12'h9f) begin
        dma_write_cnt <= dma_write_cnt + 1'b1;
    end
    else if (dma_write_cnt == 12'd720)begin
        dma_write_cnt <= 12'd0;
    end
    else begin
        dma_write_cnt <= dma_write_cnt;
    end
end

always @(posedge pclk_div2) begin
    if (!core_rst_n) begin
        ch1_rd_frame_req <=1'b0;
    end else if (dma_write_cnt == 12'd720) begin
       ch1_rd_frame_req <= 1'b1;
    end
    else if (ch1_rframe_req_ack) begin
        ch1_rd_frame_req <= 1'b0;
    end
    else if (ch1_rd_req_timeout) begin
        ch1_rd_frame_req <= 1'b0;
    end
    else begin
        ch1_rd_frame_req <= ch1_rd_frame_req;
    end
end
`else
always @(posedge pclk_div2) begin
    if (!core_rst_n) begin
        ch1_rd_frame_req <=1'b0;
    end else if (pcie_ch1_pre_dma_pulse) begin
       ch1_rd_frame_req <= 1'b1;
    end
    else if (ch1_rframe_req_ack) begin
        ch1_rd_frame_req <= 1'b0;
    end
    else if (ch1_rd_req_timeout) begin
        ch1_rd_frame_req <= 1'b0;
    end
    else begin
        ch1_rd_frame_req <= ch1_rd_frame_req;
    end
end
`endif






endmodule