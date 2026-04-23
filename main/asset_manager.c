#include "asset_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "asset_manager";

// SD卡配置（ESP32-S3常见引脚）
// ⚠️ 重要: 根据实际硬件连接修改以下引脚定义
// 当前配置对应: CLK=GPIO39, CMD=GPIO38, D0=GPIO40
#define SD_PIN_CLK  39
#define SD_PIN_CMD  38
#define SD_PIN_D0   40
// 1位模式下不需要CS和D1-D3，但保留定义供参考
// #define SD_PIN_CS   37
// #define SD_PIN_D1   -1
// #define SD_PIN_D2   -1  
// #define SD_PIN_D3   -1

#define MOUNT_POINT_SD "/sdcard"
#define ASSET_DIR_SD   "/sdcard/assets"

// 存储模式（固定使用SD卡）
static storage_mode_t g_current_storage_mode = STORAGE_MODE_SD_CARD;
static bool g_storage_initialized = false;
static sdmmc_card_t *g_card = NULL;

/**
 * @brief 初始化SD卡
 */
static esp_err_t init_sd_card(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    // SD卡挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };

    // SDMMC主机配置（使用1位模式以节省引脚）
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;  // 使用1位模式
    
    // 【调试选项】如果SD卡初始化仍失败,可尝试降低时钟频率:
    // host.max_freq_khz = SDMMC_FREQ_PROBING;  // 400kHz (最稳定,用于调试)
    // host.max_freq_khz = 10000;               // 10MHz (中等速度)
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;     // 20MHz (默认,正常应工作)

    // SDMMC槽配置
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.width = 1;  // 1位模式
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // 启用内部上拉
    
    // 设置GPIO驱动能力(高编号GPIO需要更强的驱动)
    gpio_set_drive_capability(SD_PIN_CLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_PIN_CMD, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_PIN_D0, GPIO_DRIVE_CAP_3);
    
    ESP_LOGI(TAG, "SD card pins configured: CLK=%d, CMD=%d, D0=%d", 
             SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);

    // 挂载SD卡
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT_SD, &host, &slot_config, &mount_config, &g_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (0x%x - %s)", ret, esp_err_to_name(ret));
        
        // 详细错误诊断
        switch (ret) {
            case ESP_ERR_TIMEOUT:
                ESP_LOGE(TAG, "Possible causes:");
                ESP_LOGE(TAG, "  1. SD card not inserted properly");
                ESP_LOGE(TAG, "  2. Wrong pin configuration (CLK=%d, CMD=%d, D0=%d)", 
                         SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);
                ESP_LOGE(TAG, "  3. SD card damaged or incompatible");
                ESP_LOGE(TAG, "  4. Power supply insufficient");
                break;
            case ESP_ERR_INVALID_STATE:
                ESP_LOGE(TAG, "SD card already mounted or in invalid state");
                break;
            default:
                ESP_LOGE(TAG, "Unknown error code: 0x%x", ret);
                break;
        }
        
        return ret;
    }

    // 打印SD卡信息
    sdmmc_card_print_info(stdout, g_card);
    
    // 创建资产目录
    struct stat st;
    if (stat(ASSET_DIR_SD, &st) != 0) {
        ESP_LOGI(TAG, "Creating assets directory: %s", ASSET_DIR_SD);
        mkdir(ASSET_DIR_SD, 0755);
    }

    ESP_LOGI(TAG, "SD card initialized successfully");
    return ESP_OK;
}

/**
 * @brief 获取当前存储路径
 */
static void get_current_asset_dir(char *path, size_t path_size)
{
    snprintf(path, path_size, "%s", ASSET_DIR_SD);
}

/**
 * @brief 生成资产文件路径
 */
static void get_asset_file_path(const char *mac_address, char *path, size_t path_size, const char *extension)
{
    char asset_dir[64];
    get_current_asset_dir(asset_dir, sizeof(asset_dir));

    // 将MAC地址中的':'替换为'_'作为文件名
    char safe_mac[MAC_ADDR_LEN + 1];
    strncpy(safe_mac, mac_address, MAC_ADDR_LEN);
    safe_mac[MAC_ADDR_LEN] = '\0';
    
    for (int i = 0; i < strlen(safe_mac); i++) {
        if (safe_mac[i] == ':') {
            safe_mac[i] = '_';
        }
    }
    
    snprintf(path, path_size, "%s/%s.%s", asset_dir, safe_mac, extension);
}

/**
 * @brief 初始化资产管理器（固定使用SD卡）
 */
