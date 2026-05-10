#ifndef LED_INDICATOR_H
#define LED_INDICATOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化WS2812 LED指示器
 * 
 * @note 应在系统启动时调用一次
 */
void led_indicator_init(void);

/**
 * @brief 设置LED颜色（常亮）
 * 
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief LED闪烁指定次数
 * 
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 * @param count 闪烁次数 (1-10)
 * 
 * @note 此函数会阻塞直到闪烁完成
 */
void led_blink(uint8_t r, uint8_t g, uint8_t b, uint8_t count);

/**
 * @brief 关闭LED
 */
void led_off(void);

/**
 * @brief 摄像头状态指示 - 关闭（红色）
 */
void led_camera_off(void);

/**
 * @brief 摄像头状态指示 - 注册模式（绿色）
 */
void led_camera_registration(void);

/**
 * @brief 摄像头状态指示 - 盘点模式（蓝色）
 */
void led_camera_inventory(void);

/**
 * @brief 拍摄反馈 - 正面（闪烁1次）
 * 
 * @param is_inventory 是否为盘点模式
 */
void led_capture_front(bool is_inventory);

/**
 * @brief 拍摄反馈 - 侧面（闪烁2次）
 * 
 * @param is_inventory 是否为盘点模式
 */
void led_capture_side(bool is_inventory);

/**
 * @brief 拍摄反馈 - 顶部（闪烁3次）
 * 
 * @param is_inventory 是否为盘点模式
 */
void led_capture_top(bool is_inventory);

#ifdef __cplusplus
}
#endif

#endif /* LED_INDICATOR_H */
