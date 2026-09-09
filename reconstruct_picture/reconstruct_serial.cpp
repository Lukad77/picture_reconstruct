#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

// ==========================================
// 1. 定义多线程流水线的数据包
// ==========================================

// 队列A的数据包：包含读入的原始图像和对应的坐标信息
struct RawFramePacket {
    int frameId;
    double stageX;
    double stageY;
    cv::Mat image;
};

// 队列B的数据包：包含一帧图像提取出的所有光斑信号
struct SignalPacket {
    std::vector<SpotSignal> signals;
};

// ==========================================
// 2. 线程安全的背压队列 (Thread-Safe Bounded Queue)
// ==========================================
template <typename T>
class BoundedQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
    std::atomic<bool> finished_{false};

public:
    explicit BoundedQueue(size_t max_size = 100) : max_size_(max_size) {}

    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 背压：如果队列满，则挂起当前线程等待下游消费
        not_full_.wait(lock, [this]() { return queue_.size() < max_size_; });
        queue_.push(std::move(item));
        lock.unlock();
        not_empty_.notify_one(); 
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this]() { return !queue_.empty() || finished_; });

        if (queue_.empty() && finished_) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one(); 
        return true;
    }

    void setFinished() {
        finished_ = true;
        not_empty_.notify_all(); 
    }
};
struct StagePos {
    double x;
    double y;
};

struct SpotInfo {
    int id;
    cv::Point2f cameraCenter;   // 鍏夋枒鍦ㄧ浉鏈哄浘鍍忎腑鐨勪腑蹇�
    cv::Point2f sampleOffset;   // 璇ュ厜鏂戠浉瀵逛簬鎵弿涓績鐨勬牱鍝佸潗鏍囧亸绉�
    float radius;
    float gain;
};

struct FrameData {
    cv::Mat image;
    StagePos stage;
};

static cv::Point scaledPoint(int x, int y, int width, int height) {
    return cv::Point(
        static_cast<int>(std::round(x * width / 512.0)),
        static_cast<int>(std::round(y * height / 512.0))
    );
}

static cv::Mat createGroundTruth(int width, int height) {
    cv::Mat gt = cv::Mat::zeros(height, width, CV_32FC1);

    // 妯℃嫙绾崇背绾� / 寰€氶亾銆傚潗鏍囨寜 512 鍩哄噯缂╂斁锛屾柟渚垮垏鎹㈠埌 1024/2048銆�
    cv::line(gt, scaledPoint(60, 100, width, height), scaledPoint(450, 120, width, height), cv::Scalar(1.0f), 4);
    cv::line(gt, scaledPoint(80, 250, width, height), scaledPoint(430, 250, width, height), cv::Scalar(0.8f), 5);
    cv::line(gt, scaledPoint(120, 400, width, height), scaledPoint(400, 340, width, height), cv::Scalar(1.0f), 4);
    cv::line(gt, scaledPoint(80, 360, width, height), scaledPoint(470, 460, width, height), cv::Scalar(0.7f), 3);
    cv::line(gt, scaledPoint(40, 180, width, height), scaledPoint(460, 70, width, height), cv::Scalar(0.6f), 2);

    // 澧炲姞缁嗗皬缁撴瀯瀵嗗害锛屼娇閲嶅缓鍚庣殑鎻掑€煎拰鍘诲櫔鏇村洶闅俱€�
    for (int i = 0; i < 160; ++i) {
        int x = 30 + (i * 67) % 460;
        int y = 35 + (i * 97) % 440;
        int rr = 2 + (i % 4);
        float val = 0.35f + 0.55f * static_cast<float>((i * 13) % 100) / 100.0f;
        cv::circle(gt, scaledPoint(x, y, width, height), rr, cv::Scalar(val), -1);
    }

    // 妯℃嫙鍦嗗舰 / 鐜舰缁撴瀯
    cv::circle(gt, scaledPoint(260, 260, width, height), static_cast<int>(55 * width / 512.0), cv::Scalar(0.5f), 4);
    cv::circle(gt, scaledPoint(360, 160, width, height), static_cast<int>(30 * width / 512.0), cv::Scalar(0.9f), 3);
    cv::circle(gt, scaledPoint(170, 150, width, height), static_cast<int>(38 * width / 512.0), cv::Scalar(0.65f), 3);

    // 浣庨鑳屾櫙绾圭悊锛屽鍔犲悗缁儗鏅墸闄ら毦搴︺€�
    for (int y = 0; y < gt.rows; ++y) {
        float* row = gt.ptr<float>(y);
        for (int x = 0; x < gt.cols; ++x) {
            float texture = 0.04f * std::sin(0.018f * x) + 0.035f * std::cos(0.014f * y);
            row[x] = std::max(0.0f, row[x] + texture);
        }
    }

    // 杞诲井妯＄硦锛屾ā鎷熺湡瀹炶崸鍏夊垎甯�
    cv::GaussianBlur(gt, gt, cv::Size(0, 0), 1.2);

    return gt;
}

