#ifndef UART_HANDLER_1_H
#define UART_HANDLER_1_H

#include "business_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

void uart_handler_1_init(void);
void uart_handler_1_on_event(be_event_t event, const void *data);
void uart_handler_1_send_json(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* UART_HANDLER_1_H */