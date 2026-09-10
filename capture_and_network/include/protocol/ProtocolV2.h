#ifndef PROTOCOL_V2_H
#define PROTOCOL_V2_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace protocolv2 {

constexpr uint32_t kMagic = 0x50525632u; // PRV2
constexpr uint16_t kVersion = 2;

enum class MessageType : uint16_t {
    Hello = 1,
    ResumeRequest = 2,
    ResumeReply = 3,
    LineBegin = 10,
    LineCommit = 11,
    LineAbort = 12,
    FrameBegin = 20,
    FrameChunk = 21,
    Ack = 30,
    Nack = 31,
    TaskCheckpoint = 41,
    TaskFinish = 40
};

struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    MessageType type;
    uint32_t bodyLength;
};

struct FrameMetaV2 {
    uint64_t taskId;
    uint32_t lineId;
    uint32_t attemptId;
    uint32_t frameIndex;
    uint64_t frameSeq;
    double stageX;
    double stageY;
    uint32_t rows;
    uint32_t cols;
    uint32_t pixelType;
    uint32_t elemSize;
    uint64_t dataLength;
    uint32_t fragmentSize;
    uint32_t fragmentCount;
    uint32_t crc32;
};

struct ResumeReply {
    uint64_t taskId;
    uint64_t highestDurableFrameSeq;
};

struct AckMessage {
    uint64_t taskId;
    uint64_t frameSeq;
};

struct NackMessage {
    uint64_t taskId;
    uint64_t frameSeq;
    uint32_t reason;
};

struct TaskCheckpoint {
    uint64_t taskId = 0;
    uint32_t lineId = 0;
    uint32_t attemptId = 0;
    uint64_t lastFrameSeq = 0;
    uint64_t lastAckedFrameSeq = 0;
    bool finalized = false;
};

struct AttemptState {
    uint64_t taskId = 0;
    uint32_t lineId = 0;
    uint32_t attemptId = 0;
    uint64_t frameIndex = 0;
    uint64_t lastAckedFrameSeq = 0;
    bool aborted = false;
    bool committed = false;

    static AttemptState beginNew(uint64_t taskId_, uint32_t lineId_, uint32_t attemptId_, uint64_t frameIndex_ = 0) {
        AttemptState state;
        state.taskId = taskId_;
        state.lineId = lineId_;
        state.attemptId = attemptId_;
        state.frameIndex = frameIndex_;
        state.lastAckedFrameSeq = 0;
        state.aborted = false;
        state.committed = false;
        return state;
    }

    bool isStaleFor(uint32_t expectedLineId, uint32_t expectedAttemptId) const {
        return lineId != expectedLineId || attemptId != expectedAttemptId;
    }

    bool isValidFor(const TaskCheckpoint& checkpoint) const {
        return !aborted && taskId == checkpoint.taskId && lineId == checkpoint.lineId && attemptId == checkpoint.attemptId;
    }
};

enum class ProcessingPhase : uint8_t {
    Idle = 0,
    Running = 1,
    Aborted = 2,
    Committed = 3
};

struct ProcessingResumeState {
    uint64_t taskId = 0;
    uint32_t lineId = 0;
    uint32_t attemptId = 0;
    uint64_t frameIndex = 0;
    uint64_t lastFrameSeq = 0;
    uint64_t lastAckedFrameSeq = 0;
    ProcessingPhase phase = ProcessingPhase::Idle;
    bool aborted = false;
    bool committed = false;

    static ProcessingResumeState beginNew(uint64_t taskId_, uint32_t lineId_, uint32_t attemptId_) {
        ProcessingResumeState state;
        state.taskId = taskId_;
        state.lineId = lineId_;
        state.attemptId = attemptId_;
        state.frameIndex = 0;
        state.lastFrameSeq = 0;
        state.lastAckedFrameSeq = 0;
        state.phase = ProcessingPhase::Running;
        state.aborted = false;
        state.committed = false;
        return state;
    }

    bool matches(uint32_t expectedLineId, uint32_t expectedAttemptId) const {
        return lineId == expectedLineId && attemptId == expectedAttemptId;
    }

    bool isStaleFor(uint32_t expectedLineId, uint32_t expectedAttemptId) const {
        return !matches(expectedLineId, expectedAttemptId);
    }
};

inline bool shouldRejectOldAttempt(const TaskCheckpoint& checkpoint, uint32_t lineId, uint32_t attemptId) {
    return checkpoint.lineId != lineId || checkpoint.attemptId != attemptId || checkpoint.finalized;
}

