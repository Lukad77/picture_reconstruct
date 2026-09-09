#include <opencv2/opencv.hpp>

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

#include "common/FrameBufferPool.h"

int main() {
    const int cameraIndex = 0;
    const int width = 1920;
    const int height = 1080;
    const int type = CV_8UC3;
    const size_t bufferSize = static_cast<size_t>(width * height * 3);
    const size_t bufferCount = 4;

    FrameBufferPool pool(bufferSize, bufferCount);

    cv::VideoCapture cap(cameraIndex, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::cerr << "Failed to open camera" << std::endl;
        return 1;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));

    std::cout << "Requested width: " << width << std::endl;
    std::cout << "Requested height: " << height << std::endl;

    std::cout << "Actual width: "
              << cap.get(cv::CAP_PROP_FRAME_WIDTH) << std::endl;
    std::cout << "Actual height: "
              << cap.get(cv::CAP_PROP_FRAME_HEIGHT) << std::endl;

    int successCount = 0;
    int failCount = 0;

    for (int i = 0; i < 100; ++i) {
        auto buffer = pool.acquire();

        cv::Mat frameView(
            height,
            width,
            type,
            buffer.get()
        );

        uint8_t* before = frameView.data;

        bool ok = cap.read(frameView);
        if (!ok) {
            std::cerr << "[Frame " << i << "] cap.read failed" << std::endl;
            ++failCount;
            continue;
        }

        uint8_t* after = frameView.data;

        bool zeroCopyOk = before == after && after == buffer.get();

        std::cout << "[Frame " << i << "] "
                  << "before=" << static_cast<void*>(before)
                  << ", after=" << static_cast<void*>(after)
                  << ", pool=" << static_cast<void*>(buffer.get())
                  << ", rows=" << frameView.rows
                  << ", cols=" << frameView.cols
                  << ", channels=" << frameView.channels()
                  << ", continuous=" << frameView.isContinuous()
                  << ", result=" << (zeroCopyOk ? "OK" : "REALLOCATED")
                  << std::endl;

        if (zeroCopyOk) {
            ++successCount;
        } else {
            ++failCount;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    std::cout << "====================================" << std::endl;
    std::cout << "Zero-copy candidate result:" << std::endl;
    std::cout << "Success: " << successCount << std::endl;
    std::cout << "Failed: " << failCount << std::endl;

    if (successCount > 0 && failCount == 0) {
        std::cout << "Conclusion: OpenCV appears to write into external buffer consistently."
                  << std::endl;
        return 0;
    }

    std::cout << "Conclusion: OpenCV does not reliably use external buffer. "
              << "Do not integrate this as zero-copy."
              << std::endl;

    return 2;
}