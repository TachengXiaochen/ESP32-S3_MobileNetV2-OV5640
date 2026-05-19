#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "tag_id_validator.h"

static const char *g_last_error = NULL;

/**
 * @brief 验证 Tag ID 字符串格式
 */
bool tag_id_validator_validate(const char *tag_id)
{
    g_last_error = NULL;
    
    if (tag_id == NULL) {
        g_last_error = "Input is NULL";
        return false;
    }
    
    size_t len = strlen(tag_id);
    if (len != TAG_ID_LEN) {
        g_last_error = "Length must be 6 characters (e.g. 0x0001)";
        return false;
    }
    
    // 验证 "0x" 前缀
    if (!(tag_id[0] == '0' && (tag_id[1] == 'x' || tag_id[1] == 'X'))) {
        g_last_error = "Must start with '0x' prefix";
        return false;
    }
    
    // 验证4位十六进制字符
    for (int i = 2; i < TAG_ID_LEN; i++) {
        char c = tag_id[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f'))) {
            g_last_error = "Contains invalid hex character";
            return false;
        }
    }
    
    // 验证范围：0x0001 - 0xFFFF
    uint16_t value = 0;
    for (int i = 2; i < TAG_ID_LEN; i++) {
        value <<= 4;
        char c = tag_id[i];
        if (c >= '0' && c <= '9') {
            value += (c - '0');
        } else if (c >= 'A' && c <= 'F') {
            value += (c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            value += (c - 'a' + 10);
        }
    }
    
    if (value < TAG_ID_MIN) {
        g_last_error = "Value out of range: minimum is 0x0001";
        return false;
    }
    
    if (value > TAG_ID_MAX) {
        g_last_error = "Value out of range: maximum is 0xFFFF";
        return false;
    }
    
    return true;
}

/**
 * @brief 标准化 Tag ID 为大写格式
 */
bool tag_id_validator_normalize(char *tag_id)
{
    if (tag_id == NULL) {
        g_last_error = "Input is NULL";
        return false;
    }
    
    size_t len = strlen(tag_id);
    if (len != TAG_ID_LEN) {
        g_last_error = "Length mismatch for normalization";
        return false;
    }
    
    // 转换为大写：0x 前缀保持小写，十六进制字符转大写
    for (int i = 2; i < (int)len; i++) {
        tag_id[i] = toupper((unsigned char)tag_id[i]);
    }
    
    return true;
}

/**
 * @brief 将 Tag ID 字符串解析为 uint16_t 数值
 */
bool tag_id_validator_parse(const char *tag_id, uint16_t *out_val)
{
    if (out_val == NULL) {
        g_last_error = "Output pointer is NULL";
        return false;
    }
    
    if (!tag_id_validator_validate(tag_id)) {
        return false;
    }
    
    uint16_t value = 0;
    for (int i = 2; i < TAG_ID_LEN; i++) {
        value <<= 4;
        char c = tag_id[i];
        if (c >= '0' && c <= '9') {
            value += (c - '0');
        } else if (c >= 'A' && c <= 'F') {
            value += (c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            value += (c - 'a' + 10);
        }
    }
    
    *out_val = value;
    return true;
}

/**
 * @brief 将 uint16_t 数值格式化为 Tag ID 字符串
 */
bool tag_id_validator_format(uint16_t value, char *out_buf, size_t buf_size)
{
    if (out_buf == NULL) {
        g_last_error = "Output buffer is NULL";
        return false;
    }
    
    if (buf_size < TAG_ID_STR_LEN) {
        g_last_error = "Buffer too small (need 7 bytes)";
        return false;
    }
    
    if (value < TAG_ID_MIN || value > TAG_ID_MAX) {
        g_last_error = "Value out of valid range (0x0001-0xFFFF)";
        return false;
    }
    
    snprintf(out_buf, buf_size, "0x%04X", value);
    return true;
}

/**
 * @brief 获取 Tag ID 验证错误描述
 */
const char* tag_id_validator_get_error_string(void)
{
    return g_last_error ? g_last_error : "No error";
}