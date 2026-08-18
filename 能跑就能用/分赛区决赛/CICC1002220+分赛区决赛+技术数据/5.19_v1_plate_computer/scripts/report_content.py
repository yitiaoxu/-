# -*- coding: utf-8 -*-

TITLE = u"\u8f66\u724c\u8bc6\u522b\u7cfb\u7edf\u5ef6\u8fdf\u5206\u6790\u4e0e\u4f18\u5316\u6280\u672f\u62a5\u544a"
SUBTITLE = u"RK3568 + RKNN INT8 + PCIe 1280\u00d7720 \u00b7 5.19_v1_plate_computer"

SECTIONS = []


def block(h1=None, h2=None, paras=None, paras_after=None, bullets=None, table=None, code=None):
    SECTIONS.append({
        "h1": h1, "h2": h2,
        "paras": paras or [],
        "paras_after": paras_after or [],
        "bullets": bullets or [],
        "table": table,
        "code": code,
    })


block(paras=[
    u"\u9879\u76ee\u8def\u5f84\uff1a/userdata/CY777/5.19_v1_plate_computer",
    u"\u672c\u62a5\u544a\u6c47\u603b\uff1a\u5404\u9636\u6bb5\u5ef6\u8fdf\u5b9e\u6d4b\u3001\u5df2\u91c7\u53d6\u4f18\u5316\u3001"
    u"INT8 RKNN \u4e0e CPU \u53cd\u91cf\u5316\u5904\u7406\u3002",
])

block(h1=u"\u4e00\u3001\u5ef6\u8fdf\u53e3\u5f84\u8bf4\u660e",
      table=(
          [u"\u6307\u6807", u"\u542b\u4e49", u"\u8d77\u6b62\u70b9"],
          [
              ["pcie_dma", u"PCIe \u53d6\u56fe", u"DMA \u5f00\u59cb \u2192 \u8bfb\u5b8c\u4e00\u5e27"],
              ["rgb_convert", u"RGB565/888 \u2192 BGR", "captureInferBgrFrame"],
              ["process_wall", u"\u7b97\u6cd5\u603b\u8017\u65f6", u"letterbox + detect + rec + draw"],
              ["e2e_total", u"\u53d6\u56fe\u540e\u5230 UI \u5c31\u7eea", u"DMA \u7ed3\u675f\u540e \u2192 \u5b8c\u6210"],
          ],
      ),
      paras=[
          u"\u7ec8\u7aef [latency] \u5206\u6bb5\u65e5\u5fd7\u5728 PLATE_PROFILE=1 \u65f6\u6253\u5370\uff08\u9ed8\u8ba4\u5f00\u542f\uff09\u3002",
          u"\u6ce8\uff1ae2e \u4e0d\u542b pcie_dma\u3002\u4e32\u884c\u6574\u5e27\u5468\u671f \u2248 27 + 88 \u2248 115 ms\u3002",
      ])

block(h1=u"\u4e8c\u3001\u5404\u9636\u6bb5\u5ef6\u8fdf\u5b9e\u6d4b\uff08PCIe frame\u2248347/348\uff0c\u53cc\u8f66\u724c\uff09",
      table=(
          [u"\u9636\u6bb5", u"\u8017\u65f6(ms)", u"\u5360 process_wall", u"\u5907\u6ce8"],
          [
              ["pcie_dma", "27~28", u"\u2014", u"\u9010\u884c DMA \u504f\u9ad8"],
              ["rgb_convert", "6", u"\u2014", u"CPU \u8f6c\u8272"],
              ["letterbox", "4~5", "~6%", u"1280\u00d7720 \u2192 640"],
              ["det_input", "1.7", "~2%", u"\u6784\u9020 NPU \u8f93\u5165"],
              ["det_npu", "25~26", "~32%", u"rknn_run INT8"],
              ["det_output", "11~12", "~15%", u"outputs_get + \u53cd\u91cf\u5316"],
              ["det_decode", "0.2~0.3", "<1%", u"3-head decode"],
              ["det_post", "0.1", "<1%", "NMS"],
              ["detect(total)", "57~58", "~71%", u"\u68c0\u6d4b\u5168\u6d41\u7a0b"],
              ["rec_roi", "0.9", "~1%", u"ROI \u88c1\u526a"],
              ["rec_npu", "2.8~3.0", "~4%", u"\u8bc6\u522b NPU"],
              ["rec(total)", "4.6~5.0", "~6%", u"2 \u5757\u8f66\u724c"],
              ["draw(\u4f18\u5316\u524d)", "13~14", "~17%", u"clone + FreeType \u4e2d\u6587"],
              ["draw(\u53ea\u753b\u6846\u540e)", "2~4", "~3%", u"\u9884\u4f30"],
              ["qimage", "1.3", u"\u2014", u"Mat \u2192 QImage"],
              ["process_wall", "80~81", "100%", u"infer_wall + draw"],
              ["e2e_total", "85~88", u"\u2014", u"\u4e0d\u542b DMA"],
          ],
      ),
      bullets=[
          u"\u74f6\u9888\u6392\u5e8f\uff1apcie_dma > det_npu > det_output > draw > letterbox / rgb / rec",
          u"rec \u4ec5 ~3 ms\uff0c\u975e\u4e3b\u8981\u74f6\u9888",
      ])