esp_err_t asset_manager_init(void)
{
    if (g_storage_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = init_sd_card();

    if (ret == ESP_OK) {
        g_storage_initialized = true;
    }

    return ret;
}

/**
 * @brief 保存资产记录到当前存储介质
 */
esp_err_t asset_save(const asset_record_t *record)
{
    if (!g_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!record || !record->is_valid) {
        ESP_LOGE(TAG, "Invalid asset record");
        return ESP_ERR_INVALID_ARG;
    }

    char file_path[128];
    get_asset_file_path(record->mac_address, file_path, sizeof(file_path), "dat");

    ESP_LOGI(TAG, "Saving asset to: %s", file_path);

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", file_path);
        return ESP_FAIL;
    }

    // 【关键修复】直接写入传入的record指针,避免在栈上创建副本
    // 原代码: fwrite(record, sizeof(asset_record_t), 1, f) 是正确的
    // 但调用方在栈上创建了15KB的临时变量,导致栈溢出风险
    size_t written = fwrite(record, sizeof(asset_record_t), 1, f);
    fclose(f);

    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write asset record");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Asset saved successfully for MAC: %s", record->mac_address);
    return ESP_OK;
}

/**
 * @brief 从当前存储介质加载资产记录
 */
esp_err_t asset_load(const char *mac_address, asset_record_t *record)
{
    if (!g_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!mac_address || !record) {
        return ESP_ERR_INVALID_ARG;
    }

    char file_path[128];
    get_asset_file_path(mac_address, file_path, sizeof(file_path), "dat");

    // 检查文件是否存在
    struct stat st;
    if (stat(file_path, &st) != 0) {
        ESP_LOGW(TAG, "Asset file not found: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Loading asset from: %s", file_path);

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", file_path);
        return ESP_FAIL;
    }

    // 读取资产记录
    size_t read_count = fread(record, sizeof(asset_record_t), 1, f);
    fclose(f);

    if (read_count != 1) {
        ESP_LOGE(TAG, "Failed to read asset record");
        return ESP_FAIL;
    }

    if (!record->is_valid) {
        ESP_LOGW(TAG, "Asset record is invalid");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Asset loaded successfully for MAC: %s", record->mac_address);
    return ESP_OK;
}

/**
 * @brief 删除资产记录
 */
esp_err_t asset_delete(const char *mac_address)
{
    if (!g_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!mac_address) {
        return ESP_ERR_INVALID_ARG;
    }

    char file_path[128];
    get_asset_file_path(mac_address, file_path, sizeof(file_path), "dat");

    ESP_LOGI(TAG, "Deleting asset: %s", file_path);

    if (f_unlink(file_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete asset file: %s", file_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Asset deleted successfully");
    return ESP_OK;
}

/**
 * @brief 列出所有已注册的资产
 */
esp_err_t asset_list_all(int *count)
{
    if (!g_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!count) {
        return ESP_ERR_INVALID_ARG;
    }

    char asset_dir[64];
    get_current_asset_dir(asset_dir, sizeof(asset_dir));

    DIR *dir = opendir(asset_dir);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open assets directory: %s", asset_dir);
        return ESP_FAIL;
    }

    struct dirent *entry;
    int asset_count = 0;

    printf("\n=== Registered Assets (%s) ===\n", 
           g_current_storage_mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS");
    while ((entry = readdir(dir)) != NULL) {
        // 只处理.dat文件
        if (strstr(entry->d_name, ".dat") != NULL) {
            // 将文件名中的'_'转换回':'显示
            char mac_display[MAC_ADDR_LEN + 1];
            strncpy(mac_display, entry->d_name, sizeof(mac_display) - 1);
            mac_display[sizeof(mac_display) - 1] = '\0';
            
            // 移除扩展名
            char *dot = strstr(mac_display, ".dat");
            if (dot) {
                *dot = '\0';
            }
            
            // 将'_'转换回':'
            for (int i = 0; i < strlen(mac_display); i++) {
                if (mac_display[i] == '_') {
                    mac_display[i] = ':';
                }
            }
            
            printf("  [%d] MAC: %s\n", asset_count + 1, mac_display);
            asset_count++;
        }
    }
    closedir(dir);

    *count = asset_count;
    printf("Total: %d assets\n========================\n\n", asset_count);
    
    return ESP_OK;
}

/**
 * @brief 获取当前存储模式
 */
storage_mode_t asset_get_storage_mode(void)
{
    return g_current_storage_mode;
}

/**
 * @brief 切换存储模式（已禁用，固定为SD卡）
 */
esp_err_t asset_switch_storage_mode(storage_mode_t mode)
{
    if (mode != STORAGE_MODE_SD_CARD) {
        ESP_LOGW(TAG, "Storage mode is fixed to SD Card. Switching failed.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    return ESP_OK;
}

/**
 * @brief 反初始化管理器（卸载SD卡）
 */
void asset_manager_deinit(void)
{
    if (!g_storage_initialized) {
        return; // 未初始化，直接返回
    }

    if (g_card != NULL) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT_SD, g_card);
        g_card = NULL;
    }
    
    g_storage_initialized = false;
}