static std::vector<SpotInfo> createSpotArray(
    int rows,
    int cols,
    int cameraWidth,
    int cameraHeight,
    float cameraSpacing,
    float sampleSpacing
) {
    std::vector<SpotInfo> spots;
    int id = 0;

    float cx = cameraWidth * 0.5f;
    float cy = cameraHeight * 0.5f;

    float sx = -(cols - 1) * cameraSpacing * 0.5f;
    float sy = -(rows - 1) * cameraSpacing * 0.5f;

    float ox = -(cols - 1) * sampleSpacing * 0.5f;
    float oy = -(rows - 1) * sampleSpacing * 0.5f;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            SpotInfo s;
            s.id = id++;
            s.cameraCenter = cv::Point2f(
                cx + sx + c * cameraSpacing,
                cy + sy + r * cameraSpacing
            );
            s.sampleOffset = cv::Point2f(
                ox + c * sampleSpacing,
                oy + r * sampleSpacing
            );
            s.radius = 8.0f;

            // 妯℃嫙澶氱劍鐐瑰厜寮轰笉鍧囧寑锛氳鍒楀浐瀹氬樊寮� + 缂撴參绌洪棿璋冨埗銆�
            s.gain = 0.72f
                + 0.42f * static_cast<float>((r * 3 + c * 2) % 7) / 6.0f
                + 0.06f * std::sin(0.7f * r + 0.35f * c);

            spots.push_back(s);
        }
    }

    return spots;
}

static float sampleBilinear(const cv::Mat& img, float x, float y) {
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x0 < 0 || y0 < 0 || x1 >= img.cols || y1 >= img.rows) {
        return 0.0f;
    }

    float dx = x - x0;
    float dy = y - y0;

    float v00 = img.at<float>(y0, x0);
    float v10 = img.at<float>(y0, x1);
    float v01 = img.at<float>(y1, x0);
    float v11 = img.at<float>(y1, x1);

    float v0 = v00 * (1.0f - dx) + v10 * dx;
    float v1 = v01 * (1.0f - dx) + v11 * dx;

    return v0 * (1.0f - dy) + v1 * dy;
}

static void drawGaussianSpot(
    cv::Mat& frame,
    cv::Point2f center,
    float amplitude,
    float sigma
) {
    int radius = static_cast<int>(std::ceil(3.0f * sigma));

    for (int yy = -radius; yy <= radius; ++yy) {
        for (int xx = -radius; xx <= radius; ++xx) {
            int px = static_cast<int>(std::round(center.x)) + xx;
            int py = static_cast<int>(std::round(center.y)) + yy;

            if (px < 0 || py < 0 || px >= frame.cols || py >= frame.rows) {
                continue;
            }

            float d2 = static_cast<float>(xx * xx + yy * yy);
            float val = amplitude * std::exp(-d2 / (2.0f * sigma * sigma));
            frame.at<float>(py, px) += val;
        }
    }
}

