#include "V2Transfer.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
double run(bool asynchronous, uint16_t port, const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    fs::remove_all(root);
    v2transfer::Receiver receiver(port, root / "receiver");
    receiver.setFrameHandler([](const v2transfer::Frame&) {});
    std::thread server([&] { receiver.serveUntilFinished(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v2transfer::Sender sender("127.0.0.1", port, root / "sender");
    if (!sender.resume()) throw std::runtime_error(sender.lastError());
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t seq = 1; seq <= 32; ++seq) {
        v2transfer::Frame frame;
        frame.frameSeq = seq; frame.frameIndex = static_cast<uint32_t>(seq - 1);
        frame.rows = 1024; frame.cols = 1024; frame.elemSize = 1;
        frame.pixels.assign(1024 * 1024, static_cast<uint8_t>(seq));
        const bool ok = asynchronous ? sender.submit(frame) : sender.send(frame);
        if (!ok) throw std::runtime_error(sender.lastError());
    }
    if (!sender.finish()) throw std::runtime_error(sender.lastError());
    server.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto stats = sender.stats();
    std::cout << (asynchronous ? "windowed" : "synchronous") << ": " << 32.0 / seconds
              << " frames/s, " << seconds << " s, connections=" << stats.connections
              << ", peak_queue_bytes=" << stats.peakQueuedBytes << '\n';
    fs::remove_all(root);
    return seconds;
}
}

int main() {
    namespace fs = std::filesystem;
    try {
        const fs::path base = fs::temp_directory_path() / "picture_reconstruct_v2_benchmark";
        const double synchronous = run(false, 19229, base / "sync");
        const double windowed = run(true, 19230, base / "async");
        std::cout << "relative speedup=" << synchronous / windowed << "x\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
