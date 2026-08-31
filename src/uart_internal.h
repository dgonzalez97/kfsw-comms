#ifndef KFSW_COMMS_UART_INTERNAL_H
#define KFSW_COMMS_UART_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <csp/csp_interface.h>

int kfsw_uart_open_all(void);
size_t kfsw_uart_count(void);
const char *kfsw_uart_first_interface_name(void);

#endif
