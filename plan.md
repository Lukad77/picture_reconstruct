# picture_reconstruct 可靠传输与断点恢复升级计划

> 目标：基于当前 `Lukad77/picture_reconstruct` 仓库，完成一版可用于 **C++ / 后端岗位简历展示** 的可靠采集、持久化、TCP 断点续传与任务恢复实现。
>
> 实现原则：**不引入 HTTP / gRPC / Kafka / Redis / 数据库 / DDS**。保留 C++17 + Linux POSIX Socket + 自定义二进制协议主线。
>
> 本计划是给 coding agent 直接执行的工程任务书。除非遇到现有代码无法满足的编译/接口问题，否则不要扩大需求范围。

---

# 0. 已确认业务需求

以下需求视为冻结，不再由 coding agent 自行更改。

## 0.1 扫描模型

一次完整扫描任务：

```text
Task
 └── Line
      └── Attempt
           └── Frame
                └── Chunk
```

定义：

- `Task`：一次完整扫描任务。
- `Line`：平移台沿 X 方向完成一次完整扫描，之后 `Y += step`。
- `Attempt`：某一 Line 的一次执行尝试。
- `Frame`：平移台移动到一个位置、停止、采集的一帧图像。
- `Chunk`：单个 Frame 在一次 TCP 连接中的传输分片。

确定关系：

```text
1 Line = N Frames
1 position = 1 Frame
```

采集方式：

```text
move stage
→ stop
→ capture one frame
→ move stage
→ stop
→ capture one frame
→ ...
```

网络发送的是 **相机 Raw Frame 原始像素数据**，不是 JPEG / PNG / TIFF 编码 blob。

---

## 0.2 恢复粒度

任务恢复：

```text
按 Line 恢复
```

如果 Line 538 扫描中途程序异常：

```text
completedLine = 537
```

重启后：

```text
Line 538 整行重新扫描
```

不尝试从 Line 538 内某一个 Frame 的机械位置继续。

网络恢复：

```text
按 Frame 恢复
```

已经 ACK 的 Frame 不重传，未 ACK 的 Frame 重连后重新发送。

Chunk：

```text
不做跨 TCP 连接 Chunk 级续传
```

如果 Frame 10073 传输到中间 Chunk 时连接断开：

```text
重连后从 Frame 10073 的第 0 个 Chunk 重新发送整个 Frame
```

---

## 0.3 TCP 断线策略

TCP 断线时：

```text
扫描继续
↓
新 Frame 持久化到 SenderSpool
↓
网络线程持续重连
↓
重连完成后补传未 ACK Frame
```

不能因为 TCP 断线立即停止扫描。

但磁盘不是无限资源，因此必须支持 Spool 容量限制和 Backpressure：

```text
Spool 未达到硬限制
→ 扫描继续

Spool 达到硬限制
→ 请求扫描侧暂停/阻塞
→ 网络恢复并释放空间后继续
```

第一版不要求复杂的高低水位算法，但必须有 `maxSpoolBytes` 硬限制。

---

## 0.4 进程崩溃策略

已扫描但尚未 ACK 的 Frame 必须从磁盘恢复：

```text
scan frame
→ persist sender spool
→ eligible to send
```

程序不能依赖纯内存队列保存未 ACK 数据。

例如：

```text
扫描完成到 Line 1000
网络只确认到某个更早的 Frame
进程 crash
```

重启后：

- 已完成 Line 对应但未 ACK 的 Frame 从 SenderSpool 恢复发送；
- 不重新扫描已经 committed 的 Line；
- 未完成 Line 的旧 Attempt 整体废弃。

---

## 0.5 Attempt 语义

未完成 Line 的上一 Attempt：

```text
全部标记为 Aborted / Invalid
不参与最终重建
```

例如：

```text
Line 538 Attempt 1
扫描到 FrameIndex 72
进程 crash

恢复后：

Attempt 1 = Aborted
Attempt 2 = Running
FrameIndex 从 0 重新开始
```

接收端即使已经收到 Attempt 1 的部分 Frame，也不能把它们加入最终重建。

---

## 0.6 原始数据保存

Receiver：

```text
收到 Frame
→ CRC 校验
→ 持久化 ReceiverSpool / Raw Archive
→ ACK
→ 进入重建流水线
```

接收端需要同时：

1. 保存 Raw Frame；
2. 将有效 Frame 送入 CUDA / OpenCV 重建流水线。

Sender：

```text
收到 Durable ACK 后
→ 删除对应 SenderSpool Frame
```

发送端不长期保存已经 ACK 的原始 Frame。

---

# 1. 当前仓库基线与必须先修的问题

coding agent 开始实现功能前，先完成本节。

当前仓库主线：

```text
capture_and_network/
reconstruct_picture/
```

采集侧已经存在：

```text
FrameBufferPool
CameraCapture
BlockingQueue
TcpClient
RawFrameSender
RawFrameProtocol
CameraNetworkSender
```

现有 `RawFrameProtocol`：

```text
RFW1
CHK1
VERSION = 1
```

并且已经具有：

```text
Frame
→ 64 KiB Chunk
→ TcpClient::sendAll()
```

这些实现应尽量复用，不要推翻。

---

## 1.1 修复 RawFrame 类型错误

当前 `RawFrame` 中存在：

```cpp
std::shared_ptr<uint16_t[]> buffer;
```

但构造函数接收：

```cpp
std::shared_ptr<uint8_t[]>
```

`data()` 又返回：

```cpp
const uint8_t*
```

统一成：

```cpp
std::shared_ptr<uint8_t[]> buffer;
```

要求：

- `FrameBufferPool` 继续以字节数组管理网络/采集 Buffer；
- Pixel 类型只由 `type / elemSize / rows / cols` 描述；
- 不让 Buffer 的 C++ 元素类型绑定实际像素位宽。

验收：

```text
capture_and_network 可正常编译
RawFrame 构造、复制/移动、data() 类型一致
```

---

