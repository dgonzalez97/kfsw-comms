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
	uint16_t prefix_length;
	uint16_t peer;
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t tx_errors;
	uint32_t rx_errors;
	uint32_t dropped_packets;
	uint32_t frame_errors;
};

typedef bool (*kfsw_uart_visitor_t)(const struct kfsw_uart_info *uart_info, void *context);

struct kfsw_uart_test_result {
	uint16_t peer;
	const char *interface_name;
	uint32_t round_trip_ms;
};

/** Read the first CSP UART configuration for legacy one-interface callers. */
void kfsw_uart_get_info(struct kfsw_uart_info *info);

/** Visit every independently configured CSP KISS/UART interface. */
void kfsw_uart_visit(kfsw_uart_visitor_t visitor, void *context);

/** Ping the configured peer after verifying that its route uses this UART. */
int kfsw_uart_test(uint32_t timeout_ms, struct kfsw_uart_test_result *test_result);

/** Ping a peer and report the KISS/UART interface selected by the CSP route. */
int kfsw_uart_test_peer(uint16_t peer, uint32_t timeout_ms,
			struct kfsw_uart_test_result *test_result);

#ifdef __cplusplus
}
#endif

#endif
