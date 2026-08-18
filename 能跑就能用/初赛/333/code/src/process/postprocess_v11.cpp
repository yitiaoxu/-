#include "postprocess_v11.h"

#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <cmath>

#include "utils/logging.h"

namespace yolov11
{
    typedef struct
    {
        float xmin;
        float ymin;
        float xmax;
        float ymax;
        float score;
        int classId;
    } DetectRect;
    
    static int input_w = 640;
    static int input_h = 640;
    static float objectThreshold = 0.35;
    static float nmsThreshold = 0.7;
    static int headNum = 3;
    static int class_num = 2;
    static int strides[3] = {8, 16, 32};
    static int mapSize[3][2] = {{80, 80}, {40, 40}, {20, 20}};
    
#define ZQ_MAX(a, b) ((a) > (b) ? (a) : (b))
#define ZQ_MIN(a, b) ((a) < (b) ? (a) : (b))

    static inline float fast_exp(float x)
    {
        // return exp(x);
        union
        {
            uint32_t i;
            float f;
        } v;
        v.i = (12102203.1616540672 * x + 1064807160.56887296);
        return v.f;
    }

    float sigmoid(float x)
    {
        return 1 / (1 + fast_exp(-x));
    }

    static inline float IOU(float XMin1, float YMin1, float XMax1, float YMax1, float XMin2, float YMin2, float XMax2, float YMax2)
    {
        float Inter = 0;
        float Total = 0;
        float XMin = 0;
        float YMin = 0;
        float XMax = 0;
        float YMax = 0;
        float Area1 = 0;
        float Area2 = 0;
        float InterWidth = 0;
        float InterHeight = 0;

        XMin = ZQ_MAX(XMin1, XMin2);
        YMin = ZQ_MAX(YMin1, YMin2);
        XMax = ZQ_MIN(XMax1, XMax2);
        YMax = ZQ_MIN(YMax1, YMax2);

        InterWidth = XMax - XMin;
        InterHeight = YMax - YMin;

        InterWidth = (InterWidth >= 0) ? InterWidth : 0;
        InterHeight = (InterHeight >= 0) ? InterHeight : 0;

        Inter = InterWidth * InterHeight;

        Area1 = (XMax1 - XMin1) * (YMax1 - YMin1);
        Area2 = (XMax2 - XMin2) * (YMax2 - YMin2);

        Total = Area1 + Area2 - Inter;

        return float(Inter) / float(Total);
    }

    static float DeQnt2F32(int8_t qnt, int zp, float scale)
    {
        return ((float)qnt - (float)zp) * scale;
    }

    std::vector<float> GenerateMeshgrid()
    {
        std::vector<float> meshgrid;
        if (headNum == 0)
        {
            NN_LOG_ERROR("=== yolov11 Meshgrid Generate failed! ");
            exit(-1);
        }

        for (int index = 0; index < headNum; index++)
        {
            for (int i = 0; i < mapSize[index][0]; i++)
            {
                for (int j = 0; j < mapSize[index][1]; j++)
                {
                    meshgrid.push_back(float(j + 0.5));
                    meshgrid.push_back(float(i + 0.5));
                }
            }
        }

        printf("=== yolov11 Meshgrid Generate success! \n");
        return meshgrid;
    }

