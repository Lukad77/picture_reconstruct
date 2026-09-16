#include "V2Transfer.h"
#include "PipelineConfig.h"

#include <opencv2/opencv.hpp>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
struct Spot { int id; cv::Point2f center; cv::Point2f offset; float radius; float gain; };
struct Signal { uint64_t frame; int spot; double x; double y; double value; };

bool syncFile(const fs::path& path) {
#ifdef _WIN32
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0) return false;
    const bool ok = _commit(_fileno(file)) == 0;
#else
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    const bool ok = fsync(fileno(file)) == 0;
#endif
    return fclose(file) == 0 && ok;
}

std::vector<Spot> readSpots(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open spots CSV: " + path);
    std::vector<Spot> spots;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::stringstream stream(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(stream, field, ',')) fields.push_back(field);
        if (fields.size() < 7) continue;
        spots.push_back({std::stoi(fields[0]), {std::stof(fields[1]), std::stof(fields[2])},
                         {std::stof(fields[3]), std::stof(fields[4])},
                         std::stof(fields[5]), std::max(1e-6f, std::stof(fields[6]))});
    }
    if (spots.empty()) throw std::runtime_error("spots CSV has no calibration rows");
    return spots;
}

class Reconstructor {
public:
    Reconstructor(std::vector<Spot> spots, int width, int height, fs::path output)
        : spots_(std::move(spots)), output_(std::move(output)),
          accum_(height, width, CV_32FC1, cv::Scalar(0)),
          weight_(height, width, CV_32FC1, cv::Scalar(0)) {}

    void consume(const v2transfer::Frame& frame) {
        if (!seen_.insert(frame.frameSeq).second) return;
        const uint64_t expected = uint64_t(frame.rows) * frame.cols * frame.elemSize;
        if (expected != frame.pixels.size()) throw std::runtime_error("raw frame byte count mismatch");
        cv::Mat wrapped(static_cast<int>(frame.rows), static_cast<int>(frame.cols),
                        static_cast<int>(frame.pixelType), const_cast<uint8_t*>(frame.pixels.data()));
        cv::Mat gray;
        if (wrapped.channels() == 1) gray = wrapped;
        else cv::cvtColor(wrapped, gray, cv::COLOR_BGR2GRAY);
        cv::Mat processed;
        gray.convertTo(processed, CV_32FC1);
        const int border = std::max(1, std::min({20, processed.rows / 4, processed.cols / 4}));
        std::vector<float> edge;
        for (int y = 0; y < processed.rows; ++y) for (int x = 0; x < processed.cols; ++x)
            if (x < border || y < border || x >= processed.cols - border || y >= processed.rows - border)
                edge.push_back(processed.at<float>(y, x));
        std::nth_element(edge.begin(), edge.begin() + edge.size() / 2, edge.end());
        processed -= edge[edge.size() / 2];
        cv::max(processed, 0.0, processed);
        cv::GaussianBlur(processed, processed, {}, 0.8);
        for (const auto& spot : spots_) {
            const int cx = static_cast<int>(std::lround(spot.center.x));
            const int cy = static_cast<int>(std::lround(spot.center.y));
            const int radius = static_cast<int>(std::lround(spot.radius));
            double sum = 0; int count = 0;
            for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy > radius * radius) continue;
                const int x = cx + dx, y = cy + dy;
                if (x >= 0 && y >= 0 && x < processed.cols && y < processed.rows) {
                    sum += processed.at<float>(y, x); ++count;
                }
            }
            const double value = count ? sum / count / spot.gain : 0;
            const double sampleX = frame.stageX + spot.offset.x;
            const double sampleY = frame.stageY + spot.offset.y;
            add(sampleX, sampleY, value);
            signals_.push_back({frame.frameSeq, spot.id, sampleX, sampleY, value});
        }
        std::cout << "reconstructed frame=" << frame.frameSeq << " stage=(" << frame.stageX << ',' << frame.stageY << ")\n";
    }

    void save() const {
        fs::create_directories(output_);
        cv::Mat reconstruction(accum_.size(), CV_32FC1, cv::Scalar(0));
        for (int y = 0; y < accum_.rows; ++y) for (int x = 0; x < accum_.cols; ++x) {
            const float w = weight_.at<float>(y, x);
            if (w > 1e-6f) reconstruction.at<float>(y, x) = accum_.at<float>(y, x) / w;
        }
        cv::Mat normalized, weight16;
        cv::normalize(reconstruction, normalized, 0, 65535, cv::NORM_MINMAX, CV_16UC1);
        cv::normalize(weight_, weight16, 0, 65535, cv::NORM_MINMAX, CV_16UC1);
        const fs::path reconstructionTmp = output_ / "reconstruction.part.tif";
        const fs::path weightTmp = output_ / "weight_map.part.tif";
        const fs::path csvTmp = output_ / "spot_signals.part.csv";
        if (!cv::imwrite(reconstructionTmp.string(), normalized) || !cv::imwrite(weightTmp.string(), weight16))
            throw std::runtime_error("cannot write reconstruction images");
        std::ofstream csv(csvTmp);
        csv << "frame_seq,spot_id,sample_x,sample_y,intensity\n";
        for (const auto& signal : signals_)
            csv << signal.frame << ',' << signal.spot << ',' << signal.x << ',' << signal.y << ',' << signal.value << '\n';
        csv.flush();
        if (!csv) throw std::runtime_error("cannot write spot signal CSV");
        csv.close();
        if (!syncFile(reconstructionTmp) || !syncFile(weightTmp) || !syncFile(csvTmp))
            throw std::runtime_error("cannot make reconstruction outputs durable");
        auto publish = [](const fs::path& temporary, const fs::path& final) {
            std::error_code ec;
            fs::rename(temporary, final, ec);
#ifdef _WIN32
            if (ec) {
                ec.clear(); fs::remove(final, ec); ec.clear();
                fs::rename(temporary, final, ec);
            }
#endif
            if (ec) throw std::runtime_error("cannot publish output: " + final.string());
        };
        publish(reconstructionTmp, output_ / "reconstruction.tif");
        publish(weightTmp, output_ / "weight_map.tif");
        publish(csvTmp, output_ / "spot_signals.csv");
        std::cout << "saved " << seen_.size() << " frames to " << output_ << '\n';
    }