## 1.2 保留 TcpClient 为纯 Socket 层

当前已有：

```cpp
connectToServer()
closeConnection()
isConnected()
sendAll()
```

增加：

```cpp
bool recvAll(void* data, size_t size);
```

可选增加：

```cpp
void setSendTimeout(...);
void setRecvTimeout(...);
```

不要把以下逻辑塞入 `TcpClient`：

```text
TaskID
ACK
CRC
Resume
Spool
Line 状态
```

目标依赖关系：

```text
ReliableSession
      ↓
Protocol
      ↓
TcpClient
      ↓
POSIX Socket
```

---

## 1.3 移除重建侧 CAM1 私有协议

当前 `reconstruct_picture/src/main.cpp` 存在独立协议：

```cpp
#pragma pack(push, 1)
struct NetHeader {
    char magic[4];   // CAM1
    int frameId;
    int imageSize;
    double stageX;
    double stageY;
};
#pragma pack(pop)
```

并通过：

```cpp
cv::imdecode(...)
```

解码网络数据。

升级完成后：

- 正式重建服务端不得再使用 `CAM1`；
- 不再假定网络传入压缩图像；
- 统一使用 Protocol V2；
- Raw Frame 按 `rows / cols / pixelType / elemSize` 恢复为 `cv::Mat`；
- 旧 `mock_sender.py` 可以保留为 legacy，也可以改造成 Protocol V2 mock，但正式流程不能依赖它。

---

## 1.4 修复重建侧输入尺寸与输出尺寸耦合

当前 worker 使用：

```cpp
int width = outputWidth;
int height = outputHeight;
```

作为输入 Raw Frame 的 CUDA Buffer 尺寸。

这是错误语义。

必须区分：

```text
inputFrameWidth / inputFrameHeight
```

和：

```text
reconstructionOutputWidth / reconstructionOutputHeight
```

处理 Raw Frame 时使用：

```cpp
frame.cols
frame.rows
```

或者在任务初始化时校验相机固定尺寸，并据真实 Raw Frame 尺寸分配 GPU Buffer。

不能使用 `512x512 reconstruction output` 推导相机输入图大小。

第一版允许假定任务内所有 Raw Frame 尺寸一致，但第一次收到 Frame 时必须确定/校验尺寸。

---

# 2. 总体目标架构

最终发送端：

```text
ScanTask / Stage Controller
          │
          ▼
     CameraCapture
          │
          ▼
     FrameBufferPool
          │
          ▼
      RawFrame
          │
          ▼
     SenderSpool
      persist first
          │
          ▼
   Ready Frame Queue
          │
          ▼
 ReliableFrameSender
          │
          ▼
   ReliableSession
          │
          ▼
    Protocol V2
          │
          ▼
      TcpClient
```

最终接收端：

```text
      TcpServer
          │
          ▼
     ClientSession
          │
          ▼
   ProtocolReceiver
          │
          ▼
    FrameAssembler
          │
          ▼
       CRC32
          │
          ▼
    ReceiverSpool
          │
        ┌─┴────────────┐
        │              │
        ▼              ▼
       ACK      ReconstructionQueue
                       │
                       ▼
                 CUDA / OpenCV
                       │
                       ▼
                 Reconstruction
```

任务状态：

```text
Created
  ↓
Running
  ↓
Completed

Running
  ↓ crash/restart
Recovering
  ↓
Running
```

网络状态：

```text
Disconnected
   ↓
Connecting
   ↓
Handshaking
   ↓
Resuming
   ↓
Transferring
   ↓
Disconnected
```

任务状态和网络状态必须解耦。

---

# 3. 标识模型

第一版使用以下字段。

```cpp
using TaskId = uint64_t;
using FrameSeq = uint64_t;

struct FrameIdentity {
    TaskId taskId;
    uint32_t lineId;
    uint32_t attemptId;
    uint32_t frameIndex;
    FrameSeq frameSeq;
};
```

语义：

### `taskId`

一次扫描任务唯一 ID。

第一版可由：

```text
timestamp + random/process component
```

生成。

只要求同一部署环境中不会重复。

---

### `lineId`

扫描 Line 序号。

建议：

```text
从 0 开始
```

整个代码保持一致，不混用 0-based / 1-based。

---

### `attemptId`

某一个 Line 的扫描尝试。

例如：

```text
Line 538 Attempt 0
Line 538 Attempt 1
```

每次 Line 因 crash 被重新执行：

```text
attemptId++
```

---

### `frameIndex`

当前 Attempt 内的位置序号：

```text
0, 1, 2, ...
```

一个位置严格对应一个 Frame。

---

### `frameSeq`

整个 Task 内网络发送使用的全局单调序号。

作用：

- SenderSpool 文件索引；
- ACK；
- Resume；
- 幂等；
- 日志定位；
- 网络排序。

要求：

```text
同一 Task 内不复用 frameSeq
```

即使 Attempt 作废，旧 FrameSeq 也不能重新使用。

---

# 4. Protocol V2

不要直接 `send(struct)`。

所有整数采用显式网络字节序序列化。

禁止依赖：

```text
#pragma pack
编译器 padding
本机 endian
```

对于 double：

- 使用 `memcpy` 到 `uint64_t`；
- 再按 uint64 网络序序列化；
- parse 时逆操作。

---

## 4.1 CommonHeader

```cpp
enum class MessageType : uint16_t {
    Hello         = 1,
    ResumeRequest = 2,
    ResumeReply   = 3,

    LineBegin     = 10,
    LineCommit    = 11,
    LineAbort     = 12,

    FrameBegin    = 20,
    FrameChunk    = 21,

    Ack           = 30,
    Nack          = 31,

    TaskFinish    = 40
};
```

```cpp
struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    MessageType type;
    uint32_t bodyLength;
};
```

建议：

```text
MAGIC = "PRV2" 对应固定 uint32_t
VERSION = 2
```

每条消息：

```text
MessageHeader
+
bodyLength bytes body
```

Server 必须：