static FrameData simulateFrame(
    const cv::Mat& groundTruth,
    const std::vector<SpotInfo>& spots,
    StagePos stage,
    int cameraWidth,
    int cameraHeight,
    int frameIndex
) {
    cv::Mat frame(cameraHeight, cameraWidth, CV_32FC1, cv::Scalar(35.0f));

    // 鍔犲叆缂撴參鍙樺寲鐨勮儗鏅搴︼紝妯℃嫙鐩告満鏆楃數娴� / 鐓ф槑涓嶅潎銆�
    for (int y = 0; y < frame.rows; ++y) {
        float* row = frame.ptr<float>(y);
        for (int x = 0; x < frame.cols; ++x) {
            row[x] += 10.0f * static_cast<float>(x) / cameraWidth
                   + 7.0f * static_cast<float>(y) / cameraHeight
                   + 3.0f * std::sin(0.01f * x + 0.013f * y + 0.03f * frameIndex);
        }
    }

    // 浜氬儚绱犳満姊版紓绉伙紝璁╁悓涓€ stage 浣嶇疆骞朵笉瀹屽叏绛変环銆�
    float driftX = 0.35f * std::sin(0.017f * frameIndex);
    float driftY = 0.30f * std::cos(0.013f * frameIndex);

    for (const auto& spot : spots) {
        float sampleX = static_cast<float>(stage.x + spot.sampleOffset.x) + driftX;
        float sampleY = static_cast<float>(stage.y + spot.sampleOffset.y) + driftY;

        float intensity = sampleBilinear(groundTruth, sampleX, sampleY);
        float temporalGain = 1.0f + 0.08f * std::sin(0.021f * frameIndex + 0.37f * spot.id);
        float amplitude = 2600.0f * intensity * spot.gain * temporalGain;

        // 姣忎釜鍏夋枒杞诲井鎶栧姩鍜屼笉鍚� PSF 瀹藉害锛屾彁楂樺畾浣� / 瑙ｅ嵎绉毦搴︺€�
        cv::Point2f jitteredCenter(
            spot.cameraCenter.x + 0.28f * std::sin(0.19f * frameIndex + 0.11f * spot.id),
            spot.cameraCenter.y + 0.25f * std::cos(0.17f * frameIndex + 0.07f * spot.id)
        );
        float sigma = 2.4f + 0.35f * std::sin(0.5f * spot.id);
        drawGaussianSpot(frame, jitteredCenter, amplitude, sigma);
    }

    // 鏇村己璇诲嚭鍣０锛涘抚鏁板鍚庡骞冲潎鍜岄瞾妫掗噸寤虹畻娉曡姹傛洿楂樸€�
    cv::Mat noise(frame.size(), CV_32FC1);
    cv::randn(noise, 0.0, 18.0);
    frame += noise;

    // 闄愬箙
    cv::max(frame, 0.0, frame);

    return FrameData{frame, stage};
}

static double extractSpotIntensity(
    const cv::Mat& frame,
    const SpotInfo& spot,
    float background = 35.0f
) {
    int cx = static_cast<int>(std::round(spot.cameraCenter.x));
    int cy = static_cast<int>(std::round(spot.cameraCenter.y));
    int r = static_cast<int>(std::round(spot.radius));

    double sum = 0.0;
    int count = 0;

    for (int yy = -r; yy <= r; ++yy) {
        for (int xx = -r; xx <= r; ++xx) {
            if (xx * xx + yy * yy > r * r) {
                continue;
            }

            int px = cx + xx;
            int py = cy + yy;

            if (px < 0 || py < 0 || px >= frame.cols || py >= frame.rows) {
                continue;
            }

            float v = frame.at<float>(py, px) - background;
            if (v < 0.0f) {
                v = 0.0f;
            }

            sum += v;
            count++;
        }
    }

    if (count == 0) {
        return 0.0;
    }

    return sum / static_cast<double>(count);
}


static void accumulateFrameToRecon(
    const FrameData& frame,
    const std::vector<SpotInfo>& spots,
    cv::Mat& accum,
    cv::Mat& weight
) {
    for (const auto& spot : spots) {
        double signal = extractSpotIntensity(frame.image, spot);

        float sampleX = static_cast<float>(frame.stage.x + spot.sampleOffset.x);
        float sampleY = static_cast<float>(frame.stage.y + spot.sampleOffset.y);

        int x = static_cast<int>(std::round(sampleX));
        int y = static_cast<int>(std::round(sampleY));

        if (x < 0 || y < 0 || x >= accum.cols || y >= accum.rows) {
            continue;
        }

        accum.at<float>(y, x) += static_cast<float>(signal / std::max(spot.gain, 1e-6f));
        weight.at<float>(y, x) += 1.0f;
    }
}

