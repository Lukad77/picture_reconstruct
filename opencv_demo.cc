#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>

struct RegionInfo {
    int label = 0;
    cv::Rect bbox;
    cv::Point2d centroid;
    std::vector<cv::Point> pixels;
};

struct MotionPoint {
    int x = 0;
    int y = 0;
    bool laserOn = false; // true: 曝光, false: 空行程/关快门
};

// 提取连通区域
std::vector<RegionInfo> extractRegions(const cv::Mat& binary) {
    cv::Mat labels, stats, centroids;
    int numLabels = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);

    std::vector<RegionInfo> regions;
    for (int label = 1; label < numLabels; ++label) { // 0 是背景
        int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area < 10) continue; // 去掉小噪声

        RegionInfo region;
        region.label = label;
        region.bbox = cv::Rect(
            stats.at<int>(label, cv::CC_STAT_LEFT),
            stats.at<int>(label, cv::CC_STAT_TOP),
            stats.at<int>(label, cv::CC_STAT_WIDTH),
            stats.at<int>(label, cv::CC_STAT_HEIGHT)
        );
        region.centroid = cv::Point2d(
            centroids.at<double>(label, 0),
            centroids.at<double>(label, 1)
        );

        for (int y = 0; y < labels.rows; ++y) {
            const int* rowPtr = labels.ptr<int>(y);
            for (int x = 0; x < labels.cols; ++x) {
                if (rowPtr[x] == label) {
                    region.pixels.emplace_back(x, y);
                }
            }
        }
        regions.push_back(region);
    }
    return regions;
}

// 简化版路径规划：按最近邻顺序访问各区域
std::vector<RegionInfo> planRegionOrder(std::vector<RegionInfo> regions) {
    if (regions.empty()) return {};

    std::vector<RegionInfo> ordered;
    std::vector<bool> used(regions.size(), false);

    int current = 0;
    ordered.push_back(regions[current]);
    used[current] = true;

    for (size_t step = 1; step < regions.size(); ++step) {
        double bestDist = 1e18;
        int bestIdx = -1;

        for (size_t i = 0; i < regions.size(); ++i) {
            if (used[i]) continue;
            double dx = regions[i].centroid.x - regions[current].centroid.x;
            double dy = regions[i].centroid.y - regions[current].centroid.y;
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = static_cast<int>(i);
            }
        }

        current = bestIdx;
        used[current] = true;
        ordered.push_back(regions[current]);
    }

    return ordered;
}

// 对单个区域生成蛇形扫描路径
std::vector<MotionPoint> generateSnakePathForRegion(const cv::Mat& binary, const RegionInfo& region) {
    std::vector<MotionPoint> path;

    for (int y = region.bbox.y; y < region.bbox.y + region.bbox.height; ++y) {
        if ((y - region.bbox.y) % 2 == 0) {
            // 左 -> 右
            for (int x = region.bbox.x; x < region.bbox.x + region.bbox.width; ++x) {
                bool fg = binary.at<uchar>(y, x) > 0;
                path.push_back({x, y, fg});
            }
        } else {
            // 右 -> 左
            for (int x = region.bbox.x + region.bbox.width - 1; x >= region.bbox.x; --x) {
                bool fg = binary.at<uchar>(y, x) > 0;
                path.push_back({x, y, fg});
            }
        }
    }

    return path;
}

int main() {
    // 1) 读取图像
    cv::Mat src = cv::imread("input.png");
    if (src.empty()) {
        std::cerr << "Failed to load input.png\n";
        return -1;
    }

    // 2) 缩放
    double scale = 0.5; // 对应加工分辨率调整
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(), scale, scale, cv::INTER_AREA);

    // 3) 灰度化
    cv::Mat gray;
    cv::cvtColor(resized, gray, cv::COLOR_BGR2GRAY);

    // 4) 二值化：黑色前景 -> 白色前景，方便 connectedComponents
    cv::Mat binary;
    cv::threshold(gray, binary, 127, 255, cv::THRESH_BINARY_INV);

    // 5) 区域解析
    std::vector<RegionInfo> regions = extractRegions(binary);
    std::cout << "Detected regions: " << regions.size() << "\n";

    // 6) 简化版全局顺序规划
    std::vector<RegionInfo> orderedRegions = planRegionOrder(regions);

    // 7) 生成最终运动路径
    std::vector<MotionPoint> fullPath;
    for (size_t i = 0; i < orderedRegions.size(); ++i) {
        auto regionPath = generateSnakePathForRegion(binary, orderedRegions[i]);

        // 区域之间插入空行程点（关快门）
        if (!fullPath.empty() && !regionPath.empty()) {
            MotionPoint jumpPoint = regionPath.front();
            jumpPoint.laserOn = false;
            fullPath.push_back(jumpPoint);
        }

        fullPath.insert(fullPath.end(), regionPath.begin(), regionPath.end());
    }

    // 8) 输出部分路径，模拟“发给运动控制模块”
    for (size_t i = 0; i < std::min<size_t>(fullPath.size(), 80); ++i) {
        std::cout << "x=" << fullPath[i].x
                  << ", y=" << fullPath[i].y
                  << ", laser=" << (fullPath[i].laserOn ? "ON" : "OFF")
                  << "\n";
    }

    // 9) 可视化
    cv::Mat vis;
    cv::cvtColor(binary, vis, cv::COLOR_GRAY2BGR);
    for (const auto& r : orderedRegions) {
        cv::rectangle(vis, r.bbox, cv::Scalar(0, 255, 0), 1);
        cv::circle(vis, r.centroid, 2, cv::Scalar(0, 0, 255), -1);
    }

    cv::imshow("binary", binary);
    cv::imshow("regions", vis);
    cv::waitKey(0);
    return 0;
}