import socket
import struct
import time
import csv
import os
import threading
from queue import Queue

# --- 后台读盘线程 ---
def disk_reader_worker(frame_dir, positions, q):
    print("[*] 读盘线程已启动，正在向内存队列中异步加载数据...")
    missing_count = 0
    
    for frame_id, stage_x, stage_y in positions:
        filename = f"frame_{frame_id:05d}.tif"
        filepath = os.path.join(frame_dir, filename)

        if not os.path.exists(filepath):
            missing_count += 1
            continue

        with open(filepath, 'rb') as f:
            image_data = f.read()

        # 提前在内存里把协议头和图片字节拼好
        header = struct.pack('<4siidd', b'CAM1', frame_id, len(image_data), stage_x, stage_y)
        
        # put() 会在队列满时自动阻塞，防止内存撑爆
        q.put(header + image_data)

    # 放入结束标志
    q.put(None)
    print(f"[*] 读盘线程结束。共丢失 {missing_count} 帧。")


def send_stream_async(frame_dir, scan_csv, host='127.0.0.1', port=8080):
    # 1. 读取坐标配置
    positions = []
    with open(scan_csv, 'r') as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            if len(row) >= 3:
                positions.append((int(row[0]), float(row[1]), float(row[2])))

    print(f"[*] 发现 {len(positions)} 帧坐标信息。")
    
    # 2. 建立网络连接
    print(f"[*] 尝试连接 C++ 服务端 {host}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 增加 Socket 发送缓冲区大小
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 * 1024 * 1024)
    
    try:
        sock.connect((host, port))
        print("[*] 连接成功！")
    except ConnectionRefusedError:
        print("[!] 错误: C++ 服务端未启动或端口不通！")
        return

    # 3. 创建限制大小的内存缓冲队列 (1000 帧大约占用 1GB~2GB 内存)
    buffer_queue = Queue(maxsize=1000)

    # 4. 启动后台读盘线程
    reader_thread = threading.Thread(target=disk_reader_worker, args=(frame_dir, positions, buffer_queue))
    reader_thread.daemon = True
    reader_thread.start()

    # 先稍微等几秒钟，让读盘线程把队列塞满，以保证等会儿能测出极速网络并发
    print("[*] 正在预热内存队列，请稍等 3 秒钟...")
    time.sleep(3)

    print("\n>>> 开始向 C++ 极速发射流媒体数据 <<<")
    send_start = time.time()
    frames_sent = 0

    # 5. 主线程：纯网络极速 I/O
    while True:
        payload = buffer_queue.get()
        if payload is None: # 遇到结束标志
            break
            
        sock.sendall(payload)
        frames_sent += 1
        
        if frames_sent % 1000 == 0:
            print(f"    已发送 {frames_sent} 帧...")

    send_time = time.time() - send_start
    fps = frames_sent / send_time if send_time > 0 else 0
    
    print(f"\n[*] 传输完成！")
    print(f"[*] 总发送帧数: {frames_sent}")
    print(f"[*] 网络发射耗时: {send_time:.2f} 秒")
    print(f"[*] 极限吞吐率: {fps:.2f} FPS")

    sock.close()
    reader_thread.join()

if __name__ == "__main__":
    FRAME_DIR = "frames" 
    SCAN_CSV = "scan_positions.csv"
    send_stream_async(FRAME_DIR, SCAN_CSV)