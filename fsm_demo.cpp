#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ============================================================
// 线程安全队列
// ============================================================

template <typename T>
class ThreadSafeQueue {
public:
    void push(const T& value) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(value);
        }
        cv_.notify_one();
    }

    void push(T&& value) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(std::move(value));
        }
        cv_.notify_one();
    }

    T wait_and_pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty()) {
            throw std::runtime_error("queue stopped");
        }
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    bool wait_and_pop_for(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || stopped_; })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

// ============================================================
// 基础数据结构
// ============================================================

struct Position {
    double x = 0.0;
    double y = 0.0;
};

struct Segment {
    int id = 0;
    Position target;
};

enum class FsmState {
    Idle,
    TaskLoaded,
    MoveToPoint,
    WaitPlatformArrived,
    OpenShutterAndCapture,
    WaitCaptureDone,
    CloseShutter,
    SegmentFinished,
    TaskFinished,
    Error
};

enum class CommandType {
    MovePlatform,
    OpenShutter,
    CloseShutter,
    CaptureImage,
    Stop
};

enum class EventType {
    PlatformArrived,
    PlatformMoveFailed,
    ShutterOpened,
    ShutterClosed,
    ShutterFailed,
    CaptureDone,
    CaptureFailed,
    Timeout,
    Stopped
};

struct Command {
    CommandType type;
    int segment_id = -1;
    Position pos{};
};

struct Event {
    EventType type;
    int segment_id = -1;
    std::string message;
};

// ============================================================
// 工作线程：平台 / 快门 / 相机
// 实际工程中这里会接设备SDK、串口、TCP、采集卡等
// ============================================================

class PlatformWorker {
public:
    PlatformWorker(ThreadSafeQueue<Command>& cmd_q, ThreadSafeQueue<Event>& evt_q)
        : cmd_q_(cmd_q), evt_q_(evt_q) {}

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        try {
            while (true) {
                Command cmd = cmd_q_.wait_and_pop();
                if (cmd.type == CommandType::Stop) {
                    evt_q_.push(Event{EventType::Stopped, cmd.segment_id, "Platform stopped"});
                    break;
                }

                if (cmd.type == CommandType::MovePlatform) {
                    std::cout << "[Platform] Moving to (" << cmd.pos.x << ", " << cmd.pos.y
                              << ") for segment " << cmd.segment_id << "\n";

                    // 模拟运动耗时
                    std::this_thread::sleep_for(80ms);

                    // 实际工程中这里应读取平台到位反馈
                    evt_q_.push(Event{EventType::PlatformArrived, cmd.segment_id, "Platform arrived"});
                }
            }
        } catch (const std::exception& e) {
            evt_q_.push(Event{EventType::PlatformMoveFailed, -1, e.what()});
        }
    }

    ThreadSafeQueue<Command>& cmd_q_;
    ThreadSafeQueue<Event>& evt_q_;
    std::thread worker_;
};

class ShutterWorker {
public:
    ShutterWorker(ThreadSafeQueue<Command>& cmd_q, ThreadSafeQueue<Event>& evt_q)
        : cmd_q_(cmd_q), evt_q_(evt_q) {}

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        try {
            while (true) {
                Command cmd = cmd_q_.wait_and_pop();
                if (cmd.type == CommandType::Stop) {
                    evt_q_.push(Event{EventType::Stopped, cmd.segment_id, "Shutter stopped"});
                    break;
                }

                if (cmd.type == CommandType::OpenShutter) {
                    std::cout << "[Shutter] Opening for segment " << cmd.segment_id << "\n";
                    std::this_thread::sleep_for(5ms); // 模拟硬件响应
                    evt_q_.push(Event{EventType::ShutterOpened, cmd.segment_id, "Shutter opened"});
                } else if (cmd.type == CommandType::CloseShutter) {
                    std::cout << "[Shutter] Closing for segment " << cmd.segment_id << "\n";
                    std::this_thread::sleep_for(5ms);
                    evt_q_.push(Event{EventType::ShutterClosed, cmd.segment_id, "Shutter closed"});
                }
            }
        } catch (const std::exception& e) {
            evt_q_.push(Event{EventType::ShutterFailed, -1, e.what()});
        }
    }

    ThreadSafeQueue<Command>& cmd_q_;
    ThreadSafeQueue<Event>& evt_q_;
    std::thread worker_;
};