```text
recvAll(CommonHeader)
→ validate magic/version/bodyLength
→ recvAll(body)
→ dispatch by MessageType
```

必须设置最大 `bodyLength` 防止异常包导致超大内存分配。

---

## 4.2 Hello

```cpp
struct HelloMessage {
    uint64_t taskId;
};
```

连接建立后首先发送。

Server 返回可使用 ACK 或单独 HelloReply；第一版推荐直接进入 Resume 流程，不额外增加消息。

---

## 4.3 ResumeRequest

```cpp
struct ResumeRequest {
    uint64_t taskId;
};
```

---

## 4.4 ResumeReply

不要只返回一个 `lastFrameSeq`，因为可能存在 Attempt 作废和非连续状态。

第一版定义：

```cpp
struct ResumeReply {
    uint64_t taskId;
    uint64_t highestDurableFrameSeq;
};
```

同时 Sender 在重连时 **以本地 SenderSpool 为事实数据集合**：

```text
遍历本地仍存在的合法 Frame
如果 frameSeq <= highestDurableFrameSeq：
    可删除（Server 已可靠持久化）
否则：
    重传
```

约束：

第一版采用“接收 durable frameSeq 单调连续”的策略。

Receiver 只推进：

```text
highestDurableFrameSeq
```

到连续已持久化的位置。

如果未来引入窗口/乱序，再升级为 bitmap / ranges；本版本不实现。

---

# 5. Frame 协议

## 5.1 FrameBegin

```cpp
struct FrameMeta {
    uint64_t taskId;

    uint32_t lineId;
    uint32_t attemptId;
    uint32_t frameIndex;
    uint64_t frameSeq;

    double stageX;
    double stageY;

    uint32_t rows;
    uint32_t cols;
    uint32_t pixelType;
    uint32_t elemSize;

    uint64_t dataLength;

    uint32_t fragmentSize;
    uint32_t fragmentCount;

    uint32_t crc32;
};
```

CRC32：

```text
对完整 Raw Frame payload 计算
不包含 FrameMeta
```

---

## 5.2 FrameChunk

```cpp
struct FrameChunkMeta {
    uint64_t taskId;
    uint64_t frameSeq;

    uint32_t chunkIndex;
    uint32_t payloadSize;
    uint64_t offset;
};
```

Wire：

```text
MessageHeader(type = FrameChunk)
FrameChunkMeta
Raw payload
```

仍可默认：

```text
fragmentSize = 64 * 1024
```

复用现有 RawFrameSender 设计。

---

## 5.3 Chunk 恢复规则

同一连接：

```text
按 Chunk 依次发送
```

如果发送中断：

```text
Frame 状态仍为未 ACK
```

重连后：

```text
重新发送 FrameBegin
重新从 Chunk 0 开始
```

不保存 partial chunk resume 状态。

Receiver 如果存在未完成 partial frame：

```text
连接断开后丢弃临时 assembly 文件/buffer
```

只有完整 CRC 成功的 Frame 才成为 Durable Frame。

---

# 6. ACK / NACK

## 6.1 Durable ACK 定义

```cpp
struct AckMessage {
    uint64_t taskId;
    uint64_t frameSeq;
};
```

ACK 的唯一语义：

```text
完整收到所有 Chunk
+
Frame CRC32 校验通过
+
Raw Frame 已可靠写入 ReceiverSpool
+
Receiver durable progress 已更新
```

此时才能：

```text
send ACK(frameSeq)
```

ACK 不代表：

```text
CUDA 完成
ROI 完成
Reconstruction 完成
```

---

## 6.2 NACK

```cpp
enum class NackReason : uint32_t {
    BadCrc = 1,
    InvalidMeta = 2,
    InvalidChunk = 3,
    InvalidAttempt = 4,
    InternalError = 5
};

struct NackMessage {
    uint64_t taskId;
    uint64_t frameSeq;
    uint32_t reason;
};
```

第一版：

- CRC 错误：NACK，该 Frame 重新完整发送；
- 非法 Meta：NACK；
- Server 内部持久化失败：NACK / 断连接均可，但日志必须明确。

---

# 7. Line 事务语义

将 Line 视为轻量业务事务。

消息：

```cpp
struct LineMessage {
    uint64_t taskId;
    uint32_t lineId;
    uint32_t attemptId;
};
```

生命周期：

```text
LINE_BEGIN
↓
Frame 0
Frame 1
...
Frame N
↓
LINE_COMMIT
```

如果 Line 未完成：

```text
LINE_ABORT
```

---

## 7.1 LineBegin

开始新的 Attempt 时发送。

如果网络当前断开：

- 扫描不能等待网络；
- LineBegin 状态必须本地持久化；
- 重连后在发送属于该 Attempt 的 Frame 前补发 LineBegin。

---

## 7.2 LineCommit

Line 机械扫描完成后：

```text
所有 Frame 已至少成功写入 SenderSpool
+
平移台完成该 X Line
+
Y += step 已完成
```

更新本地 TaskCheckpoint，并记录该 Attempt 为 committed。

网络断开不能阻止本地 commit。

重连后必须把 LineCommit 控制状态同步给 Receiver。

---

## 7.3 LineAbort

进程重启时：

```text
Checkpoint.completedLine = N
```

如果发现 Spool / Task metadata 中存在：

```text
Line N+1 的未完成 Attempt
```

则：

```text
旧 Attempt = Aborted
```

其 Frame：

- Sender 不再发送；
- 本地 SenderSpool 删除；
- 如果 Receiver 已经收到部分 Frame，则通过 LineAbort 让 Receiver 标记 Invalidated；
- Invalidated Frame 不进入最终重建；
- Receiver 可删除对应 Raw Frame，第一版建议删除。

---

# 8. TaskCheckpoint

新增：

