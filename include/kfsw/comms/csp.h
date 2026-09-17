#ifndef KFSW_COMMS_CSP_H
#define KFSW_COMMS_CSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Print every packet the router receives and every packet this node sends.
 *
 * Each line has the source and destination node and port, priority, flags and
 * size. Off by default.
 *
 * @param enabled True to trace, false to stop.
 */
void kfsw_csp_set_packet_trace(bool enabled);

/** Whether packet tracing is currently on. */
bool kfsw_csp_get_packet_trace(void);

/** What this node answers when something asks it what it is. */
struct kfsw_csp_info {
	uint16_t address;       /**< This node's CSP address. */
	const char *hostname;   /**< Name reported to a remote identity request. */
	const char *model;      /**< Hardware or composition this image was built for. */
	const char *revision;   /**< Image revision. */
	const char *build_date; /**< Date this image was compiled. */
	const char *build_time; /**< Time this image was compiled. */
	bool initialized;       /**< libcsp and its interfaces are up. */
	bool router_running;    /**< The router thread is started and forwarding. */
	size_t free_buffers;    /**< Free packet buffers. */
};

/** One registered link, and what it has carried since boot. */
struct kfsw_csp_interface_info {
	const char *name;         /**< Interface name, as a route table refers to it. */
	uint16_t address;         /**< Address this node answers to on this link. */
	uint16_t prefix_length;   /**< Bits of the address that must match, over 14-bit
				     node IDs. */
	bool is_default;          /**< Used for addresses no other route matches. */
	uint32_t tx_packets;      /**< Packets handed to the driver. */
	uint32_t rx_packets;      /**< Packets assembled from the driver. */
	uint32_t tx_errors;       /**< Sends the driver refused. */
	uint32_t rx_errors;       /**< Frames that arrived malformed or too large. */
	uint32_t dropped_packets; /**< Packets dropped for lack of a buffer or a route. */
};

/**
 * libcsp's own error counters. They are 8-bit and wrap, and libcsp updates
 * them without locking, so read them as an indication and not as an exact
 * count.
 */
struct kfsw_csp_counters {
	uint8_t buffer_out;     /**< Times no packet buffer was free. */
	uint8_t conn_out;       /**< Times no connection slot was free. */
	uint8_t conn_overflow;  /**< Packets dropped by a full connection queue. */
	uint8_t conn_noroute;   /**< Packets dropped with no route to the destination. */
	uint8_t invalid_reply;  /**< Replies that matched no open connection. */
	uint8_t last_error;     /**< Code of the last libcsp error. */
	uint8_t last_can_error; /**< Code of the last CAN framing error. */
};

/** One entry of the static routing table. */
struct kfsw_csp_route_info {
	uint16_t address;           /**< Destination this entry matches. */
	uint16_t prefix_length;     /**< Bits of it that must match; 0 is the default route. */
	const char *interface_name; /**< Link the packet leaves by. */
	uint16_t via;               /**< Link-layer next hop, meaningful only when has_via. */
	bool has_via;               /**< The entry has a next hop. */
};

typedef bool (*kfsw_csp_interface_visitor_t)(const struct kfsw_csp_interface_info *interface_info,
					     void *context);

typedef bool (*kfsw_csp_route_visitor_t)(const struct kfsw_csp_route_info *route_info,
					 void *context);

/** Maximum route-table string length accepted by the pinned libcsp parser. */
#define KFSW_CSP_ROUTE_TABLE_MAX_LENGTH 99U

/**
 * @brief Set the revision this node reports. Call before CSP is initialised.
 */
void kfsw_csp_set_revision(const char *revision);

/** Initialize libcsp, the configured interfaces, and static routes once. */
int kfsw_csp_init(void);

/** Start the CSP router thread. */
int kfsw_csp_start(void);

/** Copy the current local CSP identity information. */
void kfsw_csp_get_info(struct kfsw_csp_info *info);

/**
 * @brief Call @p visitor for each registered interface, under libcsp's lock.
 *
 * Return false from @p visitor to stop early.
 */
void kfsw_csp_visit_interfaces(kfsw_csp_interface_visitor_t visitor, void *context);

/**
 * @brief Call @p visitor for each static route. Return false to stop early.
 */
void kfsw_csp_visit_routes(kfsw_csp_route_visitor_t visitor, void *context);

/**
 * @brief Called for each packet the router accepts, from the router thread.
 *
 * @param source_node Node the packet came from.
 * @param destination_port Port it was addressed to.
 */
typedef void (*kfsw_csp_inbound_hook_t)(uint16_t source_node, uint8_t destination_port);

