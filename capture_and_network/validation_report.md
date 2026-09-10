# Phase 1-5 验证报告

## 1. 执行环境
- 时间: 2026-09-10 22:12:27 CST
- 工作目录: /home/leo/vscode_prjs/pic_reconstruct/capture_and_network
- OS: Linux leo-OMEN-by-HP-Laptop 5.15.0-139-generic #149~20.04.1-Ubuntu SMP Wed Apr 16 08:29:56 UTC 2025 x86_64 x86_64 x86_64 GNU/Linux

## 2. 验证脚本
```bash
cmake -S . -B build
cmake --build build -j2
./build/test_sender_spool_resume
./build/test_reliable_resume
./build/test_phase5_checkpoint
./build/test_processing_resume
```

## 3. 编译与构建
```bash
-- Configuring done
-- Generating done
-- Build files have been written to: /home/leo/vscode_prjs/pic_reconstruct/capture_and_network/build
---
[ 39%] Built target lan_video_core
[ 56%] Built target test_phase5_checkpoint
[ 56%] Built target test_processing_resume
[ 73%] Built target test_sender_spool_resume
[ 73%] Built target test_camera
[ 91%] Built target raw_test_server
[ 91%] Built target raw_camera_client
[100%] Built target test_reliable_resume
```

## 4. 回归测试执行
### 运行: ./build/test_sender_spool_resume
```bash
sender spool resume ok
```
- 退出码: 0

### 运行: ./build/test_reliable_resume
```bash
[TcpClient] 已连接服务器: 127.0.0.1:19002
[ResumeSession] resume reply: taskId=1, highestDurableFrameSeq=0
[ResumeSession] replaying 1 pending frames
[RawFrameSender] 已发送原始帧 frameId=42, size=16, fragments=1
server: totalReceived=16, expected=16
resume state machine ok
```
- 退出码: 0

### 运行: ./build/test_phase5_checkpoint
```bash
phase5 checkpoint ok
```
- 退出码: 0

### 运行: ./build/test_processing_resume
```bash
processing resume ok
```
- 退出码: 0

## 5. 结论
- 结果：全部验证通过
- 结论：Phase 2-5 关键恢复与 checkpoint 逻辑已通过回归验证

