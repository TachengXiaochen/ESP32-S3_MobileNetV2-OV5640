#ifndef UART_HANDLER_0_H
#define UART_HANDLER_0_H

#include "business_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

void uart_handler_0_init(void);
void uart_handler_0_on_event(be_event_t event, const void *data);

// UI 引导函数（从 cmd_handler.h 迁移）
void show_main_menu(void);
void show_registration_step1(const char *tag_id);
void show_registration_step2(void);
void show_registration_step3(void);
void show_inventory_step1(const char *tag_id);
void show_inventory_step2(void);
void show_inventory_step3(void);
void show_verification_existing_guide(const char *tag_id, const char *item_name,
                                       char storage_area, uint32_t current_qty);
void show_verification_add_qty_guide(const char *tag_id, const char *item_name,
                                      uint32_t current_qty);
void show_verification_failed(float confidence, float threshold);
void show_verification_retry_guide(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_HANDLER_0_H */