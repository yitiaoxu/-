#include "cv_draw.h"

#include "utils/logging.h"

#include <sstream>

static double roundToDecimal(double num, int decimalPlaces)
{
    double factor = pow(10, decimalPlaces);
    return round(num * factor) / factor;
}

// 在img上画出检测结果
void DrawDetections(cv::Mat &img, const std::vector<Detection> &objects)
{
    NN_LOG_DEBUG("draw %ld objects", objects.size());
    for (const auto &object : objects)
    {
        // 只绘制红色(0,0,255)和绿色(0,255,0)框，过滤掉蓝色(255,0,0)框
        if (object.color == cv::Scalar(255, 0, 0) || object.color == cv::Scalar(0, 0, 0)) {
            continue; // 跳过蓝色框和黑色框
        }
        
        // 只绘制红色和绿色框
        cv::rectangle(img, object.box, object.color, 2);
        // class name with confidence
        // auto conf = roundToDecimal(object.confidence, 3);
        // std::ostringstream oss;
        // oss.precision(3);
        // oss << std::fixed << conf;
        // std::string draw_string = object.className + " " + oss.str();

        // // 删除黑色背景框，直接绘制文本
        // cv::putText(img, draw_string, cv::Point(object.box.x, object.box.y - 7), 
        //             cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(255, 255, 255), 2);
    }
}