class CameraWorker {
public:
    CameraWorker(ThreadSafeQueue<Command>& cmd_q, ThreadSafeQueue<Event>& evt_q)
        : cmd_q_(cmd_q), evt_q_(evt_q) {}

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        try {
            while (true) {
                Command cmd = cmd_q_.wait_and_pop();
                if (cmd.type == CommandType::Stop) {
                    evt_q_.push(Event{EventType::Stopped, cmd.segment_id, "Camera stopped"});
                    break;
                }

                if (cmd.type == CommandType::CaptureImage) {
                    std::cout << "[Camera] Capturing image for segment " << cmd.segment_id << "\n";

                    // 模拟曝光+取帧耗时
                    std::this_thread::sleep_for(20ms);

                    // 实际工程中这里可把图像frame写入图像队列给网络线程
                    evt_q_.push(Event{EventType::CaptureDone, cmd.segment_id, "Capture done"});
                }
            }
        } catch (const std::exception& e) {
            evt_q_.push(Event{EventType::CaptureFailed, -1, e.what()});
        }
    }

    ThreadSafeQueue<Command>& cmd_q_;
    ThreadSafeQueue<Event>& evt_q_;
    std::thread worker_;
};

// ============================================================
// FSM控制器
// ============================================================

class ProcessController {
public:
    ProcessController(std::vector<Segment> segments)
        : segments_(std::move(segments)),
          platform_worker_(platform_cmd_q_, event_q_),
          shutter_worker_(shutter_cmd_q_, event_q_),
          camera_worker_(camera_cmd_q_, event_q_) {}

    void start() {
        platform_worker_.start();
        shutter_worker_.start();
        camera_worker_.start();

        fsm_thread_ = std::thread([this] { fsmLoop(); });
    }

    void join() {
        if (fsm_thread_.joinable()) {
            fsm_thread_.join();
        }
        platform_worker_.join();
        shutter_worker_.join();
        camera_worker_.join();
    }

private:
    void fsmLoop() {
        std::cout << "[FSM] Start\n";
        state_ = FsmState::TaskLoaded;

        while (running_) {
            switch (state_) {
                case FsmState::TaskLoaded:
                    if (segments_.empty()) {
                        state_ = FsmState::TaskFinished;
                    } else {
                        current_index_ = 0;
                        state_ = FsmState::MoveToPoint;
                    }
                    break;

                case FsmState::MoveToPoint: {
                    const Segment& seg = segments_[current_index_];
                    std::cout << "[FSM] MoveToPoint: segment " << seg.id << "\n";
                    platform_cmd_q_.push(Command{CommandType::MovePlatform, seg.id, seg.target});
                    state_ = FsmState::WaitPlatformArrived;
                    break;
                }

                case FsmState::WaitPlatformArrived: {
                    const Segment& seg = segments_[current_index_];
                    Event evt;
                    if (!event_q_.wait_and_pop_for(evt, 1000ms)) {
                        handleError("WaitPlatformArrived timeout");
                        break;
                    }

                    if (evt.segment_id != seg.id) {
                        handleUnexpectedEvent(evt, "WaitPlatformArrived");
                        break;
                    }

                    if (evt.type == EventType::PlatformArrived) {
                        std::cout << "[FSM] Platform arrived at segment " << seg.id << "\n";
                        state_ = FsmState::OpenShutterAndCapture;
                    } else {
                        handleError("Platform move failed or unexpected event");
                    }
                    break;
                }

                case FsmState::OpenShutterAndCapture: {
                    const Segment& seg = segments_[current_index_];

                    // 关键逻辑：
                    // 平台到位后，同时下发“开快门”和“拍照”命令
                    // 工程上是同一个状态点发两个命令，而不是串行等待开完再拍
                    std::cout << "[FSM] Open shutter and trigger capture for segment " << seg.id << "\n";

                    shutter_cmd_q_.push(Command{CommandType::OpenShutter, seg.id, {}});
                    camera_cmd_q_.push(Command{CommandType::CaptureImage, seg.id, {}});

                    // 这里进入等待拍照完成状态
                    // 如果你特别希望“确认快门已开再算拍照合法”，可加额外门控逻辑
                    state_ = FsmState::WaitCaptureDone;
                    break;
                }

                case FsmState::WaitCaptureDone: {
                    const Segment& seg = segments_[current_index_];
                    bool capture_done = false;
                    bool shutter_opened = false;

                    // 等待两个关键信号：
                    // 1) 快门已打开
                    // 2) 拍照已完成
                    //
                    // 你的业务规则最终以“拍完照后关闭快门”为准，
                    // 所以这里至少要保证 CaptureDone 收到。
                    //
                    // 更稳妥的做法是同时等到 ShutterOpened + CaptureDone。
                    auto deadline = std::chrono::steady_clock::now() + 1000ms;

                    while (std::chrono::steady_clock::now() < deadline) {
                        Event evt;
                        if (!event_q_.wait_and_pop_for(evt, 50ms)) {
                            continue;
                        }

                        if (evt.segment_id != seg.id) {
                            handleUnexpectedEvent(evt, "WaitCaptureDone");
                            continue;
                        }

                        if (evt.type == EventType::ShutterOpened) {
                            shutter_opened = true;
                            std::cout << "[FSM] Shutter opened for segment " << seg.id << "\n";
                        } else if (evt.type == EventType::CaptureDone) {
                            capture_done = true;
                            std::cout << "[FSM] Capture done for segment " << seg.id << "\n";
                        } else if (evt.type == EventType::CaptureFailed ||
                                   evt.type == EventType::ShutterFailed) {
                            handleError("Shutter/Capture failed");
                            break;
                        }

                        if (capture_done && shutter_opened) {
                            state_ = FsmState::CloseShutter;
                            break;
                        }
                    }

                    if (state_ == FsmState::WaitCaptureDone) {
                        handleError("WaitCaptureDone timeout");
                    }
                    break;
                }

                case FsmState::CloseShutter: {
                    const Segment& seg = segments_[current_index_];
                    std::cout << "[FSM] Close shutter for segment " << seg.id << "\n";
                    shutter_cmd_q_.push(Command{CommandType::CloseShutter, seg.id, {}});
                    state_ = FsmState::SegmentFinished;
                    break;
                }

                case FsmState::SegmentFinished: {
                    const Segment& seg = segments_[current_index_];
                    Event evt;
                    if (!event_q_.wait_and_pop_for(evt, 1000ms)) {
                        handleError("WaitShutterClosed timeout");
                        break;
                    }

                    if (evt.segment_id != seg.id) {
                        handleUnexpectedEvent(evt, "SegmentFinished");
                        break;
                    }

                    if (evt.type == EventType::ShutterClosed) {
                        std::cout << "[FSM] Segment " << seg.id << " finished\n";

                        ++current_index_;
                        if (current_index_ >= segments_.size()) {
                            state_ = FsmState::TaskFinished;
                        } else {
                            state_ = FsmState::MoveToPoint;
                        }
                    } else {
                        handleError("Expected ShutterClosed");
                    }
                    break;
                }

                case FsmState::TaskFinished:
                    std::cout << "[FSM] Task finished\n";
                    stopAllWorkers();
                    running_ = false;
                    break;

                case FsmState::Error:
                    std::cout << "[FSM] Error state, stopping all workers\n";
                    stopAllWorkers();
                    running_ = false;
                    break;

                default:
                    handleError("Unknown FSM state");
                    break;
            }
        }
    }

