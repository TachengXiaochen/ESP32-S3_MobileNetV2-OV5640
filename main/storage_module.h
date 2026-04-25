#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <stdbool.h>
#include "asset_manager.h"

// 资产保存结果枚举
typedef enum {
    SAVE_RESULT_SUCCESS_NEW = 0,      // 成功创建新资产
    SAVE_RESULT_SUCCESS_OVERWRITE = 1, // 成功覆盖已有资产
    SAVE_RESULT_FAILED = 2             // 保存失败
} save_result_t;

/**
 * @brief 初始化存储模块（SD卡）
 * @return true 成功, false 失败
 */
bool storage_module_init(void);

/**
 * @brief 保存资产记录到 SD 卡
 * @param record 资产记录指针
 * @return 保存结果枚举值
 */
save_result_t storage_module_save_asset(const asset_record_t *record);

#endif // STORAGE_MODULE_H