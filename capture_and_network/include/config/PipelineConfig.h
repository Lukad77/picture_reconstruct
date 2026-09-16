#pragma once

#include "V2Transfer.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace pipelineconfig {

struct SenderConfig {
    uint64_t taskId = 1;
    std::string host;
    uint16_t port = 0;
    std::filesystem::path spoolDirectory;
    v2transfer::SenderOptions transfer;
    std::string captureMode;
    int cameraIndex = -1;
    int width = 640;
    int height = 480;
    int fps = 30;
    int warmupFrames = 5;
    std::filesystem::path scanCsv;
};

struct ReceiverConfig {
    uint64_t taskId = 1;
    uint16_t port = 0;
    std::filesystem::path spoolDirectory;
    v2transfer::ReceiverOptions transfer;
    std::filesystem::path spotsCsv;
    std::filesystem::path outputDirectory;
    int outputWidth = 0;
    int outputHeight = 0;
};

std::filesystem::path discoverConfigPath(
    int argc, char** argv, const char* environmentName, const std::filesystem::path& defaultPath);
SenderConfig loadSenderConfig(const std::filesystem::path& path);
ReceiverConfig loadReceiverConfig(const std::filesystem::path& path);

} // namespace pipelineconfig
