# GUI waveform simulation for axi4_ctrl frame-buffer TB
# Run from sim/modelsim: vsim -do axi4_ctrl_sim.do
set ROOT [file normalize [file join [pwd] ../..]]

if {[file exists work]} {
    vdel -lib work -all
}
vlib work
vmap work work

set INCDIR [file join $ROOT source]

vlog -work work -timescale 1ns/1ps +incdir+$INCDIR \
    [file join $ROOT sim/models/W_FIFO_16i_128o.v] \
    [file join $ROOT sim/models/R_FIFO_128i_128o.v] \
    [file join $ROOT source/axi_ctrl/axi4_ctrl.v] \
    [file join $ROOT sim/tb/rgb565_frame_gen.v] \
    [file join $ROOT sim/tb/dma_read_bfm.v] \
    [file join $ROOT sim/tb/axi4_mem_model.v] \
    [file join $ROOT sim/tb/axi4_ctrl_tb.v]

vsim -novopt work.axi4_ctrl_tb

add wave -divider "clk/rst"
add wave /axi4_ctrl_tb/axi_clk
add wave /axi4_ctrl_tb/axi_reset
add wave /axi4_ctrl_tb/wframe_pclk
add wave /axi4_ctrl_tb/rframe_pclk

add wave -divider "frame index"
add wave -hex /axi4_ctrl_tb/rc_wframe_index
add wave -hex /axi4_ctrl_tb/rc_rframe_index

add wave -divider "write side"
add wave /axi4_ctrl_tb/wr_start
add wave /axi4_ctrl_tb/wframe_vsync
add wave /axi4_ctrl_tb/wframe_data_en
add wave -hex /axi4_ctrl_tb/wframe_data
add wave /axi4_ctrl_tb/wr_frame_done

add wave -divider "AXI write"
add wave /axi4_ctrl_tb/axi_awvalid
add wave /axi4_ctrl_tb/axi_awready
add wave -hex /axi4_ctrl_tb/axi_awaddr
add wave /axi4_ctrl_tb/axi_wvalid
add wave /axi4_ctrl_tb/axi_wready
add wave /axi4_ctrl_tb/axi_wlast
add wave -hex /axi4_ctrl_tb/axi_wdata

add wave -divider "read side PCIe"
add wave /axi4_ctrl_tb/rframe_vsync
add wave /axi4_ctrl_tb/rframe_data_en
add wave /axi4_ctrl_tb/rframe_data_valid
add wave -hex /axi4_ctrl_tb/rframe_data
add wave -hex /axi4_ctrl_tb/rd_beat_cnt
add wave -hex /axi4_ctrl_tb/score_err_cnt

add wave -divider "AXI read"
add wave /axi4_ctrl_tb/axi_arvalid
add wave /axi4_ctrl_tb/axi_arready
add wave /axi4_ctrl_tb/axi_rvalid
add wave /axi4_ctrl_tb/axi_rlast

add wave -divider "mem stats"
add wave -hex /axi4_ctrl_tb/slot_write_bytes_0
add wave -hex /axi4_ctrl_tb/slot_write_bytes_1
add wave -hex /axi4_ctrl_tb/write_burst_cnt
add wave -hex /axi4_ctrl_tb/read_burst_cnt

run -all
echo "=== TC03 read window: zoom waveform to t=227850000..228110000 (ps) ==="
echo "=== Look for: rframe_data_en=1, rframe_data_valid=1, rframe_data non-zero ==="
