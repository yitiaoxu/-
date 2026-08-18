# Export standard-set waveforms (B/C/E/F) to docs/images/sim_hdmi/
# Run from sim/modelsim (GUI): vsim -do "do ddr_write_path_capture.do"
set ROOT [file normalize [file join [pwd] ../..]]
set OUTDIR [file normalize [file join $ROOT ../docs/images/sim_hdmi]]

if {![file exists $OUTDIR]} {
    file mkdir $OUTDIR
}

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
    [file join $ROOT source/hdmi_ddr_write_path.v] \
    [file join $ROOT sim/tb/rgb888_frame_gen.v] \
    [file join $ROOT sim/tb/axi4_mem_model.v] \
    [file join $ROOT sim/tb/ddr_write_path_tb.v]

vsim -novopt work.ddr_write_path_tb
do ddr_write_path_wave.do
run -all

proc export_wave_png {start_ns end_ns outfile} {
    global OUTDIR
    WaveRestoreZoom "${start_ns} ns" "${end_ns} ns"
    set path [file join $OUTDIR $outfile]
    if {[catch {write format wave -format png -file $path} err]} {
        puts "WARN: PNG export failed ($err). Screenshot manually: $path"
    } else {
        puts "Exported: $path"
    }
}

# B: input registers (TC-V01)
export_wave_png 180 380 sim_hdmi_input_regs.png

# C: RGB565 convert (first active line)
export_wave_png 300 600 sim_hdmi_rgb565_convert.png

# E: full overview
export_wave_png 0 40000 sim_hdmi_write_overview.png

# F: first AXI burst (adjust if needed after zoom)
export_wave_png 500 1200 sim_hdmi_axi_burst.png

puts "Done. Check $OUTDIR for PNG files."