block(h1=u"\u4e09\u3001\u5df2\u91c7\u53d6\u7684\u4f18\u5316\u63aa\u65bd")

block(h2=u"3.1 \u6027\u80fd\u4e0e\u53ef\u89c2\u6d4b\u6027",
      bullets=[
          u"\u5168\u94fe\u8def [latency] Profiling\uff1aHostFrameTimings + PlateTimings\uff0cPLATE_PROFILE=1 \u9ed8\u8ba4\u5f00\u542f",
          u"PLATE_PROFILE_EVERY=N \u63a7\u5236\u6253\u5370\u9891\u7387",
          u"UI \u9876\u680f\u663e\u793a process_wall \u4e0e \u7aef\u5230\u7aef",
      ])

block(h2=u"3.2 UI \u4e0e\u4ea4\u4e92\u4f18\u5316",
      bullets=[
          u"\u5de6\u4fa7\u9762\u677f\u5b9e\u65f6\u663e\u793a\u8f66\u724c\u53f7\uff08\u767d\u8272\u5927\u5b57\uff09",
          u"\u8f66\u724c\u7ed3\u679c\u53d8\u5316\u624d\u66f4\u65b0\uff0c\u907f\u514d\u6bcf\u5e27\u5237\u65b0\u5806\u79ef\u5361\u6b7b",
          u"\u5e27\u5408\u5e76\u5237\u65b0 + Qt FastTransformation",
          u"\u300c\u754c\u9762\u521d\u59cb\u5316\u300d\u4ec5\u91cd\u7f6e UI\uff0c\u4e0d\u52a0\u8f7d\u9a71\u52a8",
          u"\u6279\u91cf\u652f\u6301\u5355\u56fe/\u6587\u4ef6\u5939/\u89c6\u9891",
      ])

block(h2=u"3.3 \u7b97\u6cd5\u4e0e\u53ef\u89c6\u5316",
      bullets=[
          u"INT8 \u4e09\u5934 decode + obj \u65e9\u7b5b\uff08logitThreshold\uff09",
          u"VizStabilizer \u68c0\u6d4b\u6846 EMA \u5e73\u6ed1\uff08PLATE_VIZ_SMOOTH\uff09",
          u"\u53ea\u753b\u6846\u4e0d\u753b\u5b57\uff1aviz_draw_labels=false\uff0cPLATE_DRAW_LABELS=1 \u6062\u590d\u6807\u7b7e",
          u"\u7a33\u5b9a\u77e9\u5f62\u6846\uff08PLATE_VIZ_STABLE_RECT\uff09",
      ])

block(h2=u"3.4 \u5de5\u7a0b\u5316",
      bullets=[
          u"plate_rknn_pipeline \u4e0e Qt/CLI \u5171\u7528",
          u"PCIe 1280\u00d7720 \u6574\u5e27/\u9010\u884c DMA\uff08PCIE_USE_FRAME\uff09",
          u"FPGA RGB888 \u8def\u5f84\uff08PCIE_FPGA_RGB888\uff09",
          u"run.sh \u52a0\u8f7d pango_pci_driver",
      ])

