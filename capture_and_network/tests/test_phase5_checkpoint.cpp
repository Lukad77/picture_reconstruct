#include <iostream>
#include <vector>

#include "ProtocolV2.h"

int main() {
    protocolv2::TaskCheckpoint checkpoint{1, 7, 2, 13, 11, false};

    auto begin = protocolv2::makeLineBeginMessage(1, 7, 2, 0);
    auto commit = protocolv2::makeLineCommitMessage(1, 7, 2, 13);
    auto abort = protocolv2::makeLineAbortMessage(1, 7, 2);
    auto persisted = protocolv2::makeTaskCheckpointMessage(checkpoint);

    if (begin.empty() || commit.empty() || abort.empty() || persisted.empty()) {
        std::cerr << "phase5 messages are empty" << std::endl;
        return 1;
    }

    protocolv2::AttemptState state = protocolv2::AttemptState::beginNew(1, 7, 2, 0);
    if (state.lineId != 7 || state.attemptId != 2 || state.frameIndex != 0) {
        std::cerr << "attempt state reset is wrong" << std::endl;
        return 2;
    }

    if (protocolv2::shouldRejectOldAttempt(checkpoint, 7, 2)) {
        std::cerr << "matching attempt should remain valid" << std::endl;
        return 3;
    }

    if (!protocolv2::shouldRejectOldAttempt(checkpoint, 7, 1)) {
        std::cerr << "stale attempt should be rejected" << std::endl;
        return 4;
    }

    protocolv2::TaskCheckpoint parsed{};
    if (!protocolv2::parseTaskCheckpoint(persisted.data(), persisted.size(), parsed)) {
        std::cerr << "checkpoint parse failed" << std::endl;
        return 5;
    }

    if (parsed.taskId != 1 || parsed.lineId != 7 || parsed.attemptId != 2 || parsed.lastAckedFrameSeq != 11) {
        std::cerr << "checkpoint payload mismatch" << std::endl;
        return 6;
    }

    std::cout << "phase5 checkpoint ok" << std::endl;
    return 0;
}