/**
 * @brief Watch inbound packets. Pass NULL to stop.
 *
 * The hook runs on the router thread, so it must return quickly and must not
 * block. One hook at a time; a second call replaces the first.
 */
void kfsw_csp_set_inbound_hook(kfsw_csp_inbound_hook_t hook);

/** Copy libcsp's error counters. Safe before initialization; they read zero. */
void kfsw_csp_get_counters(struct kfsw_csp_counters *counters);

/** Set every error counter back to zero, to start a bench run from a clean state. */
void kfsw_csp_clear_counters(void);

/** Name of a @ref kfsw_csp_counters last_error code, or "unknown". */
const char *kfsw_csp_error_name(uint8_t code);

/** Name of a @ref kfsw_csp_counters last_can_error code, or "unknown". */
const char *kfsw_csp_can_error_name(uint8_t code);

/**
 * @brief Check a route table without applying it.
 *
 * Returns 0 when the table is well formed, -ENETDOWN when CSP has not been
 * initialised yet and the interfaces a table names do not exist, and a libcsp
 * error otherwise.
 */
int kfsw_csp_route_table_check(const char *route_table, size_t *entry_count);

/**
 * @brief Validate and replace routes after initialization, before router start.
 *
 * @param route_table Table in libcsp's CIDR syntax.
 *
 * @retval 0 The table is loaded.
 * @retval CSP_ERR_NOTSUP The router is running; routes remain unchanged.
 * @retval CSP_ERR_INVAL CSP is not initialized or the table is invalid.
 * @return A negative libcsp error when the table is malformed, too large for
 *         CONFIG_CSP_RTABLE_SIZE, or fails to load.
 */
int kfsw_csp_route_table_apply(const char *route_table);

/** Send a standard CSP ping using CRC32 and return its round-trip time. */
int kfsw_csp_ping(uint16_t node, uint32_t timeout_ms, size_t payload_size, uint32_t *round_trip_ms);

struct kfsw_csp_clock {
	int32_t seconds;
	uint32_t nanoseconds;
};

/**
 * @brief Read this node's RTC clock.
 */
void kfsw_csp_clock_get(struct kfsw_csp_clock *clock);

/**
 * @brief Whether a clock reading has been set.
 */
bool kfsw_csp_clock_is_set(const struct kfsw_csp_clock *clock);

/**
 * @brief Set this node's RTC clock.
 *
 * Returns 0 on success, -ENOTSUP where the composition carries no real-time
 * clock, and -EINVAL for a time the platform will not accept.
 */
int kfsw_csp_clock_set(const struct kfsw_csp_clock *clock);

/**
 * @brief Read another node's RTC clock.
 *
 * Returns 0 and fills @p clock, or a negative errno. A node whose clock was
 * never set answers zero seconds.
 */
int kfsw_csp_clock_read(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_clock *clock);

/**
 * @brief Set another node's clock.
 *
 * @p clock is filled with the time the node reads back after setting it. The
 * link delay is not compensated.
 */
int kfsw_csp_clock_write(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_clock *clock);

/* Field sizes are defined here so callers don't need the libcsp headers. */
#define KFSW_CSP_IDENTITY_HOSTNAME_SIZE 21U
#define KFSW_CSP_IDENTITY_MODEL_SIZE 31U
#define KFSW_CSP_IDENTITY_REVISION_SIZE 21U
#define KFSW_CSP_IDENTITY_DATE_SIZE 13U
#define KFSW_CSP_IDENTITY_TIME_SIZE 10U

/** Identity a remote node reports about itself. Every field is terminated. */
struct kfsw_csp_identity {
	char hostname[KFSW_CSP_IDENTITY_HOSTNAME_SIZE];
	char model[KFSW_CSP_IDENTITY_MODEL_SIZE];
	char revision[KFSW_CSP_IDENTITY_REVISION_SIZE];
	char date[KFSW_CSP_IDENTITY_DATE_SIZE];
	char time[KFSW_CSP_IDENTITY_TIME_SIZE];
};

/**
 * @brief Ask a remote node to identify itself.
 * Gets the hostname, model, revision, and build date and time.
 *
 * @param node Remote CSP address.
 * @param timeout_ms Reply timeout in milliseconds.
 * @param[out] identity Destination identity.
 *
 * @retval CSP_ERR_NONE The node answered.
 * @retval CSP_ERR_INVAL CSP is not running, @p identity is NULL, or the
 *                       address is out of range.
 * @retval CSP_ERR_TIMEDOUT No answer arrived in time.
 */
int kfsw_csp_identify(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_identity *identity);

#ifdef __cplusplus
}
#endif

#endif
