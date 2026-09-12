#include <zephyr/kernel.h>

#include <endian.h>
#include <errno.h>
#include <string.h>

#include <csp/csp.h>
#include <csp/csp_cmp.h>
#include <csp/csp_hooks.h>
#include <csp/csp_id.h>
#include <csp/csp_iflist.h>
#include <csp/csp_interface.h>
#include <csp/csp_rtable.h>
#include <csp/interfaces/csp_if_lo.h>

#include <kfsw/platform/wallclock.h>
#include <kfsw/comms/csp.h>
#if CONFIG_KFSW_CSP_CAN
#include <kfsw/comms/can.h>
#endif

#if CONFIG_KFSW_CSP_KISS_UART
#include "uart_internal.h"
#endif

static bool initialized;
static bool router_running;
static K_MUTEX_DEFINE(lifecycle_lock);

/* What `csp ident` reports as the revision. The compiled default stands until
 * the composition replaces it, so a build that never calls the setter still
 * answers something rather than nothing.
 */
static const char *revision = CONFIG_KFSW_CSP_REVISION;

void kfsw_csp_set_revision(const char *value)
{
	if (initialized || (value == NULL) || (value[0] == '\0')) {
		return;
	}
	revision = value;
}

/*
 * Self-addressed traffic uses libcsp's loopback, given this node's address. It
 * is short-circuited before the routing table is consulted, so it cannot
 * conflict with an interface that also covers the address.
 */
static void configure_loopback_address(void)
{
	csp_if_lo.addr = CONFIG_KFSW_CSP_ADDRESS;
}

static int check_route_table(const char *route_table, size_t *entry_count)
{
	int entries;

	if (route_table == NULL || strnlen(route_table, KFSW_CSP_ROUTE_TABLE_MAX_LENGTH + 1U) >
					   KFSW_CSP_ROUTE_TABLE_MAX_LENGTH) {
		return CSP_ERR_INVAL;
	}

	entries = csp_rtable_check(route_table);
	if (entries < 0) {
		return entries;
	}
	/*
	 * This pinned CIDR implementation advances and then clamps its insertion
	 * index, so one declared slot is not safely iterable. Reject that boundary
	 * before csp_rtable_load() can silently hide the last route.
	 */
	if (entries >= CONFIG_CSP_RTABLE_SIZE) {
		return CSP_ERR_NOMEM;
	}

	if (entry_count != NULL) {
		*entry_count = (size_t)entries;
	}
	return CSP_ERR_NONE;
}

/* Called only before the router starts. */
static int load_route_table(const char *route_table)
{
	size_t expected_entries;
	int result = check_route_table(route_table, &expected_entries);

	if (result != CSP_ERR_NONE || expected_entries == 0U) {
		return result != CSP_ERR_NONE ? result : CSP_ERR_INVAL;
	}

	csp_rtable_clear();
	result = csp_rtable_load(route_table);
	if (result < 0 || (size_t)result != expected_entries) {
		/* Never expose a partially loaded table: no routes is a state an
		 * operator can diagnose, half a table is not. */
		csp_rtable_clear();
		return result < 0 ? result : CSP_ERR_INVAL;
	}
	return CSP_ERR_NONE;
}

int kfsw_csp_route_table_apply(const char *route_table)
{
	int result;

	if (route_table == NULL) {
		return CSP_ERR_INVAL;
	}
	k_mutex_lock(&lifecycle_lock, K_FOREVER);
	/* libcsp readers keep pointers into the live CIDR table. */
	if (router_running) {
		result = CSP_ERR_NOTSUP;
	} else if (!initialized) {
		result = CSP_ERR_INVAL;
	} else {
		result = load_route_table(route_table);
	}
	k_mutex_unlock(&lifecycle_lock);
	return result;
}

static int configure_routes(void)
{
	const char *const route_table = CONFIG_KFSW_CSP_ROUTE_TABLE;

	if (route_table[0] != '\0') {
		return load_route_table(route_table);
	}

#if CONFIG_KFSW_CSP_KISS_UART
	if (kfsw_uart_count() != 1U) {
		/* Selecting the first link implicitly is unsafe with multiple links. */
		return CSP_ERR_INVAL;
	}

	return csp_rtable_set(0, 0, csp_iflist_get_by_name(kfsw_uart_first_interface_name()),
			      CSP_NO_VIA_ADDRESS);
#else
	return CSP_ERR_NONE;
#endif
}

