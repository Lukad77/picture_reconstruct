#include "../include/core_types.h"
#include "../include/concurrent_queue.h"
#include "../include/reconstruction_core.h"
#include "../include/io_utils.h"
#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include "../include/cuda_kernels.cuh"
#include <cuda_runtime.h>
// 辅助函数：确保从 Socket 读够指定长度的字节
bool recvExact(int sock, char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) return false; // 网络断开或出错
        total += n;
    }
    return true;
}

// 严丝合缝的网络包头定义 (1字节对齐，防止编译器自动填充)
#pragma pack(push, 1)
struct NetHeader {
    char magic[4];   // "CAM1"
    int frameId;
    int imageSize;
    double stageX;
    double stageY;
};
#pragma pack(pop)
int main(int argc, char** argv) {
    try {
        std::string frameDir = "frames";
        std::string scanCsv = "scan_positions.csv";
        std::string spotCsv = "spots.csv";
        std::string outputDir = "reconstruct_output";

        if (argc >= 2) frameDir = argv[1];
        if (argc >= 3) scanCsv = argv[2];
        if (argc >= 4) spotCsv = argv[3];

        std::filesystem::create_directories(outputDir);

        ResourceMonitor monitor(outputDir + "/resource_usage.csv", outputDir + "/resource_summary.txt");
        cv::setNumThreads(1); // 极其关键：防止 OpenCV 算子与线程池抢核心

        int outputWidth = 512, outputHeight = 512, deconvIterations = 15;
        const int psfSize = 15; const double psfSigma = 1.6;

        monitor.setExperimentConfig(outputWidth, outputHeight, deconvIterations, psfSize, psfSigma, cv::getNumThreads());
        monitor.mark("Program started");

        std::vector<StagePos> positions = readScanPositions(scanCsv);
        std::vector<SpotInfo> spots = readSpots(spotCsv);
        monitor.setDatasetInfo(positions.size(), spots.size());

        cv::Mat accum(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));
        cv::Mat weight(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));
        std::vector<SpotSignal> allSignals;
        std::mutex allSignalsMutex;

        std::cout << "Loaded frames: " << positions.size() << ", Spots: " << spots.size() << "\n";
        
        // --- 1. 初始化缓冲队列 ---
        BoundedQueue<RawFramePacket> rawQueue(50);
        BoundedQueue<SignalPacket> signalQueue(200);

        std::atomic<double> totalIoTime{0.0}, totalPreprocessTime{0.0}, totalRoiTime{0.0}, totalAccumTime{0.0};
        std::atomic<int> processedFramesCount{0};

        // --- 2. 启动单线程 Producer (未来改成接收网络数据) ---
        // std::thread producerThread([&]() {
        //     using Clock = std::chrono::steady_clock;
        //     for (const auto& pos : positions) {
        //         auto start = Clock::now();
        //         cv::Mat raw = readFrameAsFloat(makeFramePath(frameDir, pos.frameId));
        //         totalIoTime = totalIoTime + std::chrono::duration<double>(Clock::now() - start).count();
        //         rawQueue.push({pos.frameId, pos.x, pos.y, raw});
        //     }
        //     rawQueue.setFinished();
        // });
        // --- 2. 启动单线程 Producer (现在的角色是 TCP Server) ---
        std::thread producerThread([&]() {
            int server_fd = socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            
            struct sockaddr_in address;
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(8080);
            
            bind(server_fd, (struct sockaddr *)&address, sizeof(address));
            listen(server_fd, 3);
            
            std::cout << "[Network] Waiting for Python sender on port 8080...\n";
            int client_socket = accept(server_fd, nullptr, nullptr);
            std::cout << "[Network] Sender connected! Receiving stream...\n";

            while (true) {
                NetHeader header;
                // 读包头
                if (!recvExact(client_socket, (char*)&header, sizeof(NetHeader))) break;
                if (strncmp(header.magic, "CAM1", 4) != 0) {
                    std::cerr << "[Network] Bad magic word, connection desynced!\n";
                    break;
                }

                // 读图像二进制数据
                std::vector<uchar> imgBuffer(header.imageSize);
                if (!recvExact(client_socket, (char*)imgBuffer.data(), header.imageSize)) break;

                // 推入流水线队列
                rawQueue.push({header.frameId, header.stageX, header.stageY, std::move(imgBuffer)});
            }

            std::cout << "[Network] Stream ended or sender disconnected.\n";
            close(client_socket);
            close(server_fd);
            rawQueue.setFinished(); // 通知 Worker 线程可以收尾了
        });
        // --- 3. 启动多线程 Worker Pool (未来部分算子迁入 GPU) ---
        int numWorkers = std::max(1, (int)std::thread::hardware_concurrency() - 2);
        std::vector<std::thread> workerThreads;
  for (int i = 0; i < numWorkers; ++i) {
            workerThreads.emplace_back([&]() {
                using Clock = std::chrono::steady_clock;
                double localPreTime = 0.0, localRoiTime = 0.0;
                
                // ==========================================
                // 【绝招】线程级显存池 (Thread-Local GPU Buffer)
                // ==========================================
                int width = outputWidth; // 假设所有图大小一致，比如 512
                int height = outputHeight;
                size_t bytes = width * height * sizeof(float);
                
                float *d_input, *d_temp, *d_output;
                cudaMalloc(&d_input, bytes);
                cudaMalloc(&d_temp, bytes);
                cudaMalloc(&d_output, bytes);

                // --- 开始循环处理图片 ---
                RawFramePacket packet;
                while (rawQueue.pop(packet)) {
                    // 1. CPU 解码 (这个必须在 CPU 做)
                    cv::Mat raw = cv::imdecode(packet.imgBuffer, cv::IMREAD_UNCHANGED);
                    if (raw.empty()) continue;
                    
                    cv::Mat f;
                    if (raw.channels() == 1) f = raw.clone();
                    else cv::cvtColor(raw, f, cv::COLOR_BGR2GRAY);
                    f.convertTo(f, CV_32FC1);

                    // 2. CPU 计算背景 (代价极小)
                    float bg = estimateBorderBackground(f, 20);

                    auto t1 = Clock::now();

                    // ==========================================
                    // 3. GPU 极速流水线 (零分配开销)
                    // ==========================================
                    // a) 将图像送入专属显存
                    cudaMemcpy(d_input, f.ptr<float>(), bytes, cudaMemcpyHostToDevice);
                    
                    // b) 在 GPU 内一次性跑完去背景和高斯滤波
                    runPreprocessAndBlurCuda(d_input, d_temp, d_output, width, height, bg);
                    
                    // c) 将最终结果拷回 CPU
                    cv::Mat pre(height, width, CV_32FC1);
                    cudaMemcpy(pre.ptr<float>(), d_output, bytes, cudaMemcpyDeviceToHost);

                    localPreTime += std::chrono::duration<double>(Clock::now() - t1).count();

                    auto t2 = Clock::now();
                    std::vector<SpotSignal> frameSignals;
                    frameSignals.reserve(spots.size());
                    for (const auto& spot : spots) {
                        double intensity = extractCircularRoiMean(pre, spot) / spot.gain;
                        frameSignals.push_back({packet.frameId, spot.id, packet.stageX + spot.sampleOffset.x, packet.stageY + spot.sampleOffset.y, intensity});
                    }
                    localRoiTime += std::chrono::duration<double>(Clock::now() - t2).count();
                    
                    signalQueue.push({frameSignals});
                    if (++processedFramesCount % 100 == 0) std::cout << "Processed " << processedFramesCount << " frames...\n";
                }
                totalPreprocessTime = totalPreprocessTime + localPreTime;
                totalRoiTime = totalRoiTime + localRoiTime;
            });
        }

        // --- 4. 启动单线程 Accumulator ---
        std::thread accumulatorThread([&]() {
            using Clock = std::chrono::steady_clock;
            double localAccumTime = 0.0;
            SignalPacket packet;
            while (signalQueue.pop(packet)) {
                auto t1 = Clock::now();
                for (const auto& sig : packet.signals) {
                    addBilinearSample(accum, weight, sig.sampleX, sig.sampleY, sig.intensity);
                }
                localAccumTime += std::chrono::duration<double>(Clock::now() - t1).count();
                std::lock_guard<std::mutex> lock(allSignalsMutex);
                allSignals.insert(allSignals.end(), packet.signals.begin(), packet.signals.end());
            }
            totalAccumTime = totalAccumTime + localAccumTime;
        });

        // --- 5. 优雅停机 ---
        producerThread.join();
        for (auto& t : workerThreads) t.join();
        signalQueue.setFinished(); 
        accumulatorThread.join();

        monitor.addStageTime("frame_io", totalIoTime.load());
        monitor.addStageTime("preprocess", totalPreprocessTime.load());
        monitor.addStageTime("roi_extraction", totalRoiTime.load());
        monitor.addStageTime("accumulation", totalAccumTime.load());
        monitor.mark("Pipeline finished");

        // --- 6. 后处理收尾 ---
        cv::Mat reconRaw = normalizeByWeight(accum, weight);
        cv::imwrite(outputDir + "/reconstruction_raw.tif", normalizeTo16U(reconRaw));
        cv::imwrite(outputDir + "/weight_map.tif", normalizeTo16U(weight));

        cv::Mat reconFilled = fillHolesByInpaint(reconRaw, weight);
        cv::imwrite(outputDir + "/reconstruction_filled.tif", normalizeTo16U(reconFilled));

        cv::Mat reconFiltered;
        cv::GaussianBlur(reconFilled, reconFiltered, cv::Size(0, 0), 0.8);
        cv::imwrite(outputDir + "/reconstruction_filtered.tif", normalizeTo16U(reconFiltered));

        cv::Mat psf = makeGaussianPsf(psfSize, psfSigma);
        cv::Mat reconDeconv = lucyRichardsonDeconvolution(reconFiltered, psf, deconvIterations);
        cv::imwrite(outputDir + "/reconstruction_deconvolved.tif", normalizeTo16U(reconDeconv));

        saveSignalsCsv(outputDir + "/spot_signals.csv", allSignals);
        monitor.finish("Program finished");

        std::cout << "Done! Results saved to " << outputDir << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n"; return 1;
    }
    return 0;
}