// C++ Standard Library
#include <thread>
#include <atomic>
#include <cstdint>

// Third Party Library
#include <opencv2/opencv.hpp>

// Project Headers
#include "BlockingQueue.h"
#include "RawFrame.h"
#include "FrameBufferPool.h"

class CameraCapture {
public:
    CameraCapture(
        int cameraIndex,
        BlockingQueue<RawFrame>& outputQueue,
        FrameBufferPool& bufferPool
    );
    ~CameraCapture();

    void setConfig(int width, int height, int fps);

    bool start();
    void stop();

private:
    void captureLoop();

private:
    int cameraIndex_;
    int width_;
    int height_;
    int fps_;

    BlockingQueue<RawFrame>& outputQueue_;
    FrameBufferPool& bufferPool_;

    std::thread worker_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> frameId_;
};