#include "deepsort_adapter.h"

DeepSortAdapter::DeepSortAdapter(const std::string& model_path, int batch_size, int feature_dim, int max_budget, rknn_core_mask npu_id) {
    // 创建DeepSORT实例，假设CPU ID为1
    deepsort = new DeepSort(model_path, batch_size, feature_dim, 1, npu_id);
}

DeepSortAdapter::~DeepSortAdapter() {
    if (deepsort) {
        delete deepsort;
        deepsort = nullptr;
    }
}

std::vector<TrackingBox> DeepSortAdapter::update(const std::vector<cv::Rect>& boxes, const std::vector<float>& confidences, const cv::Mat& frame) {
    // 将cv::Rect和confidences转换为DetectBox
    std::vector<DetectBox> detect_boxes;
    
    for (size_t i = 0; i < boxes.size(); i++) {
        if (i < confidences.size()) {
            const auto& box = boxes[i];
            DetectBox det_box(
                box.x,                // x1
                box.y,                // y1
                box.x + box.width,    // x2
                box.y + box.height,   // y2
                confidences[i]        // confidence
            );
            det_box.classID = 0;     // 行人类别ID为0
            detect_boxes.push_back(det_box);
        }
    }
    
    // 调用DeepSORT进行跟踪
    if (!detect_boxes.empty()) {
        cv::Mat mutable_frame = frame.clone(); // Create a mutable copy of the frame
        deepsort->sort(mutable_frame, detect_boxes);
    }
    
    // 转换DeepSORT的结果为TrackingBox格式
    std::vector<TrackingBox> tracking_boxes;
    for (const auto& det : detect_boxes) {
        // 只返回有跟踪ID的检测框
        if (det.trackID > 0) {
            TrackingBox track;
            track.id = static_cast<int>(det.trackID);
            track.box = cv::Rect(
                static_cast<int>(det.x1),
                static_cast<int>(det.y1),
                static_cast<int>(det.x2 - det.x1),
                static_cast<int>(det.y2 - det.y1)
            );
            track.confidence = det.confidence;
            tracking_boxes.push_back(track);
        }
    }
    
    return tracking_boxes;
} 