private:
    void add(double x, double y, double value) {
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const double dx = x - x0, dy = y - y0;
        const struct { int x; int y; double w; } points[] = {
            {x0, y0, (1-dx)*(1-dy)}, {x0+1, y0, dx*(1-dy)},
            {x0, y0+1, (1-dx)*dy}, {x0+1, y0+1, dx*dy}};
        for (const auto& point : points) if (point.x >= 0 && point.y >= 0 && point.x < accum_.cols && point.y < accum_.rows) {
            accum_.at<float>(point.y, point.x) += static_cast<float>(value * point.w);
            weight_.at<float>(point.y, point.x) += static_cast<float>(point.w);
        }
    }
    std::vector<Spot> spots_;
    fs::path output_;
    cv::Mat accum_, weight_;
    std::set<uint64_t> seen_;
    std::vector<Signal> signals_;
};
}

int main(int argc, char** argv) {
    try {
        pipelineconfig::ReceiverConfig config;
        if (argc == 1 || (argc == 3 && std::string(argv[1]) == "--config")) {
            const auto path = pipelineconfig::discoverConfigPath(
                argc, argv, "PICTURE_RECONSTRUCT_RECEIVER_CONFIG", "config/receiver.json");
            config = pipelineconfig::loadReceiverConfig(path);
        } else {
            if (argc < 7) {
                std::cerr << "usage: linux_reconstruct_receiver [--config path] | <port> <receiver-spool> <spots.csv> <output-dir> <output-width> <output-height>\n";
                return 2;
            }
            config.port = static_cast<uint16_t>(std::stoi(argv[1])); config.spoolDirectory = argv[2];
            config.spotsCsv = argv[3]; config.outputDirectory = argv[4];
            config.outputWidth = std::stoi(argv[5]); config.outputHeight = std::stoi(argv[6]);
            config.transfer.expectedTaskId = config.taskId;
        }
        Reconstructor reconstructor(readSpots(config.spotsCsv.string()), config.outputWidth, config.outputHeight, config.outputDirectory);
        // 先重放 receiver spool 中已有的完整帧，支持 receiver 重启后继续出图。
        const fs::path taskDir = config.spoolDirectory / std::to_string(config.taskId);
        std::error_code ec;
        std::vector<fs::path> savedPaths;
        if (fs::exists(taskDir, ec)) for (const auto& entry : fs::directory_iterator(taskDir))
            if (entry.path().extension() == ".frame") savedPaths.push_back(entry.path());
        std::sort(savedPaths.begin(), savedPaths.end(), [](const fs::path& a, const fs::path& b) {
            return std::stoull(a.stem().string()) < std::stoull(b.stem().string());
        });
        for (const auto& path : savedPaths) {
            v2transfer::Frame saved;
            if (!v2transfer::loadFrame(path, saved)) throw std::runtime_error("corrupt receiver spool: " + path.string());
            reconstructor.consume(saved);
        }
        // Receiver 负责协议、校验、持久化；本程序只提供重构回调。
        v2transfer::Receiver receiver(config.port, config.spoolDirectory, config.transfer);
        receiver.setFrameHandler([&](const v2transfer::Frame& frame) { reconstructor.consume(frame); });
        // TaskFinish 到达后写出最终 TIFF 和 spot 信号 CSV。
        receiver.setFinishHandler([&](uint64_t) { reconstructor.save(); });
        std::cout << "waiting for Protocol V2 sender on port " << config.port << '\n';
        if (!receiver.serveUntilFinished()) throw std::runtime_error("receiver stopped before TaskFinish: " + receiver.lastError());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
