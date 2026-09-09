#ifndef RAW_FRAME_PROTOCOL_H
#define RAW_FRAME_PROTOCOL_H

#include <cstdint>
#include <vector>
#include <cstddef>

struct RawFrameHeader {
    uint64_t frameId;
    uint32_t rows;
    uint32_t cols;
    uint32_t type;
    uint32_t elemSize;
    uint64_t totalBytes;
    uint32_t fragmentSize;
    uint32_t fragmentCount;
};

struct RawChunkHeader {
    uint64_t frameId;
    uint32_t fragmentIndex;
    uint32_t payloadSize;
    uint64_t offset;
};

class RawFrameProtocol {
public:
    static constexpr uint32_t FRAME_MAGIC = 0x52465731; // "RFW1"
    static constexpr uint32_t CHUNK_MAGIC = 0x43484B31; // "CHK1"

    static constexpr uint16_t VERSION = 1;

    static constexpr size_t FRAME_HEADER_SIZE = 48;
    static constexpr size_t CHUNK_HEADER_SIZE = 28;

public:
    static std::vector<uint8_t> buildFrameHeader(const RawFrameHeader& header);
    static std::vector<uint8_t> buildChunkHeader(const RawChunkHeader& header);

    static bool parseFrameHeader(const uint8_t* data, size_t size, RawFrameHeader& header);
    static bool parseChunkHeader(const uint8_t* data, size_t size, RawChunkHeader& header);

private:
    static void appendUint16(std::vector<uint8_t>& buffer, uint16_t value);
    static void appendUint32(std::vector<uint8_t>& buffer, uint32_t value);
    static void appendUint64(std::vector<uint8_t>& buffer, uint64_t value);

    static bool readUint16(const uint8_t* data, size_t size, size_t& offset, uint16_t& value);
    static bool readUint32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value);
    static bool readUint64(const uint8_t* data, size_t size, size_t& offset, uint64_t& value);
};

#endif