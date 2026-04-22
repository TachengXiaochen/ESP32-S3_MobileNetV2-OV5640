#include "asset_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "asset_manager";

// SD卡配置（ESP32-S3常见引脚）
#define SD_PIN_CLK  14
#define SD_PIN_CMD  15
#define SD_PIN_D0   2
#define SD_PIN_D1   3
#define SD_PIN_D2   4
#define SD_PIN_D3   5

#define MOUNT_POINT_SD "/sdcard"
#define ASSET_DIR_SD   "/sdcard/assets"
#define MOUNT_POINT_SPIFFS "/spiffs"
#define ASSET_DIR_SPIFFS   "/spiffs/assets"

// 存储模式
static storage_mode_t g_current_storage_mode = STORAGE_MODE_SD_CARD;
static bool g_storage_initialized = false;
static sdmmc_card_t *g_card = NULL;

/**
 * @brief 初始化SD卡
 */
static esp_err_t init_sd_card(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    // 选项：挂载文件系统
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,  // 如果挂载失败则格式化
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };

    // SDMMC主机配置（使用1位模式以节省引脚）
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;  // 使用1位模式
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // SDMMC槽配置
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.width = 1;  // 1位模式
    
    // 设置GPIO功能
    gpio_set_drive_capability(SD_PIN_CLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_PIN_CMD, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_PIN_D0, GPIO_DRIVE_CAP_3);

    // 挂载SD卡
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT_SD, &host, &slot_config, &mount_config, &g_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s)", esp_err_to_name(ret));
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
 * @brief 初始化SPIFFS
 */
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_POINT_SPIFFS,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount or format SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Partition size: total=%d, used=%d", total, used);

    // 创建资产目录
    struct stat st;
    if (stat(ASSET_DIR_SPIFFS, &st) != 0) {
        ESP_LOGI(TAG, "Creating assets directory: %s", ASSET_DIR_SPIFFS);
        mkdir(ASSET_DIR_SPIFFS, 0755);
    }

    ESP_LOGI(TAG, "SPIFFS initialized successfully");
    return ESP_OK;
}

/**
 * @brief 获取当前存储路径
 */
static void get_current_asset_dir(char *path, size_t path_size)
{
    if (g_current_storage_mode == STORAGE_MODE_SD_CARD) {
        snprintf(path, path_size, "%s", ASSET_DIR_SD);
    } else {
        snprintf(path, path_size, "%s", ASSET_DIR_SPIFFS);
    }
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
 * @brief 初始化资产管理器（根据当前模式挂载对应存储）
 */
esp_err_t asset_manager_init(void)
{
    if (g_storage_initialized) {
        ESP_LOGW(TAG, "Storage already initialized");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    
    if (g_current_storage_mode == STORAGE_MODE_SD_CARD) {
        ret = init_sd_card();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD card init failed, fallback to SPIFFS mode");
            g_current_storage_mode = STORAGE_MODE_SPIFFS;
            ret = init_spiffs();
        }
    } else {
        ret = init_spiffs();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS init failed");
            return ret;
        }
    }

    if (ret == ESP_OK) {
        g_storage_initialized = true;
        ESP_LOGI(TAG, "Asset manager initialized with %s mode", 
                 g_current_storage_mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS");
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

    // 写入资产记录
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
 * @brief 切换存储模式
 */
esp_err_t asset_switch_storage_mode(storage_mode_t mode)
{
    if (g_current_storage_mode == mode) {
        ESP_LOGI(TAG, "Already in %s mode", 
                 mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS");
        return ESP_OK;
    }

    // 先反初始化当前存储
    asset_manager_deinit();

    // 切换模式
    g_current_storage_mode = mode;

    // 初始化新模式
    esp_err_t ret = asset_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize new storage mode: %s", 
                 mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS");
        // 回滚到之前的模式
        g_current_storage_mode = (mode == STORAGE_MODE_SD_CARD) ? STORAGE_MODE_SPIFFS : STORAGE_MODE_SD_CARD;
        asset_manager_init(); // 尝试恢复到旧模式
        return ret;
    }

    ESP_LOGI(TAG, "Switched to %s mode successfully", 
             mode == STORAGE_MODE_SD_CARD ? "SD Card" : "SPIFFS");
    return ESP_OK;
}

/**
 * @brief 反初始化管理器（卸载存储）
 */
void asset_manager_deinit(void)
{
    if (g_storage_initialized) {
        if (g_current_storage_mode == STORAGE_MODE_SD_CARD) {
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT_SD, g_card);
            g_card = NULL;
            ESP_LOGI(TAG, "SD card unmounted");
        } else {
            esp_vfs_spiffs_unregister(MOUNT_POINT_SPIFFS);
            ESP_LOGI(TAG, "SPIFFS unmounted");
        }
        g_storage_initialized = false;
    }
}
