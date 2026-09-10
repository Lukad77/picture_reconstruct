#include "SenderSpool.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

struct SpoolHeader {
    uint32_t magic = 0x5350484Cu; // SPHL
    uint16_t version = 1;
    uint64_t frameId = 0;
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t type = 0;
    uint32_t elemSize = 0;
    uint64_t totalBytes = 0;
};

}  // namespace

SenderSpool::SenderSpool(std::string rootDir)
    : rootDir_(std::move(rootDir)),
      maxSpoolBytes_(0),
      bytesUsed_(0) {
    ensureRoot();
    for (const auto& entry : std::filesystem::directory_iterator(rootDir_)) {
        if (entry.is_regular_file()) {
            bytesUsed_ += static_cast<size_t>(entry.file_size());
        }
    }
}

SenderSpool::~SenderSpool() = default;

bool SenderSpool::persist(const RawFrame& frame) {
    if (frame.empty()) {
        std::cerr << "[SenderSpool] empty frame, cannot persist" << std::endl;
        return false;
    }

    ensureRoot();
    const uint64_t requiredBytes = frame.totalBytes + sizeof(SpoolHeader);
    if (maxSpoolBytes_ > 0 && !canPersist(requiredBytes)) {
        std::cerr << "[SenderSpool] spool full: required=" << requiredBytes
                  << ", used=" << bytesUsed_ << ", max=" << maxSpoolBytes_
                  << std::endl;
        return false;
    }

    const std::string path = filePathFor(frame.frameId);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[SenderSpool] cannot open spool file: " << path << std::endl;
        return false;
    }

    SpoolHeader header{};
    header.frameId = frame.frameId;
    header.rows = frame.rows;
    header.cols = frame.cols;
    header.type = frame.type;
    header.elemSize = frame.elemSize;
    header.totalBytes = frame.totalBytes;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.totalBytes));
    out.close();

    if (!out) {
        std::cerr << "[SenderSpool] write failed for frameId=" << frame.frameId << std::endl;
        return false;
    }

    bytesUsed_ += static_cast<size_t>(requiredBytes);
    return true;
}

bool SenderSpool::removeFrame(uint64_t frameId) {
    const std::string path = filePathFor(frameId);
    if (!std::filesystem::exists(path)) {
        return true;
    }

    const std::uintmax_t removedSize = std::filesystem::file_size(path);
    std::filesystem::remove(path);
    bytesUsed_ = std::max<size_t>(0, bytesUsed_ - static_cast<size_t>(removedSize));
    return true;
}

bool SenderSpool::hasFrame(uint64_t frameId) const {
    return std::filesystem::exists(filePathFor(frameId));
}

std::vector<RawFrame> SenderSpool::loadPendingFrames() const {
    std::vector<RawFrame> frames;
    ensureRoot();

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(rootDir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".spool") {
            paths.push_back(entry.path());
        }
    }

    std::sort(paths.begin(), paths.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return parseFrameIdFromFileName(a.filename().string()) <
               parseFrameIdFromFileName(b.filename().string());
    });

    for (const auto& path : paths) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            continue;
        }

        SpoolHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || header.magic != 0x5350484Cu || header.version != 1) {
            std::cerr << "[SenderSpool] invalid spool metadata: " << path << std::endl;
            continue;
        }

        std::vector<uint8_t> payload(header.totalBytes);
        in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!in || static_cast<uint64_t>(payload.size()) != header.totalBytes) {
            std::cerr << "[SenderSpool] incomplete payload: " << path << std::endl;
            continue;
        }

        auto buffer = std::shared_ptr<uint8_t[]>(new uint8_t[header.totalBytes]);
        std::memcpy(buffer.get(), payload.data(), header.totalBytes);

        frames.emplace_back(
            header.frameId,
            header.rows,
            header.cols,
            header.type,
            header.elemSize,
            header.totalBytes,
            buffer
        );
    }

    return frames;
}

bool SenderSpool::isEmpty() const {
    return std::filesystem::is_empty(rootDir_);
}

size_t SenderSpool::bytesUsed() const {
    return bytesUsed_;
}

void SenderSpool::setMaxSpoolBytes(uint64_t maxBytes) {
    maxSpoolBytes_ = maxBytes;
}

bool SenderSpool::canPersist(uint64_t requiredBytes) const {
    if (maxSpoolBytes_ == 0) {
        return true;
    }
    return (bytesUsed_ + static_cast<size_t>(requiredBytes)) <= static_cast<size_t>(maxSpoolBytes_);
}

void SenderSpool::ensureRoot() const {
    std::filesystem::create_directories(rootDir_);
}

std::string SenderSpool::filePathFor(uint64_t frameId) const {
    return (std::filesystem::path(rootDir_) / ("frame_" + std::to_string(frameId) + ".spool")).string();
}

uint64_t SenderSpool::parseFrameIdFromFileName(const std::string& filename) {
    const std::string prefix = "frame_";
    const std::string suffix = ".spool";
    if (filename.rfind(prefix, 0) != 0) {
        return 0;
    }
    if (filename.size() <= prefix.size() + suffix.size()) {
        return 0;
    }
    const std::string body = filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
    try {
        return static_cast<uint64_t>(std::stoull(body));
    } catch (...) {
        return 0;
    }
}