    void stopAllWorkers() {
        platform_cmd_q_.push(Command{CommandType::Stop});
        shutter_cmd_q_.push(Command{CommandType::Stop});
        camera_cmd_q_.push(Command{CommandType::Stop});

        platform_cmd_q_.stop();
        shutter_cmd_q_.stop();
        camera_cmd_q_.stop();
        event_q_.stop();
    }

    void handleError(const std::string& msg) {
        std::cerr << "[FSM][ERROR] " << msg << "\n";
        state_ = FsmState::Error;
    }

    void handleUnexpectedEvent(const Event& evt, const std::string& where) {
        std::cerr << "[FSM][WARN] Unexpected event in " << where
                  << ", seg=" << evt.segment_id
                  << ", msg=" << evt.message << "\n";
    }

private:
    std::vector<Segment> segments_;
    size_t current_index_ = 0;

    std::atomic<bool> running_{true};
    FsmState state_ = FsmState::Idle;

    ThreadSafeQueue<Command> platform_cmd_q_;
    ThreadSafeQueue<Command> shutter_cmd_q_;
    ThreadSafeQueue<Command> camera_cmd_q_;
    ThreadSafeQueue<Event> event_q_;

    PlatformWorker platform_worker_;
    ShutterWorker shutter_worker_;
    CameraWorker camera_worker_;

    std::thread fsm_thread_;
};

// ============================================================
// main
// ============================================================

int main() {
    std::vector<Segment> segments = {
        {1, {10.0, 5.0}},
        {2, {20.0, 5.0}},
        {3, {30.0, 5.0}}
    };

    ProcessController controller(segments);
    controller.start();
    controller.join();

    return 0;
}