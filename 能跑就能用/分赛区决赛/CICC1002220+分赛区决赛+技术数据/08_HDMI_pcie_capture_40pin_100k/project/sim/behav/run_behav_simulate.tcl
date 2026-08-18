# ----------------------------------------

# ips2l_pcie_dma_tb simulation + grouped waves

# Note: PDS may overwrite this file on re-launch; restore from git or re-run:

#   do load_ips2l_pcie_dma_wave.do

# ----------------------------------------



vsim -novopt -L work ips2l_pcie_dma_tb



view wave

view structure

view signals



set _wave_do [file normalize [file join [file dirname [info script]] ips2l_pcie_dma_wave.do]]

if {[file exists $_wave_do]} {

    do $_wave_do

} else {

    add wave *

    echo "WARN: ips2l_pcie_dma_wave.do missing, fallback to add wave *"

}



run 50us



# ----------------------------------------

