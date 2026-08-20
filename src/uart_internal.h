#ifndef KFSW_COMMS_UART_INTERNAL_H
#define KFSW_COMMS_UART_INTERNAL_H

#include <stdint.h>

#include <csp/csp_interface.h>

int kfsw_uart_open(uint16_t address, csp_iface_t **return_interface);

#endif
