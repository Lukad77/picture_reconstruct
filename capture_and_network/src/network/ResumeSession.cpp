#include "ResumeSession.h"

#include <iostream>
#include <vector>

#include "ProtocolV2.h"

ResumeSession::ResumeSession(
    const std::string& host,
    int port,
    std::string spoolRoot,
    uint64_t taskId
)
    : host_(host),
      port_(port),
      taskId_(taskId),
      state_(SessionState::Disconnected),
      highestDurableFrameSeq_(0),
      tcpClient_(host_, port_),
      frameSender_(tcpClient_),
      spool_(std::move(spoolRoot)) {}

std::vector<RawFrame> ResumeSession::pendingFrames() const {
    return spool_.loadPendingFrames();
}

bool ResumeSession::replayPendingFrames() {
    return replayPendingFrames(frameSender_);
}

bool ResumeSession::replayPendingFrames(RawFrameSender& sender) {
    auto frames = spool_.loadPendingFrames();
    if (frames.empty()) {
        return true;
    }

    std::cout << "[ResumeSession] replaying " << frames.size() << " pending frames" << std::endl;

    for (const auto& frame : frames) {
        if (!sender.sendFrame(frame)) {
            std::cerr << "[ResumeSession] failed to replay frameId=" << frame.frameId << std::endl;
            return false;
        }
    }

    return true;
}

bool ResumeSession::start() {
    return reconnectAndResume();
}

bool ResumeSession::sendFrame(const RawFrame& frame) {
    if (!spool_.persist(frame)) {
        std::cerr << "[ResumeSession] persist failed for frameId=" << frame.frameId << std::endl;
        return false;
    }

    if (!tcpClient_.isConnected()) {
        if (!reconnectAndResume()) {
            return false;
        }
    }

    if (!frameSender_.sendFrame(frame)) {
        std::cerr << "[ResumeSession] send failed for frameId=" << frame.frameId << std::endl;
        tcpClient_.closeConnection();
        state_ = SessionState::Failed;
        return false;
    }

    if (!spool_.removeFrame(frame.frameId)) {
        std::cerr << "[ResumeSession] failed to clear spool for frameId=" << frame.frameId << std::endl;
    }

    return true;
}

bool ResumeSession::reconnectAndResume() {
    state_ = SessionState::Connecting;

    if (!tcpClient_.connectToServer()) {
        state_ = SessionState::Disconnected;
        return false;
    }

    state_ = SessionState::Handshaking;
    auto hello = protocolv2::makeHelloMessage(taskId_);
    if (!tcpClient_.sendAll(hello.data(), hello.size())) {
        std::cerr << "[ResumeSession] hello send failed" << std::endl;
        tcpClient_.closeConnection();
        state_ = SessionState::Failed;
        return false;
    }

    if (!requestResume()) {
        std::cerr << "[ResumeSession] resume handshake failed" << std::endl;
        tcpClient_.closeConnection();
        state_ = SessionState::Failed;
        return false;
    }

    state_ = SessionState::Recovering;
    if (!replayPendingFrames()) {
        state_ = SessionState::Failed;
        return false;
    }

    state_ = SessionState::Ready;
    return true;
}

bool ResumeSession::requestResume() {
    auto request = protocolv2::makeResumeRequestMessage(taskId_);
    if (!tcpClient_.sendAll(request.data(), request.size())) {
        std::cerr << "[ResumeSession] resume request send failed" << std::endl;
        return false;
    }

    std::vector<uint8_t> replyBuffer(12 + 16);
    if (!tcpClient_.recvAll(replyBuffer.data(), replyBuffer.size())) {
        std::cerr << "[ResumeSession] resume reply receive failed" << std::endl;
        return false;
    }

    protocolv2::ResumeReply reply{};
    if (!protocolv2::parseResumeReply(replyBuffer.data(), replyBuffer.size(), reply)) {
        std::cerr << "[ResumeSession] invalid resume reply" << std::endl;
        return false;
    }

    highestDurableFrameSeq_ = reply.highestDurableFrameSeq;
    std::cout << "[ResumeSession] resume reply: taskId=" << reply.taskId
              << ", highestDurableFrameSeq=" << highestDurableFrameSeq_ << std::endl;

    for (const auto& frame : spool_.loadPendingFrames()) {
        if (frame.frameId <= highestDurableFrameSeq_) {
            spool_.removeFrame(frame.frameId);
        }
    }

    return true;
}

const SenderSpool& ResumeSession::spool() const {
    return spool_;
}

SenderSpool& ResumeSession::spool() {
    return spool_;
}

SessionState ResumeSession::state() const {
    return state_;
}

std::string ResumeSession::stateString() const {
    switch (state_) {
        case SessionState::Disconnected: return "Disconnected";
        case SessionState::Connecting: return "Connecting";
        case SessionState::Handshaking: return "Handshaking";
        case SessionState::Recovering: return "Recovering";
        case SessionState::Ready: return "Ready";
        case SessionState::Failed: return "Failed";
    }
    return "Unknown";
}

uint64_t ResumeSession::highestDurableFrameSeq() const {
    return highestDurableFrameSeq_;
}