block(h2=u"3.5 \u4f18\u5316\u6548\u679c\u9884\u4f30",
      table=(
          [u"\u4f18\u5316\u9879", u"\u5f71\u54cd\u9636\u6bb5", u"\u9884\u671f\u6536\u76ca"],
          [
              [u"\u53ea\u753b\u6846\u4e0d\u753b\u5b57", "draw", u"14 ms \u2192 2~4 ms"],
              ["PCIE_USE_FRAME=1", "pcie_dma", u"27 ms \u2192 8~12 ms"],
              ["PCIE_FPGA_RGB888=1", "rgb_convert", u"6 ms \u2192 2~3 ms"],
              [u"PCIe \u53cc\u7f13\u51b2\uff08\u672a\u5b9e\u73b0\uff09", u"\u6574\u5e27\u5468\u671f", u"\u7701 ~27 ms \u6392\u961f"],
          ],
      ))

block(h1=u"\u56db\u3001INT8 RKNN \u6a21\u578b\u4e0e CPU \u53cd\u91cf\u5316\u5904\u7406\u8be6\u89e3")

block(h2=u"4.1 \u6a21\u578b\u4f53\u7cfb",
      bullets=[
          u"\u68c0\u6d4b\uff1aplate_detect_int8.rknn\uff0c\u8f93\u5165 640 letterbox\uff0c\u8f93\u51fa 3 \u5934\uff08stride 8/16/32\uff09",
          u"\u5143\u6570\u636e\uff1aweights/RK_plate_detect_meta.txt\uff08nc=2, anchors\uff09",
          u"\u8bc6\u522b\uff1aplate_rec_color_int8.rknn\uff0cROI \u2192 \u8f66\u724c\u5b57\u7b26\u4e32 + \u989c\u8272",
          u"PLATE_USE_FP=1 \u5f3a\u5236 FP \u6a21\u578b",
      ])

block(h2=u"4.2 NPU \u4e0e CPU \u5206\u5de5",
      paras=[u"NPU \u6267\u884c INT8 \u5377\u79ef\uff1bCPU \u8d1f\u8d23\u524d\u540e\u5904\u7406\u4e0e\u53cd\u91cf\u5316\u3002"],
      table=(
          [u"\u9636\u6bb5", u"\u8fd0\u884c\u4f4d\u7f6e", u"INT8 \u76f8\u5173"],
          [
              ["det_input / rec_roi", "CPU", "uint8 NHWC \u6253\u5305"],
              ["det_npu / rec_npu", "NPU", u"INT8 \u5377\u79ef\u52a0\u901f"],
              ["det_output", "CPU + RKNN API", u"outputs_get + \u53cd\u91cf\u5316"],
              ["det_decode / det_post", "CPU", u"float decode + NMS"],
          ],
      ))

block(h2=u"4.3 \u8f93\u5165\uff1auint8_nhwc \u4e0e pass_through",
      bullets=[
          u"INT8 \u6a21\u578b\u5f3a\u5236 input_mode=uint8_nhwc",
          u"buildDetectInput \u5c06 letterbox \u540e BGR \u6309 NHWC uint8 \u9001\u5165 NPU",
          u"pass_through=0 \u9ed8\u8ba4\uff1b\u5931\u8d25\u65f6 pass_through=1 \u91cd\u8bd5\uff08uint8_nhwc_pt\uff09",
      ])

block(h2=u"4.4 \u8f93\u51fa\u53cd\u91cf\u5316\uff08\u6838\u5fc3\uff09",
      paras=[
          u"RknnSession::infer \u5148\u8c03\u7528 rknn_outputs_get(want_float=1)\u3002"
          u"\u82e5\u8f93\u51fa\u5168\u96f6\uff08\u6821\u51c6\u5f02\u5e38\uff09\uff0cCPU \u624b\u52a8\u53cd\u91cf\u5316\uff1a",
      ],
      code=(
          u"Affine asymmetric:  float = (int8 - zp) * scale\n"
          u"DFP:                float = int8 * (1 / 2^fl)\n"
          u"\n\u2192 float \u7279\u5f81\u56fe \u2192 decodeThreeHeads \u2192 postProcessing(NMS)"
      ),
      paras_after=[
          u"det_output \u7ea6 11~12 ms \u4e3b\u8981\u6765\u81ea rknn_outputs_get \u5185\u90e8\u62f7\u8d1d\u4e0e"
          u"\u9010\u5143\u7d20\u53cd\u91cf\u5316\uff08\u4e09\u5934\u7279\u5f81\u56fe\u5143\u7d20\u91cf\u5927\uff09\u3002",
          u"\u82e5 want_float \u5931\u8d25\u89e6\u53d1 manual INT8 dequant\uff0c\u4f1a\u989d\u5916\u518d fetch \u4e00\u6b21\u3002",
      ])

