#include "V2Transfer.h"

#include <opencv2/opencv.hpp>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
struct ScanPosition { uint32_t frameIndex; double x; double y; };

std::vector<ScanPosition> readScan(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open scan CSV: " + path);
    std::vector<ScanPosition> result;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::stringstream stream(line);
        std::string id, x, y;
        if (!std::getline(stream, id, ',') || !std::getline(stream, x, ',') || !std::getline(stream, y, ',')) continue;
        result.push_back({static_cast<uint32_t>(std::stoul(id)), std::stod(x), std::stod(y)});
    }
    if (result.empty()) throw std::runtime_error("scan CSV has no positions");
    return result;
}

cv::Mat syntheticFrame(int width, int height, const ScanPosition& position) {
    cv::Mat image(height, width, CV_8UC1, cv::Scalar(20));
    const int level = 60 + (static_cast<int>(position.x + position.y) % 160 + 160) % 160;
    cv::circle(image, {width / 2, height / 2}, 18, cv::Scalar(level), -1);
    cv::line(image, {0, static_cast<int>(position.frameIndex % height)},
             {width - 1, static_cast<int>(position.frameIndex % height)}, cv::Scalar(100), 2);
    return image;
}
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: linux_pipeline_sender <host> <port> <spool-dir> <scan.csv> <camera-index|-1> [width height fps]\n";
        return 2;
    }
    try {
        const std::string host = argv[1];
        const uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
        const std::string spool = argv[3];
        const auto positions = readScan(argv[4]);
        const int cameraIndex = std::stoi(argv[5]);
        const int width = argc > 6 ? std::stoi(argv[6]) : 640;
        const int height = argc > 7 ? std::stoi(argv[7]) : 480;
        const int fps = argc > 8 ? std::stoi(argv[8]) : 10;

        cv::VideoCapture camera;
        if (cameraIndex >= 0) {
            camera.open(cameraIndex, cv::CAP_V4L2);
            if (!camera.isOpened()) throw std::runtime_error("cannot open V4L2 camera index " + std::to_string(cameraIndex));
            camera.set(cv::CAP_PROP_FRAME_WIDTH, width);
            camera.set(cv::CAP_PROP_FRAME_HEIGHT, height);
            camera.set(cv::CAP_PROP_FPS, fps);
            cv::Mat warmup;
            for (int i = 0; i < 5; ++i) camera.read(warmup);
        }

        v2transfer::Sender sender(host, port, spool, 1);
        if (!sender.resume()) throw std::runtime_error("initial resume failed: " + sender.lastError());
        const uint64_t firstSeq = sender.nextFrameSeq();
        for (size_t index = static_cast<size_t>(firstSeq - 1); index < positions.size(); ++index) {
            cv::Mat image;
            if (cameraIndex >= 0) {
                if (!camera.read(image) || image.empty()) throw std::runtime_error("camera returned an empty frame");
            } else {
                image = syntheticFrame(width, height, positions[index]);
            }
            if (!image.isContinuous()) image = image.clone();
            v2transfer::Frame frame;
            frame.taskId = 1;
            frame.lineId = 0;
            frame.attemptId = 1;
            frame.frameIndex = positions[index].frameIndex;
            frame.frameSeq = index + 1;
            frame.stageX = positions[index].x;
            frame.stageY = positions[index].y;
            frame.rows = static_cast<uint32_t>(image.rows);
            frame.cols = static_cast<uint32_t>(image.cols);
            frame.pixelType = static_cast<uint32_t>(image.type());
            frame.elemSize = static_cast<uint32_t>(image.elemSize());
            const size_t bytes = image.total() * image.elemSize();
            frame.pixels.assign(image.data, image.data + bytes);
            if (!sender.send(frame)) throw std::runtime_error("frame send failed: " + sender.lastError());
            std::cout << "ACK frame=" << frame.frameSeq << " stage=(" << frame.stageX << ',' << frame.stageY << ")\n";
            if (fps > 0 && cameraIndex < 0) std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
        }
        if (!sender.finish()) throw std::runtime_error("task finish failed: " + sender.lastError());
        std::cout << "task finished\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
