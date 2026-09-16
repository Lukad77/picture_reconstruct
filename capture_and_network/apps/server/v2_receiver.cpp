#include "V2Transfer.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: v2_receiver <port> <receiver-spool-dir>\n";
        return 2;
    }
    v2transfer::Receiver receiver(static_cast<uint16_t>(std::atoi(argv[1])), argv[2]);
    receiver.setFrameHandler([](const v2transfer::Frame& f) {
        std::cout << "durable frame " << f.frameSeq << " " << f.rows << "x" << f.cols << "\n";
    });
    if (!receiver.serveForever()) {
        std::cerr << receiver.lastError() << "\n";
        return 1;
    }
    return 0;
}