block(h2=u"4.5 \u4e09\u5934 CPU decode",
      bullets=[
          u"decodeThreeHeads \u5bf9 stride 8/16/32 \u4e09\u4e2a\u5934\u6267\u884c decodeOneHeadAppend",
          u"obj logit \u65e9\u7b5b\uff08logitThreshold\uff09\u540e\u518d sigmoid \u89e3 box/landmarks",
          u"det_decode \u5b9e\u6d4b ~0.2 ms\uff0c\u975e\u74f6\u9888\uff1bdet_output \u662f",
      ])

block(h2=u"4.6 \u8bc6\u522b\u6a21\u578b INT8",
      paras=[u"getPlateResult \u540c\u6837\u901a\u8fc7 infer \u53d6 float logits\uff0cCPU argmax \u89e3\u7801\u8f66\u724c\u4e0e\u989c\u8272\u3002rec_npu ~3 ms\u3002"])

block(h2=u"4.7 INT8 \u6545\u969c\u6392\u67e5",
      table=(
          [u"\u73b0\u8c61", u"\u539f\u56e0", u"\u5904\u7406"],
          [
              [u"\u68c0\u6d4b\u5934\u5168 0", u"\u8f93\u5165/\u6821\u51c6", u"uint8_nhwc / pass_through"],
              [u"\u68c0\u6d4b\u5934\u5168 0.5", u"INT8 \u91cf\u5316\u5931\u8d25", u"\u91cd\u8f6c RKNN \u6216 FP"],
              [u"\u65e0\u6846", u"\u7f3a meta", u"RK_plate_detect_meta.txt"],
              [u"manual dequant \u65e5\u5fd7", u"want_float \u65e0\u6548", u"CPU \u53cd\u91cf\u5316\u515c\u5e95"],
          ],
      ))

block(h1=u"\u4e94\u3001PCIe \u6574\u4f53\u6570\u636e\u6d41",
      code=(
          "FPGA 1280x720 RGB565\n"
          "    | pcie_dma        ~27 ms\n"
          "    | rgb_convert     ~6 ms\n"
          "    | letterbox       ~5 ms\n"
          "    | det_npu INT8    ~26 ms\n"
          "    | det_output      ~12 ms  (CPU dequant)\n"
          "    | det_decode+NMS  ~0.3 ms\n"
          "    | rec_npu         ~3 ms\n"
          "    | draw            ~2-14 ms\n"
          "    | qimage          ~1.3 ms\n"
          "    v Qt \u663e\u793a + \u5de6\u4fa7\u8f66\u724c\u53f7"
      ))

block(h1=u"\u516d\u3001\u540e\u7eed\u4f18\u5316\u5efa\u8bae",
      bullets=[
          u"\u9a8c\u8bc1\u53ea\u753b\u6846\u540e draw / process_wall \u4e0b\u964d",
          u"\u5f00\u542f PCIE_USE_FRAME=1 + PCIE_FPGA_RGB888=1",
          u"PCIe \u53cc\u7f13\u51b2\uff1aInferenceThread \u62c6\u5206\u91c7\u96c6\u4e0e\u63a8\u7406\u7ebf\u7a0b",
          u"FPGA ROI \u88c1\u526a\u540e\u518d\u68c0\u6d4b",
          u"\u4f18\u5316 det_output / \u6a21\u578b 416 / RK3588",
      ])

block(h2=u"\u73af\u5883\u53d8\u91cf\u901f\u67e5",
      table=(
          [u"\u53d8\u91cf", u"\u9ed8\u8ba4", u"\u4f5c\u7528"],
          [
              ["PLATE_PROFILE", "1", u"[latency] \u65e5\u5fd7"],
              ["PLATE_PROFILE_EVERY", "1", u"\u6bcf N \u5e27\u6253\u5370"],
              ["PLATE_DRAW_LABELS", "0", u"1=\u753b\u6846+\u6587\u5b57"],
              ["PLATE_USE_FP", "0", u"1=\u5f3a\u5236 FP"],
              ["PCIE_USE_FRAME", "0", u"1=\u6574\u5e27 DMA"],
              ["PCIE_FPGA_RGB888", "0", u"1=RGB888 \u53d6\u56fe"],
              ["PLATE_VIZ_SMOOTH", "1(PCIe)", u"\u6846 EMA \u5e73\u6ed1"],
          ],
      ))

block(paras=[u"\u62a5\u544a\u751f\u6210\u65e5\u671f\uff1a2026 \u5e74"])
