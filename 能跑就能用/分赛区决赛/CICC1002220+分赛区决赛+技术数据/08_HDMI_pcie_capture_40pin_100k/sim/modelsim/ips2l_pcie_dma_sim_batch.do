# Batch regression for ips2l_pcie_dma unit TB
set ROOT [file normalize [file join [pwd] ../..]]

cd [file join $ROOT project/sim/behav]
do run_ips2l_pcie_dma_compile.tcl

vsim -c -novopt work.ips2l_pcie_dma_tb -l ips2l_pcie_dma_sim.log
run -all
quit -f
