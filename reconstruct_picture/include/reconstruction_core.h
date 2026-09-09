#pragma once
#include "core_types.h"
#include <opencv2/opencv.hpp>
#include <cuda_runtime.h>
#include "cuda_kernels.cuh"
float estimateBorderBackground(const cv::Mat& image, int borderWidth = 20);
cv::Mat preprocessFrame(const cv::Mat& raw);
double extractCircularRoiMean(const cv::Mat& frame, const SpotInfo& spot);
void addBilinearSample(cv::Mat& accum, cv::Mat& weight, double x, double y, double value);
cv::Mat normalizeByWeight(const cv::Mat& accum, const cv::Mat& weight);
cv::Mat fillHolesByInpaint(const cv::Mat& image, const cv::Mat& weight);
cv::Mat makeGaussianPsf(int size, double sigma);
cv::Mat flipPsf(const cv::Mat& psf);
cv::Mat lucyRichardsonDeconvolution(const cv::Mat& input32f, const cv::Mat& psf32f, int iterations);
cv::Mat normalizeTo16U(const cv::Mat& src);