```cpp
enum class TaskState {
    Created,
    Running,
    Recovering,
    Completed,
    Failed
};

struct TaskCheckpoint {
    uint64_t taskId;
    uint32_t completedLine;

    uint32_t nextLineAttemptId;

    uint64_t nextFrameSeq;

    double nextStageX;
    double nextStageY;

    TaskState state;

    // 必要的 ScanParameters
};
```

根据真实扫描控制代码补充 `ScanParameters`，不要凭空增加业务字段。

---

## 8.1 Atomic Save

不得：

```text
直接 truncate checkpoint.dat 再写
```

实现：

```text
checkpoint.tmp
↓
write all
↓
flush
↓
fsync(fd)
↓
close
↓
rename(checkpoint.tmp, checkpoint.dat)
↓
可选 fsync(parent directory)
```

新增：

```cpp
class TaskCheckpointManager {
public:
    bool load(TaskCheckpoint& out);
    bool saveAtomic(const TaskCheckpoint& checkpoint);
    bool exists() const;
};
```

Checkpoint 格式第一版可以使用：

```text
固定二进制
```

或简单 key-value 文本。

若用文本：

- 必须版本化；
- 必须完整校验字段；
- 不接受半文件默默默认值。

推荐固定二进制 + magic/version。

---

# 9. SenderSpool

新增：

```cpp
class SenderSpool;
```

职责：

1. Frame 在进入网络队列前持久化；
2. 程序重启恢复未 ACK Frame；
3. ACK 后删除；
4. 删除 Aborted Attempt 的 Frame；
5. 统计当前磁盘占用；
6. 达到 `maxSpoolBytes` 时提供 Backpressure。

---

## 9.1 SenderSpool 文件格式

第一版推荐：

```text
sender_spool/
└── task_<taskId>/
    ├── task.meta
    ├── frame_<frameSeq>.spool
    └── ...
```

一个 `.spool`：

```text
SpoolHeader
Raw Frame bytes
```

SpoolHeader 必须包含恢复所需全部字段：

```cpp
struct SpoolFrameHeader {
    uint32_t magic;
    uint16_t version;

    FrameMeta meta;
};
```

不要依赖内存 map 才能解释文件。

---

## 9.2 Persist-first

严格顺序：

```text
Capture
↓
SenderSpool::persist(frame)
↓
persist success
↓
进入 Ready Queue
↓
网络发送
```

如果 persist 失败：

```text
不能把 Frame 当成已采集成功
```

应上报错误并触发扫描暂停/任务失败策略。

第一版可以：

```text
persist failure → stop/pause scan + error
```

---

## 9.3 ACK 删除

收到：

```text
ACK(frameSeq)
```

验证 TaskID 后：

```text
SenderSpool::remove(frameSeq)
```

删除失败：

- 记录 ERROR；
- 不影响 ACK 状态；
- 下次启动时若 Server Resume 表明已 durable，则清理该文件。

因此 Resume 时必须能清理：

```text
frameSeq <= highestDurableFrameSeq
```

的残留文件。

---

## 9.4 Startup recovery

程序启动：

```text
load TaskCheckpoint
↓
scan SenderSpool
↓
validate spool headers + crc
↓
删除 aborted attempt Frame
↓
恢复 committed / currently valid Frame
↓
按 frameSeq 排序
↓
进入待发送集合
```

不要依赖文件系统目录遍历天然顺序。

---

## 9.5 Spool Backpressure

配置：

```cpp
uint64_t maxSpoolBytes;
```

`persist()` 前：

```text
currentBytes + newFrameBytes > maxSpoolBytes
```

则：

```text
返回 SpoolFull
```

上层应：

```text
阻塞/暂停扫描采集
```

直到：

```text
网络 ACK 删除旧 Frame
→ 空间恢复
```

实现建议：

```cpp
condition_variable
```

提供：

```cpp
waitForSpace(requiredBytes)
```

避免 busy loop。

---

# 10. ReceiverSpool

新增：

```cpp
class ReceiverSpool;
```

职责：

1. 接收完整 Frame；
2. CRC 校验；
3. Durable persist；
4. 维护 `highestDurableFrameSeq`；
5. ACK；
6. 向重建队列提供 Raw Frame；
7. Attempt Aborted 时 Invalidated / delete。

---

## 10.1 写入流程

不要直接把未完成 Frame 写成最终文件名。

使用：

```text
frame_<seq>.part
```

流程：

```text
FrameBegin
↓
create .part / assembly buffer
↓
receive chunks
↓
validate total length
↓
CRC32
↓
fsync
↓
rename .part → frame_<seq>.raw
↓
update durable progress
↓
ACK
```

连接断开：

```text
删除当前不完整 .part
```

---

## 10.2 Receiver raw file

建议：

```text
receiver_spool/
└── task_<taskId>/
    ├── state.meta
    ├── line_<lineId>_attempt_<attemptId>/
    │   ├── frame_<frameSeq>.raw
    │   └── ...
```

Raw 文件应包含 Meta 或对应 sidecar。

第一版推荐：

```text
FrameMeta + Raw payload
```

便于进程重启后无需额外 CSV 即可恢复。

---

# 11. 幂等

Receiver 使用：

```text
TaskID + FrameSeq
```

作为网络级唯一键。

如果收到已经 durable 的 Frame：

```text
不重复写盘
不重复进入 reconstruction queue
直接重新 ACK
```

这是处理：

```text
Frame 已写盘
↓
ACK 在网络中丢失
↓
Sender 重发
```

的核心。

---

## 11.1 Attempt 有效性

业务有效性使用：

```text
TaskID + LineID + AttemptID
```

Receiver 必须维护：

```cpp
enum class LineAttemptState {
    Running,
    Committed,
    Aborted
};
```

Frame durable 与 Frame valid 是两个概念：

```text
Durable = 网络/存储成功
Valid   = 所属 Attempt 没有被 Abort
```

只有：

```text
Valid Frame
```

才能进入最终 Reconstruction。

---

# 12. ReliableSession

新增：

```cpp
class ReliableSession;
```

职责：

```text
connect
handshake
resume negotiation
send control message
send frame
wait ack/nack
reconnect loop
```