class ProcessingStateMachine {
public:
    explicit ProcessingStateMachine(std::string stateDir = "processing_resume_state")
        : stateDir_(std::move(stateDir)) {
        std::filesystem::create_directories(stateDir_);
        state_ = ProcessingResumeState::beginNew(0, 0, 0);
    }

    bool beginLine(uint64_t taskId, uint32_t lineId, uint32_t attemptId) {
        state_ = ProcessingResumeState::beginNew(taskId, lineId, attemptId);
        return saveState();
    }

    bool markFrameSent(uint64_t frameSeq) {
        state_.frameIndex += 1;
        state_.lastFrameSeq = frameSeq;
        state_.phase = ProcessingPhase::Running;
        state_.aborted = false;
        return saveState();
    }

    bool checkpoint(uint64_t lastAckedFrameSeq, uint64_t lastFrameSeq = 0, bool finalized = false) {
        state_.lastAckedFrameSeq = lastAckedFrameSeq;
        if (lastFrameSeq != 0) {
            state_.lastFrameSeq = lastFrameSeq;
        }
        state_.phase = finalized ? ProcessingPhase::Committed : ProcessingPhase::Running;
        state_.committed = finalized;
        state_.aborted = false;
        return saveState();
    }

    bool abortCurrentAttempt() {
        state_.phase = ProcessingPhase::Aborted;
        state_.aborted = true;
        state_.committed = false;
        return saveState();
    }

    bool commitCurrentAttempt() {
        state_.phase = ProcessingPhase::Committed;
        state_.committed = true;
        state_.aborted = false;
        return saveState();
    }

    bool resumeFromDisk(uint64_t expectedTaskId, uint32_t expectedLineId, uint32_t expectedAttemptId) {
        if (!loadState()) {
            return false;
        }

        if (state_.taskId != expectedTaskId || state_.isStaleFor(expectedLineId, expectedAttemptId)) {
            return false;
        }

        state_.phase = ProcessingPhase::Running;
        state_.aborted = false;
        return saveState();
    }

    bool saveState() const {
        const std::filesystem::path path = std::filesystem::path(stateDir_) / "processing_state.txt";
        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }

        out << "taskId=" << state_.taskId << "\n";
        out << "lineId=" << state_.lineId << "\n";
        out << "attemptId=" << state_.attemptId << "\n";
        out << "frameIndex=" << state_.frameIndex << "\n";
        out << "lastFrameSeq=" << state_.lastFrameSeq << "\n";
        out << "lastAckedFrameSeq=" << state_.lastAckedFrameSeq << "\n";
        out << "phase=" << static_cast<int>(state_.phase) << "\n";
        out << "aborted=" << (state_.aborted ? 1 : 0) << "\n";
        out << "committed=" << (state_.committed ? 1 : 0) << "\n";
        return static_cast<bool>(out);
    }

    bool loadState() {
        const std::filesystem::path path = std::filesystem::path(stateDir_) / "processing_state.txt";
        std::ifstream in(path);
        if (!in.is_open()) {
            return false;
        }

        std::string line;
        std::unordered_map<std::string, std::string> values;
        while (std::getline(in, line)) {
            const auto pos = line.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            values[line.substr(0, pos)] = line.substr(pos + 1);
        }

        auto readU64 = [&](const std::string& key, uint64_t& target) {
            auto it = values.find(key);
            if (it == values.end()) {
                return false;
            }
            target = static_cast<uint64_t>(std::stoull(it->second));
            return true;
        };

        auto readU32 = [&](const std::string& key, uint32_t& target) {
            auto it = values.find(key);
            if (it == values.end()) {
                return false;
            }
            target = static_cast<uint32_t>(std::stoul(it->second));
            return true;
        };

        uint64_t taskId = 0;
        uint32_t lineId = 0;
        uint32_t attemptId = 0;
        uint64_t frameIndex = 0;
        uint64_t lastFrameSeq = 0;
        uint64_t lastAckedFrameSeq = 0;
        int phase = 0;
        int aborted = 0;
        int committed = 0;

        if (!readU64("taskId", taskId) || !readU32("lineId", lineId) || !readU32("attemptId", attemptId) ||
            !readU64("frameIndex", frameIndex) || !readU64("lastFrameSeq", lastFrameSeq) ||
            !readU64("lastAckedFrameSeq", lastAckedFrameSeq)) {
            return false;
        }

        auto it = values.find("phase");
        if (it == values.end()) {
            return false;
        }
        phase = std::stoi(it->second);

        it = values.find("aborted");
        if (it == values.end()) {
            return false;
        }
        aborted = std::stoi(it->second);

        it = values.find("committed");
        if (it == values.end()) {
            return false;
        }
        committed = std::stoi(it->second);

        state_.taskId = taskId;
        state_.lineId = lineId;
        state_.attemptId = attemptId;
        state_.frameIndex = frameIndex;
        state_.lastFrameSeq = lastFrameSeq;
        state_.lastAckedFrameSeq = lastAckedFrameSeq;
        state_.phase = static_cast<ProcessingPhase>(phase);
        state_.aborted = aborted != 0;
        state_.committed = committed != 0;
        return true;
    }

    const ProcessingResumeState& state() const {
        return state_;
    }

    std::string stateDir() const {
        return stateDir_;
    }

