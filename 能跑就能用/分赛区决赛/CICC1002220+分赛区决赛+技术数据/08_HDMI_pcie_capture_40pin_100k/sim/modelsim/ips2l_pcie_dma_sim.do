# GUI wave session for ips2l_pcie_dma unit TB
# Run from sim/modelsim: vsim -do ips2l_pcie_dma_sim.do
set ROOT [file normalize [file join [pwd] ../..]]

cd [file join $ROOT project/sim/behav]
do run_ips2l_pcie_dma_compile.tcl

vsim -novopt work.ips2l_pcie_dma_tb

do [file join $ROOT sim/modelsim/ips2l_pcie_dma_wave.do]

run 50us

echo "=== Wave groups loaded: expand 'DMA_Wave' in Wave window ==="
echo "=== Zoom window: 300ns .. 20us (TC-DMA03~04 BAR1 ADDR -> MWR upload) ==="
