#pragma once
#include <opencv2/opencv.hpp>
#include <vector>

// --- 原始业务数据结构 ---
struct StagePos {
    int frameId = 0;
    double x = 0.0;
    double y = 0.0;
};

struct SpotInfo {
    int id = 0;
    cv::Point2f cameraCenter;   
    cv::Point2f sampleOffset;   
    float radius = 6.0f;
    float gain = 1.0f;
};

struct SpotSignal {
    int frameId = 0;
    int spotId = 0;
    double sampleX = 0.0;
    double sampleY = 0.0;
    double intensity = 0.0;
};

// --- 多线程流水线数据包 ---
struct RawFramePacket {
    int frameId;
    double stageX;
    double stageY;
    std::vector<uchar> imgBuffer; // 改为存储原始字节
};

struct SignalPacket {
    std::vector<SpotSignal> signals;
};