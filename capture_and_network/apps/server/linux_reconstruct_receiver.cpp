#include "V2Transfer.h"

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

namespace {
namespace fs = std::filesystem;
struct Spot { int id; cv::Point2f center; cv::Point2f offset; float radius; float gain; };
struct Signal { uint64_t frame; int spot; double x; double y; double value; };

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
        if (!cv::imwrite((output_ / "reconstruction.tif").string(), normalized) ||
            !cv::imwrite((output_ / "weight_map.tif").string(), weight16))
            throw std::runtime_error("cannot write reconstruction images");
        std::ofstream csv(output_ / "spot_signals.csv");
        csv << "frame_seq,spot_id,sample_x,sample_y,intensity\n";
        for (const auto& signal : signals_)
            csv << signal.frame << ',' << signal.spot << ',' << signal.x << ',' << signal.y << ',' << signal.value << '\n';
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
    if (argc < 7) {
        std::cerr << "usage: linux_reconstruct_receiver <port> <receiver-spool> <spots.csv> <output-dir> <output-width> <output-height>\n";
        return 2;
    }
    try {
        const uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));
        const fs::path spool = argv[2];
        Reconstructor reconstructor(readSpots(argv[3]), std::stoi(argv[5]), std::stoi(argv[6]), argv[4]);
        const fs::path taskDir = spool / "1";
        std::error_code ec;
        if (fs::exists(taskDir, ec)) for (const auto& entry : fs::directory_iterator(taskDir)) {
            if (entry.path().extension() != ".frame") continue;
            v2transfer::Frame saved;
            if (!v2transfer::loadFrame(entry.path(), saved)) throw std::runtime_error("corrupt receiver spool: " + entry.path().string());
            reconstructor.consume(saved);
        }
        v2transfer::Receiver receiver(port, spool);
        receiver.setFrameHandler([&](const v2transfer::Frame& frame) { reconstructor.consume(frame); });
        receiver.setFinishHandler([&](uint64_t) { reconstructor.save(); });
        std::cout << "waiting for Protocol V2 sender on port " << port << '\n';
        if (!receiver.serveUntilFinished()) throw std::runtime_error("receiver stopped before TaskFinish: " + receiver.lastError());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
