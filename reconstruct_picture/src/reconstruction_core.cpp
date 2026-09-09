#include "../include/reconstruction_core.h"
#include <cmath>
#include <algorithm>
#include <cuda_runtime.h>
float estimateBorderBackground(const cv::Mat& image, int borderWidth) {
    std::vector<float> values;
    values.reserve(static_cast<size_t>(image.cols * borderWidth * 2 + image.rows * borderWidth * 2));

    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            bool inBorder = x < borderWidth || y < borderWidth || x >= image.cols - borderWidth || y >= image.rows - borderWidth;
            if (inBorder) values.push_back(image.at<float>(y, x));
        }
    }
    if (values.empty()) return 0.0f;
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

cv::Mat preprocessFrame(const cv::Mat& raw) {
    cv::Mat f;
    raw.convertTo(f, CV_32FC1);
    
    // 1. 在 CPU 上计算背景值 (这部分数据量极小，没必要放 GPU)
    float bg = estimateBorderBackground(f, 20);

    int width = f.cols;
    int height = f.rows;
    size_t bytes = width * height * sizeof(float);

    // 2. 分配 GPU 显存 (Device Memory)
    float *d_input = nullptr, *d_output = nullptr;
    cudaMalloc(&d_input, bytes);
    cudaMalloc(&d_output, bytes);

    // 3. 将图像数据从 CPU 内存拷贝到 GPU 显存 (Host to Device)
    cudaMemcpy(d_input, f.ptr<float>(), bytes, cudaMemcpyHostToDevice);

    // 4. 调用原生 CUDA 核函数执行并行计算 (减去背景 + 截断)
    runPreprocessCuda(d_input, d_output, width, height, bg);

    // 5. 等待 GPU 计算完成，并将结果拷回 CPU (Device to Host)
    cv::Mat corrected(height, width, CV_32FC1);
    cudaMemcpy(corrected.ptr<float>(), d_output, bytes, cudaMemcpyDeviceToHost);

    // 6. 释放显存
    cudaFree(d_input);
    cudaFree(d_output);

    // 7. 高斯滤波 (第一版先保留 OpenCV 的 CPU 实现，后续再手写 CUDA 版)
    cv::GaussianBlur(corrected, corrected, cv::Size(0, 0), 0.8);

    return corrected;
}
// cv::Mat preprocessFrame(const cv::Mat& raw) {
//     cv::Mat f;
//     raw.convertTo(f, CV_32FC1);
//     float bg = estimateBorderBackground(f, 20);
//     cv::Mat corrected = f - bg;
//     cv::max(corrected, 0.0f, corrected);
//     cv::GaussianBlur(corrected, corrected, cv::Size(0, 0), 0.8);
//     return corrected;
// }

double extractCircularRoiMean(const cv::Mat& frame, const SpotInfo& spot) {
    int cx = static_cast<int>(std::round(spot.cameraCenter.x));
    int cy = static_cast<int>(std::round(spot.cameraCenter.y));
    int r = static_cast<int>(std::round(spot.radius));

    double sum = 0.0;
    int count = 0;

    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;
            int x = cx + dx;
            int y = cy + dy;
            if (x < 0 || y < 0 || x >= frame.cols || y >= frame.rows) continue;
            sum += frame.at<float>(y, x);
            count++;
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

void addBilinearSample(cv::Mat& accum, cv::Mat& weight, double x, double y, double value) {
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    double dx = x - x0;
    double dy = y - y0;

    auto addOne = [&](int px, int py, double w) {
        if (px < 0 || py < 0 || px >= accum.cols || py >= accum.rows) return;
        accum.at<float>(py, px) += static_cast<float>(value * w);
        weight.at<float>(py, px) += static_cast<float>(w);
    };

    addOne(x0, y0, (1.0 - dx) * (1.0 - dy));
    addOne(x1, y0, dx * (1.0 - dy));
    addOne(x0, y1, (1.0 - dx) * dy);
    addOne(x1, y1, dx * dy);
}

cv::Mat normalizeByWeight(const cv::Mat& accum, const cv::Mat& weight) {
    cv::Mat recon(accum.size(), CV_32FC1, cv::Scalar(0));
    for (int y = 0; y < accum.rows; ++y) {
        for (int x = 0; x < accum.cols; ++x) {
            float w = weight.at<float>(y, x);
            if (w > 1e-6f) recon.at<float>(y, x) = accum.at<float>(y, x) / w;
        }
    }
    return recon;
}

cv::Mat fillHolesByInpaint(const cv::Mat& image, const cv::Mat& weight) {
    cv::Mat image8u;
    cv::normalize(image, image8u, 0, 255, cv::NORM_MINMAX);
    image8u.convertTo(image8u, CV_8UC1);

    cv::Mat mask(weight.size(), CV_8UC1, cv::Scalar(0));
    for (int y = 0; y < weight.rows; ++y) {
        for (int x = 0; x < weight.cols; ++x) {
            if (weight.at<float>(y, x) <= 1e-6f) mask.at<uchar>(y, x) = 255;
        }
    }

    cv::Mat filled8u;
    cv::inpaint(image8u, mask, filled8u, 3.0, cv::INPAINT_TELEA);
    cv::Mat filled32f;
    filled8u.convertTo(filled32f, CV_32FC1);
    cv::normalize(filled32f, filled32f, 0.0f, 1.0f, cv::NORM_MINMAX);
    return filled32f;
}

cv::Mat makeGaussianPsf(int size, double sigma) {
    if (size % 2 == 0) size += 1;
    cv::Mat psf(size, size, CV_32FC1, cv::Scalar(0));
    int c = size / 2;
    double sum = 0.0;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int dx = x - c;
            int dy = y - c;
            double v = std::exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
            psf.at<float>(y, x) = static_cast<float>(v);
            sum += v;
        }
    }
    psf /= static_cast<float>(sum);
    return psf;
}

cv::Mat flipPsf(const cv::Mat& psf) {
    cv::Mat flipped;
    cv::flip(psf, flipped, -1);
    return flipped;
}

cv::Mat lucyRichardsonDeconvolution(const cv::Mat& input32f, const cv::Mat& psf32f, int iterations) {
    const float eps = 1e-6f;
    cv::Mat image;
    input32f.convertTo(image, CV_32FC1);
    cv::max(image, eps, image);

    cv::Mat estimate = image.clone();
    cv::Mat psfFlipped = flipPsf(psf32f);

    for (int i = 0; i < iterations; ++i) {
        cv::Mat blurred;
        cv::filter2D(estimate, blurred, -1, psf32f, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
        cv::max(blurred, eps, blurred);
        cv::Mat ratio = image / blurred;
        cv::Mat correction;
        cv::filter2D(ratio, correction, -1, psfFlipped, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
        estimate = estimate.mul(correction);
        cv::max(estimate, eps, estimate);
    }
    return estimate;
}

cv::Mat normalizeTo16U(const cv::Mat& src) {
    cv::Mat norm32f;
    cv::normalize(src, norm32f, 0, 65535, cv::NORM_MINMAX);
    cv::Mat dst16u;
    norm32f.convertTo(dst16u, CV_16UC1);
    return dst16u;
}