    // Int8 version
    int GetConvDetectionResultInt8(int8_t **pBlob, std::vector<int> &qnt_zp, std::vector<float> &qnt_scale,
                                  std::vector<float> &DetectiontRects)
    {
        static auto meshgrid = GenerateMeshgrid();
        int ret = 0;

        int gridIndex = -2;
        float cls_max = 0;
        int cls_index = 0;

        int quant_zp_cls = 0, quant_zp_reg = 0;
        float quant_scale_cls = 0, quant_scale_reg = 0;

        DetectRect temp;
        std::vector<DetectRect> detectRects;

        for (int index = 0; index < headNum; index++)
        {
            int8_t *cls = (int8_t *)pBlob[index * 2 + 0];
            int8_t *reg = (int8_t *)pBlob[index * 2 + 1];

            quant_zp_cls = qnt_zp[index * 2 + 0];
            quant_zp_reg = qnt_zp[index * 2 + 1];

            quant_scale_cls = qnt_scale[index * 2 + 0];
            quant_scale_reg = qnt_scale[index * 2 + 1];

            for (int h = 0; h < mapSize[index][0]; h++)
            {
                for (int w = 0; w < mapSize[index][1]; w++)
                {
                    gridIndex += 2;

                    // Find max class confidence
                    cls_max = 0;
                    cls_index = 0;
                    for (int cl = 0; cl < class_num; cl++)
                    {
                        float cls_val = DeQnt2F32(cls[cl * mapSize[index][0] * mapSize[index][1] + h * mapSize[index][1] + w],
                                                quant_zp_cls, quant_scale_cls);
                        cls_val = sigmoid(cls_val);
                        
                        if (cl == 0 || cls_val > cls_max)
                        {
                            cls_max = cls_val;
                            cls_index = cl;
                        }
                    }

                    if (cls_max > objectThreshold)
                    {
                        // Process DFL (Distribution Focal Loss) for regression values
                        float regdfl[4] = {0};
                        
                        for (int lc = 0; lc < 4; lc++)
                        {
                            float sfsum = 0;
                            float locval = 0;
                            
                            // Calculate softmax for each group of 16 values
                            for (int df = 0; df < 16; df++)
                            {
                                float temp = fast_exp(DeQnt2F32(
                                    reg[((lc * 16) + df) * mapSize[index][0] * mapSize[index][1] + h * mapSize[index][1] + w],
                                    quant_zp_reg, quant_scale_reg));
                                sfsum += temp;
                            }
                            
                            // Calculate weighted sum
                            for (int df = 0; df < 16; df++)
                            {
                                float temp = fast_exp(DeQnt2F32(
                                    reg[((lc * 16) + df) * mapSize[index][0] * mapSize[index][1] + h * mapSize[index][1] + w],
                                    quant_zp_reg, quant_scale_reg));
                                float sfval = temp / sfsum;
                                locval += sfval * df;
                            }
                            
                            regdfl[lc] = locval;
                        }

                        float x1 = (meshgrid[gridIndex + 0] - regdfl[0]) * strides[index];
                        float y1 = (meshgrid[gridIndex + 1] - regdfl[1]) * strides[index];
                        float x2 = (meshgrid[gridIndex + 0] + regdfl[2]) * strides[index];
                        float y2 = (meshgrid[gridIndex + 1] + regdfl[3]) * strides[index];

                        float xmin = x1 > 0 ? x1 : 0;
                        float ymin = y1 > 0 ? y1 : 0;
                        float xmax = x2 < input_w ? x2 : input_w;
                        float ymax = y2 < input_h ? y2 : input_h;

                        if (xmin >= 0 && ymin >= 0 && xmax <= input_w && ymax <= input_h)
                        {
                            temp.xmin = xmin / input_w;
                            temp.ymin = ymin / input_h;
                            temp.xmax = xmax / input_w;
                            temp.ymax = ymax / input_h;
                            temp.classId = cls_index;
                            temp.score = cls_max;
                            detectRects.push_back(temp);
                        }
                    }
                }
            }
        }

        // Sort by confidence score
        std::sort(detectRects.begin(), detectRects.end(),
                 [](DetectRect &Rect1, DetectRect &Rect2) -> bool
                 { return (Rect1.score > Rect2.score); });

        // NMS
        NN_LOG_DEBUG("NMS Before num: %ld", detectRects.size());
        for (int i = 0; i < detectRects.size(); ++i)
        {
            float xmin1 = detectRects[i].xmin;
            float ymin1 = detectRects[i].ymin;
            float xmax1 = detectRects[i].xmax;
            float ymax1 = detectRects[i].ymax;
            int classId = detectRects[i].classId;
            float score = detectRects[i].score;

            if (classId != -1)
            {
                // Store detection results
                DetectiontRects.push_back(float(classId));
                DetectiontRects.push_back(float(score));
                DetectiontRects.push_back(float(xmin1));
                DetectiontRects.push_back(float(ymin1));
                DetectiontRects.push_back(float(xmax1));
                DetectiontRects.push_back(float(ymax1));

                // Suppress overlapping boxes
                for (int j = i + 1; j < detectRects.size(); ++j)
                {
                    if (classId == detectRects[j].classId)
                    {
                        float xmin2 = detectRects[j].xmin;
                        float ymin2 = detectRects[j].ymin;
                        float xmax2 = detectRects[j].xmax;
                        float ymax2 = detectRects[j].ymax;
                        float iou = IOU(xmin1, ymin1, xmax1, ymax1, xmin2, ymin2, xmax2, ymax2);
                        if (iou > nmsThreshold)
                        {
                            detectRects[j].classId = -1;
                        }
                    }
                }
            }
        }

        return ret;
    }

