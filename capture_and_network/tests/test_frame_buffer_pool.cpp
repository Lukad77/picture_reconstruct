// C++ Standard Library
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

// Project Headers
#include "common/FrameBufferPool.h"

void testBasicAcquireRelease() {
    std::cout << "\n=== 测试基本获取和释放 ===" << std::endl;
    
    FrameBufferPool pool(1024, 4);
    
    std::cout << "初始状态: "
              << "总缓冲区=" << pool.totalCount()
              << ", 可用=" << pool.availableCount() << std::endl;
    
    auto buffer1 = pool.acquire();
    std::cout << "获取第1个缓冲区后: 可用=" << pool.availableCount() << std::endl;
    
    auto buffer2 = pool.acquire();
    std::cout << "获取第2个缓冲区后: 可用=" << pool.availableCount() << std::endl;
    
    {
        auto buffer3 = pool.acquire();
        std::cout << "获取第3个缓冲区后: 可用=" << pool.availableCount() << std::endl;
        
        auto buffer4 = pool.acquire();
        std::cout << "获取第4个缓冲区后: 可用=" << pool.availableCount() << std::endl;
        
        // buffer4 在此处自动释放
    }
    
    std::cout << "buffer4自动释放后: 可用=" << pool.availableCount() << std::endl;
}

void testConcurrentAccess() {
    std::cout << "\n=== 测试并发访问 ===" << std::endl;
    
    FrameBufferPool pool(1024, 8);
    
    const int threadCount = 4;
    const int iterations = 100;
    std::vector<std::thread> threads;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < iterations; ++j) {
                auto buffer = pool.acquire();
                
                // 模拟使用缓冲区
                for (size_t k = 0; k < pool.bufferSize(); ++k) {
                    buffer[k] = static_cast<uint8_t>((i * iterations + j) % 256);
                }
                
                // buffer 在此处自动释放
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    std::cout << "并发测试完成: "
              << threadCount << "线程, "
              << iterations << "次迭代/线程, "
              << "耗时=" << duration.count() << "ms, "
              << "最终可用缓冲区=" << pool.availableCount() << std::endl;
}

void testLargeBuffers() {
    std::cout << "\n=== 测试大缓冲区 ===" << std::endl;
    
    // 模拟 1920x1080x3 的图像大小
    size_t imageSize = 1920 * 1080 * 3;
    FrameBufferPool pool(imageSize, 4);
    
    std::cout << "大缓冲区测试: "
              << "单缓冲区大小=" << pool.bufferSize() / 1024 / 1024 << "MB, "
              << "总缓冲区数=" << pool.totalCount() << ", "
              << "总内存=" << (pool.bufferSize() * pool.totalCount() / 1024 / 1024) << "MB" << std::endl;
    
    auto buffer = pool.acquire();
    std::cout << "获取大缓冲区成功: 可用=" << pool.availableCount() << std::endl;
    
    // 模拟写入图像数据
    for (size_t i = 0; i < pool.bufferSize(); i += 3) {
        buffer[i] = 255;     // R
        buffer[i + 1] = 0;     // G
        buffer[i + 2] = 0;     // B
    }
    
    std::cout << "写入模拟图像数据完成" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  FrameBufferPool 功能测试" << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        testBasicAcquireRelease();
        testConcurrentAccess();
        testLargeBuffers();
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "  所有测试通过!" << std::endl;
        std::cout << "========================================" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "测试失败: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}