#include <filesystem>
#include <iostream>
#include <string>

#include "ProtocolV2.h"

int main() {
    const std::string stateDir = "/tmp/processing_resume_state_test";
    std::filesystem::remove_all(stateDir);

    protocolv2::ProcessingStateMachine machine(stateDir);
    if (!machine.beginLine(1001, 7, 2)) {
        std::cerr << "beginLine failed" << std::endl;
        return 1;
    }

    if (!machine.markFrameSent(10)) {
        std::cerr << "markFrameSent failed" << std::endl;
        return 2;
    }

    if (!machine.checkpoint(7, 10, false)) {
        std::cerr << "checkpoint failed" << std::endl;
        return 3;
    }

    protocolv2::ProcessingResumeState loaded{};
    {
        protocolv2::ProcessingStateMachine reloaded(stateDir);
        if (!reloaded.loadState()) {
            std::cerr << "loadState failed" << std::endl;
            return 4;
        }
        loaded = reloaded.state();
    }

    if (loaded.taskId != 1001 || loaded.lineId != 7 || loaded.attemptId != 2 || loaded.frameIndex != 1 || loaded.lastAckedFrameSeq != 7) {
        std::cerr << "state reload mismatch" << std::endl;
        return 5;
    }

    if (!machine.resumeFromDisk(1001, 7, 2)) {
        std::cerr << "resumeFromDisk should accept matching attempt" << std::endl;
        return 6;
    }

    if (machine.resumeFromDisk(1001, 7, 1)) {
        std::cerr << "resumeFromDisk should reject stale attempt" << std::endl;
        return 7;
    }

    if (!machine.beginLine(1001, 7, 3)) {
        std::cerr << "new attempt start failed" << std::endl;
        return 8;
    }

    if (machine.state().attemptId != 3 || machine.state().frameIndex != 0) {
        std::cerr << "new attempt did not reset state" << std::endl;
        return 9;
    }

    std::cout << "processing resume ok" << std::endl;
    std::filesystem::remove_all(stateDir);
    return 0;
}
