// C++ Standard Library
#include <thread>
#include <atomic>
#include <string>

// Project Headers
#include "BlockingQueue.h"
#include "RawFrame.h"
#include "FrameBufferPool.h"
#include "CameraCapture.h"
#include "TcpClient.h"
#include "RawFrameSender.h"

class CameraNetworkSender {
public:
    CameraNetworkSender(
        int cameraIndex,
        const std::string& serverIp,
        int serverPort
    );

    ~CameraNetworkSender();

    void setCameraConfig(int width, int height, int fps);
    void setFragmentSize(uint32_t fragmentSize);

    bool start();
    void stop();

private:
    void sendLoop();

private:
    BlockingQueue<RawFrame> queue_;
    FrameBufferPool bufferPool_;

    CameraCapture camera_;
    TcpClient tcpClient_;
    RawFrameSender frameSender_;

    std::thread senderThread_;
    std::atomic<bool> running_;
};