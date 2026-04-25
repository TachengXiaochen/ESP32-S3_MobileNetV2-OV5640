#include "asset_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "driver/uart.h"  // 新增：用于UART输出
#include "esp_task_wdt.h"  // 看门狗复位函数
#include "ff.h"  // FATFS头文件，提供SS()宏和FATFS结构体定义
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

    // 【关键修复】挂载前增加较长延迟，确保 MobileNet 加载后的堆状态稳定
    vTaskDelay(pdMS_TO_TICKS(200));

    // 配置挂载参数
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

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
    
    // 创建资产目录（确保父目录存在）
    struct stat st;
    
    // 首先检查挂载点是否存在
    if (stat(MOUNT_POINT_SD, &st) != 0) {
        ESP_LOGE(TAG, "Mount point %s does not exist!", MOUNT_POINT_SD);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查并创建assets目录
    if (stat(ASSET_DIR_SD, &st) != 0) {
        ESP_LOGI(TAG, "Creating assets directory: %s", ASSET_DIR_SD);
        int ret = mkdir(ASSET_DIR_SD, 0755);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to create directory: %s (errno=%d)", ASSET_DIR_SD, errno);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Directory created successfully");
    } else {
        ESP_LOGI(TAG, "Assets directory already exists");
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

    // ✅ 修复：将MAC地址中的':'替换为'_'作为文件名（符合FATFS 8.3格式限制）
    // 例如: AA:BB:CC:DD:EE:FF -> AA_BB_CC_DD_EE_FF.dat
    char safe_mac[MAC_ADDR_LEN + 1];
    strncpy(safe_mac, mac_address, MAC_ADDR_LEN);
    safe_mac[MAC_ADDR_LEN] = '\0';
    
    for (int i = 0; i < strlen(safe_mac); i++) {
        if (safe_mac[i] == ':') {
            safe_mac[i] = '_';
        }
    }
    
    snprintf(path, path_size, "%s/%s.%s", asset_dir, safe_mac, extension);
    
    ESP_LOGI(TAG, "Generated filename: %s from MAC: %s", safe_mac, mac_address);
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
 * @param record 资产记录指针
 * @param is_overwrite 输出参数，是否为覆盖操作（可为NULL）
 * @return ESP_OK表示成功
 */
esp_err_t asset_save(const asset_record_t *record, bool *is_overwrite)
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

    // ✅ 检查文件是否已存在，判断是否为覆盖操作
    struct stat check_st;
    bool overwrite = (stat(file_path, &check_st) == 0);
    
    if (is_overwrite != NULL) {
        *is_overwrite = overwrite;
    }
    
    if (overwrite) {
        ESP_LOGW(TAG, "Asset already exists for MAC: %s, will overwrite", record->mac_address);
    } else {
        ESP_LOGI(TAG, "Creating new asset for MAC: %s", record->mac_address);
    }
    
    ESP_LOGI(TAG, "Saving asset to: %s", file_path);

    // ✅ 关键修复：AI推理后给硬件控制器足够的恢复时间
    // 防止PSRAM DMA竞争导致SD卡通信超时
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(300));  // 增加延迟到300ms

    // 【新增】写入前检查SD卡剩余空间
    esp_err_t space_check = asset_check_write_space(sizeof(asset_record_t), 10);
    if (space_check == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "Cannot save asset: SD card is full!");
        return ESP_ERR_NO_MEM;
    } else if (space_check == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Proceeding with save despite low space warning...");
    }

    // 【关键修复】在打开文件前，先确保目录存在
    // 直接使用ASSET_DIR_SD常量，避免路径拼接错误
    ESP_LOGI(TAG, "Checking directory: %s", ASSET_DIR_SD);
    
    struct stat st;
    if (stat(ASSET_DIR_SD, &st) != 0) {
        ESP_LOGW(TAG, "Assets directory not found, creating: %s", ASSET_DIR_SD);
        
        // 使用递归创建方式，先确保/sdcard存在
        if (stat(MOUNT_POINT_SD, &st) != 0) {
            ESP_LOGE(TAG, "Mount point %s does not exist! SD card may not be mounted.", MOUNT_POINT_SD);
            return ESP_ERR_INVALID_STATE;
        }
        
        int ret = mkdir(ASSET_DIR_SD, 0755);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to create directory: %s (errno=%d - %s)", 
                     ASSET_DIR_SD, errno, strerror(errno));
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Directory created successfully");
    } else {
        ESP_LOGI(TAG, "Directory already exists");
    }

    // ✅ 移除临时测试代码，减少不必要的IO操作
    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", file_path);
        ESP_LOGE(TAG, "Error code: %d - %s", errno, strerror(errno));
        
        // 常见错误诊断
        if (errno == ENOSPC) {
            ESP_LOGE(TAG, "SD card is FULL! Please free up space.");
        } else if (errno == EACCES) {
            ESP_LOGE(TAG, "Permission denied. Check SD card write protection.");
        } else if (errno == EIO) {
            ESP_LOGE(TAG, "IO error. SD card may be corrupted or disconnected.");
        }
        
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File opened successfully, path length: %d bytes", strlen(file_path));

    // 写入资产记录
    size_t written = fwrite(record, sizeof(asset_record_t), 1, f);
    fclose(f);

    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write asset record");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Asset saved successfully for MAC: %s (%s)", 
             record->mac_address, overwrite ? "OVERWRITE" : "NEW");
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

    // 【关键修复】在执行文件IO前，先复位看门狗并短暂延迟
    // 防止之前的AI推理或摄像头操作留下的DMA/PSRAM竞争导致堆损坏
    esp_task_wdt_reset();
    
    // 打印堆状态用于诊断
    ESP_LOGI(TAG, "Free heap before load: %u", (unsigned int)esp_get_free_heap_size());
    
    // 【关键修复】大幅增加延迟，给硬件控制器足够的恢复时间
    vTaskDelay(pdMS_TO_TICKS(300)); 

    char file_path[128];
    get_asset_file_path(mac_address, file_path, sizeof(file_path), "dat");

    // 检查文件是否存在
    struct stat st;
    if (stat(file_path, &st) != 0) {
        ESP_LOGW(TAG, "Asset file not found: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Loading asset from: %s", file_path);

    // 【关键修复】在打开文件前，再次确保看门狗复位并短暂延迟
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));

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

    // 【新增】显示存储空间信息
    uint64_t total_bytes, used_bytes, free_bytes;
    esp_err_t ret = asset_get_storage_info(&total_bytes, &used_bytes, &free_bytes);
    if (ret == ESP_OK) {
        float total_mb = (float)total_bytes / (1024.0f * 1024.0f);
        float used_mb = (float)used_bytes / (1024.0f * 1024.0f);
        float free_mb = (float)free_bytes / (1024.0f * 1024.0f);
        float usage_percent = ((float)used_bytes / (float)total_bytes) * 100.0f;
        
        printf("\n=== Storage Information ===\n");
        printf("  Total: %.2f MB\n", total_mb);
        printf("  Used:  %.2f MB (%.1f%%)\n", used_mb, usage_percent);
        printf("  Free:  %.2f MB (%.1f%%)\n", free_mb, 100.0f - usage_percent);
        
        // 警告：空间不足
        if (usage_percent > 90.0f) {
            printf("  ⚠️  WARNING: SD card is almost full!\n");
        } else if (usage_percent > 80.0f) {
            printf("  ⚡ NOTICE: SD card space is running low.\n");
        }
        printf("===========================\n\n");
    }

    printf("=== Registered Assets (%s) ===\n", 
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
 * @brief 获取SD卡存储空间信息
 */
esp_err_t asset_get_storage_info(uint64_t *total_bytes, uint64_t *used_bytes, uint64_t *free_bytes)
{
    if (!g_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!total_bytes || !used_bytes || !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }

    FATFS *fs;
    DWORD free_clusters, total_clusters;
    
    // 获取文件系统信息
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to get filesystem info: %d", res);
        return ESP_FAIL;
    }

    total_clusters = fs->n_fatent - 2;
    
    // 【修复】使用FATFS结构体成员直接计算，避免使用SS()宏
    // csize = clusters per sector, ssize = sector size in bytes
    uint32_t sectors_per_cluster = fs->csize;
    uint32_t bytes_per_sector = 512;  // SD卡标准扇区大小为512字节
    
    // 计算字节数
    *total_bytes = (uint64_t)total_clusters * sectors_per_cluster * bytes_per_sector;
    *free_bytes = (uint64_t)free_clusters * sectors_per_cluster * bytes_per_sector;
    *used_bytes = *total_bytes - *free_bytes;

    return ESP_OK;
}