static void kfsw_csp_router(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		(void)csp_route_work();
	}
}

K_THREAD_DEFINE(kfsw_csp_router_thread, CONFIG_KFSW_CSP_ROUTER_STACK_SIZE, kfsw_csp_router, NULL,
		NULL, NULL, CONFIG_KFSW_CSP_ROUTER_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_csp_init(void)
{
	int result;

	if (initialized) {
		return CSP_ERR_NONE;
	}

	csp_conf.hostname = CONFIG_KFSW_CSP_HOSTNAME;
	csp_conf.model = CONFIG_KFSW_CSP_MODEL;
	csp_conf.revision = revision;
	csp_init();

#if CONFIG_KFSW_CSP_KISS_UART
	result = kfsw_uart_open_all();
	if (result != CSP_ERR_NONE) {
		return result;
	}
#endif

#if CONFIG_KFSW_CSP_CAN
	/* Opened before the routes are loaded, because a route naming this
	 * interface is rejected if the interface is not registered yet.
	 */
	result = kfsw_can_open();
	if (result != 0) {
		return CSP_ERR_DRIVER;
	}
#endif

	configure_loopback_address();

	result = configure_routes();
	if (result != CSP_ERR_NONE) {
		return result;
	}

	result = csp_bind_callback(csp_service_handler, CSP_PING);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	/* The management port answers "what are you", which a ping cannot. It is
	 * how ground confirms which image is running after an update, so a node
	 * that cannot be asked cannot be verified remotely.
	 */
	result = csp_bind_callback(csp_service_handler, CSP_CMP);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	initialized = true;
	return CSP_ERR_NONE;
}

int kfsw_csp_start(void)
{
	k_mutex_lock(&lifecycle_lock, K_FOREVER);
	if (!initialized) {
		k_mutex_unlock(&lifecycle_lock);
		return CSP_ERR_INVAL;
	}

	if (router_running) {
		k_mutex_unlock(&lifecycle_lock);
		return CSP_ERR_NONE;
	}

	router_running = true;
	k_thread_start(kfsw_csp_router_thread);
	k_mutex_unlock(&lifecycle_lock);
	return CSP_ERR_NONE;
}

void kfsw_csp_get_info(struct kfsw_csp_info *info)
{
	if (info == NULL) {
		return;
	}

	info->address = CONFIG_KFSW_CSP_ADDRESS;
	info->hostname = CONFIG_KFSW_CSP_HOSTNAME;
	info->model = CONFIG_KFSW_CSP_MODEL;
	info->revision = revision;
	info->build_date = __DATE__;
	info->build_time = __TIME__;
	info->initialized = initialized;
	info->router_running = router_running;
	info->free_buffers = initialized ? csp_buffer_remaining() : 0;
}

void kfsw_csp_visit_interfaces(kfsw_csp_interface_visitor_t visitor, void *context)
{
	csp_iface_t *interface;

	if (!initialized || visitor == NULL) {
		return;
	}

	for (interface = csp_iflist_get(); interface != NULL; interface = interface->next) {
		const struct kfsw_csp_interface_info info = {
			.name = interface->name,
			.address = interface->addr,
			.prefix_length = interface->netmask,
			.is_default = interface->is_default != 0,
			.tx_packets = interface->tx,
			.rx_packets = interface->rx,
			.tx_errors = interface->tx_error,
			.rx_errors = interface->rx_error,
			.dropped_packets = interface->drop,
		};

		if (!visitor(&info, context)) {
			break;
		}
	}
}

struct route_visitor_context {
	kfsw_csp_route_visitor_t visitor;
	void *context;
	bool keep_visiting;
};

static bool visit_route(void *context, csp_route_t *route)
{
	struct route_visitor_context *visitor_context = context;

	if (!visitor_context->keep_visiting) {
		return false;
	}

	const struct kfsw_csp_route_info info = {
		.address = route->address,
		.prefix_length = route->netmask,
		.interface_name = route->iface->name,
		.via = route->via,
		.has_via = route->via != CSP_NO_VIA_ADDRESS,
	};

	visitor_context->keep_visiting = visitor_context->visitor(&info, visitor_context->context);
	return visitor_context->keep_visiting;
}

void kfsw_csp_visit_routes(kfsw_csp_route_visitor_t visitor, void *context)
{
	struct route_visitor_context visitor_context = {
		.visitor = visitor,
		.context = context,
		.keep_visiting = true,
	};

	if (!initialized || visitor == NULL) {
		return;
	}

	csp_rtable_iterate(visit_route, &visitor_context);
}

int kfsw_csp_route_table_check(const char *route_table, size_t *entry_count)
{
	/* The parser resolves every interface name against the registered list,
	 * even when only checking, so before the interfaces exist it cannot
	 * tell a bad table from a table it is too early to judge. Say which,
	 * rather than calling both invalid: a caller registering a compiled
	 * default has to be able to distinguish "wrong" from "not yet".
	 */
	if (!initialized) {
		return -ENETDOWN;
	}

	return check_route_table(route_table, entry_count);
}

#if CONFIG_KFSW_CSP_CLOCK_RTC
/*
 * Where a node's idea of the time comes from.
 *
 * libcsp ships weak hooks that read the POSIX realtime clock, which on Zephyr
 * is an offset held in RAM: every reset puts it back to zero, so a node that
 * reboots on a watchdog comes back not knowing when it is. Everything gated on
 * a valid clock then stays quiet — scheduled collection, beacons — until a
 * ground station is in view to set it, which is the worst moment to need one.
 *
 * These override the weak hooks with the board's RTC, whose counter a reset
 * does not touch. They live in this file rather than beside the rest of the
 * clock code for a linker reason worth stating: a strong definition only beats
 * a weak one if its object is actually pulled out of the archive, and nothing
 * else would have referenced a file that contains only overrides.
 */
void csp_clock_get_time(csp_timestamp_t *time)
{
	int64_t seconds = 0;

	if (time == NULL) {
		return;
	}
	time->tv_nsec = 0U;
	/* A clock present but never set reads as zero, which is what every
	 * caller already treats as "nobody has said what time it is".
	 */
	time->tv_sec = (kfsw_wallclock_get(&seconds) == 0) ? (uint32_t)seconds : 0U;
}

int csp_clock_set_time(const csp_timestamp_t *time)
{
	if (time == NULL) {
		return CSP_ERR_INVAL;
	}
	return (kfsw_wallclock_set((int64_t)time->tv_sec) == 0) ? CSP_ERR_NONE : CSP_ERR_INVAL;
}
#endif /* CONFIG_KFSW_CSP_CLOCK_RTC */

void kfsw_csp_clock_get(struct kfsw_csp_clock *clock)
{
	csp_timestamp_t now = {0};

	if (clock == NULL) {
		return;
	}
	/* libcsp's own hook, so a node reports the same time to the shell and
	 * to a remote caller. Two readings of one clock that disagree would be
	 * worse than one that is merely unset.
	 */
	csp_clock_get_time(&now);
	clock->seconds = (int32_t)now.tv_sec;
	clock->nanoseconds = now.tv_nsec;
}

bool kfsw_csp_clock_is_set(const struct kfsw_csp_clock *clock)
{
	return (clock != NULL) && (clock->seconds >= CONFIG_KFSW_CSP_CLOCK_FLOOR);
}

int kfsw_csp_clock_set(const struct kfsw_csp_clock *clock)
{
	csp_timestamp_t value;

	if (clock == NULL) {
		return -EINVAL;
	}
	value.tv_sec = (uint32_t)clock->seconds;
	value.tv_nsec = clock->nanoseconds;

	return (csp_clock_set_time(&value) == CSP_ERR_NONE) ? 0 : -ENOTSUP;
}

static int clock_transaction(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_clock *clock,
			     bool set)
{
	struct csp_cmp_clock_msg message = {0};
	int result;

	if (clock == NULL) {
		return -EINVAL;
	}
	if (!initialized) {
		return -ENETDOWN;
	}

	/* Zero seconds is how the protocol says "only tell me the time". The
	 * far side sets nothing and answers with what it has, which is why one
	 * message serves both directions.
	 */
	if (set) {
		message.clock.tv_sec = htobe32((uint32_t)clock->seconds);
		message.clock.tv_nsec = htobe32(clock->nanoseconds);
	}

	result = csp_cmp_clock(node, timeout_ms, &message);
	if (result != CSP_ERR_NONE) {
		return -ETIMEDOUT;
	}

	clock->seconds = (int32_t)be32toh(message.clock.tv_sec);
	clock->nanoseconds = be32toh(message.clock.tv_nsec);
	return 0;
}

int kfsw_csp_clock_read(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_clock *clock)
{
	return clock_transaction(node, timeout_ms, clock, false);
}

int kfsw_csp_clock_write(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_clock *clock)
{
	if (!kfsw_csp_clock_is_set(clock)) {
		/* Refused here rather than sent. Zero means "read" on the wire,
		 * so a node asked to adopt an unset clock would answer
		 * cheerfully and change nothing; and handing over an epoch
		 * nobody set would spread a wrong date rather than a missing
		 * one.
		 */
		return -EINVAL;
	}
	return clock_transaction(node, timeout_ms, clock, true);
}

static void copy_identity_field(char *destination, size_t destination_size, const char *source,
				size_t source_size)
{
	size_t length = MIN(destination_size - 1U, source_size);

	memcpy(destination, source, length);
	destination[length] = '\0';
}

int kfsw_csp_identify(uint16_t node, uint32_t timeout_ms, struct kfsw_csp_identity *identity)
{
	const unsigned int host_bits = csp_id_get_host_bits();
	struct csp_cmp_message message = {0};
	int result;

	BUILD_ASSERT(KFSW_CSP_IDENTITY_HOSTNAME_SIZE > CSP_HOSTNAME_LEN);
	BUILD_ASSERT(KFSW_CSP_IDENTITY_MODEL_SIZE > CSP_MODEL_LEN);
	BUILD_ASSERT(KFSW_CSP_IDENTITY_REVISION_SIZE > CSP_CMP_IDENT_REV_LEN);
	BUILD_ASSERT(KFSW_CSP_IDENTITY_DATE_SIZE > CSP_CMP_IDENT_DATE_LEN);
	BUILD_ASSERT(KFSW_CSP_IDENTITY_TIME_SIZE > CSP_CMP_IDENT_TIME_LEN);

	if (!initialized || !router_running || identity == NULL || node >= (1UL << host_bits)) {
		return CSP_ERR_INVAL;
	}

	memset(identity, 0, sizeof(*identity));

	result = csp_cmp_ident(node, timeout_ms, &message);
	if (result != CSP_ERR_NONE) {
		return CSP_ERR_TIMEDOUT;
	}

	copy_identity_field(identity->hostname, sizeof(identity->hostname), message.ident.hostname,
			    CSP_HOSTNAME_LEN);
	copy_identity_field(identity->model, sizeof(identity->model), message.ident.model,
			    CSP_MODEL_LEN);
	copy_identity_field(identity->revision, sizeof(identity->revision), message.ident.revision,
			    CSP_CMP_IDENT_REV_LEN);
	copy_identity_field(identity->date, sizeof(identity->date), message.ident.date,
			    CSP_CMP_IDENT_DATE_LEN);
	copy_identity_field(identity->time, sizeof(identity->time), message.ident.time,
			    CSP_CMP_IDENT_TIME_LEN);

	return CSP_ERR_NONE;
}

int kfsw_csp_ping(uint16_t node, uint32_t timeout_ms, size_t payload_size, uint32_t *round_trip_ms)
{
	const unsigned int host_bits = csp_id_get_host_bits();
	int elapsed_ms;

	if (!initialized || !router_running || round_trip_ms == NULL ||
	    node >= (1UL << host_bits) || payload_size > CSP_BUFFER_SIZE) {
		return CSP_ERR_INVAL;
	}

	*round_trip_ms = 0;
	elapsed_ms = csp_ping(node, timeout_ms, (unsigned int)payload_size, CSP_O_CRC32);
	if (elapsed_ms < 0) {
		return CSP_ERR_TIMEDOUT;
	}

	*round_trip_ms = (uint32_t)elapsed_ms;
	return CSP_ERR_NONE;
}
