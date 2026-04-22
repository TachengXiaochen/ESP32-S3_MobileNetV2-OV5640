#ifndef ASSET_MANAGER_H
#define ASSET_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 资产记录结构
#define MAC_ADDR_LEN 18
#define FEATURE_VEC_SIZE 1280

typedef struct {
    char mac_address[MAC_ADDR_LEN + 1];  // MAC地址字符串
    float front_feature[FEATURE_VEC_SIZE]; // 正面特征向量
    float side_feature[FEATURE_VEC_SIZE];  // 侧面特征向量
    float top_feature[FEATURE_VEC_SIZE];   // 顶部特征向量
    bool is_valid;                          // 记录是否有效
} asset_record_t;

// 存储模式枚举
typedef enum {
    STORAGE_MODE_SD_CARD = 0,  // SD卡存储（默认）
    STORAGE_MODE_SPIFFS = 1    // SPIFFS内部Flash存储
} storage_mode_t;

/**
 * @brief 初始化资产管理器（根据当前模式挂载对应存储）
 * @return ESP_OK表示成功
 */
esp_err_t asset_manager_init(void);

/**
 * @brief 保存资产记录到当前存储介质
 * @param record 资产记录指针
 * @return ESP_OK表示成功
 */
esp_err_t asset_save(const asset_record_t *record);

/**
 * @brief 从当前存储介质加载资产记录
 * @param mac_address MAC地址
 * @param record 输出参数，资产记录
 * @return ESP_OK表示成功，ESP_ERR_NOT_FOUND表示未找到
 */
esp_err_t asset_load(const char *mac_address, asset_record_t *record);

/**
 * @brief 删除资产记录
 * @param mac_address MAC地址
 * @return ESP_OK表示成功
 */
esp_err_t asset_delete(const char *mac_address);

/**
 * @brief 列出所有已注册的资产
 * @param count 输出参数，资产数量
 * @return ESP_OK表示成功
 */
esp_err_t asset_list_all(int *count);

/**
 * @brief 获取当前存储模式
 * @return 当前存储模式
 */
storage_mode_t asset_get_storage_mode(void);

/**
 * @brief 切换存储模式
 * @param mode 目标存储模式
 * @return ESP_OK表示成功，ESP_ERR_INVALID_STATE表示切换失败
 */
esp_err_t asset_switch_storage_mode(storage_mode_t mode);

/**
 * @brief 反初始化管理器（卸载存储）
 */
void asset_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // ASSET_MANAGER_H