/**
 * @brief 检查写入前的剩余空间是否充足
 * @param required_bytes 需要写入的字节数
 * @param warning_threshold_percent 警告阈值百分比（0-100），默认10%
 * @return ESP_OK表示空间充足，ESP_ERR_NO_MEM表示空间不足，ESP_ERR_INVALID_STATE表示空间紧张但可写入
 */
esp_err_t asset_check_write_space(size_t required_bytes, uint8_t warning_threshold_percent)
{
    if (!g_storage_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 默认警告阈值为10%
    if (warning_threshold_percent == 0 || warning_threshold_percent > 100) {
        warning_threshold_percent = 10;
    }

    uint64_t total_bytes, used_bytes, free_bytes;
    esp_err_t ret = asset_get_storage_info(&total_bytes, &used_bytes, &free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    // 检查是否有足够空间
    if (free_bytes < required_bytes) {
        ESP_LOGE(TAG, "Insufficient space! Required: %zu bytes, Available: %llu bytes", 
                 required_bytes, free_bytes);
        return ESP_ERR_NO_MEM;
    }

    // 计算使用百分比
    float usage_percent = ((float)used_bytes / (float)total_bytes) * 100.0f;
    float free_percent = 100.0f - usage_percent;

    // 检查是否低于警告阈值
    if (free_percent < warning_threshold_percent) {
        ESP_LOGW(TAG, "WARNING: SD card space is running low!");
        ESP_LOGW(TAG, "  Total: %.2f MB, Used: %.2f MB (%.1f%%), Free: %.2f MB (%.1f%%)",
                 (float)total_bytes / (1024.0f * 1024.0f),
                 (float)used_bytes / (1024.0f * 1024.0f),
                 usage_percent,
                 (float)free_bytes / (1024.0f * 1024.0f),
                 free_percent);
        return ESP_ERR_INVALID_STATE;  // 返回警告但仍可写入
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

/**
 * @brief 通过UART列出所有资产（用于cmd_handler）
 */
void asset_list_uart(void)
{
    if (!g_storage_initialized) {
        ESP_LOGE(TAG, "Storage not initialized");
        uart_write_bytes(UART_NUM_0, "\r\n[ASSET LIST]\r\n", 15);
        uart_write_bytes(UART_NUM_0, "Failed to list assets or storage not initialized.\r\n", 52);
        return;
    }

    int count = 0;
    esp_err_t ret = asset_list_all(&count);
    
    if (ret != ESP_OK) {
        uart_write_bytes(UART_NUM_0, "\r\n[ASSET LIST]\r\n", 15);
        uart_write_bytes(UART_NUM_0, "Failed to list assets.\r\n", 25);
        return;
    }

    // 显示存储空间信息
    uint64_t total_bytes, used_bytes, free_bytes;
    ret = asset_get_storage_info(&total_bytes, &used_bytes, &free_bytes);
    if (ret == ESP_OK) {
        char info_buf[256];
        float total_mb = (float)total_bytes / (1024.0f * 1024.0f);
        float used_mb = (float)used_bytes / (1024.0f * 1024.0f);
        float free_mb = (float)free_bytes / (1024.0f * 1024.0f);
        float usage_percent = ((float)used_bytes / (float)total_bytes) * 100.0f;
        
        snprintf(info_buf, sizeof(info_buf),
                 "\r\n[ASSET LIST]\r\n\r\n"
                 "=== Storage Information ===\r\n"
                 "  Total: %.2f MB\r\n"
                 "  Used:  %.2f MB (%.1f%%)\r\n"
                 "  Free:  %.2f MB (%.1f%%)\r\n"
                 "===========================\r\n\r\n",
                 total_mb, used_mb, usage_percent, free_mb, 100.0f - usage_percent);
        uart_write_bytes(UART_NUM_0, info_buf, strlen(info_buf));
    }

    // 列出资产
    char list_buf[512];
    snprintf(list_buf, sizeof(list_buf), "=== Registered Assets (SD Card) ===\r\n");
    uart_write_bytes(UART_NUM_0, list_buf, strlen(list_buf));

    // 重新读取目录以列出文件
    char asset_dir[64];
    get_current_asset_dir(asset_dir, sizeof(asset_dir));

    DIR *dir = opendir(asset_dir);
    if (!dir) {
        uart_write_bytes(UART_NUM_0, "Failed to open assets directory.\r\n", 35);
        return;
    }

    struct dirent *entry;
    int index = 1;
    while ((entry = readdir(dir)) != NULL) {
        // 跳过 "." 和 ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // 只显示.bin文件
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".bin") == 0) {
            // 提取MAC地址（文件名格式: XX_XX_XX_XX_XX_XX.bin）
            char mac_display[32];
            strncpy(mac_display, entry->d_name, sizeof(mac_display) - 1);
            mac_display[sizeof(mac_display) - 1] = '\0';
            
            // 将下划线替换为冒号
            for (int i = 0; mac_display[i] != '\0' && mac_display[i] != '.'; i++) {
                if (mac_display[i] == '_') {
                    mac_display[i] = ':';
                }
            }
            
            char item_buf[64];
            snprintf(item_buf, sizeof(item_buf), "  [%d] MAC: %s\r\n", index++, mac_display);
            uart_write_bytes(UART_NUM_0, item_buf, strlen(item_buf));
        }
    }

    closedir(dir);

    char summary_buf[64];
    snprintf(summary_buf, sizeof(summary_buf), 
             "Total: %d assets\r\n========================\r\n", count);
    uart_write_bytes(UART_NUM_0, summary_buf, strlen(summary_buf));
}
