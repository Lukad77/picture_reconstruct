#pragma once
#include "core_types.h"
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>

std::vector<StagePos> readScanPositions(const std::string& path);
std::vector<SpotInfo> readSpots(const std::string& path);
cv::Mat readFrameAsFloat(const std::string& path);
void saveSignalsCsv(const std::string& path, const std::vector<SpotSignal>& signals);
std::string makeFramePath(const std::string& frameDir, int frameId);

class ResourceMonitor {
public:
    explicit ResourceMonitor(const std::string& csvPath,
                             const std::string& summaryPath)
        : csvPath_(csvPath), summaryPath_(summaryPath) {
        startWall_ = Clock::now();
        lastWall_ = startWall_;

        startUsage_ = getUsage();
        lastUsage_ = startUsage_;

        csv_.open(csvPath_);
        if (!csv_.is_open()) {
            throw std::runtime_error("Cannot open resource log file: " + csvPath_);
        }

        csv_ << "label,"
             << "stage_wall_s,total_wall_s,"
             << "stage_cpu_s,total_cpu_s,"
             << "stage_user_cpu_s,stage_sys_cpu_s,"
             << "total_user_cpu_s,total_sys_cpu_s,"
             << "stage_cpu_usage_percent,average_cpu_usage_percent,"
             << "current_rss_mb,peak_rss_mb\n";
    }

    ~ResourceMonitor() {
        if (csv_.is_open()) {
            csv_.flush();
            csv_.close();
        }
    }

    class ScopedTimer {
    public:
        ScopedTimer(ResourceMonitor& monitor, const std::string& name)
            : monitor_(monitor), name_(name), start_(Clock::now()) {}

        ~ScopedTimer() {
            auto end = Clock::now();
            double seconds = std::chrono::duration<double>(end - start_).count();
            monitor_.addStageTime(name_, seconds);
        }

    private:
        using Clock = std::chrono::steady_clock;

        ResourceMonitor& monitor_;
        std::string name_;
        std::chrono::time_point<Clock> start_;
    };

    ScopedTimer scopedTimer(const std::string& name) {
        return ScopedTimer(*this, name);
    }

    void mark(const std::string& label) {
        auto nowWall = Clock::now();
        CpuUsage nowUsage = getUsage();

        double totalWall = secondsBetween(startWall_, nowWall);
        double stageWall = secondsBetween(lastWall_, nowWall);

        double totalUserCpu = nowUsage.userSec - startUsage_.userSec;
        double totalSysCpu = nowUsage.sysSec - startUsage_.sysSec;
        double stageUserCpu = nowUsage.userSec - lastUsage_.userSec;
        double stageSysCpu = nowUsage.sysSec - lastUsage_.sysSec;

        double totalCpu = totalUserCpu + totalSysCpu;
        double stageCpu = stageUserCpu + stageSysCpu;

        double totalCpuPercent =
            totalWall > 1e-9 ? 100.0 * totalCpu / totalWall : 0.0;

        double stageCpuPercent =
            stageWall > 1e-9 ? 100.0 * stageCpu / stageWall : 0.0;

        long currentRssKb = getCurrentRssKb();
        long peakRssKb = getPeakRssKb();

        ResourceRecord rec;
        rec.label = label;
        rec.stageWall = stageWall;
        rec.totalWall = totalWall;
        rec.stageCpu = stageCpu;
        rec.totalCpu = totalCpu;
        rec.stageUserCpu = stageUserCpu;
        rec.stageSysCpu = stageSysCpu;
        rec.totalUserCpu = totalUserCpu;
        rec.totalSysCpu = totalSysCpu;
        rec.stageCpuPercent = stageCpuPercent;
        rec.totalCpuPercent = totalCpuPercent;
        rec.currentRssMb = kbToMb(currentRssKb);
        rec.peakRssMb = kbToMb(peakRssKb);

        records_.push_back(rec);
        writeCsvRecord(rec);

        lastWall_ = nowWall;
        lastUsage_ = nowUsage;
    }

    void setExperimentConfig(int outputWidth,
                             int outputHeight,
                             int deconvIterations,
                             int psfSize,
                             double psfSigma,
                             int opencvThreads) {
        outputWidth_ = outputWidth;
        outputHeight_ = outputHeight;
        deconvIterations_ = deconvIterations;
        psfSize_ = psfSize;
        psfSigma_ = psfSigma;
        opencvThreads_ = opencvThreads;
    }

    void setDatasetInfo(size_t frameCount, size_t spotCount) {
        frameCount_ = frameCount;
        spotCount_ = spotCount;
        totalSpotSamples_ = frameCount_ * spotCount_;
    }

    void addStageTime(const std::string& name, double seconds) {
        stageTimes_[name] += seconds;
    }

    void addCounter(const std::string& name, double value) {
        counters_[name] += value;
    }

