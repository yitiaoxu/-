`timescale 1ns / 1ps
//============================================================================
// TB：ch1_roi_bbox_detect + pcie_ch1_meta_solo
//
// compile：tb_05_roi_pcie_meta.v +
//          ch1_roi_bbox_detect.v + pcie_ch1_meta_solo.v
//
// 默认仅跑前 48 拍 DMA 以快速出波形；全帧（约 115214 拍）请加编译宏： +define+FULL_DMA_FRAME
//============================================================================

module tb_05_roi_pcie_meta;

    reg clk_pix = 0;
    reg clk_div = 0;
    reg rst_pix = 0;
    reg rst_div = 0;

    reg        de_in = 0;
    reg        vs_in = 0;
    reg [15:0] din   = 0;

    wire [32*6*8-1:0] roi_flat;
    wire [3:0]       roi_cnt;
    wire [31:0]      frame_id_roi;

    reg              dma_wr_data_req = 0;
    reg [127:0]      ch1_data_pat    = 0;

    wire [127:0] dma_wr_data;
    wire        ch1_data_req;

    integer k;

    always #4 clk_pix = ~clk_pix;
    always #10 clk_div = ~clk_div;

    localparam integer FW = 220;
    localparam integer FH = 100;

    ch1_roi_bbox_detect #(
        .W          (FW),
        .H          (FH),
        .ROW_TH     (16),
        .MIN_W      (60),
        .MIN_H      (15),
        .MAX_W      (700),
        .MAX_H      (200),
        .GOOD_HIT_TH(800)
    ) u_roi (
        .clk_pix     (clk_pix),
        .rst_n_pix   (rst_pix),
        .de_in       (de_in),
        .vs_in       (vs_in),
        .din         (din),
        .clk_div2    (clk_div),
        .rst_n_div2  (rst_div),
        .o_roi_flat  (roi_flat),
        .o_roi_count (roi_cnt),
        .o_frame_id  (frame_id_roi)
    );

    pcie_ch1_meta_solo u_meta (
        .clk             (clk_div),
        .rst_n           (rst_div),
        .dma_wr_data_req (dma_wr_data_req),
        .i_roi_count     (roi_cnt),
        .i_roi_flat      (roi_flat),
        .i_frame_id      (frame_id_roi),
        .dma_wr_data     (dma_wr_data),
        .ch1_data_req    (ch1_data_req),
        .ch1_data        (ch1_data_pat),
        .o_pre_dma_pulse ()
    );

    task automatic roi_frame_white_box;
        integer r, x;
        begin
            @(posedge clk_pix);
            vs_in = 1'b1;

            // 白条带：宽约 80、高约 22，居中，满足宽高比近似车牌条带（示例）
            for (r = 0; r < FH; r = r + 1) begin
                for (x = 0; x < FW; x = x + 1) begin
                    @(posedge clk_pix);
                    de_in = 1'b1;
                    if (r >= 35 && r <= 56 && x >= 60 && x <= 160)
                        din = 16'hFFFF;
                    else
                        din = 16'h0000;
                end
                @(posedge clk_pix);
                de_in = 0;
                @(posedge clk_pix);
            end

            @(posedge clk_pix);
            vs_in = 1'b0;
            @(posedge clk_pix);
        end
    endtask

    initial begin
        rst_pix = 0;
        rst_div = 0;
        vs_in   = 0;
        de_in   = 0;
        din     = 0;

        repeat (30) @(posedge clk_div);
        rst_pix = 1;
        rst_div = 1;
        repeat (20) @(posedge clk_div);

        roi_frame_white_box;

        // 留时间给 CDC 锁存 roi / frame_id
        repeat (40) @(posedge clk_div);

`ifdef FULL_DMA_FRAME
        for (k = 0; k < 115214; k = k + 1) begin
            @(posedge clk_div);
            dma_wr_data_req = 1'b1;
            ch1_data_pat    = {4{32'hA5A5BEEF + k}};
            if (k == 0) #1 $display("[TB05] meta beat0 low32=0x%h (RTL 为 {h,w,fid,magic} 低 32bit 为 magic)", dma_wr_data[31:0]);
        end
`else
        for (k = 0; k < 48; k = k + 1) begin
            @(posedge clk_div);
            dma_wr_data_req = 1'b1;
            ch1_data_pat    = {4{32'h0000_C000 + k[15:0]}};
            if (k == 0) #1 $display("[TB05] meta beat0 low32=0x%h (应为 524B3031)", dma_wr_data[31:0]);
        end
`endif

        @(posedge clk_div);
        dma_wr_data_req = 0;

        repeat (256) @(posedge clk_div);

        `ifndef FULL_DMA_FRAME
            $display("[TB05] 当前为快速 48 拍；全帧请加 +define+FULL_DMA_FRAME 后重跑。");
        `endif
        $display("[TB05] DONE.");
        $finish;
    end
endmodule