    // Float version
    int GetConvDetectionResult(float **pBlob, std::vector<float> &DetectiontRects)
    {
        static auto meshgrid = GenerateMeshgrid();
        int ret = 0;

        int gridIndex = -2;
        float cls_max = 0;
        int cls_index = 0;

        DetectRect temp;
        std::vector<DetectRect> detectRects;

        for (int index = 0; index < headNum; index++)
        {
            float *cls = (float *)pBlob[index * 2 + 0];
            float *reg = (float *)pBlob[index * 2 + 1];

            for (int h = 0; h < mapSize[index][0]; h++)
            {
                for (int w = 0; w < mapSize[index][1]; w++)
                {
                    gridIndex += 2;

                    // Find max class confidence
                    cls_max = 0;
                    cls_index = 0;
                    for (int cl = 0; cl < class_num; cl++)
                    {
                        float cls_val = sigmoid(cls[cl * mapSize[index][0] * mapSize[index][1] + h * mapSize[index][1] + w]);
                        
                        if (cl == 0 || cls_val > cls_max)
                        {
                            cls_max = cls_val;
                            cls_index = cl;
                        }
                    }

                    if (cls_max > objectThreshold)
                    {
                        // Process DFL (Distribution Focal Loss) for regression values
                        float regdfl[4] = {0};
                        
                        for (int lc = 0; lc < 4; lc++)
                        {
                            float sfsum = 0;
                            float locval = 0;
                            
                            // Calculate softmax for each group of 16 values
                            for (int df = 0; df < 16; df++)
                            {
                                float temp = exp(reg[((lc * 16) + df) * mapSize[index][0] * mapSize[index][1] + h * mapSize[index][1] + w]);
                                sfsum += temp;
                            }
                            
                            // Calculate weighted sum
                            for (int df = 0; df < 16; df++)
                            {
                                float temp = exp(reg[((lc * 16) + df) * mapSize[index][0] * mapSize[index][1] + h * mapSize[index][1] + w]);
                                float sfval = temp / sfsum;
                                locval += sfval * df;
                            }
                            
                            regdfl[lc] = locval;
                        }

                        float x1 = (meshgrid[gridIndex + 0] - regdfl[0]) * strides[index];
                        float y1 = (meshgrid[gridIndex + 1] - regdfl[1]) * strides[index];
                        float x2 = (meshgrid[gridIndex + 0] + regdfl[2]) * strides[index];
                        float y2 = (meshgrid[gridIndex + 1] + regdfl[3]) * strides[index];

                        float xmin = x1 > 0 ? x1 : 0;
                        float ymin = y1 > 0 ? y1 : 0;
                        float xmax = x2 < input_w ? x2 : input_w;
                        float ymax = y2 < input_h ? y2 : input_h;

                        if (xmin >= 0 && ymin >= 0 && xmax <= input_w && ymax <= input_h)
                        {
                            temp.xmin = xmin / input_w;
                            temp.ymin = ymin / input_h;
                            temp.xmax = xmax / input_w;
                            temp.ymax = ymax / input_h;
                            temp.classId = cls_index;
                            temp.score = cls_max;
                            detectRects.push_back(temp);
                        }
                    }
                }
            }
        }

        // Sort by confidence score
        std::sort(detectRects.begin(), detectRects.end(),
                 [](DetectRect &Rect1, DetectRect &Rect2) -> bool
                 { return (Rect1.score > Rect2.score); });

        // NMS
        NN_LOG_DEBUG("NMS Before num: %ld", detectRects.size());
        for (int i = 0; i < detectRects.size(); ++i)
        {
            float xmin1 = detectRects[i].xmin;
            float ymin1 = detectRects[i].ymin;
            float xmax1 = detectRects[i].xmax;
            float ymax1 = detectRects[i].ymax;
            int classId = detectRects[i].classId;
            float score = detectRects[i].score;

            if (classId != -1)
            {
                // Store detection results
                DetectiontRects.push_back(float(classId));
                DetectiontRects.push_back(float(score));
                DetectiontRects.push_back(float(xmin1));
                DetectiontRects.push_back(float(ymin1));
                DetectiontRects.push_back(float(xmax1));
                DetectiontRects.push_back(float(ymax1));

                // Suppress overlapping boxes
                for (int j = i + 1; j < detectRects.size(); ++j)
                {
                    if (classId == detectRects[j].classId)
                    {
                        float xmin2 = detectRects[j].xmin;
                        float ymin2 = detectRects[j].ymin;
                        float xmax2 = detectRects[j].xmax;
                        float ymax2 = detectRects[j].ymax;
                        float iou = IOU(xmin1, ymin1, xmax1, ymax1, xmin2, ymin2, xmax2, ymax2);
                        if (iou > nmsThreshold)
                        {
                            detectRects[j].classId = -1;
                        }
                    }
                }
            }
        }

        return ret;
    }
} 