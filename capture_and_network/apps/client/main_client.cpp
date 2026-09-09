// C++ Standard Library
#include <iostream>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>

// Project Headers
#include "CameraNetworkSender.h"

/*编译整个项目：
   cmake -S . -B build
    cmake --build build -j
*/

//运行客户端：./build/raw_camera_client
//运行服务器：./build/raw_test_server

std::atomic<bool> gRunning(true);

void signalHandler(int signal) {
    if (signal == SIGINT) {
        gRunning = false;
    }
}

int main() {
    signal(SIGINT, signalHandler);

    int cameraIndex = 0;
    std::string serverIp = "127.0.0.1";
    int serverPort = 9000;

    CameraNetworkSender sender(cameraIndex, serverIp, serverPort);

    sender.setCameraConfig(640, 480, 30);

    // 原始 BGR 图像较大，建议 32KB ~ 256KB 之间
    sender.setFragmentSize(32 * 1024);

    if (!sender.start()) {
        std::cerr << "客户端启动失败" << std::endl;
        return 1;
    }

    std::cout << "客户端运行中，按 Ctrl+C 退出" << std::endl;
    std::cout << "内存池已启用，预分配固定大小缓冲区，减少动态内存分配开销" << std::endl;

    while (gRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    sender.stop();

    return 0;
}