第一版采用：

```text
Stop-and-Wait Frame ACK
```

即：

```text
send one frame
↓
wait ACK/NACK
↓
next frame
```

明确不实现：

```text
Sliding Window
Cumulative ACK
RTT estimation
Selective ACK
```

这些列为 future work。

---

## 12.1 重连

连接失败：

```text
Disconnected
↓
sleep
↓
connect
↓
Hello
↓
ResumeRequest
↓
ResumeReply
↓
reconcile local spool
↓
Transferring
```

第一版重连间隔可固定：

```text
1 second
```

或简单：

```text
1s → 2s → 4s → max 10s
```

如果实现 exponential backoff，不要同时扩大成 heartbeat 系统。

---

# 13. CameraNetworkSender 改造

当前行为：

```text
TCP 初次连接失败 → start() 失败
sendLoop 检测断线 → break
```

升级后必须改变。

建议拆分为：

```text
capture path
network path
```

Capture：

```text
CameraCapture
↓
RawFrame + Task/Line metadata
↓
SenderSpool
```

Network：

```text
SenderSpool pending frames
↓
ReliableSession
↓
ACK remove
```

网络线程退出条件只能是：

```text
application stop / task finish
```

不能因为一次 TCP 断开直接退出。

---

# 14. RawFrame 元数据扩展

当前 `RawFrame` 只有：

```text
frameId
rows
cols
type
elemSize
totalBytes
buffer
timestamp
```

升级为至少包含：

```cpp
uint64_t taskId;
uint32_t lineId;
uint32_t attemptId;
uint32_t frameIndex;
uint64_t frameSeq;

double stageX;
double stageY;
```

建议：

- 删除或废弃旧 `frameId`；
- 统一使用 `frameSeq`；
- 如果为了兼容旧接口暂时保留 `frameId`，明确它只是 legacy alias，并在 Phase 1 结束后清掉。

---

# 15. Reconstruction 接收层改造

不要继续把全部网络逻辑写在：

```text
reconstruct_picture/src/main.cpp
```

最少拆出：

```text
reconstruct_picture/include/network/
reconstruct_picture/src/network/
```

建议文件：

```text
TcpServer.h/.cpp
ProtocolReceiver.h/.cpp
FrameAssembler.h/.cpp
```

`main.cpp` 只负责：

```text
初始化配置
初始化 Receiver
初始化 Reconstruction pipeline
启动线程
等待退出
```

---

# 16. Raw Frame → cv::Mat

因为网络发送 Raw Frame，不调用 `imdecode()`。

根据：

```text
rows
cols
pixelType
elemSize
```

恢复。

优先方案：

```cpp
cv::Mat view(rows, cols, pixelType, rawBytes);
```

然后根据生命周期：

```text
若 rawBytes 会在 Worker 使用期间保持有效
→ 可零拷贝 view

否则
→ clone / 复制到 pipeline-owned buffer
```

第一版优先保证正确性，不要求为了零拷贝制造复杂生命周期。

必须验证：

```text
dataLength == rows * cols * elemSize
```

对于多通道 OpenCV 类型，要确认 `elemSize` 是每像素总字节数，而不是单通道字节数。

---

# 17. Reconstruction Attempt 隔离

严禁 Aborted Attempt 的信号进入最终：

```text
accum
weight
allSignals
```

最安全第一版策略：

```text
Receiver 仅在 LineCommit 后才把该 Attempt 的 Frame 标记为 ReadyForProcessing
```

但这会延迟实时重建。

考虑项目已有边收边重建需求，第一版采用：

```text
Running Attempt Frame 可以处理
但必须支持回滚
```

会让实现复杂。

因此本计划为简历可讲版选择更稳妥的策略：

```text
Receiver durable 保存 Frame
↓
等 LINE_COMMIT
↓
该 Attempt 的全部 Frame 按 frameIndex / frameSeq 进入 ReconstructionQueue
```

这样天然保证：

```text
Aborted Attempt 永远不进入 accum
```

代价：

```text
重建延迟一个 Line
```

本版本接受此代价。

如果真实项目必须严格实时边扫边显示，再单独升级为 per-line temporary accumulation + commit/rollback，不在本次范围。

---

# 18. LineCommit 与网络异步

扫描端本地 Line Commit 不等待网络。

例如断网：

```text
Line 537 完成
→ 本地 checkpoint commit
→ Line 538 继续扫描
```

重连后：

- 发送 LineBegin；
- 发送 Frame；
- 发送 LineCommit。

确保 Receiver 最终获得完整 Line 事务状态。

对于一个 committed Line：

```text
所有 Frame 必须已成功 SenderSpool persist
```

否则不允许本地 commit。

---

# 19. 推荐目录改造

发送端：

```text
capture_and_network/
├── include/
│   ├── common/
│   │   ├── RawFrame.h
│   │   └── FrameBufferPool.h
│   ├── capture/
│   │   └── CameraCapture.h
│   ├── task/
│   │   ├── ScanTask.h
│   │   ├── TaskCheckpoint.h
│   │   └── TaskCheckpointManager.h
│   ├── storage/
│   │   └── SenderSpool.h
│   ├── protocol/
│   │   ├── ProtocolCommon.h
│   │   ├── FrameProtocol.h
│   │   ├── ControlProtocol.h
│   │   └── CRC32.h
│   ├── network/
│   │   ├── TcpClient.h
│   │   ├── ReliableSession.h
│   │   └── ReliableFrameSender.h
│   └── app/
│       └── CameraNetworkSender.h
│
└── src/
    └── 对应实现
```

接收端：

```text
reconstruct_picture/
├── include/
│   ├── network/
│   │   ├── TcpServer.h
│   │   ├── ProtocolReceiver.h
│   │   └── FrameAssembler.h
│   ├── storage/
│   │   └── ReceiverSpool.h
│   ├── task/
│   │   └── RemoteTaskState.h
│   └── ...
└── src/
    └── ...
```

