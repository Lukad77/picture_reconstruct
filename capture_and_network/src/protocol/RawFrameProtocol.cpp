#include "RawFrameProtocol.h"

std::vector<uint8_t> RawFrameProtocol::buildFrameHeader(const RawFrameHeader& header) {
    std::vector<uint8_t> buffer;
    buffer.reserve(FRAME_HEADER_SIZE);

    appendUint32(buffer, FRAME_MAGIC);
    appendUint16(buffer, VERSION);
    appendUint16(buffer, static_cast<uint16_t>(FRAME_HEADER_SIZE));

    appendUint64(buffer, header.frameId);
    appendUint32(buffer, header.rows);
    appendUint32(buffer, header.cols);
    appendUint32(buffer, header.type);
    appendUint32(buffer, header.elemSize);
    appendUint64(buffer, header.totalBytes);
    appendUint32(buffer, header.fragmentSize);
    appendUint32(buffer, header.fragmentCount);

    return buffer;
}

std::vector<uint8_t> RawFrameProtocol::buildChunkHeader(const RawChunkHeader& header) {
    std::vector<uint8_t> buffer;
    buffer.reserve(CHUNK_HEADER_SIZE);

    appendUint32(buffer, CHUNK_MAGIC);
    appendUint64(buffer, header.frameId);
    appendUint32(buffer, header.fragmentIndex);
    appendUint32(buffer, header.payloadSize);
    appendUint64(buffer, header.offset);

    return buffer;
}

bool RawFrameProtocol::parseFrameHeader(
    const uint8_t* data,
    size_t size,
    RawFrameHeader& header
) {
    if (size < FRAME_HEADER_SIZE) {
        return false;
    }

    size_t offset = 0;

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerSize = 0;

    if (!readUint32(data, size, offset, magic)) {
        return false;
    }

    if (!readUint16(data, size, offset, version)) {
        return false;
    }

    if (!readUint16(data, size, offset, headerSize)) {
        return false;
    }

    if (magic != FRAME_MAGIC) {
        return false;
    }

    if (version != VERSION) {
        return false;
    }

    if (headerSize != FRAME_HEADER_SIZE) {
        return false;
    }

    if (!readUint64(data, size, offset, header.frameId)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.rows)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.cols)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.type)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.elemSize)) {
        return false;
    }

    if (!readUint64(data, size, offset, header.totalBytes)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.fragmentSize)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.fragmentCount)) {
        return false;
    }

    return true;
}

bool RawFrameProtocol::parseChunkHeader(
    const uint8_t* data,
    size_t size,
    RawChunkHeader& header
) {
    if (size < CHUNK_HEADER_SIZE) {
        return false;
    }

    size_t offset = 0;

    uint32_t magic = 0;

    if (!readUint32(data, size, offset, magic)) {
        return false;
    }

    if (magic != CHUNK_MAGIC) {
        return false;
    }

    if (!readUint64(data, size, offset, header.frameId)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.fragmentIndex)) {
        return false;
    }

    if (!readUint32(data, size, offset, header.payloadSize)) {
        return false;
    }

    if (!readUint64(data, size, offset, header.offset)) {
        return false;
    }

    return true;
}

void RawFrameProtocol::appendUint16(std::vector<uint8_t>& buffer, uint16_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

void RawFrameProtocol::appendUint32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

void RawFrameProtocol::appendUint64(std::vector<uint8_t>& buffer, uint64_t value) {
    buffer.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

bool RawFrameProtocol::readUint16(
    const uint8_t* data,
    size_t size,
    size_t& offset,
    uint16_t& value
) {
    if (offset + 2 > size) {
        return false;
    }

    value = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) |
        static_cast<uint16_t>(data[offset + 1])
    );

    offset += 2;
    return true;
}

bool RawFrameProtocol::readUint32(
    const uint8_t* data,
    size_t size,
    size_t& offset,
    uint32_t& value
) {
    if (offset + 4 > size) {
        return false;
    }

    value =
        (static_cast<uint32_t>(data[offset]) << 24) |
        (static_cast<uint32_t>(data[offset + 1]) << 16) |
        (static_cast<uint32_t>(data[offset + 2]) << 8) |
        static_cast<uint32_t>(data[offset + 3]);

    offset += 4;
    return true;
}

bool RawFrameProtocol::readUint64(
    const uint8_t* data,
    size_t size,
    size_t& offset,
    uint64_t& value
) {
    if (offset + 8 > size) {
        return false;
    }

    value =
        (static_cast<uint64_t>(data[offset]) << 56) |
        (static_cast<uint64_t>(data[offset + 1]) << 48) |
        (static_cast<uint64_t>(data[offset + 2]) << 40) |
        (static_cast<uint64_t>(data[offset + 3]) << 32) |
        (static_cast<uint64_t>(data[offset + 4]) << 24) |
        (static_cast<uint64_t>(data[offset + 5]) << 16) |
        (static_cast<uint64_t>(data[offset + 6]) << 8) |
        static_cast<uint64_t>(data[offset + 7]);

    offset += 8;
    return true;
}