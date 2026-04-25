#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <stdbool.h>
#include "asset_manager.h"

/**
 * @brief 初始化存储模块（SD卡）
 * @return true 成功, false 失败
 */
bool storage_module_init(void);

/**
 * @brief 保存资产记录到 SD 卡
 * @param record 资产记录指针
 * @return true 成功, false 失败
 */
bool storage_module_save_asset(const asset_record_t *record);

#endif // STORAGE_MODULE_H