协议公共代码重复问题：

优先方案：

```text
在仓库根目录新增 common_protocol/
```

供 sender / receiver 共同链接。

若本次改顶层 CMake 工作量过大，可以暂时：

```text
capture_and_network/include/protocol
```

作为公共协议源并由 reconstruct target 引入。

但禁止 sender/receiver 各自复制一份 Protocol V2 实现。

---

# 20. CMake

更新：

```text
capture_and_network/CMakeLists.txt
reconstruct_picture 对应 CMake
```

新增源文件必须加入 target。

建议编译选项继续：

```text
-Wall -Wextra -Wpedantic
```

开发/测试建议增加：

```text
-Werror
```

若当前旧代码会导致大量历史 warning，可只对新增 protocol/storage/network target 使用 `-Werror`。

---

# 21. Mock 故障注入测试文件

必须新增一个单文件测试工具：

```text
tests/mock_fault_injector.py
```

该文件是本次计划的必做项，不是可选项。

目标：

> 不依赖真实相机和平移台，通过 Protocol V2 模拟 Sender 或 Receiver，并主动制造网络/协议故障，验证 ACK、重连、Resume、CRC、幂等、Attempt Abort 和 Spool 恢复。

使用 Python 的原因：

- 易于主动 close socket；
- 易于精确控制发送到哪个 Chunk 时断开；
- 易于修改 CRC；
- 易于 drop ACK；
- 不与 C++ 被测实现共享代码，避免“同一个 bug 两边一起通过”。

---

## 21.1 单文件两种模式

```bash
python3 tests/mock_fault_injector.py receiver ...
python3 tests/mock_fault_injector.py sender ...
```

使用 argparse subcommand。

---

## 21.2 mock receiver 模式

用于测试真实 C++ Sender。

实现最小 Protocol V2 parser。

支持参数：

```bash
--host 127.0.0.1
--port 8080
--state-file /tmp/mock_receiver_state.json
```

故障参数：

```bash
--disconnect-after-frame N
--disconnect-mid-frame FRAME_SEQ
--disconnect-after-chunk K
--drop-ack FRAME_SEQ
--nack-once FRAME_SEQ
--ack-delay-ms N
--forget-connection-state
```

语义：

### A. 正常接收

```bash
python3 tests/mock_fault_injector.py receiver --port 8080
```

应：

- 完整接收 Frame；
- 校验 CRC；
- 持久化 mock state；
- ACK；
- ResumeReply 返回 durable progress。

---

### B. Frame 完成后、ACK 前断线

```bash
--drop-ack 100
```

流程：

```text
收到 Frame 100
persist durable state
不发送 ACK / 主动断开
```

Sender 必须：

```text
重连
→ Resume
→ Server 表明 100 已 durable
→ Sender 删除本地 frame 100
→ 不重复业务处理
```

这是幂等关键测试。

---

### C. Frame 中途断线

```bash
--disconnect-mid-frame 101 --disconnect-after-chunk 3
```

Receiver：

```text
收到 FrameBegin 101
收到 chunk 0,1,2
chunk 3 后 close()
```

Sender：

```text
重连
→ Frame 101 从 Chunk 0 完整重传
```

不得从 Chunk 4 续传。

---

### D. NACK once

```bash
--nack-once 102
```

第一次完整收到 102 后：

```text
返回 NACK(BadCrc)
```

第二次：

```text
正常 ACK
```

验证 Sender Frame 级重试。

---

## 21.3 mock sender 模式

用于测试真实 C++ Receiver。

输入可以生成固定 Raw Frame：

```text
rows = 64
cols = 64
type = CV_8UC1 或约定值
payload = deterministic bytes
```

支持：

```bash
--frames N
--lines N
--frames-per-line N
```

故障参数：

```bash
--bad-crc-frame FRAME_SEQ
--duplicate-frame FRAME_SEQ
--disconnect-mid-frame FRAME_SEQ
--abort-line LINE_ID
--restart-at-line LINE_ID
```

---

### E. Bad CRC

```bash
--bad-crc-frame 5
```

FrameMeta CRC 与 payload 不一致。

C++ Receiver 必须：

```text
不 durable persist final raw
不推进 durable progress
返回 NACK(BadCrc)
```

---

### F. Duplicate Frame

```bash
--duplicate-frame 6
```

同一个：

```text
TaskID + FrameSeq
```

发送两次。

Receiver 必须：

```text
只持久化一次
只进入 ReconstructionQueue 一次
第二次直接 ACK
```

---

### G. Abort Line

模拟：

```text
LINE_BEGIN line=2 attempt=0
发送部分 Frame
LINE_ABORT line=2 attempt=0

LINE_BEGIN line=2 attempt=1
重新发送完整 Line
LINE_COMMIT line=2 attempt=1
```

验收：

```text
attempt 0 原始 Frame 最终标记 invalid / 删除
attempt 0 不进入 reconstruction
attempt 1 进入 reconstruction
```

---

## 21.4 mock state-file

mock receiver 必须把 durable progress 保存到：

```text
JSON state file
```

例如：

```json
{
  "task_id": 123,
  "highest_durable_frame_seq": 100
}
```

这样关闭 mock receiver 进程并重新运行后仍能模拟：

```text
Receiver restart + Resume
```

---

## 21.5 Mock 工具退出码

规范：

```text
0 = scenario success / clean finish
1 = protocol/test assertion failed
2 = CLI/config error
```

输出使用明确前缀：

```text
[MOCK][INFO]
[MOCK][FAULT]
[MOCK][ASSERT]
```

禁止仅打印模糊信息。

---

# 22. 必须覆盖的故障测试矩阵

coding agent 完成后至少执行以下测试。

