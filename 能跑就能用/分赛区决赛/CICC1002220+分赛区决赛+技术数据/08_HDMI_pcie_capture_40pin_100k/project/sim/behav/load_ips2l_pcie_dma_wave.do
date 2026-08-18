# Load grouped DMA waves from any vsim cwd.
# Recommended (always works):
#   do C:/Users/23694/Desktop/ziguang_project/02_pcie_image_test_1080p_v1/08_HDMI_pcie_capture_40pin_100k/project/sim/behav/load_ips2l_pcie_dma_wave.do
onerror {resume}

set _loader_dir [file dirname [info script]]
set _found ""

foreach _c [list \
    [file join $_loader_dir ips2l_pcie_dma_wave.do] \
    [file join $_loader_dir ../../../sim/modelsim/ips2l_pcie_dma_wave.do] \
    [file join [pwd] ips2l_pcie_dma_wave.do] \
    [file join [pwd] sim/modelsim/ips2l_pcie_dma_wave.do] \
    [file join [pwd] ../sim/modelsim/ips2l_pcie_dma_wave.do] \
] {
    set _p [file normalize $_c]
    if {[file exists $_p]} {
        set _found $_p
        break
    }
}

if {$_found eq ""} {
    echo "ERROR: ips2l_pcie_dma_wave.do not found"
    echo "       loader_dir=$_loader_dir"
    echo "       cwd=[pwd]"
} else {
    echo "Loading wave groups: $_found"
    do $_found
}
