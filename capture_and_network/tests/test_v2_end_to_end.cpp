#include "V2Transfer.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "picture_reconstruct_v2_e2e";
    fs::remove_all(root);
    std::atomic<int> delivered{0};
    std::atomic<bool> finished{false};
    v2transfer::Receiver receiver(19227, root / "receiver");
    receiver.setFrameHandler([&](const v2transfer::Frame&) { ++delivered; });
    receiver.setFinishHandler([&](uint64_t task) { finished = task == 1; });
    std::thread server([&] { receiver.serveUntilFinished(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    v2transfer::Sender sender("127.0.0.1", 19227, root / "sender");
    v2transfer::Frame f;
    f.frameSeq = 1; f.rows = 512; f.cols = 512; f.elemSize = 1;
    f.stageX = 12.5; f.stageY = 34.75;
    f.pixels.resize(512 * 512);
    for (size_t i = 0; i < f.pixels.size(); ++i) f.pixels[i] = static_cast<uint8_t>(i & 0xff);
    receiver.dropNextAckForTest();
    bool ok = sender.send(f, 5) && sender.pendingCount() == 0;
    for (int i = 0; i < 100 && delivered.load() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ok = ok && delivered == 1;
    v2transfer::Frame loaded;
    ok = ok && v2transfer::loadFrame(root / "receiver" / "1" / "1.frame", loaded) &&
         loaded.pixels == f.pixels && loaded.stageX == f.stageX && loaded.stageY == f.stageY;
    f.frameSeq = 2; f.frameIndex = 1; f.pixels[0] = 99;
    receiver.disconnectNextFrameForTest();
    const bool secondSent = sender.send(f, 5);
    for (int i = 0; i < 100 && delivered.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ok = ok && secondSent && sender.pendingCount() == 0 && delivered == 2;
    const bool secondLoaded = v2transfer::loadFrame(root / "receiver" / "1" / "2.frame", loaded);
    ok = ok && secondLoaded && loaded.pixels == f.pixels;
    receiver.dropNextFinishAckForTest();
    const bool finishOk = sender.finish(5);
    ok = ok && finishOk;
    server.join();
    ok = ok && finished && fs::exists(root / "receiver" / "completed" / "1.done") &&
         !fs::exists(root / "receiver" / "1");
    fs::remove_all(root);
    if (!ok) {
        std::cerr << "V2 end-to-end failed: " << sender.lastError() << " receiver: " << receiver.lastError() << "\n";
        return 1;
    }
    std::cout << "V2 durable ACK, lost ACK resume, mid-frame disconnect and finish replay OK\n";
    return 0;
}