| ID | 故障 | 预期 |
|---|---|---|
| T01 | 正常 2 Lines × 5 Frames | 全部 ACK，SenderSpool 清空 |
| T02 | Frame 中途 TCP 断开 | 重连后整个 Frame 重发 |
| T03 | Frame durable 后 ACK 丢失 | Resume 后不重复业务持久化 |
| T04 | TCP 断线持续采集 | Frame 继续进入 SenderSpool |
| T05 | Sender 进程 kill 后重启 | 未 ACK committed Frame 从 Spool 恢复 |
| T06 | Receiver 进程 kill 后重启 | Resume 返回已 durable progress |
| T07 | CRC 错误 | Receiver NACK，Frame 不进入重建 |
| T08 | Duplicate Frame | Receiver 幂等，只处理一次 |
| T09 | Line 中途 crash | 旧 Attempt Aborted，新 Attempt 从 FrameIndex 0 |
| T10 | Aborted Attempt 已部分到达 Receiver | 旧 Attempt 不参与 reconstruction |
| T11 | SenderSpool 达到 max bytes | 扫描侧出现 Backpressure，不 OOM/无限写盘 |
| T12 | Receiver partial `.part` 后 crash | 重启清理 partial，不错误 ACK |

---

# 23. 测试目录

建议：

```text
tests/
├── mock_fault_injector.py
├── integration/
│   ├── test_normal.sh
│   ├── test_reconnect.sh
│   └── test_crash_recovery.sh
└── data/
```

如果本次严格只新增一个 mock 文件，则：

```text
tests/mock_fault_injector.py
```

是强制项，其余 shell 脚本可后补。

---

# 24. 日志要求

新增模块统一日志至少包含：

```text
taskId
lineId
attemptId
frameSeq
```

关键日志：

```text
[PERSIST]
[SEND]
[ACK]
[NACK]
[RECONNECT]
[RESUME]
[LINE_BEGIN]
[LINE_COMMIT]
[LINE_ABORT]
[INVALIDATE]
[SPOOL_FULL]
```

示例：

```text
[ACK] task=42 line=12 attempt=0 frameSeq=1234
```

不要只打印：

```text
发送成功
接收失败
```

面试演示和故障定位都依赖这些日志。

第一版不要求引入 spdlog，可使用现有 stdout/stderr。

---

# 25. 安全与边界校验

ProtocolReceiver 必须检查：

```text
magic
version
message type
bodyLength max
rows / cols > 0
dataLength max
fragmentCount 合理
fragmentIndex < fragmentCount
offset + payloadSize <= dataLength
CRC32
```

配置最大 Frame 大小，例如：

```text
256 MiB
```

避免异常 Header 触发巨额内存分配。

不要信任网络传来的：

```text
rows
cols
dataLength
fragmentCount
```

---

# 26. 实现阶段

严格按阶段推进。每阶段完成后先编译和测试，再进入下一阶段。

---

## Phase 0 — Baseline Fix

任务：

- [ ] 修复 `RawFrame` uint8/uint16 类型错误
- [ ] TcpClient 增加 `recvAll`
- [ ] 修复 reconstruction 输入尺寸问题
- [ ] 保证现有代码至少可构建
- [ ] 记录现有 baseline 行为

完成标准：

```text
无新增可靠传输功能
但代码基线正确可编译
```

---

## Phase 1 — Protocol V2 + Raw Frame End-to-End

任务：

- [ ] 新增 CommonHeader
- [ ] MessageType
- [ ] FrameMeta
- [ ] FrameChunkMeta
- [ ] 显式网络序 serialize/parse
- [ ] sender 使用 V2
- [ ] receiver 使用 V2
- [ ] 删除正式 CAM1 接收路径
- [ ] receiver 不再 imdecode
- [ ] Raw Frame 恢复 cv::Mat
- [ ] 保留 64 KiB Chunk

完成标准：

```text
真实 C++ sender
→ Protocol V2
→ 真实 C++ receiver
→ Raw Frame
→ reconstruction queue
```

在稳定网络下跑通。

---

## Phase 2 — CRC + ReceiverSpool + ACK

任务：

- [ ] CRC32
- [ ] FrameAssembler
- [ ] `.part` 临时文件
- [ ] ReceiverSpool durable rename
- [ ] ACK
- [ ] NACK
- [ ] duplicate Frame 幂等
- [ ] highestDurableFrameSeq

完成标准：

```text
ACK 只有 durable 后产生
duplicate 不重复处理
bad CRC 不 ACK
```

---

## Phase 3 — SenderSpool

任务：

- [ ] persist-first
- [ ] startup scan
- [ ] ACK 删除
- [ ] spool metadata
- [ ] maxSpoolBytes
- [ ] condition_variable backpressure

完成标准：

```text
kill sender 后未 ACK Frame 仍在磁盘
重启能恢复
```

---

## Phase 4 — Reconnect + Resume

任务：

- [ ] ReliableSession 状态机
- [ ] 自动 reconnect
- [ ] Hello
- [ ] ResumeRequest
- [ ] ResumeReply
- [ ] local spool 与 remote durable progress reconcile
- [ ] partial Frame 重连后整帧重发

完成标准：

```text
拔断 socket / mock close
↓
扫描继续
↓
spool 累积
↓
自动重连
↓
补传
↓
spool 清空
```

---

## Phase 5 — Line / Attempt / Checkpoint

任务：

- [ ] TaskCheckpoint
- [ ] Atomic save
- [ ] LineBegin
- [ ] LineCommit
- [ ] LineAbort
- [ ] AttemptID
- [ ] crash 后旧 Attempt invalid
- [ ] 新 Attempt 从 FrameIndex 0
- [ ] Receiver 不重建 aborted Attempt
- [ ] committed Line 未 ACK Frame 继续补传

完成标准：

```text
Line 中途 kill sender
↓
重启
↓
旧 Attempt abort
↓
整 Line 重扫
↓
旧数据不进入最终重建
```

---

## Phase 6 — Mock Fault Integration Tests

任务：

- [ ] 新增 `tests/mock_fault_injector.py`
- [ ] receiver mode
- [ ] sender mode
- [ ] disconnect mid-frame
- [ ] drop ACK
- [ ] NACK once
- [ ] bad CRC
- [ ] duplicate Frame
- [ ] abort Attempt
- [ ] persisted mock resume state
- [ ] 执行 T01-T12

