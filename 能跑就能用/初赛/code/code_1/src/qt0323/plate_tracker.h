#pragma once

#include <opencv2/opencv.hpp>
#include <QString>
#include <deque>
#include <string>
#include <unordered_map>
#include <algorithm>

class PlateTracker {
public:
    int hits_to_confirm = 3;
    int misses_to_drop = 8;
    double iou_gate = 0.25;
    double ema_alpha_init = 0.45;
    double ema_alpha_locked = 0.18;
    int text_window = 7;
    int text_min_votes = 4;
    int color_min_votes = 3;

    struct Track {
        cv::Rect2f smoothed;
        int hits = 0;
        int misses = 0;
        bool confirmed = false;
        std::deque<std::string> text_hist;
        std::deque<std::string> color_hist;
        std::string stable_text = "...";
        std::string stable_color = "...";
    };

    void reset() { tr_ = Track{}; }

    cv::Rect update(const cv::Rect& detection, const std::string& text, const std::string& color) {
        const bool have_det = detection.area() > 0;
        bool same_target = false;

        if (have_det && tr_.hits > 0) {
            cv::Rect prev = toRect(tr_.smoothed);
            same_target = iou(detection, prev) >= iou_gate;
        }

        if (have_det && (tr_.hits == 0 || same_target)) {
            if (tr_.hits == 0) {
                tr_.smoothed = cv::Rect2f(detection);
            } else {
                const double a = tr_.confirmed ? ema_alpha_locked : ema_alpha_init;
                tr_.smoothed = emaBlend(tr_.smoothed, detection, a);
            }
            tr_.hits = std::min(tr_.hits + 1, 1 << 20);
            tr_.misses = 0;
            if (tr_.hits >= hits_to_confirm) tr_.confirmed = true;

            voteInto(tr_.text_hist, text, text_window);
            voteInto(tr_.color_hist, color, text_window);
            recommit(tr_.text_hist, tr_.stable_text, text_min_votes);
            recommit(tr_.color_hist, tr_.stable_color, color_min_votes);
        } else if (have_det && !same_target) {
            tr_.misses++;
            if (tr_.misses >= misses_to_drop) {
                reset();
                tr_.smoothed = cv::Rect2f(detection);
                tr_.hits = 1;
                voteInto(tr_.text_hist, text, text_window);
                voteInto(tr_.color_hist, color, text_window);
            }
        } else {
            tr_.misses++;
        }

        if (tr_.confirmed && tr_.misses < misses_to_drop) {
            return toRect(tr_.smoothed);
        }
        if (tr_.misses >= misses_to_drop) {
            reset();
        }
        return cv::Rect();
    }

    bool isVisible() const { return tr_.confirmed && tr_.misses < misses_to_drop; }
    cv::Rect visibleRect() const { return isVisible() ? toRect(tr_.smoothed) : cv::Rect(); }
    QString stableText() const { return QString::fromStdString(tr_.stable_text); }
    QString stableColor() const { return QString::fromStdString(tr_.stable_color); }
    int hits() const { return tr_.hits; }
    int misses() const { return tr_.misses; }

private:
    Track tr_;

    static double iou(const cv::Rect& a, const cv::Rect& b) {
        cv::Rect inter = a & b;
        const double i = inter.area();
        const double u = a.area() + b.area() - i;
        return u > 0.0 ? i / u : 0.0;
    }

    static cv::Rect toRect(const cv::Rect2f& r) {
        return cv::Rect(cvRound(r.x), cvRound(r.y), cvRound(r.width), cvRound(r.height));
    }

    static cv::Rect2f emaBlend(const cv::Rect2f& prev, const cv::Rect& cur, double a) {
        return cv::Rect2f(
            (1.0f - a) * prev.x + a * cur.x,
            (1.0f - a) * prev.y + a * cur.y,
            (1.0f - a) * prev.width + a * cur.width,
            (1.0f - a) * prev.height + a * cur.height
        );
    }

    static void voteInto(std::deque<std::string>& q, const std::string& s, int win) {
        if (s.empty() || s == "识别失败" || s == "推理错误" || s == "Unknown") return;
        q.push_back(s);
        while (static_cast<int>(q.size()) > win) q.pop_front();
    }

    static void recommit(const std::deque<std::string>& q, std::string& dst, int min_votes) {
        if (q.empty()) return;
        std::unordered_map<std::string, int> cnt;
        for (const auto& v : q) cnt[v]++;
        auto best = std::max_element(cnt.begin(), cnt.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
        if (best != cnt.end() && best->second >= min_votes) dst = best->first;
    }
};
