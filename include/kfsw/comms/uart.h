#ifndef KFSW_COMMS_UART_H
#define KFSW_COMMS_UART_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kfsw_uart_info {
	const char *device_name;
	uint32_t baudrate;
	bool ready;
	bool interface_started;
	const char *interface_name;
	uint16_t node;
	uint16_t peer;
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t tx_errors;
	uint32_t rx_errors;
	uint32_t dropped_packets;
	uint32_t frame_errors;
};

struct kfsw_uart_test_result {
	uint16_t peer;
	const char *interface_name;
	uint32_t round_trip_ms;
};

/** Read the dedicated CSP UART configuration and KISS interface statistics. */
void kfsw_uart_get_info(struct kfsw_uart_info *info);

/** Ping the configured peer after verifying that its route uses this UART. */
int kfsw_uart_test(uint32_t timeout_ms,
		   struct kfsw_uart_test_result *test_result);

#ifdef __cplusplus
}
#endif

#endif