完成标准：

```text
核心故障场景均可重复复现
日志能证明恢复过程
无人工修改状态文件才能恢复
```

---

# 27. Coding Agent 工作约束

1. **不要引入 HTTP。**
2. **不要引入 gRPC。**
3. **不要引入数据库。**
4. **不要引入消息队列。**
5. **不要实现 Sliding Window。**
6. **不要实现 Chunk 级跨连接 Resume。**
7. **不要为了“优雅”大规模重写 CUDA / Reconstruction 算法。**
8. **不要修改现有重建数学逻辑，除非为 Raw Frame 输入适配所必需。**
9. 优先复用现有：
   - `FrameBufferPool`
   - `BlockingQueue`
   - `TcpClient`
   - `RawFrameSender` 的分片思想
   - `RawFrameProtocol` 的手工序列化思想
10. 新代码应使用 RAII，避免裸资源泄漏。
11. 每个 Phase 提交前必须保证项目编译。
12. 所有协议变更统一修改公共 Protocol 实现，禁止 sender/receiver 各写一份。
13. 不要把所有功能堆进 `main.cpp` 或 `CameraNetworkSender`。
14. 不要让 `TcpClient` 感知业务语义。
15. 不要将“socket recv 成功”等价为业务 ACK。

---

# 28. 建议类职责

## TcpClient

只做：

```text
connect
sendAll
recvAll
close
socket options
```

---

## ReliableSession

做：

```text
network state
connect/reconnect
Hello
Resume
send message
wait ACK/NACK
```

---

## ReliableFrameSender

做：

```text
FrameMeta
chunk iteration
FrameBegin
FrameChunk
frame retry
```

---

## SenderSpool

做：

```text
persist
load
remove
invalidate attempt
disk quota
```

---

## TaskCheckpointManager

做：

```text
load
atomic save
```

---

## ProtocolReceiver

做：

```text
message framing
dispatch
protocol validation
```

---

## FrameAssembler

做：

```text
FrameBegin
Chunk append
length validation
CRC
complete frame
```

---

## ReceiverSpool

做：

```text
durable storage
idempotency
progress
attempt invalidation
```

---

# 29. Definition of Done

本次升级只有同时满足以下条件才算完成。

### Build

- [ ] sender 编译通过
- [ ] receiver 编译通过
- [ ] Protocol V2 公共代码只有一份实现
- [ ] 无 RawFrame 类型不一致

### Normal Path

- [ ] Raw Frame 端到端发送
- [ ] Receiver 正确构造 cv::Mat
- [ ] 重建流水线可消费有效 Frame
- [ ] ACK 后 SenderSpool 删除

### Network Fault

- [ ] TCP 中断不停止扫描（Spool 未满时）
- [ ] 自动重连
- [ ] Resume
- [ ] 未 ACK Frame 恢复
- [ ] Mid-frame disconnect 整帧重传

### Process Crash

- [ ] Sender kill 后未 ACK Frame 不丢
- [ ] Receiver kill 后 durable progress 不丢
- [ ] partial Receiver Frame 不被当成 durable

### Task Recovery

- [ ] Line 级 Checkpoint
- [ ] Line 中途 crash 后整行重新扫描
- [ ] AttemptID 增加
- [ ] 旧 Attempt Frame 不参与最终重建
- [ ] 已 committed Line 不重复扫描

### Data Integrity

- [ ] CRC32
- [ ] NACK
- [ ] Duplicate 幂等
- [ ] ACK 丢失不造成重复业务处理

### Test

- [ ] `tests/mock_fault_injector.py` 存在
- [ ] T01-T12 至少有手工/脚本执行记录
- [ ] 关键恢复日志清晰

---

# 30. 本次明确不做的 Future Work

不要在本次任务主动实现：

```text
Sliding Window
Cumulative ACK
Selective ACK
Zero-copy network send
io_uring
epoll multi-client server
Heartbeat
RTT estimator
Adaptive retry
Compression
TLS
Authentication
HTTP control plane
Prometheus metrics
Reconstruction checkpoint
Multi-node reconstruction
Kafka / Redis / DB
```

这些可以在 README 中列为后续优化方向。

---

# 31. 最终简历可描述能力

完成本计划后，项目可以真实描述为：

> 面向长时间光学扫描任务，基于 C++17/Linux Socket 设计采集与可靠传输框架；通过内存池和有界队列解耦采集链路，自定义版本化二进制协议支持 Raw Frame 分片传输，并以 Frame 为网络恢复单位实现 CRC32、业务 ACK、持久化 Sender/Receiver Spool、幂等处理及 TCP 重连接续传；以扫描 Line 为任务 Checkpoint，引入 Attempt 隔离机制，在进程异常后废弃未完成 Line 并从最近已提交 Line 恢复，同时保证已扫描未确认数据无需重新采集。

注意：

只有对应代码和测试完成后，才可以把上述能力写成“已实现”。

---

# 32. Coding Agent 最终交付物

完成任务后必须给出：

```text
1. 新增文件清单
2. 修改文件清单
3. Protocol V2 字段说明
4. Sender 状态机说明
5. Receiver 状态机说明
6. SenderSpool / ReceiverSpool 文件格式
7. Checkpoint 文件格式
8. mock_fault_injector.py 使用说明
9. T01-T12 测试结果
10. 已知限制 / Future Work
```

并保证仓库中至少新增：

```text
plan.md
tests/mock_fault_injector.py
```

本计划执行过程中如发现真实扫描控制代码并未存在于当前仓库，不要伪造硬件控制实现。应：

```text
保留 ScanTask / Checkpoint 接口
通过 mock scan task 完成恢复逻辑验证
```

并在最终交付中明确：

```text
“真实平移台控制接口待接入，可靠传输与任务状态机制已通过 mock 验证。”
```
