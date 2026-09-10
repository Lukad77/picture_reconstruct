#ifndef RESUME_SESSION_H
#define RESUME_SESSION_H

#include <string>
#include <vector>

#include "RawFrame.h"
#include "RawFrameSender.h"
#include "SenderSpool.h"
#include "TcpClient.h"

enum class SessionState {
    Disconnected,
    Connecting,
    Handshaking,
    Recovering,
    Ready,
    Failed
};

class ResumeSession {
public:
    ResumeSession(
        const std::string& host,
        int port,
        std::string spoolRoot = "sender_spool",
        uint64_t taskId = 1
    );

    std::vector<RawFrame> pendingFrames() const;
    bool replayPendingFrames();
    bool replayPendingFrames(RawFrameSender& sender);

    bool start();
    bool sendFrame(const RawFrame& frame);
    bool reconnectAndResume();
    bool requestResume();

    const SenderSpool& spool() const;
    SenderSpool& spool();
    SessionState state() const;
    std::string stateString() const;
    uint64_t highestDurableFrameSeq() const;

private:
    std::string host_;
    int port_;
    uint64_t taskId_;
    SessionState state_;
    uint64_t highestDurableFrameSeq_;

    TcpClient tcpClient_;
    RawFrameSender frameSender_;
    SenderSpool spool_;
};

#endif
