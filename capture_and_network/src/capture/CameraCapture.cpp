#include "CameraCapture.h"

#include <iostream>
#include <chrono>
#include <cstring>

CameraCapture::CameraCapture(
    int cameraIndex,
    BlockingQueue<RawFrame>& outputQueue,
    FrameBufferPool& bufferPool
)
    : cameraIndex_(cameraIndex),
      width_(640),
      height_(480),
      fps_(10),
      outputQueue_(outputQueue),
      bufferPool_(bufferPool),
      running_(false),
      frameId_(0) {}

CameraCapture::~CameraCapture() {
    stop();
}

void CameraCapture::setConfig(int width, int height, int fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;
}

bool CameraCapture::start() {
    if (running_) {
        return true;
    }

    running_ = true;
    worker_ = std::thread(&CameraCapture::captureLoop, this);

    return true;
}

void CameraCapture::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (worker_.joinable()) {
        worker_.join();
    }
}

void CameraCapture::captureLoop() {
    // 关键修改：Ubuntu 下显式使用 V4L2
    cv::VideoCapture cap(cameraIndex_, cv::CAP_V4L2);

    if (!cap.isOpened()) {
        std::cerr << "[CameraCapture] 无法打开摄像头: "
                  << cameraIndex_ << std::endl;
        running_ = false;
        return;
    }

    // 先不要设置 FPS，很多摄像头对 FPS 设置不稳定
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width_);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height_);

    std::cout << "[CameraCapture] 摄像头采集线程启动" << std::endl;
    std::cout << "[CameraCapture] backend = "
              << cap.getBackendName() << std::endl;

    std::cout << "[CameraCapture] actual width = "
              << cap.get(cv::CAP_PROP_FRAME_WIDTH)
              << ", height = "
              << cap.get(cv::CAP_PROP_FRAME_HEIGHT)
              << ", fps = "
              << cap.get(cv::CAP_PROP_FPS)
              << std::endl;

    cv::Mat frame;
    int emptyCount = 0;

    // 预热摄像头，丢弃前几帧
    for (int i = 0; i < 10 && running_; ++i) {
        cap.read(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    while (running_) {
        bool ok = cap.read(frame);

        if (!ok || frame.empty()) {
            ++emptyCount;

            // std::cerr << "[CameraCapture] 采集到空帧, ok="
            //           << ok
            //           << ", emptyCount="
            //           << emptyCount
            //           << std::endl;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        emptyCount = 0;

        if (!frame.isContinuous()) {
            frame = frame.clone();
        }

        uint64_t bytes = static_cast<uint64_t>(frame.total() * frame.elemSize());

        if (bytes > bufferPool_.bufferSize()) {
            std::cerr << "[CameraCapture] 当前帧大小超过内存池 buffer 大小, bytes="
                      << bytes
                      << ", poolBufferSize="
                      << bufferPool_.bufferSize()
                      << std::endl;
            continue;
        }

        auto buffer = bufferPool_.acquire();

        std::memcpy(buffer.get(), frame.data, static_cast<size_t>(bytes));

        uint64_t id = ++frameId_;

        RawFrame rawFrame(
            id,
            static_cast<uint32_t>(frame.rows),
            static_cast<uint32_t>(frame.cols),
            static_cast<uint32_t>(frame.type()),
            static_cast<uint32_t>(frame.elemSize()),
            bytes,
            buffer
        );

        outputQueue_.push(rawFrame);

        std::cout << "[CameraCapture] 采集成功 frameId="
                  << id
                  << ", size="
                  << frame.cols
                  << "x"
                  << frame.rows
                  << ", type="
                  << frame.type()
                  << std::endl;

        if (fps_ > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / fps_)
            );
        }
    }

    cap.release();
    running_ = false;

    std::cout << "[CameraCapture] 摄像头采集线程退出" << std::endl;
}