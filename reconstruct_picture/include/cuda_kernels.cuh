#pragma once

// C++ 调用的包装函数声明
// d_input 和 d_output 必须是已经分配好的显存指针 (Device Pointers)
void runPreprocessCuda(const float* d_input, float* d_output, int width, int height, float bg_value);
void runPreprocessAndBlurCuda(const float* d_input, float* d_temp, float* d_output, int width, int height, float bg_value);