#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

struct PlateCandidate {
    cv::RotatedRect rect;
    double score = 0.0;
};

static std::vector<cv::Point2f> orderCornersClockwise(const std::vector<cv::Point2f>& pts) {
    std::vector<cv::Point2f> ordered(4);
    std::vector<float> sums, diffs;
    sums.reserve(4);
    diffs.reserve(4);

    for (const auto& p : pts) {
        sums.push_back(p.x + p.y);
        diffs.push_back(p.y - p.x);
    }

    auto minSumIdx = static_cast<int>(std::min_element(sums.begin(), sums.end()) - sums.begin());
    auto maxSumIdx = static_cast<int>(std::max_element(sums.begin(), sums.end()) - sums.begin());
    auto minDiffIdx = static_cast<int>(std::min_element(diffs.begin(), diffs.end()) - diffs.begin());
    auto maxDiffIdx = static_cast<int>(std::max_element(diffs.begin(), diffs.end()) - diffs.begin());

    ordered[0] = pts[minSumIdx];   // top-left
    ordered[1] = pts[minDiffIdx];  // top-right
    ordered[2] = pts[maxSumIdx];   // bottom-right
    ordered[3] = pts[maxDiffIdx];  // bottom-left
    return ordered;
}

static PlateCandidate findBestPlate(const cv::Mat& srcBgr) {
    cv::Mat hsv;
    cv::cvtColor(srcBgr, hsv, cv::COLOR_BGR2HSV);

    // Typical blue plate HSV range; tune for your camera/lighting.
    cv::Mat blueMask;
    cv::inRange(hsv, cv::Scalar(80, 20, 20), cv::Scalar(150, 255, 255), blueMask);

    // Improve connectivity and reduce fragmented character regions.
    cv::Mat kernelClose = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 3));
    cv::Mat kernelOpen = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(blueMask, blueMask, cv::MORPH_CLOSE, kernelClose);
    cv::morphologyEx(blueMask, blueMask, cv::MORPH_OPEN, kernelOpen);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(blueMask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    PlateCandidate best;
    best.score = -1.0;

    const double imgArea = static_cast<double>(srcBgr.cols * srcBgr.rows);

    for (const auto& contour : contours) {
        if (contour.size() < 5) {
            continue;
        }

        cv::RotatedRect rr = cv::minAreaRect(contour);
        double w = rr.size.width;
        double h = rr.size.height;
        if (w <= 1.0 || h <= 1.0) {
            continue;
        }

        double longSide = std::max(w, h);
        double shortSide = std::min(w, h);
        double ratio = longSide / shortSide;
        double area = w * h;
        double areaRatio = area / imgArea;

        // Typical single-line plate aspect ratio is around 2.2~5.5.
        if (ratio < 2.2 || ratio > 5.5) {
            continue;
        }
        if (areaRatio < 0.005 || areaRatio > 0.95) {
            continue;
        }

        // Score by area suitability and ratio closeness to ~3.2.
        double ratioScore = 1.0 - std::min(std::abs(ratio - 3.2) / 3.2, 1.0);
        double areaScore = std::min(areaRatio / 0.08, 1.0);
        double score = 0.65 * ratioScore + 0.35 * areaScore;

        if (score > best.score) {
            best.rect = rr;
            best.score = score;
        }
    }

    return best;
}

static PlateCandidate findBestPlateByEdge(const cv::Mat& srcBgr) {
    cv::Mat gray;
    cv::cvtColor(srcBgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0.0);

    cv::Mat edge;
    cv::Canny(gray, edge, 60, 180);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 3));
    cv::morphologyEx(edge, edge, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edge, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    PlateCandidate best;
    best.score = -1.0;
    const double imgArea = static_cast<double>(srcBgr.cols * srcBgr.rows);

    for (const auto& contour : contours) {
        if (contour.size() < 5) {
            continue;
        }

        cv::RotatedRect rr = cv::minAreaRect(contour);
        double w = rr.size.width;
        double h = rr.size.height;
        if (w <= 1.0 || h <= 1.0) {
            continue;
        }

        double longSide = std::max(w, h);
        double shortSide = std::min(w, h);
        double ratio = longSide / shortSide;
        double areaRatio = (w * h) / imgArea;
        if (ratio < 2.0 || ratio > 6.5) {
            continue;
        }
        if (areaRatio < 0.003 || areaRatio > 0.98) {
            continue;
        }

        cv::Rect br = rr.boundingRect();
        br &= cv::Rect(0, 0, srcBgr.cols, srcBgr.rows);
        if (br.empty()) {
            continue;
        }

        double contourArea = cv::contourArea(contour);
        double rectArea = static_cast<double>(br.area());
        double fill = (rectArea > 1.0) ? (contourArea / rectArea) : 0.0;
        double ratioScore = 1.0 - std::min(std::abs(ratio - 3.2) / 3.2, 1.0);
        double areaScore = std::min(areaRatio / 0.10, 1.0);
        double fillScore = std::min(fill, 1.0);
        double score = 0.45 * ratioScore + 0.35 * areaScore + 0.20 * fillScore;

        if (score > best.score) {
            best.rect = rr;
            best.score = score;
        }
    }

    return best;
}

