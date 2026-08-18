# One-shot timing probe for ips2l_pcie_dma_tb (128x1 mode, clk=8ns)
set TB /ips2l_pcie_dma_tb
set CTRL ${TB}/u_dut/u_ips2l_pcie_dma_controller

proc log_event {tag} {
    global now TB CTRL
    set addr [examine -radix hex ${CTRL}/dma_cmd_l_addr]
    set mwr [examine ${CTRL}/o_mwr32_req]
    set req [examine ${TB}/o_dma_write_data_req]
    set req_cnt [examine -radix unsigned ${TB}/req_pulse_cnt]
    set sb_base [examine -radix hex ${TB}/sb_base_addr]
    set sb_done [examine ${TB}/sb_xfer_done]
    set done [examine ${CTRL}/dma_status_done]
    echo "TIME=${now}ps EVENT=$tag mwr32=$mwr req=$req req_cnt=$req_cnt sb_base=$sb_base sb_done=$sb_done dma_done=$done addr=$addr"
}

when -label w_cmd_vld "${CTRL}/dma_cmd_reg_vld == 1" {
    log_event "BAR1_CMD_0x100_captured"
}

when -label w_addr_vld "${CTRL}/dma_cmd_l_addr_vld == 1" {
    log_event "BAR1_ADDR_0x110_captured"
}

when -label w_mwr_rise "${CTRL}/o_mwr32_req == 1" {
    log_event "mwr32_req_assert"
}

when -label w_req1 "${TB}/o_dma_write_data_req == 1 && ${TB}/req_pulse_cnt == 0" {
    log_event "first_dma_write_data_req"
}

when -label w_req16 "${TB}/req_pulse_cnt == 15 && ${TB}/o_dma_write_data_req == 1" {
    log_event "last_dma_write_data_req"
}

when -label w_sb_base "${TB}/sb_base_addr == 32'hA0000000" {
    log_event "scoreboard_base_addr_match"
}

when -label w_sb_done "${TB}/sb_xfer_done == 1" {
    log_event "scoreboard_xfer_done"
}

when -label w_done_rise "${CTRL}/dma_status_done == 1" {
    log_event "dma_status_done_set"
}

run -all
global now
echo "FINISHED_TIME=${now}ps"
