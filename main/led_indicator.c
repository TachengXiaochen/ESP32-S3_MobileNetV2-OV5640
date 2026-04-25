#include "led_indicator.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led_indicator";

// WS2812配置
#define LED_GPIO            GPIO_NUM_48
#define LED_RMT_CHANNEL     RMT_CHANNEL_0
#define LED_RESOLUTION_HZ   10000000  // 10MHz分辨率
#define LED_TICK_NS         100       // 100ns per tick
#define T0H_TICKS           4         // 0码高电平 400ns
#define T0L_TICKS           8         // 0码低电平 850ns
#define T1H_TICKS           8         // 1码高电平 800ns
#define T1L_TICKS           4         // 1码低电平 450ns
#define RESET_TICKS         600       // 复位信号 >50us

// WS2812颜色结构
typedef struct {
    uint8_t green;
    uint8_t red;
    uint8_t blue;
} ws2812_color_t;

// 亮度系数（0-255），默认50%亮度避免过亮
#define LED_BRIGHTNESS      128

/**
 * @brief 将RGB值转换为WS2812的GRB格式并发送
 */
static void ws2812_send_color(uint8_t r, uint8_t g, uint8_t b)
{
    // 应用亮度系数
    r = (uint16_t)r * LED_BRIGHTNESS / 255;
    g = (uint16_t)g * LED_BRIGHTNESS / 255;
    b = (uint16_t)b * LED_BRIGHTNESS / 255;
    
    // WS2812使用GRB顺序
    ws2812_color_t color = {
        .green = g,
        .red = r,
        .blue = b
    };
    
    // 构建RMT符号数组（24位数据 + 1个复位信号）
    rmt_symbol_word_t symbols[25];
    uint8_t *data = (uint8_t *)&color;
    
    for (int i = 0; i < 3; i++) {  // 3个字节
        for (int j = 7; j >= 0; j--) {  // 每个字节8位，高位先发
            if (data[i] & (1 << j)) {
                // 1码
                symbols[i * 8 + (7 - j)].level0 = 1;
                symbols[i * 8 + (7 - j)].duration0 = T1H_TICKS;
                symbols[i * 8 + (7 - j)].level1 = 0;
                symbols[i * 8 + (7 - j)].duration1 = T1L_TICKS;
            } else {
                // 0码
                symbols[i * 8 + (7 - j)].level0 = 1;
                symbols[i * 8 + (7 - j)].duration0 = T0H_TICKS;
                symbols[i * 8 + (7 - j)].level1 = 0;
                symbols[i * 8 + (7 - j)].duration1 = T0L_TICKS;
            }
        }
    }
    
    // 添加复位信号
    symbols[24].level0 = 0;
    symbols[24].duration0 = RESET_TICKS;
    symbols[24].level1 = 0;
    symbols[24].duration1 = 0;
    
    // 配置RMT通道
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    
    rmt_channel_handle_t tx_channel = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel));
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
    
    // 创建RMT编码器配置（使用copy encoder直接发送符号）
    rmt_copy_encoder_config_t copy_encoder_config = {};
    rmt_encoder_handle_t copy_encoder = NULL;
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder));
    
    // 发送数据
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };
    
    ESP_ERROR_CHECK(rmt_transmit(tx_channel, copy_encoder, symbols, sizeof(symbols), &transmit_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_channel, -1));
    
    // 清理
    rmt_del_encoder(copy_encoder);
    rmt_disable(tx_channel);
    rmt_del_channel(tx_channel);
}

void led_indicator_init(void)
{
    ESP_LOGI(TAG, "Initializing WS2812 LED on GPIO%d", LED_GPIO);
    
    // 初始化GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // 初始状态：关闭（红色表示摄像头关闭）
    led_camera_off();
    
    ESP_LOGI(TAG, "LED indicator initialized");
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_send_color(r, g, b);
}

void led_blink(uint8_t r, uint8_t g, uint8_t b, uint8_t count)
{
    if (count == 0 || count > 10) {
        ESP_LOGW(TAG, "Invalid blink count: %d", count);
        return;
    }
    
    // ✅ 修复：闪烁前先熄灭当前LED，避免视觉冲突
    ws2812_send_color(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(50));  // 短暂延迟确保熄灭
    
    for (int i = 0; i < count; i++) {
        // 点亮
        ws2812_send_color(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(200));  // 亮200ms
        
        // 熄灭
        ws2812_send_color(0, 0, 0);
        
        // 间隔（最后一次不间隔）
        if (i < count - 1) {
            vTaskDelay(pdMS_TO_TICKS(100));  // 间隔100ms
        }
    }
}

void led_off(void)
{
    ws2812_send_color(0, 0, 0);
}

void led_camera_off(void)
{
    // 红色常亮表示摄像头关闭
    ws2812_send_color(255, 0, 0);
}

void led_camera_registration(void)
{
    // 绿色常亮表示注册模式
    ws2812_send_color(0, 255, 0);
}

void led_camera_inventory(void)
{
    // 蓝色常亮表示盘点模式
    ws2812_send_color(0, 0, 255);
}

void led_capture_front(bool is_inventory)
{
    if (is_inventory) {
        // 盘点模式：蓝色闪烁1次
        led_blink(0, 0, 255, 1);
    } else {
        // 注册模式：绿色闪烁1次
        led_blink(0, 255, 0, 1);
    }
}

void led_capture_side(bool is_inventory)
{
    if (is_inventory) {
        // 盘点模式：蓝色闪烁2次
        led_blink(0, 0, 255, 2);
    } else {
        // 注册模式：绿色闪烁2次
        led_blink(0, 255, 0, 2);
    }
}

void led_capture_top(bool is_inventory)
{
    if (is_inventory) {
        // 盘点模式：蓝色闪烁3次
        led_blink(0, 0, 255, 3);
    } else {
        // 注册模式：绿色闪烁3次
        led_blink(0, 255, 0, 3);
    }
}