static cv::Mat rectifyPlate(const cv::Mat& src, const cv::RotatedRect& rr, std::vector<cv::Point2f>* orderedCornersOut) {
    cv::Point2f corners[4];
    rr.points(corners);
    std::vector<cv::Point2f> pts(corners, corners + 4);
    auto ordered = orderCornersClockwise(pts);

    float widthA = cv::norm(ordered[2] - ordered[3]);
    float widthB = cv::norm(ordered[1] - ordered[0]);
    float maxWidth = std::max(widthA, widthB);

    float heightA = cv::norm(ordered[1] - ordered[2]);
    float heightB = cv::norm(ordered[0] - ordered[3]);
    float maxHeight = std::max(heightA, heightB);

    int outW = std::max(static_cast<int>(std::round(maxWidth)), 1);
    int outH = std::max(static_cast<int>(std::round(maxHeight)), 1);

    // Normalize output to landscape orientation.
    if (outH > outW) {
        std::swap(outW, outH);
    }

    std::vector<cv::Point2f> dst = {
        cv::Point2f(0.0f, 0.0f),
        cv::Point2f(static_cast<float>(outW - 1), 0.0f),
        cv::Point2f(static_cast<float>(outW - 1), static_cast<float>(outH - 1)),
        cv::Point2f(0.0f, static_cast<float>(outH - 1))
    };

    cv::Mat M = cv::getPerspectiveTransform(ordered, dst);
    cv::Mat warped;
    cv::warpPerspective(src, warped, M, cv::Size(outW, outH), cv::INTER_CUBIC, cv::BORDER_REPLICATE);

    if (orderedCornersOut != nullptr) {
        *orderedCornersOut = ordered;
    }
    return warped;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage:\n";
        std::cout << "  plate_detect_correct <input_image> [output_dir]\n";
        return 0;
    }

    std::string imagePath = argv[1];
    std::string outputDir = (argc >= 3) ? argv[2] : ".";
    fs::create_directories(outputDir);

    cv::Mat src = cv::imread(imagePath);
    if (src.empty()) {
        std::cerr << "Failed to read image: " << imagePath << "\n";
        return 1;
    }

    PlateCandidate best = findBestPlate(src);
    if (best.score < 0.0) {
        best = findBestPlateByEdge(src);
    }
    if (best.score < 0.0) {
        std::cerr << "No plausible plate region detected.\n";
        return 2;
    }

    std::vector<cv::Point2f> corners;
    cv::Mat corrected = rectifyPlate(src, best.rect, &corners);
    cv::Rect roiRect = best.rect.boundingRect();
    roiRect &= cv::Rect(0, 0, src.cols, src.rows);
    cv::Mat rawRoi = src(roiRect).clone();

    // Draw ROI quadrilateral on the source image.
    cv::Mat vis = src.clone();
    for (int i = 0; i < 4; ++i) {
        cv::line(vis, corners[i], corners[(i + 1) % 4], cv::Scalar(0, 255, 0), 2);
    }

    fs::path inPath(imagePath);
    std::string stem = inPath.stem().string();
    fs::path roiPath = fs::path(outputDir) / (stem + "_plate_roi.jpg");
    fs::path correctedPath = fs::path(outputDir) / (stem + "_plate_corrected.jpg");
    fs::path maskPath = fs::path(outputDir) / (stem + "_plate_boxed.jpg");

    cv::imwrite(roiPath.string(), rawRoi);
    cv::imwrite(correctedPath.string(), corrected);
    cv::imwrite(maskPath.string(), vis);

    std::cout << "Plate detected and corrected.\n";
    std::cout << "Raw ROI: " << roiPath.string() << "\n";
    std::cout << "Corrected: " << correctedPath.string() << "\n";
    std::cout << "Boxed image: " << maskPath.string() << "\n";
    return 0;
}
