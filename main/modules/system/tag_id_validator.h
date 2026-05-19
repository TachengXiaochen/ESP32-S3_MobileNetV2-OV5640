#ifndef TAG_ID_VALIDATOR_H
#define TAG_ID_VALIDATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tag ID 格式规范
 * 
 * 长度：6字符（不含终止符）
 * 格式：0x + 4位十六进制
 * 示例：0x0001, 0x00AB, 0x1234, 0xFFFF
 * 范围：0x0001 - 0xFFFF（共65,535个唯一标识）
 * 大小写：不敏感（0xABCD = 0xabcd）
 */
#define TAG_ID_LEN          6       // "0xFFFF" = 6字符
#define TAG_ID_STR_LEN      7       // 含终止符
#define TAG_ID_MAX          0xFFFF  // 最大值
#define TAG_ID_MIN          0x0001  // 最小值

/**
 * @brief 验证 Tag ID 字符串格式
 * 
 * 验证规则：
 * - 必须以 "0x" 或 "0X" 开头
 * - 必须包含4位十六进制字符（0-9, A-F, a-f）
 * - 值必须在 0x0001 - 0xFFFF 范围内
 * - 不得为 0x0000
 * 
 * @param tag_id Tag ID字符串
 * @return true  格式正确且在有效范围内
 * @return false 格式错误或超出范围
 */
bool tag_id_validator_validate(const char *tag_id);

/**
 * @brief 标准化 Tag ID 为大写格式
 * 
 * 将小写十六进制转换为大写，确保一致的存储格式。
 * 例如: "0xabcd" → "0xABCD"
 * 
 * @param tag_id   输入输出参数，需预先分配 TAG_ID_STR_LEN 字节
 * @return true    标准化成功
 * @return false   输入为NULL或格式无效
 */
bool tag_id_validator_normalize(char *tag_id);

/**
 * @brief 将 Tag ID 字符串解析为 uint16_t 数值
 * 
 * @param tag_id  Tag ID 字符串（如 "0x0001"）
 * @param out_val 输出参数，解析后的数值
 * @return true   解析成功
 * @return false  输入无效或解析失败
 */
bool tag_id_validator_parse(const char *tag_id, uint16_t *out_val);

/**
 * @brief 将 uint16_t 数值格式化为 Tag ID 字符串
 * 
 * @param value   uint16_t 数值
 * @param out_buf 输出缓冲区，至少 TAG_ID_STR_LEN 字节
 * @param buf_size 缓冲区大小
 * @return true   格式化成功
 * @return false  缓冲区太小或参数无效
 */
bool tag_id_validator_format(uint16_t value, char *out_buf, size_t buf_size);

/**
 * @brief 获取 Tag ID 验证错误描述
 * 
 * @return 错误描述字符串（用于调试和用户提示）
 */
const char* tag_id_validator_get_error_string(void);

#ifdef __cplusplus
}
#endif

#endif /* TAG_ID_VALIDATOR_H */