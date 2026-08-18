#ifndef PCIE_DMA_READ_TEST_H
#define PCIE_DMA_READ_TEST_H

#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>

#define PCIE_DRIVER_FILE_PATH "/dev/pango_pci_driver"

#define TYPE 'S'
#define PCI_MAP_ADDR_CMD _IOWR(TYPE, 2, int)
#define PCI_DMA_WRITE_CMD _IOWR(TYPE, 5, int)
#define PCI_READ_FROM_KERNEL_CMD _IOWR(TYPE, 6, int)
#define PCI_UMAP_ADDR_CMD _IOWR(TYPE, 7, int)
#define PCI_READ_DMA_DATA_CMD _IOWR(TYPE, 10, int)
#define PCI_MAP_FRAME_CMD _IOWR(TYPE, 11, int)
#define PCI_READ_FRAME_CMD _IOWR(TYPE, 12, int)
#define PCI_MAP_INFER_FRAME_CMD _IOWR(TYPE, 13, int)
#define PCI_READ_INFER_FRAME_CMD _IOWR(TYPE, 14, int)
#define PCI_GET_PLATE_ROI_CMD _IOWR(TYPE, 15, int)

#define PCIE_FRAME_WIDTH 1280
#define PCIE_FRAME_HEIGHT 720
#define PCIE_ROW_BYTES 2560
#define PCIE_ROW_STRIDE 4096
#define PCIE_FRAME_BYTES (PCIE_FRAME_WIDTH * PCIE_FRAME_HEIGHT * 2)
#define PCIE_FRAME_MAP_BYTES (PCIE_FRAME_HEIGHT * PCIE_ROW_STRIDE)
#define PCIE_RGB888_ROW_BYTES (PCIE_FRAME_WIDTH * 3)
#define PCIE_RGB888_ROW_STRIDE 4096
#define PCIE_RGB888_FRAME_BYTES (PCIE_FRAME_WIDTH * PCIE_FRAME_HEIGHT * 3)
#define PCIE_RGB888_FRAME_MAP_BYTES (PCIE_FRAME_HEIGHT * PCIE_RGB888_ROW_STRIDE)

#define DMA_MAX_PACKET_SIZE 4096

typedef enum _OPERATION_NUM_ {
    write_num = 0,
    read_num,
    w_r_num,
    info_num,
    tandem_num,
    performance_num
} op_num;

typedef struct _BAR_INFO_ {
    unsigned long bar_base;
    unsigned long bar_len;
} BAR_BASE_INFO;

typedef struct _CAP_INFO_ {
    unsigned char flag;
    unsigned char id;
    unsigned char addr_offset;
    unsigned char next_offset;
} CAP_INFO;

typedef struct _CAP_LIST_ {
    unsigned char cap_status;
    unsigned char cap_error;
    CAP_INFO cap_buf[256];
} CAP_LIST;

typedef struct _PCI_INFO_ {
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int cmd_reg;
    unsigned int status_reg;
    unsigned int revision_id;
    unsigned int class_prog;
    unsigned int class_device;
    BAR_BASE_INFO bar[6];
    unsigned int min_gnt;
    unsigned int max_lat;
    unsigned int link_speed;
    unsigned int link_width;
    unsigned int mps;
    unsigned int mrrs;
    unsigned int data[1024];
} PCI_DEVICE_INFO;

typedef struct _LOAD_DATA_ {
    unsigned int num_words;
    unsigned int block_words[1024];
} LOAD_DATA_INFO;

typedef struct _PCI_LOAD_ {
    unsigned char link_status;
    unsigned int crc;
    unsigned char axi_direction;
    unsigned char load_status;
    unsigned int total_num_words;
    LOAD_DATA_INFO data_block;
} PCI_LOAD_INFO;

typedef struct _COMMAND_ {
    unsigned char w_r;
    unsigned char step;
    unsigned int addr;
    unsigned int data;
    unsigned int cnt;
    unsigned int delay;
    PCI_DEVICE_INFO get_pci_dev_info;
    CAP_LIST cap_info;
    PCI_LOAD_INFO load_info;
} COMMAND_OPERATION;

typedef struct _DMA_DATA_ {
    unsigned char read_buf[DMA_MAX_PACKET_SIZE];
    unsigned char write_buf[DMA_MAX_PACKET_SIZE];
} DMA_DATA;

typedef struct _DMA_OPERATION_ {
    unsigned int current_len;
    unsigned int offset_addr;
    unsigned int cmd;
    DMA_DATA data;
} DMA_OPERATION;

typedef struct _FRAME_CAPTURE_ {
    unsigned int width;
    unsigned int height;
    unsigned int row_bytes;
    unsigned long user_buf;
} FRAME_CAPTURE;

typedef struct _PLATE_ROI_INFO_ {
    unsigned short x;
    unsigned short y;
    unsigned short w;
    unsigned short h;
    unsigned char valid;
    unsigned char frame_id;
    unsigned short reserved;
} PLATE_ROI_INFO;

#endif // PCIE_DMA_READ_TEST_H