    void setCounter(const std::string& name, double value) {
        counters_[name] = value;
    }

    void finish(const std::string& label = "Program finished") {
        mark(label);
        writeSummary();
    }

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    struct CpuUsage {
        double userSec = 0.0;
        double sysSec = 0.0;
    };

    struct ResourceRecord {
        std::string label;
        double stageWall = 0.0;
        double totalWall = 0.0;
        double stageCpu = 0.0;
        double totalCpu = 0.0;
        double stageUserCpu = 0.0;
        double stageSysCpu = 0.0;
        double totalUserCpu = 0.0;
        double totalSysCpu = 0.0;
        double stageCpuPercent = 0.0;
        double totalCpuPercent = 0.0;
        double currentRssMb = 0.0;
        double peakRssMb = 0.0;
    };

    std::string csvPath_;
    std::string summaryPath_;
    std::ofstream csv_;
    std::vector<ResourceRecord> records_;

    std::map<std::string, double> stageTimes_;
    std::map<std::string, double> counters_;

    size_t frameCount_ = 0;
    size_t spotCount_ = 0;
    size_t totalSpotSamples_ = 0;

    int outputWidth_ = 0;
    int outputHeight_ = 0;
    int deconvIterations_ = 0;
    int psfSize_ = 0;
    double psfSigma_ = 0.0;
    int opencvThreads_ = 1;

    TimePoint startWall_;
    TimePoint lastWall_;
    CpuUsage startUsage_;
    CpuUsage lastUsage_;

    static double secondsBetween(const TimePoint& a, const TimePoint& b) {
        return std::chrono::duration<double>(b - a).count();
    }

    static CpuUsage getUsage() {
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);

        CpuUsage result;
        result.userSec =
            static_cast<double>(usage.ru_utime.tv_sec) +
            static_cast<double>(usage.ru_utime.tv_usec) / 1e6;

        result.sysSec =
            static_cast<double>(usage.ru_stime.tv_sec) +
            static_cast<double>(usage.ru_stime.tv_usec) / 1e6;

