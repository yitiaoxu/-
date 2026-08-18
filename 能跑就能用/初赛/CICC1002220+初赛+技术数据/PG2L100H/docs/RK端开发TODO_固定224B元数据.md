# RK 端开发 TODO：固定 224B Metadata + 原图 + ROI

本文档给 RK 端同事使用，目标是适配 FPGA 当前输出格式：

```text
每帧 DMA buffer = 224B metadata + 1280x720 RGB565 原图
```

FPGA 当前每帧最多输出 1 个 ROI，ROI 来自 FPGA 端二值掩膜的整帧最小外接矩形。后续 FPGA 端升级多 ROI 后，RK 端解析结构不需要大改。

---

## 1. 必须先改的常量

原来如果只接收原图：

```c
#define IMG_W      1280
#define IMG_H      720
#define FRAME_SIZE (IMG_W * IMG_H * 2)
```

现在改为：

```c
#define IMG_W       1280u
#define IMG_H       720u
#define META_LEN    224u
#define IMG_BYTES   (IMG_W * IMG_H * 2u)      /* 1843200 */
#define FRAME_SIZE  (META_LEN + IMG_BYTES)    /* 1843424 */
#define ROI_MAGIC   0x524B3031u
```

关键点：

- 每帧总长度从 `1843200` 改为 `1843424`
- 原图像素起点从 `buf` 改为 `buf + 224`
- `buf[0..223]` 是 FPGA metadata，不是图像

---

## 2. 修改 DMA / PCIe 读帧长度

查找 RK 端代码中类似位置：

```c
read(fd, buf, 1280 * 720 * 2);
```

或：

```c
read(fd, buf, FRAME_SIZE);
mmap(..., frame_len, ...);
ioctl(fd, ..., frame_len);
```

统一确认帧长度为：

```c
FRAME_SIZE    /* 1843424 */
```

如果驱动层和应用层都写死了 `1843200`，两边都要同步改。

---

## 3. 新增 Metadata / ROI 结构体

```c
typedef struct {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t width;
    uint32_t height;
    uint32_t roi_count;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
} fpga_frame_meta_t;

typedef struct {
    uint32_t x1;
    uint32_t y1;
    uint32_t x2;
    uint32_t y2;
    uint32_t color;
    uint32_t confidence;
} fpga_roi_t;
```

帧头固定 32 字节，ROI 数组从 `buf + 32` 开始。

---

## 4. 新增帧解析函数

```c
static int parse_fpga_frame(
    uint8_t *buf,
    fpga_frame_meta_t **meta,
    fpga_roi_t **rois,
    uint8_t **rgb565
) {
    fpga_frame_meta_t *m = (fpga_frame_meta_t *)buf;

    if (m->magic != ROI_MAGIC) {
        return -1;
    }

    if (m->width != IMG_W || m->height != IMG_H) {
        return -2;
    }

    if (m->roi_count > 8) {
        m->roi_count = 8;
    }

    *meta  = m;
    *rois  = (fpga_roi_t *)(buf + sizeof(fpga_frame_meta_t)); /* buf + 32 */
    *rgb565 = buf + META_LEN;                                 /* buf + 224 */

    return 0;
}
```

---

## 5. 修改原图解码入口

原来可能是：

```c
rgb565_to_rgb888(frame_buf, rgb888, IMG_W, IMG_H);
```

现在必须改为：

```c
fpga_frame_meta_t *meta;
fpga_roi_t *rois;
uint8_t *rgb565;

if (parse_fpga_frame(frame_buf, &meta, &rois, &rgb565) == 0) {
    rgb565_to_rgb888(rgb565, rgb888, IMG_W, IMG_H);
}
```

核心要求：

```text
所有图像解码、显示、保存、算法输入，都从 rgb565 开始，不再从 frame_buf 开始。
```

---

## 6. ROI 裁剪函数

RGB565 裁剪示例：

```c
static int crop_roi_rgb565(
    const uint8_t *rgb565,
    uint32_t x1,
    uint32_t y1,
    uint32_t x2,
    uint32_t y2,
    uint8_t *crop_buf,
    uint32_t *crop_w,
    uint32_t *crop_h
) {
    if (x1 >= IMG_W || x2 >= IMG_W || y1 >= IMG_H || y2 >= IMG_H) return -1;
    if (x2 <= x1 || y2 <= y1) return -2;

    uint32_t w = x2 - x1 + 1;
    uint32_t h = y2 - y1 + 1;

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = rgb565 + ((y1 + y) * IMG_W + x1) * 2;
        uint8_t *dst = crop_buf + y * w * 2;
        memcpy(dst, src, w * 2);
    }

    *crop_w = w;
    *crop_h = h;
    return 0;
}
```

