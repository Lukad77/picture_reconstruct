#ifndef SENDER_SPOOL_H
#define SENDER_SPOOL_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "RawFrame.h"

class SenderSpool {
public:
    explicit SenderSpool(std::string rootDir = "sender_spool");
    ~SenderSpool();

    bool persist(const RawFrame& frame);
    bool removeFrame(uint64_t frameId);
    bool hasFrame(uint64_t frameId) const;
    std::vector<RawFrame> loadPendingFrames() const;
    bool isEmpty() const;
    size_t bytesUsed() const;

    void setMaxSpoolBytes(uint64_t maxBytes);
    bool canPersist(uint64_t requiredBytes) const;

private:
    std::string rootDir_;
    uint64_t maxSpoolBytes_;
    mutable size_t bytesUsed_;

    void ensureRoot() const;
    std::string filePathFor(uint64_t frameId) const;
    static uint64_t parseFrameIdFromFileName(const std::string& filename);
};

#endif
