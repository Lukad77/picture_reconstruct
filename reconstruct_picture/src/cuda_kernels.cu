#include "../include/cuda_kernels.cuh"
#include <cuda_runtime.h>
#include <iostream>

// ==========================================
// 1. GPU 核函数 (Kernel): 真正运行在成百上千个 GPU 核心上的代码
// ==========================================
__global__ void preprocessKernel(const float* input, float* output, int width, int height, float bg_value) {
    // 计算当前线程对应的 2D 图像坐标
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    // 边界检查，防止越界访存
    if (x < width && y < height) {
        int idx = y * width + x; // 将 2D 坐标展平为 1D 索引
        
        // 读取像素，减去背景
        float val = input[idx] - bg_value;
        
        // 非线性截断 (类似深度学习里的 ReLU)
        output[idx] = val > 0.0f ? val : 0.0f; 
    }
}

// ==========================================
// 2. Host 包装函数: 运行在 CPU 上，负责配置和启动 GPU
// ==========================================
void runPreprocessCuda(const float* d_input, float* d_output, int width, int height, float bg_value) {
    // 定义每个 Block 的线程数 (通常是 16x16 或 32x32)
    dim3 threadsPerBlock(16, 16);
    
    // 计算需要多少个 Block 才能覆盖整张图像
    dim3 numBlocks((width + threadsPerBlock.x - 1) / threadsPerBlock.x,
                   (height + threadsPerBlock.y - 1) / threadsPerBlock.y);

    // 启动核函数 (异步非阻塞)
    preprocessKernel<<<numBlocks, threadsPerBlock>>>(d_input, d_output, width, height, bg_value);
    
    // 捕获可能发生的内核启动错误
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        std::cerr << "CUDA Error: " << cudaGetErrorString(err) << std::endl;
    }
}
// 在 src/cuda_kernels.cu 中添加：

// 针对 sigma=0.8 的 3x3 高斯模糊核权重 (预先计算好)
__constant__ float d_gaussianKernel[9] = {
    0.0625f, 0.125f, 0.0625f,
    0.125f,  0.25f,  0.125f,
    0.0625f, 0.125f, 0.0625f
};

// ==========================================
// CUDA 核函数: 3x3 高斯滤波
// ==========================================
__global__ void gaussianBlurKernel(const float* input, float* output, int width, int height) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= 1 && x < width - 1 && y >= 1 && y < height - 1) {
        float sum = 0.0f;
        int k = 0;
        
        // 展开 3x3 卷积循环，极大提升 GPU 执行效率
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int px = x + dx;
                int py = y + dy;
                sum += input[py * width + px] * d_gaussianKernel[k++];
            }
        }
        output[y * width + x] = sum;
    } else if (x < width && y < height) {
        // 边缘像素直接复制，不模糊
        output[y * width + x] = input[y * width + x];
    }
}

// ==========================================
// 重新封装的 Host 调用函数 (一站式解决)
// ==========================================
void runPreprocessAndBlurCuda(const float* d_input, float* d_temp, float* d_output, int width, int height, float bg_value) {
    dim3 threadsPerBlock(16, 16);
    dim3 numBlocks((width + threadsPerBlock.x - 1) / threadsPerBlock.x,
                   (height + threadsPerBlock.y - 1) / threadsPerBlock.y);

    // 1. 在 GPU 上执行背景减除
    preprocessKernel<<<numBlocks, threadsPerBlock>>>(d_input, d_temp, width, height, bg_value);
    
    // 同步确保第一步算完 (同一个流内通常是顺序的，这里写上更严谨)
    cudaDeviceSynchronize();

    // 2. 紧接着在 GPU 上执行高斯滤波
    gaussianBlurKernel<<<numBlocks, threadsPerBlock>>>(d_temp, d_output, width, height);
}