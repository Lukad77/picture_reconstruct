#include <filesystem>
#include <iostream>
#include <vector>

#include <opencv2/core.hpp>

#include "SenderSpool.h"

int main() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "sender_spool_resume_test";
    std::filesystem::remove_all(root);

    SenderSpool spool(root.string());

    auto buffer = std::shared_ptr<uint8_t[]>(new uint8_t[16]);
    for (int i = 0; i < 16; ++i) {
        buffer[i] = static_cast<uint8_t>(i + 7);
    }

    RawFrame frame(
        42,
        4,
        4,
        CV_8UC1,
        1,
        16,
        buffer
    );

    if (!spool.persist(frame)) {
        std::cerr << "persist failed" << std::endl;
        return 1;
    }

    if (!spool.hasFrame(42)) {
        std::cerr << "frame not persisted" << std::endl;
        return 2;
    }

    auto pending = spool.loadPendingFrames();
    if (pending.size() != 1 || pending[0].frameId != 42) {
        std::cerr << "resume load mismatch" << std::endl;
        return 3;
    }

    if (!spool.removeFrame(42)) {
        std::cerr << "remove failed" << std::endl;
        return 4;
    }

    std::cout << "sender spool resume ok" << std::endl;
    std::filesystem::remove_all(root);
    return 0;
}
