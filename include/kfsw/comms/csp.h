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
	/* When this image was compiled, from the machine that built it. The
	 * same pair a remote node reports, so a local answer and a remote one
	 * can be compared without knowing which came from where.
	 */
	const char *build_date;
	const char *build_time;
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

typedef bool (*kfsw_csp_interface_visitor_t)(const struct kfsw_csp_interface_info *interface_info,
					     void *context);

typedef bool (*kfsw_csp_route_visitor_t)(const struct kfsw_csp_route_info *route_info,
					 void *context);

/** Maximum route-table string length accepted by the pinned libcsp parser. */
#define KFSW_CSP_ROUTE_TABLE_MAX_LENGTH 99U

/** Initialize libcsp, the configured interfaces, and static routes once. */
/**
 * @brief Set the revision this node reports, before CSP is initialised.
 *
 * The revision is what ground reads back to confirm which image is running, so
 * it has to name the build rather than a fixed string. The value comes from
 * the composition: this layer sits below the service that resolves it and
 * cannot reach up for it.
 *
 * Has no effect once kfsw_csp_init() has run, and a NULL or empty string
 * leaves the compiled default in place.
 */
void kfsw_csp_set_revision(const char *revision);

int kfsw_csp_init(void);

/** Start the single K-FSW-owned CSP router thread. */
int kfsw_csp_start(void);

/** Copy the current local CSP lifecycle and identity information. */
void kfsw_csp_get_info(struct kfsw_csp_info *info);

/** Visit a snapshot of each registered libcsp interface. */
void kfsw_csp_visit_interfaces(kfsw_csp_interface_visitor_t visitor, void *context);

/** Visit a snapshot of each configured libcsp static route. */
void kfsw_csp_visit_routes(kfsw_csp_route_visitor_t visitor, void *context);

/**
 * Validate a complete libcsp-native route table without changing live routes.
 *
 * Interfaces referenced by name must already be registered. On success,
 * entry_count receives the number of parsed entries when it is non-NULL.
 */
/**
 * @brief Check a route table without applying it.
 *
 * Returns 0 when the table is well formed, -ENETDOWN when CSP has not been
 * initialised yet and the interfaces a table names do not exist, and a libcsp
 * error otherwise. The middle case is not a rejection: it means the question
 * cannot be answered yet.
 */
int kfsw_csp_route_table_check(const char *route_table, size_t *entry_count);

/**
 * @brief Replace the routing table with a validated one.
 *
 * Validates before it touches anything, so a malformed table is refused rather
 * than leaving the router with part of one. If the load then disagrees with
 * what validation counted, the table is cleared instead of left half applied:
 * no routes at all is a state an operator can diagnose, and a partial table is
 * not.
 *
 * The change is not persisted. A route table is the one setting that can put a
 * node out of reach, and a wrong one that survived a reboot would be
 * permanent; this way the compiled table comes back on the next boot.
 *
 * @param route_table Table in libcsp's CIDR syntax.
 *
 * @retval 0 The table is loaded.
 * @return A negative libcsp error when the table is malformed, too large for
 *         CONFIG_CSP_RTABLE_SIZE, or fails to load.
 */
int kfsw_csp_route_table_apply(const char *route_table);

/** Send a standard CSP ping using CRC32 and return its round-trip time. */
int kfsw_csp_ping(uint16_t node, uint32_t timeout_ms, size_t payload_size, uint32_t *round_trip_ms);

/* Field sizes are stated here rather than taken from libcsp so that a caller
 * does not have to include the protocol headers. They are checked against the
 * wire message where the two meet.
 */
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
 *
 * Answers the question a ping cannot: not whether something is reachable, but
 * what is running there. The revision is what changes after a firmware update,
 * so this is how ground confirms that a new image is the one now executing.
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
