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
 * @brief 验证MAC地址格式
 * 
 * @param mac MAC地址字符串，格式应为 XX:XX:XX:XX:XX:XX
 * @return true 格式正确
 * @return false 格式错误
 */
bool cmd_handler_validate_mac(const char *mac);

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
void show_registration_step1(const char *mac);

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
void show_inventory_step1(const char *mac);

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
