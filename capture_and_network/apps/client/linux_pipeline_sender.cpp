#include "V2Transfer.h"
#include "PipelineConfig.h"

#include <opencv2/opencv.hpp>

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
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
    try {
        pipelineconfig::SenderConfig config;
        if (argc == 1 || (argc == 3 && std::string(argv[1]) == "--config")) {
            const auto path = pipelineconfig::discoverConfigPath(
                argc, argv, "PICTURE_RECONSTRUCT_SENDER_CONFIG", "config/sender.json");
            config = pipelineconfig::loadSenderConfig(path);
        } else {
            if (argc < 6) {
                std::cerr << "usage: linux_pipeline_sender [--config path] | <host> <port> <spool-dir> <scan.csv> <camera-index|-1> [width height fps]\n";
                return 2;
            }
            config.host = argv[1]; config.port = static_cast<uint16_t>(std::stoi(argv[2]));
            config.spoolDirectory = argv[3]; config.scanCsv = argv[4]; config.cameraIndex = std::stoi(argv[5]);
            config.captureMode = config.cameraIndex >= 0 ? "v4l2" : "synthetic";
            config.width = argc > 6 ? std::stoi(argv[6]) : 640;
            config.height = argc > 7 ? std::stoi(argv[7]) : 480;
            config.fps = argc > 8 ? std::stoi(argv[8]) : 10;
        }
        const auto positions = readScan(config.scanCsv.string());

        cv::VideoCapture camera;
        if (config.captureMode == "v4l2") {
            camera.open(config.cameraIndex, cv::CAP_V4L2);
            if (!camera.isOpened()) throw std::runtime_error("cannot open V4L2 camera index " + std::to_string(config.cameraIndex));
            camera.set(cv::CAP_PROP_FRAME_WIDTH, config.width);
            camera.set(cv::CAP_PROP_FRAME_HEIGHT, config.height);
            camera.set(cv::CAP_PROP_FPS, config.fps);
            cv::Mat warmup;
            for (int i = 0; i < config.warmupFrames; ++i) camera.read(warmup);
        }

        v2transfer::Sender sender(config.host, config.port, config.spoolDirectory, config.taskId, config.transfer);
        if (!sender.resume()) throw std::runtime_error("initial resume failed: " + sender.lastError());
        const uint64_t firstSeq = sender.nextFrameSeq();
        std::mutex captureMutex;
        std::condition_variable captureCv;
        std::deque<v2transfer::Frame> captured;
        uint64_t capturedBytes = 0;
        bool captureDone = false;
        bool captureCancelled = false;
        std::exception_ptr captureError;

        std::thread captureThread([&] {
            try {
                for (size_t index = static_cast<size_t>(firstSeq - 1); index < positions.size(); ++index) {
                    cv::Mat image;
                    if (config.captureMode == "v4l2") {
                        if (!camera.read(image) || image.empty()) throw std::runtime_error("camera returned an empty frame");
                    } else {
                        image = syntheticFrame(config.width, config.height, positions[index]);
                    }
                    if (!image.isContinuous()) image = image.clone();
                    v2transfer::Frame frame;
                    frame.taskId = config.taskId;
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

                    std::unique_lock<std::mutex> lock(captureMutex);
                    captureCv.wait(lock, [&] {
                        return captureCancelled || capturedBytes == 0 ||
                               capturedBytes + frame.pixels.size() <= config.transfer.maxQueuedBytes;
                    });
                    if (captureCancelled) break;
                    capturedBytes += frame.pixels.size();
                    captured.push_back(std::move(frame));
                    lock.unlock();
                    captureCv.notify_all();
                    if (config.fps > 0 && config.captureMode == "synthetic")
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / config.fps));
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(captureMutex);
                captureError = std::current_exception();
            }
            { std::lock_guard<std::mutex> lock(captureMutex); captureDone = true; }
            captureCv.notify_all();
        });

        bool submitOk = true;
        while (true) {
            v2transfer::Frame frame;
            {
                std::unique_lock<std::mutex> lock(captureMutex);
                captureCv.wait(lock, [&] { return captureDone || !captured.empty(); });
                if (captured.empty()) break;
                frame = std::move(captured.front());
                captured.pop_front();
                capturedBytes -= frame.pixels.size();
            }
            captureCv.notify_all();
            if (!sender.submit(frame)) {
                submitOk = false;
                { std::lock_guard<std::mutex> lock(captureMutex); captureCancelled = true; }
                captureCv.notify_all();
                break;
            }
            std::cout << "spooled frame=" << frame.frameSeq << " stage=(" << frame.stageX << ',' << frame.stageY << ")\n";
        }
        captureThread.join();
        if (captureError) std::rethrow_exception(captureError);
        if (!submitOk) throw std::runtime_error("frame submit failed: " + sender.lastError());
        if (!sender.finish()) throw std::runtime_error("task finish failed: " + sender.lastError());
        const auto stats = sender.stats();
        std::cout << "task finished frames=" << stats.frames << " bytes=" << stats.bytes
                  << " connections=" << stats.connections << " reconnects=" << stats.reconnects << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
