#ifndef CMD_HANDLER_H
#define CMD_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化命令处理器
 * 
 * @note 应在系统启动时调用一次
 */
void cmd_handler_init(void);

/**
 * @brief 处理单条串口命令
 * 
 * @param cmd_line 完整的命令字符串（已去除换行符）
 * 
 * @note 此函数会解析命令并分发到对应的任务队列
 */
void cmd_handler_process(const char *cmd_line);

/**
 * @brief 显示帮助信息到串口
 * 
 * @note 用于响应 help/? 命令
 */
void cmd_handler_show_help(void);

/**
 * @brief ⭐ 验证 Tag ID 格式
 * 
 * @param tag_id Tag ID 字符串，格式应为 0x0001-0xFFFF
 * @return true 格式正确
 * @return false 格式错误
 */
bool cmd_handler_validate_tag_id(const char *tag_id);

/**
 * @brief ⭐ 显示验证式更新流程的引导信息
 * 
 * @param tag_id      要验证的 Tag ID
 * @param item_name   现有物品名称
 * @param storage_area 存放区域
 * @param current_qty  当前数量
 */
void show_verification_existing_guide(const char *tag_id, const char *item_name, 
                                       char storage_area, uint32_t current_qty);

/**
 * @brief ⭐ 显示验证通过后的累加数量引导
 * 
 * @param tag_id Tag ID
 * @param item_name 物品名称
 * @param current_qty 当前数量
 */
void show_verification_add_qty_guide(const char *tag_id, const char *item_name, uint32_t current_qty);

/**
 * @brief ⭐ 显示验证失败信息
 * 
 * @param confidence 置信度
 * @param threshold 阈值
 */
void show_verification_failed(float confidence, float threshold);

/**
 * @brief ⭐ 显示验证重试提示
 */
void show_verification_retry_guide(void);

/**
 * @brief 显示主菜单（开机/任务完成后）
 * 
 * @note 用于引导用户进行下一步操作
 */
void show_main_menu(void);

/**
 * @brief 显示注册流程第一步引导
 * 
 * @param mac MAC地址字符串
 */
void show_registration_step1(const char *tag_id);

/**
 * @brief 显示注册流程第二步引导
 */
void show_registration_step2(void);

/**
 * @brief 显示注册流程第三步引导
 */
void show_registration_step3(void);

/**
 * @brief 显示盘点流程第一步引导
 * 
 * @param mac MAC地址字符串
 */
void show_inventory_step1(const char *tag_id);

/**
 * @brief 显示盘点流程第二步引导
 */
void show_inventory_step2(void);

/**
 * @brief 显示盘点流程第三步引导
 */
void show_inventory_step3(void);

#ifdef __cplusplus
}
#endif

#endif /* CMD_HANDLER_H */
