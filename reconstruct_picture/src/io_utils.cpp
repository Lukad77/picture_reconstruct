#include "../include/io_utils.h"
#include <sstream>
#include <filesystem>

// 匿名命名空间，作为文件内私有函数
namespace {
    std::vector<std::string> splitCsvLine(const std::string& line) {
        std::vector<std::string> tokens;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, ',')) tokens.push_back(item);
        return tokens;
    }
}

std::vector<StagePos> readScanPositions(const std::string& path) {
    std::ifstream fin(path);
    if (!fin.is_open()) throw std::runtime_error("Cannot open scan position file: " + path);
    std::vector<StagePos> positions;
    std::string line;
    std::getline(fin, line); // header
    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        auto tokens = splitCsvLine(line);
        if (tokens.size() < 3) continue;
        StagePos pos;
        pos.frameId = std::stoi(tokens[0]);
        pos.x = std::stod(tokens[1]);
        pos.y = std::stod(tokens[2]);
        positions.push_back(pos);
    }
    return positions;
}

std::vector<SpotInfo> readSpots(const std::string& path) {
    std::ifstream fin(path);
    if (!fin.is_open()) throw std::runtime_error("Cannot open spot file: " + path);
    std::vector<SpotInfo> spots;
    std::string line;
    std::getline(fin, line); // header
    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        auto tokens = splitCsvLine(line);
        if (tokens.size() < 7) continue;
        SpotInfo spot;
        spot.id = std::stoi(tokens[0]);
        spot.cameraCenter.x = std::stof(tokens[1]);
        spot.cameraCenter.y = std::stof(tokens[2]);
        spot.sampleOffset.x = std::stof(tokens[3]);
        spot.sampleOffset.y = std::stof(tokens[4]);
        spot.radius = std::stof(tokens[5]);
        spot.gain = std::stof(tokens[6]);
        if (spot.gain <= 1e-6f) spot.gain = 1.0f;
        spots.push_back(spot);
    }
    return spots;
}

cv::Mat readFrameAsFloat(const std::string& path) {
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) throw std::runtime_error("Cannot read frame: " + path);
    cv::Mat gray, f;
    if (img.channels() == 1) gray = img;
    else cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    gray.convertTo(f, CV_32FC1);
    return f;
}

void saveSignalsCsv(const std::string& path, const std::vector<SpotSignal>& signals) {
    std::ofstream fout(path);
    if (!fout.is_open()) throw std::runtime_error("Cannot write signal file: " + path);
    fout << "frame_id,spot_id,sample_x,sample_y,intensity\n";
    for (const auto& s : signals) {
        fout << s.frameId << "," << s.spotId << "," << s.sampleX << "," << s.sampleY << "," << s.intensity << "\n";
    }
}

std::string makeFramePath(const std::string& frameDir, int frameId) {
    char name[256];
    std::snprintf(name, sizeof(name), "frame_%05d.tif", frameId);
    std::filesystem::path p = std::filesystem::path(frameDir) / name;
    return p.string();
}