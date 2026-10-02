#ifndef KFSW_COMMS_UART_INTERNAL_H
#define KFSW_COMMS_UART_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <csp/csp_interface.h>

int kfsw_uart_open_all(void);
#if CONFIG_KFSW_CSP_UART_CODEC
int kfsw_uart_codec_attach(csp_iface_t *iface);
int kfsw_uart_codec_check(void);
#endif

#endif
