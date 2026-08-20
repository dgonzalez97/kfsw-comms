#ifndef KFSW_COMMS_CSP_H
#define KFSW_COMMS_CSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kfsw_csp_info {
	uint16_t address;
	const char *hostname;
	const char *model;
	const char *revision;
	bool initialized;
	bool router_running;
	size_t free_buffers;
};

struct kfsw_csp_interface_info {
	const char *name;
	uint16_t address;
	uint16_t prefix_length;
	bool is_default;
	uint32_t tx_packets;
	uint32_t rx_packets;
	uint32_t tx_errors;
	uint32_t rx_errors;
	uint32_t dropped_packets;
};

struct kfsw_csp_route_info {
	uint16_t address;
	uint16_t prefix_length;
	const char *interface_name;
	uint16_t via;
	bool has_via;
};

typedef bool (*kfsw_csp_interface_visitor_t)(
	const struct kfsw_csp_interface_info *interface_info, void *context);

typedef bool (*kfsw_csp_route_visitor_t)(
	const struct kfsw_csp_route_info *route_info, void *context);

/** Initialize libcsp, the configured interfaces, and static routes once. */
int kfsw_csp_init(void);

/** Start the single K-FSW-owned CSP router thread. */
int kfsw_csp_start(void);

/** Copy the current local CSP lifecycle and identity information. */
void kfsw_csp_get_info(struct kfsw_csp_info *info);

/** Visit a snapshot of each registered libcsp interface. */
void kfsw_csp_visit_interfaces(kfsw_csp_interface_visitor_t visitor,
			       void *context);

/** Visit a snapshot of each configured libcsp static route. */
void kfsw_csp_visit_routes(kfsw_csp_route_visitor_t visitor, void *context);

/** Send a standard CSP ping using CRC32 and return its round-trip time. */
int kfsw_csp_ping(uint16_t node, uint32_t timeout_ms, size_t payload_size,
		  uint32_t *round_trip_ms);

#ifdef __cplusplus
}
#endif

#endif