调用方式：

```c
for (uint32_t i = 0; i < meta->roi_count; i++) {
    uint8_t crop_buf[MAX_CROP_SIZE];
    uint32_t crop_w, crop_h;

    if (crop_roi_rgb565(
            rgb565,
            rois[i].x1, rois[i].y1,
            rois[i].x2, rois[i].y2,
            crop_buf,
            &crop_w,
            &crop_h) == 0) {
        /* crop_buf 可送 OCR / 车牌识别 / 保存文件 */
    }
}
```

---

## 7. 画框显示函数

如果显示层使用 RGB888，可在 RGB888 图像上画框：

```c
static void draw_rect_rgb888(
    uint8_t *rgb888,
    uint32_t x1,
    uint32_t y1,
    uint32_t x2,
    uint32_t y2,
    uint8_t r,
    uint8_t g,
    uint8_t b
) {
    if (x1 >= IMG_W || x2 >= IMG_W || y1 >= IMG_H || y2 >= IMG_H) return;
    if (x2 <= x1 || y2 <= y1) return;

    for (uint32_t x = x1; x <= x2; x++) {
        uint8_t *p1 = rgb888 + (y1 * IMG_W + x) * 3;
        uint8_t *p2 = rgb888 + (y2 * IMG_W + x) * 3;
        p1[0] = r; p1[1] = g; p1[2] = b;
        p2[0] = r; p2[1] = g; p2[2] = b;
    }

    for (uint32_t y = y1; y <= y2; y++) {
        uint8_t *p1 = rgb888 + (y * IMG_W + x1) * 3;
        uint8_t *p2 = rgb888 + (y * IMG_W + x2) * 3;
        p1[0] = r; p1[1] = g; p1[2] = b;
        p2[0] = r; p2[1] = g; p2[2] = b;
    }
}
```

调用：

```c
for (uint32_t i = 0; i < meta->roi_count; i++) {
    draw_rect_rgb888(
        rgb888,
        rois[i].x1,
        rois[i].y1,
        rois[i].x2,
        rois[i].y2,
        255, 0, 0
    );
}
```

---

## 8. 推荐主流程

```c
while (running) {
    uint8_t *frame_buf = get_one_pcie_frame(FRAME_SIZE);

    fpga_frame_meta_t *meta;
    fpga_roi_t *rois;
    uint8_t *rgb565;

    int ret = parse_fpga_frame(frame_buf, &meta, &rois, &rgb565);
    if (ret != 0) {
        continue;
    }

    rgb565_to_rgb888(rgb565, rgb888, IMG_W, IMG_H);

    for (uint32_t i = 0; i < meta->roi_count; i++) {
        draw_rect_rgb888(
            rgb888,
            rois[i].x1,
            rois[i].y1,
            rois[i].x2,
            rois[i].y2,
            255, 0, 0
        );

        crop_roi_rgb565(
            rgb565,
            rois[i].x1,
            rois[i].y1,
            rois[i].x2,
            rois[i].y2,
            crop_buf,
            &crop_w,
            &crop_h
        );
    }

    display_rgb888(rgb888, IMG_W, IMG_H);
}
```

---

## 9. 联调时建议打印

每帧或每 30 帧打印一次：

```c
printf("magic=0x%08x frame=%u w=%u h=%u roi_count=%u\n",
       meta->magic, meta->frame_id, meta->width, meta->height, meta->roi_count);

for (uint32_t i = 0; i < meta->roi_count; i++) {
    printf("roi[%u]: x1=%u y1=%u x2=%u y2=%u color=%u conf=%u\n",
           i,
           rois[i].x1,
           rois[i].y1,
           rois[i].x2,
           rois[i].y2,
           rois[i].color,
           rois[i].confidence);
}
```

---

## 10. 最小验收标准

RK 端第一版只需要验收 4 件事：

1. `magic == 0x524B3031`
2. `width == 1280 && height == 720`
3. 原图从 `buf + 224` 解码后显示正常
4. `roi_count == 0/1`，有 ROI 时能在图上画出红框

---

## 11. 当前 FPGA 端说明

当前 FPGA 端已经实现：

- 原图和 ROI metadata 走同一条 PCIe
- 每帧前 224B 固定 metadata
- 后面紧跟 `1280x720 RGB565` 原图
- ROI 坐标来自 `ch1_roi_bbox_detect.v`
- 当前版本每帧最多 1 个 ROI

当前 ROI 字段：

```text
ROI[0].x1
ROI[0].y1
ROI[0].x2
ROI[0].y2
ROI[0].color      = 0
ROI[0].confidence = 50
```

如果 `roi_count == 0`，说明该帧 FPGA 没检测到前景区域。
