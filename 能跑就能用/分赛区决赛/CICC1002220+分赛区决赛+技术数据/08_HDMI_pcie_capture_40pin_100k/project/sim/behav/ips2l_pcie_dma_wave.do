# Grouped waveform layout for ips2l_pcie_dma unit TB
# Source from ips2l_pcie_dma_sim.do after vsim, or: do ips2l_pcie_dma_wave.do
onerror {resume}
quietly WaveActivateNextPane {} 0

set TB   /ips2l_pcie_dma_tb
set DUT  ${TB}/u_dut
set RX   ${DUT}/u_ips2l_pcie_dma_rx_top
set CTRL ${DUT}/u_ips2l_pcie_dma_controller
set TX   ${DUT}/u_ips2l_pcie_dma_tx_top
set MWRD ${TX}/u_ips2l_pcie_dma_tx_mwr_rd_ctrl

# ---------------------------------------------------------------------------
# 01 TB: clock / reset / test counters
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {01_TB_clk_rst
} ${TB}/clk
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {01_TB_clk_rst
} ${TB}/rst_n

add wave -noupdate -expand -group {DMA_Wave
} -expand -group {02_TB_test_status
} -radix unsigned ${TB}/test_err_cnt
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {02_TB_test_status
} -radix unsigned ${TB}/req_pulse_cnt
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {02_TB_test_status
} -radix unsigned ${TB}/max_dma_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {02_TB_test_status
} ${TB}/dma_active

# ---------------------------------------------------------------------------
# 02 Host inbound: axis_master (BAR1 MWR TLP from host BFM)
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {03_Host_axis_master
} -expand -group {handshake
} ${TB}/axis_master_tvld
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {03_Host_axis_master
} -expand -group {handshake
} ${TB}/axis_master_trdy
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {03_Host_axis_master
} -expand -group {handshake
} ${TB}/axis_master_tlast
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {03_Host_axis_master
} -expand -group {tlp_header_payload
} -radix hex ${TB}/axis_master_tdata
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {03_Host_axis_master
} -expand -group {tlp_header_payload
} -radix hex ${TB}/axis_master_tkeep
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {03_Host_axis_master
} -expand -group {tlp_header_payload
} -radix hex ${TB}/axis_master_tuser

# ---------------------------------------------------------------------------
# 03 DUT internal: BAR1 write decode (rx_top -> controller)
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {rx_top_bar1
} ${RX}/o_bar1_wr_en
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {rx_top_bar1
} -radix hex ${RX}/o_bar1_wr_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {rx_top_bar1
} -radix hex ${RX}/o_bar1_wr_data
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {rx_top_bar1
} -radix hex ${RX}/o_bar1_wr_byte_en

add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {controller_regs
} -radix hex ${CTRL}/dma_cmd_reg
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {controller_regs
} -radix hex ${CTRL}/dma_cmd_l_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {controller_regs
} -radix hex ${CTRL}/dma_cmd_h_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {controller_regs
} ${CTRL}/dma_cmd_reg_vld
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {controller_regs
} ${CTRL}/dma_cmd_l_addr_vld
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {04_DUT_BAR1_wr
} -expand -group {controller_regs
} ${CTRL}/dma_l_addr_cfg_done

# ---------------------------------------------------------------------------
# 04 DUT: DMA scheduler (MWR request + host target address)
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} ${CTRL}/o_mwr32_req
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} ${CTRL}/i_mwr32_req_ack
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} -radix hex ${CTRL}/o_req_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} -radix unsigned ${CTRL}/o_req_length
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} ${CTRL}/dma_wr_rd_cmd_flag
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} ${CTRL}/cross_4kb_boundary
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} -expand -group {dma_status
} -radix hex ${CTRL}/o_dma_status
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} -expand -group {dma_status
} ${CTRL}/dma_status_done
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {05_DUT_DMA_scheduler
} -expand -group {dma_status
} ${CTRL}/dma_status_busy

# ---------------------------------------------------------------------------
# 05 User read interface (FPGA frame buffer side, image upload path)
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} ${TB}/o_dma_write_data_req
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} -radix hex ${TB}/o_dma_write_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} -radix hex ${TB}/i_dma_write_data

add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} -expand -group {tx_mwr_rd_ctrl
} ${MWRD}/o_bar_rd_clk_en
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} -expand -group {tx_mwr_rd_ctrl
} -radix hex ${MWRD}/o_bar_rd_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} -expand -group {tx_mwr_rd_ctrl
} -radix hex ${MWRD}/i_bar_rd_data

add wave -noupdate -expand -group {DMA_Wave
} -expand -group {06_User_read_TB_ports
} -expand -group {pattern_src
} -radix unsigned ${TB}/u_data_src/beat_cnt

# ---------------------------------------------------------------------------
# 06 Host outbound: axis_slave2 MWR TLP (FPGA -> host memory)
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {07_Host_axis_slave2_MWR
} -expand -group {handshake
} ${TB}/axis_slave2_tvld
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {07_Host_axis_slave2_MWR
} -expand -group {handshake
} ${TB}/axis_slave2_trdy
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {07_Host_axis_slave2_MWR
} -expand -group {handshake
} ${TB}/axis_slave2_tlast
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {07_Host_axis_slave2_MWR
} -expand -group {tlp_data
} -radix hex ${TB}/axis_slave2_tdata
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {07_Host_axis_slave2_MWR
} -expand -group {tlp_data
} -radix hex ${TB}/axis_slave2_tuser

# ---------------------------------------------------------------------------
# 07 Host outbound: axis_slave0 CPLD (BAR0 status readback)
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {08_Host_axis_slave0_CPLD
} ${TB}/axis_slave0_tvld
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {08_Host_axis_slave0_CPLD
} -radix hex ${TB}/axis_slave0_tdata
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {08_Host_axis_slave0_CPLD
} ${TB}/axis_slave0_tlast
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {08_Host_axis_slave0_CPLD
} -radix hex ${TB}/last_cpld_data
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {08_Host_axis_slave0_CPLD
} ${TB}/cpld_received

# ---------------------------------------------------------------------------
# 08 Scoreboard
# ---------------------------------------------------------------------------
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {09_Scoreboard
} -radix hex ${TB}/sb_base_addr
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {09_Scoreboard
} -radix unsigned ${TB}/sb_beat_cnt
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {09_Scoreboard
} -radix unsigned ${TB}/sb_err_cnt
add wave -noupdate -expand -group {DMA_Wave
} -expand -group {09_Scoreboard
} ${TB}/sb_xfer_done

TreeUpdate [SetDefaultTree]

configure wave -namecolwidth 260
configure wave -valuecolwidth 110
configure wave -justifyvalue left
configure wave -signalnamewidth 1
configure wave -snapdistance 10
configure wave -datasetprefix 0
configure wave -rowmargin 4
configure wave -childrowmargin 2
configure wave -gridoffset 0
configure wave -gridperiod 1
configure wave -griddelta 40
configure wave -timeline 0
configure wave -timelineunits ns
update

# TC-DMA03~04: BAR1 ADDR write through MWR upload complete (~300ns .. 15us)
WaveRestoreZoom {300 ns} {20 us}
