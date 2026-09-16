#include "V2Transfer.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: v2_sender <host> <port> <sender-spool-dir> <frame-count>\n";
        return 2;
    }
    v2transfer::Sender sender(argv[1], static_cast<uint16_t>(std::atoi(argv[2])), argv[3]);
    if (!sender.resume()) std::cerr << "startup resume: " << sender.lastError() << "\n";
    int count = std::atoi(argv[4]);
    for (int i = 1; i <= count; ++i) {
        v2transfer::Frame frame;
        frame.frameSeq = static_cast<uint64_t>(i);
        frame.frameIndex = static_cast<uint32_t>(i - 1);
        frame.rows = 256; frame.cols = 256; frame.pixelType = 0; frame.elemSize = 1;
        frame.pixels.resize(256 * 256);
        for (size_t n = 0; n < frame.pixels.size(); ++n)
            frame.pixels[n] = static_cast<uint8_t>((n + i) & 0xff);
        if (!sender.send(frame)) {
            std::cerr << "frame " << i << ": " << sender.lastError() << "\n";
            return 1;
        }
        std::cout << "ACK frame " << i << "\n";
    }
    return 0;
}
