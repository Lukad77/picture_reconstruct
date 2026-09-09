#include <opencv2/opencv.hpp>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    int cameraIndex = 0;

    cv::VideoCapture cap(cameraIndex, cv::CAP_V4L2);

    if (!cap.isOpened()) {
        std::cerr << "无法打开摄像头: " << cameraIndex << std::endl;
        return 1;
    }

    std::cout << "摄像头打开成功" << std::endl;

    // 先不要设置 fps
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    for (int i = 0; i < 30; ++i) {
        cv::Mat frame;
        bool ok = cap.read(frame);

        std::cout << "read ok=" << ok
                  << ", empty=" << frame.empty()
                  << ", size=" << frame.cols << "x" << frame.rows
                  << ", type=" << frame.type()
                  << std::endl;

        if (!frame.empty()) {
            cv::imshow("camera", frame);
            cv::waitKey(30);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    cap.release();
    return 0;
}