        return result;
    }

    static long getPeakRssKb() {
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);

        // Linux 下 ru_maxrss 单位为 KB
        return usage.ru_maxrss;
    }

    static long getCurrentRssKb() {
        std::ifstream fin("/proc/self/status");
        if (!fin.is_open()) {
            return -1;
        }

        std::string line;
        while (std::getline(fin, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line);

                std::string key;
                long value = -1;
                std::string unit;

                iss >> key >> value >> unit;
                return value;
            }
        }

        return -1;
    }

    static double kbToMb(long kb) {
        if (kb < 0) {
            return -1.0;
        }
        return static_cast<double>(kb) / 1024.0;
    }

    static std::string escapeCsv(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') {
                out += "\"\"";
            } else {
                out += c;
            }
        }
        out += "\"";
        return out;
    }

    void writeCsvRecord(const ResourceRecord& rec) {
        csv_ << escapeCsv(rec.label) << ","
             << rec.stageWall << ","
             << rec.totalWall << ","
             << rec.stageCpu << ","
             << rec.totalCpu << ","
             << rec.stageUserCpu << ","
             << rec.stageSysCpu << ","
             << rec.totalUserCpu << ","
             << rec.totalSysCpu << ","
             << rec.stageCpuPercent << ","
             << rec.totalCpuPercent << ","
             << rec.currentRssMb << ","
             << rec.peakRssMb << "\n";

        csv_.flush();
    }

    double getStageTime(const std::string& name) const {
        auto it = stageTimes_.find(name);
        if (it == stageTimes_.end()) {
            return 0.0;
        }
        return it->second;
    }

    double getCounter(const std::string& name) const {
        auto it = counters_.find(name);
        if (it == counters_.end()) {
            return 0.0;
        }
        return it->second;
    }

    void writeSummary() {
        if (records_.empty()) {
            return;
        }

        const ResourceRecord& last = records_.back();

        double maxCurrentRss = 0.0;
        double maxPeakRss = 0.0;
        double maxStageCpuPercent = 0.0;
        std::string maxStageCpuLabel;

        // 极短阶段的 CPU 使用率容易因为计时精度出现 >100%，这里不参与最大值统计
        const double minStageWallForCpuPercent = 0.01;

        for (const auto& rec : records_) {
            if (rec.currentRssMb > maxCurrentRss) {
                maxCurrentRss = rec.currentRssMb;
            }
            if (rec.peakRssMb > maxPeakRss) {
                maxPeakRss = rec.peakRssMb;
            }
            if (rec.stageWall >= minStageWallForCpuPercent &&
                rec.stageCpuPercent > maxStageCpuPercent) {
                maxStageCpuPercent = rec.stageCpuPercent;
                maxStageCpuLabel = rec.label;
            }
        }

        double frameIoTime = getStageTime("frame_io");
        double preprocessTime = getStageTime("preprocess");
        double roiTime = getStageTime("roi_extraction");
        double accumulationTime = getStageTime("accumulation");
        double deconvTime = getStageTime("deconvolution");
        double saveImageTime = getStageTime("save_images");
        double saveCsvTime = getStageTime("save_csv");

        double algorithmTime =
            preprocessTime +
            roiTime +
            accumulationTime +
            getStageTime("normalize_reconstruction") +
            getStageTime("hole_filling") +
            getStageTime("gaussian_filter") +
            getStageTime("psf_generation") +
            deconvTime;

        double fps = last.totalWall > 1e-9
            ? static_cast<double>(frameCount_) / last.totalWall
            : 0.0;

        double spotsPerSecond = algorithmTime > 1e-9
            ? static_cast<double>(totalSpotSamples_) / algorithmTime
            : 0.0;

        double ioFps = frameIoTime > 1e-9
            ? static_cast<double>(frameCount_) / frameIoTime
            : 0.0;

        double deconvPerIteration = deconvIterations_ > 0
            ? deconvTime / static_cast<double>(deconvIterations_)
            : 0.0;

        double totalPixels = static_cast<double>(outputWidth_) *
                             static_cast<double>(outputHeight_);

        double deconvPixelsPerSecond = deconvTime > 1e-9
            ? totalPixels * static_cast<double>(std::max(deconvIterations_, 1)) / deconvTime
            : 0.0;

        std::ofstream summary(summaryPath_);
        if (!summary.is_open()) {
            throw std::runtime_error("Cannot open resource summary file: " + summaryPath_);
        }

        summary << "Resource usage summary\n";
        summary << "======================\n\n";

        summary << "Experiment configuration\n";
        summary << "------------------------\n";
        summary << "Frames: " << frameCount_ << "\n";
        summary << "Spots per frame: " << spotCount_ << "\n";
        summary << "Total spot samples: " << totalSpotSamples_ << "\n";
        summary << "Output size: " << outputWidth_ << " x " << outputHeight_ << "\n";
        summary << "Deconvolution iterations: " << deconvIterations_ << "\n";
        summary << "PSF size: " << psfSize_ << "\n";
        summary << "PSF sigma: " << psfSigma_ << "\n";
        summary << "OpenCV threads: " << opencvThreads_ << "\n\n";

        summary << "Timing summary\n";
        summary << "--------------\n";
        summary << "Total wall time: " << last.totalWall << " s\n";
        summary << "Algorithm wall time: " << algorithmTime << " s\n";
        summary << "Frame I/O time: " << frameIoTime << " s\n";
        summary << "Preprocessing time: " << preprocessTime << " s\n";
        summary << "ROI extraction time: " << roiTime << " s\n";
        summary << "Accumulation time: " << accumulationTime << " s\n";
        summary << "Normalize reconstruction time: "
                << getStageTime("normalize_reconstruction") << " s\n";
        summary << "Hole filling time: " << getStageTime("hole_filling") << " s\n";
        summary << "Gaussian filtering time: " << getStageTime("gaussian_filter") << " s\n";
        summary << "PSF generation time: " << getStageTime("psf_generation") << " s\n";
        summary << "Deconvolution time: " << deconvTime << " s\n";
        summary << "Deconvolution time per iteration: "
                << deconvPerIteration << " s\n";
        summary << "Save image time: " << saveImageTime << " s\n";
        summary << "Save CSV time: " << saveCsvTime << " s\n\n";

        summary << "Throughput\n";
        summary << "----------\n";
        summary << "End-to-end FPS: " << fps << " frames/s\n";
        summary << "Frame I/O FPS: " << ioFps << " frames/s\n";
        summary << "Spot processing throughput: "
                << spotsPerSecond << " spots/s\n";
        summary << "Deconvolution pixel throughput: "
                << deconvPixelsPerSecond << " pixel-iterations/s\n\n";

        summary << "CPU / memory\n";
        summary << "------------\n";
        summary << "Total CPU time : " << last.totalCpu << " s\n";
        summary << "Total user CPU : " << last.totalUserCpu << " s\n";
        summary << "Total sys CPU  : " << last.totalSysCpu << " s\n";
        summary << "Average CPU usage: " << last.totalCpuPercent << " %\n";
        summary << "Max current RSS observed: " << maxCurrentRss << " MB\n";
        summary << "Peak RSS: " << maxPeakRss << " MB\n";

        if (!maxStageCpuLabel.empty()) {
            summary << "Max stage CPU usage: " << maxStageCpuPercent
                    << " % at [" << maxStageCpuLabel << "]\n";
        } else {
            summary << "Max stage CPU usage: N/A\n";
        }

        summary << "\nDetailed stage CSV log: " << csvPath_ << "\n";
    }
};