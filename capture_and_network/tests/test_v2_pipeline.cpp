#include "V2Transfer.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "picture_reconstruct_v2_pipeline";
    fs::remove_all(root);
    std::atomic<int> delivered{0};
    std::atomic<bool> finalized{false};
    v2transfer::ReceiverOptions receiverOptions;
    receiverOptions.expectedTaskId = 7;
    v2transfer::Receiver receiver(19228, root / "receiver", receiverOptions);
    receiver.setFrameHandler([&](const v2transfer::Frame&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        ++delivered;
    });
    receiver.setFinishHandler([&](uint64_t task) { finalized = task == 7 && delivered == 8; });
    std::thread server([&] { receiver.serveUntilFinished(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    v2transfer::SenderOptions senderOptions;
    senderOptions.maxInFlightFrames = 4;
    v2transfer::Sender sender("127.0.0.1", 19228, root / "sender", 7, senderOptions);
    bool ok = sender.resume();
    for (uint64_t seq = 1; seq <= 8 && ok; ++seq) {
        v2transfer::Frame frame;
        frame.taskId = 7; frame.frameSeq = seq; frame.frameIndex = static_cast<uint32_t>(seq - 1);
        frame.rows = 512; frame.cols = 512; frame.elemSize = 1; frame.pixels.assign(512 * 512, static_cast<uint8_t>(seq));
        ok = sender.submit(frame);
    }
    ok = ok && sender.flush();
    const int deliveredAtFlush = delivered.load();
    ok = ok && deliveredAtFlush < 8;
    ok = sender.finish() && ok;
    server.join();

    const auto senderStats = sender.stats();
    const auto receiverStats = receiver.stats();
    ok = ok && delivered == 8 && finalized && senderStats.frames == 8 && senderStats.connections == 1 &&
         receiverStats.frames == 8 && !fs::exists(root / "receiver" / "7") &&
         fs::exists(root / "receiver" / "completed" / "7.done");
    fs::remove_all(root);
    if (!ok) {
        std::cerr << "pipeline test failed delivered_at_flush=" << deliveredAtFlush
                  << " sender_connections=" << senderStats.connections << "\n";
        return 1;
    }
    std::cout << "V2 long connection, windowed send, async reconstruction and finish barrier OK\n";
    return 0;
}
