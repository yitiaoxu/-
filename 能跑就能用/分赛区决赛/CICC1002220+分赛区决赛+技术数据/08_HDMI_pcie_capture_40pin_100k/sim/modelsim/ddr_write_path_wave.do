onerror {resume}
quietly WaveActivateNextPane {} 0

add wave -divider "clk / rst"
add wave /ddr_write_path_tb/pixclk_in
add wave /ddr_write_path_tb/axi_clk
add wave /ddr_write_path_tb/rst_n
add wave /ddr_write_path_tb/axi_reset

add wave -divider "RGB888 Input"
add wave /ddr_write_path_tb/de_in
add wave /ddr_write_path_tb/vs_in
add wave -radix hexadecimal /ddr_write_path_tb/r_in
add wave -radix hexadecimal /ddr_write_path_tb/g_in
add wave -radix hexadecimal /ddr_write_path_tb/b_in

add wave -divider "Video Input Registers"
add wave /ddr_write_path_tb/de_in_d0
add wave -radix hexadecimal /ddr_write_path_tb/r_in_d0
add wave -radix hexadecimal /ddr_write_path_tb/g_in_d0
add wave -radix hexadecimal /ddr_write_path_tb/b_in_d0

add wave -divider "RGB565 / Write Side"
add wave /ddr_write_path_tb/wframe_data_en
add wave -radix hexadecimal /ddr_write_path_tb/wframe_data
add wave /ddr_write_path_tb/wframe_vsync
add wave /ddr_write_path_tb/hdmi_vsync_eof

add wave -divider "Frame Buffer Index"
add wave -radix unsigned /ddr_write_path_tb/rc_wframe_index
add wave -radix unsigned /ddr_write_path_tb/rc_rframe_index

add wave -divider "AXI Write"
add wave /ddr_write_path_tb/axi_awvalid
add wave /ddr_write_path_tb/axi_awready
add wave -radix hexadecimal /ddr_write_path_tb/axi_awaddr
add wave /ddr_write_path_tb/axi_wvalid
add wave /ddr_write_path_tb/axi_wready
add wave /ddr_write_path_tb/axi_wlast
add wave -radix hexadecimal /ddr_write_path_tb/axi_wdata

add wave -divider "Scoreboard"
add wave -radix unsigned /ddr_write_path_tb/cvt_err_cnt
add wave -radix unsigned /ddr_write_path_tb/test_err_cnt
add wave -radix unsigned /ddr_write_path_tb/slot_write_bytes_0
add wave -radix unsigned /ddr_write_path_tb/write_burst_cnt

WaveRestoreZoom {0 ns} {40 us}