private:
    std::string stateDir_;
    ProcessingResumeState state_;
};

inline void appendUint16(std::vector<uint8_t>& buffer, uint16_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void appendUint32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void appendUint64(std::vector<uint8_t>& buffer, uint64_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline uint32_t encodeUint32BE(uint32_t value) {
    return ((value & 0xFFu) << 24) |
           ((value & 0xFF00u) << 8) |
           ((value & 0xFF0000u) >> 8) |
           ((value & 0xFF000000u) >> 24);
}

inline uint32_t decodeUint32BE(uint32_t value) {
    return encodeUint32BE(value);
}

inline uint64_t encodeUint64BE(uint64_t value) {
    uint32_t high = static_cast<uint32_t>(value >> 32);
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFFULL);
    return (static_cast<uint64_t>(encodeUint32BE(high)) << 32) |
           static_cast<uint64_t>(encodeUint32BE(low));
}

inline uint64_t decodeUint64BE(uint64_t value) {
    return encodeUint64BE(value);
}

inline std::vector<uint8_t> makeMessageHeader(MessageType type, uint32_t bodyLength) {
    std::vector<uint8_t> buffer;
    buffer.reserve(12);
    appendUint32(buffer, kMagic);
    appendUint16(buffer, kVersion);
    appendUint16(buffer, static_cast<uint16_t>(type));
    appendUint32(buffer, bodyLength);
    return buffer;
}

inline std::vector<uint8_t> makeHelloMessage(uint64_t taskId) {
    std::vector<uint8_t> body;
    appendUint64(body, taskId);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::Hello, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline std::vector<uint8_t> makeResumeRequestMessage(uint64_t taskId) {
    std::vector<uint8_t> body;
    appendUint64(body, taskId);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::ResumeRequest, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline std::vector<uint8_t> makeResumeReplyMessage(uint64_t taskId, uint64_t highestDurableFrameSeq) {
    std::vector<uint8_t> body;
    appendUint64(body, taskId);
    appendUint64(body, highestDurableFrameSeq);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::ResumeReply, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline std::vector<uint8_t> makeLineBeginMessage(uint64_t taskId, uint32_t lineId, uint32_t attemptId, uint64_t frameIndex) {
    std::vector<uint8_t> body;
    appendUint64(body, taskId);
    appendUint32(body, lineId);
    appendUint32(body, attemptId);
    appendUint64(body, frameIndex);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::LineBegin, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline std::vector<uint8_t> makeLineCommitMessage(uint64_t taskId, uint32_t lineId, uint32_t attemptId, uint64_t lastFrameSeq) {
    std::vector<uint8_t> body;
    appendUint64(body, taskId);
    appendUint32(body, lineId);
    appendUint32(body, attemptId);
    appendUint64(body, lastFrameSeq);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::LineCommit, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline std::vector<uint8_t> makeLineAbortMessage(uint64_t taskId, uint32_t lineId, uint32_t attemptId) {
    std::vector<uint8_t> body;
    appendUint64(body, taskId);
    appendUint32(body, lineId);
    appendUint32(body, attemptId);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::LineAbort, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline std::vector<uint8_t> makeTaskCheckpointMessage(const TaskCheckpoint& checkpoint) {
    std::vector<uint8_t> body;
    appendUint64(body, checkpoint.taskId);
    appendUint32(body, checkpoint.lineId);
    appendUint32(body, checkpoint.attemptId);
    appendUint64(body, checkpoint.lastFrameSeq);
    appendUint64(body, checkpoint.lastAckedFrameSeq);
    appendUint32(body, checkpoint.finalized ? 1u : 0u);
    std::vector<uint8_t> msg = makeMessageHeader(MessageType::TaskCheckpoint, static_cast<uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

inline bool parseTaskCheckpoint(const uint8_t* data, size_t size, TaskCheckpoint& checkpoint) {
    if (size < 12 + 36) {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t typeRaw = 0;
    uint32_t bodyLength = 0;

    if (offset + 4 > size) return false;
    magic = static_cast<uint32_t>(data[offset]) << 24 |
            static_cast<uint32_t>(data[offset + 1]) << 16 |
            static_cast<uint32_t>(data[offset + 2]) << 8 |
            static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    if (offset + 2 > size) return false;
    version = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]));
    offset += 2;
    if (offset + 2 > size) return false;
    typeRaw = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]));
    offset += 2;
    if (offset + 4 > size) return false;
    bodyLength = static_cast<uint32_t>(data[offset]) << 24 |
                 static_cast<uint32_t>(data[offset + 1]) << 16 |
                 static_cast<uint32_t>(data[offset + 2]) << 8 |
                 static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    if (magic != kMagic || version != kVersion || static_cast<MessageType>(typeRaw) != MessageType::TaskCheckpoint || bodyLength != 36) {
        return false;
    }

    uint64_t taskId = 0;
    uint32_t lineId = 0;
    uint32_t attemptId = 0;
    uint64_t lastFrameSeq = 0;
    uint64_t lastAckedFrameSeq = 0;
    uint32_t finalized = 0;

    for (int i = 7; i >= 0; --i) {
        taskId = (taskId << 8) | data[offset + (7 - i)];
    }
    offset += 8;
    lineId = static_cast<uint32_t>(data[offset]) << 24 | static_cast<uint32_t>(data[offset + 1]) << 16 |
             static_cast<uint32_t>(data[offset + 2]) << 8 | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    attemptId = static_cast<uint32_t>(data[offset]) << 24 | static_cast<uint32_t>(data[offset + 1]) << 16 |
                static_cast<uint32_t>(data[offset + 2]) << 8 | static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    for (int i = 7; i >= 0; --i) {
        lastFrameSeq = (lastFrameSeq << 8) | data[offset + (7 - i)];
    }
    offset += 8;
    for (int i = 7; i >= 0; --i) {
        lastAckedFrameSeq = (lastAckedFrameSeq << 8) | data[offset + (7 - i)];
    }
    offset += 8;
    finalized = static_cast<uint32_t>(data[offset]) << 24 |
                static_cast<uint32_t>(data[offset + 1]) << 16 |
                static_cast<uint32_t>(data[offset + 2]) << 8 |
                static_cast<uint32_t>(data[offset + 3]);

    checkpoint.taskId = taskId;
    checkpoint.lineId = lineId;
    checkpoint.attemptId = attemptId;
    checkpoint.lastFrameSeq = lastFrameSeq;
    checkpoint.lastAckedFrameSeq = lastAckedFrameSeq;
    checkpoint.finalized = finalized != 0;
    return true;
}

inline bool parseResumeReply(const uint8_t* data, size_t size, ResumeReply& reply) {
    if (size < 12 + 16) {
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t typeRaw = 0;
    uint32_t bodyLength = 0;

    if (offset + 4 > size) return false;
    magic = static_cast<uint32_t>(data[offset]) << 24 |
            static_cast<uint32_t>(data[offset + 1]) << 16 |
            static_cast<uint32_t>(data[offset + 2]) << 8 |
            static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    if (offset + 2 > size) return false;
    version = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]));
    offset += 2;
    if (offset + 2 > size) return false;
    typeRaw = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]));
    offset += 2;
    if (offset + 4 > size) return false;
    bodyLength = static_cast<uint32_t>(data[offset]) << 24 |
                 static_cast<uint32_t>(data[offset + 1]) << 16 |
                 static_cast<uint32_t>(data[offset + 2]) << 8 |
                 static_cast<uint32_t>(data[offset + 3]);
    offset += 4;

    if (magic != kMagic || version != kVersion || static_cast<MessageType>(typeRaw) != MessageType::ResumeReply || bodyLength != 16) {
        return false;
    }

    uint64_t taskId = 0;
    uint64_t highest = 0;
    for (int i = 7; i >= 0; --i) {
        taskId = (taskId << 8) | data[offset + (7 - i)];
    }
    offset += 8;
    for (int i = 7; i >= 0; --i) {
        highest = (highest << 8) | data[offset + (7 - i)];
    }
    offset += 8;

    reply.taskId = taskId;
    reply.highestDurableFrameSeq = highest;
    return true;
}

} // namespace protocolv2

#endif