static cv::Mat finalizeReconstruction(
    const cv::Mat& accum,
    const cv::Mat& weight
) {
    CV_Assert(accum.size() == weight.size());
    CV_Assert(accum.type() == CV_32FC1);
    CV_Assert(weight.type() == CV_32FC1);

    cv::Mat recon(accum.rows, accum.cols, CV_32FC1, cv::Scalar(0));

    for (int y = 0; y < accum.rows; ++y) {
        const float* accumRow = accum.ptr<float>(y);
        const float* weightRow = weight.ptr<float>(y);
        float* reconRow = recon.ptr<float>(y);

        for (int x = 0; x < accum.cols; ++x) {
            float w = weightRow[x];
            if (w > 0.0f) {
                reconRow[x] = accumRow[x] / w;
            }
        }
    }

    // 琛ョ偣鍜屽钩婊戙€傜湡瀹炵郴缁熼噷鍙互鎹㈡垚鏇村鏉傜殑鎻掑€兼垨鍙嶅嵎绉€�
    cv::GaussianBlur(recon, recon, cv::Size(0, 0), 0.8);

    return recon;
}

static cv::Mat normalizeTo16U(const cv::Mat& src32f) {
    cv::Mat norm32f;
    double minVal = 0.0;
    double maxVal = 0.0;
    cv::minMaxLoc(src32f, &minVal, &maxVal);

    if (std::abs(maxVal - minVal) < 1e-12) {
        return cv::Mat::zeros(src32f.size(), CV_16UC1);
    }

    cv::normalize(src32f, norm32f, 0, 65535, cv::NORM_MINMAX);

    cv::Mat dst16u;
    norm32f.convertTo(dst16u, CV_16UC1);

    return dst16u;
}

static void writeSpotsCsv(
    const std::string& path,
    const std::vector<SpotInfo>& spots
) {
    std::ofstream s(path);
    s << "spot_id,camera_x,camera_y,sample_offset_x,sample_offset_y,radius,gain\n";
    for (const auto& spot : spots) {
        s << spot.id << ","
          << spot.cameraCenter.x << ","
          << spot.cameraCenter.y << ","
          << spot.sampleOffset.x << ","
          << spot.sampleOffset.y << ","
          << spot.radius << ","
          << spot.gain << "\n";
    }
}

