/**
 * @file blur_detection.c
 * @brief 模糊度检测实现 - 拉普拉斯方差检测算法
 */

#include "blur_detection.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

#define TAG "blur_detection"

/**
 * @brief 3x3拉普拉斯卷积核
 */
static const int laplacian_kernel[3][3] = {
    {0,  1, 0},
    {1, -4, 1},
    {0,  1, 0}
};

/**
 * @brief 将RGB888图像转换为灰度图像
 * 
 * @param rgb 输入RGB图像数据
 * @param width 图像宽度
 * @param height 图像高度
 * @param gray 输出灰度图像缓冲区（需要预先分配 width*height 字节）
 */
static void rgb_to_gray(const uint8_t* rgb, int width, int height, uint8_t* gray) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            // 使用标准灰度转换公式: Y = 0.299R + 0.587G + 0.114B
            gray[y * width + x] = (uint8_t)(
                0.299f * rgb[idx] +      // R
                0.587f * rgb[idx + 1] +  // G
                0.114f * rgb[idx + 2]    // B
            );
        }
    }
}

/**
 * @brief 对灰度图像应用拉普拉斯卷积
 * 
 * @param gray 输入灰度图像
 * @param width 图像宽度
 * @param height 图像高度
 * @param laplacian 输出拉普拉斯响应（需要预先分配 width*height 字节）
 */
static void apply_laplacian(const uint8_t* gray, int width, int height, int16_t* laplacian) {
    // 跳过边缘像素（卷积核大小为3x3）
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int sum = 0;
            
            // 应用3x3拉普拉斯卷积核
            for (int ky = -1; ky <= 1; ky++) {
                for (int kx = -1; kx <= 1; kx++) {
                    int pixel = gray[(y + ky) * width + (x + kx)];
                    int kernel_val = laplacian_kernel[ky + 1][kx + 1];
                    sum += pixel * kernel_val;
                }
            }
            
            laplacian[y * width + x] = (int16_t)sum;
        }
    }
    
    // 边缘像素设为0（无法计算卷积）
    for (int y = 0; y < height; y++) {
        laplacian[y * width] = 0;
        laplacian[y * width + width - 1] = 0;
    }
    for (int x = 0; x < width; x++) {
        laplacian[x] = 0;
        laplacian[(height - 1) * width + x] = 0;
    }
}

/**
 * @brief 计算拉普拉斯响应的方差
 * 
 * @param laplacian 拉普拉斯响应数组
 * @param width 图像宽度
 * @param height 图像高度
 * @return float 方差值
 */
static float compute_variance(const int16_t* laplacian, int width, int height) {
    // 计算均值（跳过边缘像素）
    long long sum = 0;
    int count = 0;
    
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int16_t val = laplacian[y * width + x];
            sum += val * val;  // 使用平方值计算方差
            count++;
        }
    }
    
    if (count == 0) {
        return 0.0f;
    }
    
    float mean = (float)sum / count;
    
    // 计算方差
    float variance = 0.0f;
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int16_t val = laplacian[y * width + x];
            float diff = (float)(val * val) - mean;
            variance += diff * diff;
        }
    }
    
    return variance / count;
}

float blur_detect_laplacian_variance(const image_t* img) {
    if (!img || !img->data || img->width <= 0 || img->height <= 0 || img->channels != 3) {
        return 0.0f;
    }
    
    int width = img->width;
    int height = img->height;
    int total_pixels = width * height;
    
    // 分配临时缓冲区
    uint8_t* gray = (uint8_t*)malloc(total_pixels);
    int16_t* laplacian = (int16_t*)malloc(total_pixels * sizeof(int16_t));
    
    if (!gray || !laplacian) {
        free(gray);
        free(laplacian);
        return 0.0f;
    }
    
    // 转换为灰度
    rgb_to_gray(img->data, width, height, gray);
    
    // 应用拉普拉斯卷积
    apply_laplacian(gray, width, height, laplacian);
    
    // 计算方差
    float variance = compute_variance(laplacian, width, height);
    
    // 清理内存
    free(gray);
    free(laplacian);
    
    return variance;
}

bool blur_detect_is_sharp(const image_t* img, float threshold) {
    float variance = blur_detect_laplacian_variance(img);
    
    if (variance <= 0.0f) {
        return false;
    }
    
    return variance > threshold;
}
