@echo off
set bin_path=F:/Modelsim/win64
cd C:/Users/23694/Desktop/ziguang_project/02_pcie_image_test_1080p_v1/08_HDMI_pcie_capture_40pin_100k/project/sim/behav
call "%bin_path%/modelsim"   -do "do {run_behav_compile.tcl};do {run_behav_simulate.tcl}" -l run_behav_simulate.log
if "%errorlevel%"=="1" goto END
if "%errorlevel%"=="0" goto SUCCESS
:END
exit 1
:SUCCESS
exit 0