int main(int argc, char** argv) {
    try {
        std::string frameDir = "frames";
        std::string scanCsv = "scan_positions.csv";
        std::string spotCsv = "spots.csv";
        std::string outputDir = "reconstruct_output";

        int outputWidth = 512;
        int outputHeight = 512;
        int deconvIterations = 15;

        if (argc >= 2) frameDir = argv[1];
        if (argc >= 3) scanCsv = argv[2];
        if (argc >= 4) spotCsv = argv[3];

        std::filesystem::create_directories(outputDir);

        ResourceMonitor monitor(
            outputDir + "/resource_usage.csv",
            outputDir + "/resource_summary.txt"
        );

        // 极其重要：强制 OpenCV 在内部算子中单线程运行
        // 这样才不会与我们的 std::thread 线程池发生抢占冲突！
        cv::setNumThreads(1);

        const int psfSize = 15;
        const double psfSigma = 1.6;

        monitor.setExperimentConfig(
            outputWidth, outputHeight, deconvIterations,
            psfSize, psfSigma, cv::getNumThreads()
        );

        monitor.mark("Program started and output directory initialized");

        std::vector<StagePos> positions;
        std::vector<SpotInfo> spots;

        {
            auto timer = monitor.scopedTimer("load_metadata");
            positions = readScanPositions(scanCsv);
            spots = readSpots(spotCsv);
        }

        if (positions.empty()) throw std::runtime_error("No scan positions loaded.");
        if (spots.empty()) throw std::runtime_error("No spots loaded.");

        monitor.setDatasetInfo(positions.size(), spots.size());
        monitor.mark("Loaded scan_positions.csv and spots.csv");

        cv::Mat accum(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));
        cv::Mat weight(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));

        std::vector<SpotSignal> allSignals;
        std::mutex allSignalsMutex; // 保护全局 vector 的并发写入

        monitor.mark("Allocated reconstruction buffers");

        std::cout << "Loaded scan positions: " << positions.size() << "\n";
        std::cout << "Loaded spots: " << spots.size() << "\n";
        std::cout << "Starting multi-threaded pipeline...\n";

        // ==========================================
        // 流水线架构核心部分
        // ==========================================

        // 1. 初始化缓冲队列
        BoundedQueue<RawFramePacket> rawQueue(50); // 限制 50 帧驻留内存，防止 OOM
        BoundedQueue<SignalPacket> signalQueue(200);

        // 用于收集各个线程的局部耗时
        std::atomic<double> totalIoTime{0.0};
        std::atomic<double> totalPreprocessTime{0.0};
        std::atomic<double> totalRoiTime{0.0};
        std::atomic<double> totalAccumTime{0.0};
        std::atomic<int> processedFramesCount{0};

        // 2. 启动单线程 Producer (I/O 读取)
        std::thread producerThread([&]() {
            using Clock = std::chrono::steady_clock;
            for (const auto& pos : positions) {
                auto start = Clock::now();
                std::string framePath = makeFramePath(frameDir, pos.frameId);
                cv::Mat raw = readFrameAsFloat(framePath);
                auto end = Clock::now();
                totalIoTime = totalIoTime + std::chrono::duration<double>(end - start).count();

                rawQueue.push({pos.frameId, pos.x, pos.y, raw});
            }
            rawQueue.setFinished(); // I/O 结束，通知下游
        });

        // 3. 启动多线程 Worker Pool (预处理 + ROI提取)
        // 留出 2 个核给主线程(Accumulator)和 IO 线程
        int numWorkers = std::max(1, (int)std::thread::hardware_concurrency() - 2);
        std::cout << "Launched " << numWorkers << " worker threads.\n";
        std::vector<std::thread> workerThreads;

        for (int i = 0; i < numWorkers; ++i) {
            workerThreads.emplace_back([&]() {
                using Clock = std::chrono::steady_clock;
                double localPreTime = 0.0;
                double localRoiTime = 0.0;

                RawFramePacket packet;
                while (rawQueue.pop(packet)) {
                    // a) 预处理
                    auto t1 = Clock::now();
                    cv::Mat pre = preprocessFrame(packet.image);
                    auto t2 = Clock::now();
                    localPreTime += std::chrono::duration<double>(t2 - t1).count();

                    // b) ROI 提取
                    std::vector<SpotSignal> frameSignals;
                    frameSignals.reserve(spots.size());
                    for (const auto& spot : spots) {
                        double intensity = extractCircularRoiMean(pre, spot);
                        intensity /= static_cast<double>(spot.gain);
                        double sampleX = packet.stageX + spot.sampleOffset.x;
                        double sampleY = packet.stageY + spot.sampleOffset.y;

                        frameSignals.push_back({packet.frameId, spot.id, sampleX, sampleY, intensity});
                    }
                    auto t3 = Clock::now();
                    localRoiTime += std::chrono::duration<double>(t3 - t2).count();

                    // 将提取的结果推入队列 B
                    signalQueue.push({frameSignals});

                    // 打印进度
                    int count = ++processedFramesCount;
                    if (count % 100 == 0) {
                        std::cout << "Processed " << count << " frames...\n";
                    }
                }
                
                // 线程结束前，将局部时间累加到全局
                totalPreprocessTime = totalPreprocessTime + localPreTime;
                totalRoiTime = totalRoiTime + localRoiTime;
            });
        }

        // 4. 启动单线程 Accumulator (矩阵累加)
        std::thread accumulatorThread([&]() {
            using Clock = std::chrono::steady_clock;
            double localAccumTime = 0.0;

            SignalPacket packet;
            while (signalQueue.pop(packet)) {
                auto t1 = Clock::now();
                // 单线程独占 accum 和 weight，无需加互斥锁，速度极快
                for (const auto& sig : packet.signals) {
                    addBilinearSample(accum, weight, sig.sampleX, sig.sampleY, sig.intensity);
                }
                auto t2 = Clock::now();
                localAccumTime += std::chrono::duration<double>(t2 - t1).count();

                // 将数据存入全局 vector (这步需要短暂加锁)
                std::lock_guard<std::mutex> lock(allSignalsMutex);
                allSignals.insert(allSignals.end(), packet.signals.begin(), packet.signals.end());
            }
            totalAccumTime = totalAccumTime + localAccumTime;
        });

        // 5. 等待所有线程安全退出 (优雅停机)
        producerThread.join();
        for (auto& t : workerThreads) {
            t.join();
        }
        signalQueue.setFinished(); // Worker全部结束，通知 Accumulator 不再有新数据
        accumulatorThread.join();

        // 汇总多线程里的耗时记录
        monitor.addStageTime("frame_io", totalIoTime.load());
        monitor.addStageTime("preprocess", totalPreprocessTime.load());
        monitor.addStageTime("roi_extraction", totalRoiTime.load());
        monitor.addStageTime("accumulation", totalAccumTime.load());
        monitor.mark("Pipeline finished: Multi-threaded I/O, preprocessing, ROI extraction and accumulation");

        std::cout << "Pipeline finished successfully.\n";

        // ==========================================
        // 收尾与后处理阶段 (保持单线程原样)
        // ==========================================

        cv::Mat reconRaw;
        {
            auto timer = monitor.scopedTimer("normalize_reconstruction");
            reconRaw = normalizeByWeight(accum, weight);
        }
        monitor.mark("Finished weight normalization and raw reconstruction");

        {
            auto timer = monitor.scopedTimer("save_images");
            cv::imwrite(outputDir + "/reconstruction_raw.tif", normalizeTo16U(reconRaw));
            cv::imwrite(outputDir + "/weight_map.tif", normalizeTo16U(weight));
        }
        monitor.mark("Saved raw reconstruction and weight map");

        cv::Mat reconFilled;
        {
            auto timer = monitor.scopedTimer("hole_filling");
            reconFilled = fillHolesByInpaint(reconRaw, weight);
        }
        monitor.mark("Finished hole filling");

        {
            auto timer = monitor.scopedTimer("save_images");
            cv::imwrite(outputDir + "/reconstruction_filled.tif", normalizeTo16U(reconFilled));
        }
        monitor.mark("Saved filled reconstruction");

        cv::Mat reconFiltered;
        {
            auto timer = monitor.scopedTimer("gaussian_filter");
            cv::GaussianBlur(reconFilled, reconFiltered, cv::Size(0, 0), 0.8);
        }
        monitor.mark("Finished Gaussian filtering");

        {
            auto timer = monitor.scopedTimer("save_images");
            cv::imwrite(outputDir + "/reconstruction_filtered.tif", normalizeTo16U(reconFiltered));
        }
        monitor.mark("Saved filtered reconstruction");

        cv::Mat psf;
        {
            auto timer = monitor.scopedTimer("psf_generation");
            psf = makeGaussianPsf(psfSize, psfSigma);
        }
        monitor.mark("Generated Gaussian PSF");

        cv::Mat reconDeconv;
        {
            auto timer = monitor.scopedTimer("deconvolution");
            reconDeconv = lucyRichardsonDeconvolution(reconFiltered, psf, deconvIterations);
        }
        monitor.mark("Finished Lucy-Richardson deconvolution");

        {
            auto timer = monitor.scopedTimer("save_images");
            cv::imwrite(outputDir + "/reconstruction_deconvolved.tif", normalizeTo16U(reconDeconv));
        }
        monitor.mark("Saved deconvolved reconstruction");

        {
            auto timer = monitor.scopedTimer("save_csv");
            saveSignalsCsv(outputDir + "/spot_signals.csv", allSignals);
        }
        monitor.mark("Saved spot signal CSV");

        monitor.finish("Program finished");

        std::cout << "Done.\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}