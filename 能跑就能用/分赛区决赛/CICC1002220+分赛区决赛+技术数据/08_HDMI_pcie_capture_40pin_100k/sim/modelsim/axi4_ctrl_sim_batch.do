# Batch (no GUI) axi4_ctrl frame-buffer simulation
# Run from sim/modelsim: vsim -c -do axi4_ctrl_sim_batch.do
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
    [file join $ROOT sim/tb/axi4_ctrl_tb.v] \
    -l compile.log

vsim -c -novopt work.axi4_ctrl_tb -l sim.log
run -all
quit -f
