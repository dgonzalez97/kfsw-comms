#include <zephyr/kernel.h>

#include <endian.h>
#include <errno.h>
#include <string.h>

#include <csp/csp.h>
#include <csp/csp_cmp.h>
#include <csp/csp_debug.h>
#include <csp/csp_hooks.h>
#include <csp/csp_id.h>
#include <csp/csp_iflist.h>
#include <csp/csp_interface.h>
#include <csp/csp_rtable.h>
#include <csp/interfaces/csp_if_lo.h>

#include <kfsw/platform/time.h>
#include <kfsw/platform/wallclock.h>
#include <kfsw/comms/csp.h>
#if CONFIG_KFSW_CSP_CAN
#include <kfsw/comms/can.h>
#endif

#if CONFIG_KFSW_CSP_KISS_UART
#include "uart_internal.h"
#endif

/* libcsp does not mask addresses; out of range they corrupt the header. */
BUILD_ASSERT(CONFIG_KFSW_CSP_ADDRESS < KFSW_CSP_BROADCAST_ADDRESS,
	     "the local CSP address must be below the broadcast address");
#if CONFIG_KFSW_CSP_KISS_UART
BUILD_ASSERT(CONFIG_KFSW_CSP_UART_PEER_ADDRESS < KFSW_CSP_BROADCAST_ADDRESS,
	     "the UART peer address must be below the broadcast address");
#endif

static bool initialized;
static bool router_running;
static K_MUTEX_DEFINE(lifecycle_lock);

/* Revision reported by csp ident until the composition sets it. */
static const char *revision = CONFIG_KFSW_CSP_REVISION;

void kfsw_csp_set_revision(const char *value)
{
	if (initialized || (value == NULL) || (value[0] == '\0')) {
		return;
	}
	revision = value;
}

/* Loopback handles traffic to this node's address before the route table. */
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
	 * The pinned CIDR table clamps its insertion index, so a table that uses every
	 * slot would lose its last route in csp_rtable_load().
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
		/* Clear a partially loaded table. */
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
	const char *route_table = CONFIG_KFSW_CSP_ROUTE_TABLE;

	/* Nothing leaves the node unless the composition names the link. */
	if (route_table[0] == '\0') {
		route_table = "0/0 LOOP";
	}

	return load_route_table(route_table);
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
	csp_conf.version = CONFIG_KFSW_CSP_VERSION;
	csp_init();
	__ASSERT(csp_id_get_max_nodeid() == KFSW_CSP_BROADCAST_ADDRESS,
		 "libcsp and K-FSW disagree on the CSP version");

#if CONFIG_KFSW_CSP_KISS_UART
	result = kfsw_uart_open_all();
	if (result != CSP_ERR_NONE) {
		return result;
	}
#endif

#if CONFIG_KFSW_CSP_CAN
	/* Open before loading routes that name this interface. */
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

	/* Management port, which answers csp ident. */
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
	info->initialized = initialized;
	info->router_running = router_running;
	info->free_buffers = initialized ? csp_buffer_remaining() : 0;
	info->libcsp = KFSW_LIBCSP_REVISION;
	info->protocol = csp_conf.version;
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
	/* Before the interfaces exist the parser can't resolve names, so report
	 * -ENETDOWN instead of an invalid table.
	 */
	if (!initialized) {
		return -ENETDOWN;
	}

	return check_route_table(route_table, entry_count);
}

void kfsw_csp_set_packet_trace(bool enabled)
{
	csp_dbg_packet_print = enabled ? 1U : 0U;
}

bool kfsw_csp_get_packet_trace(void)
{
	return csp_dbg_packet_print != 0U;
}

void kfsw_csp_get_counters(struct kfsw_csp_counters *counters)
{
	if (counters == NULL) {
		return;
	}

	counters->buffer_out = csp_dbg_buffer_out;
	counters->conn_out = csp_dbg_conn_out;
	counters->conn_overflow = csp_dbg_conn_ovf;
	counters->conn_noroute = csp_dbg_conn_noroute;
	counters->invalid_reply = csp_dbg_inval_reply;
	counters->last_error = csp_dbg_errno;
	counters->last_can_error = csp_dbg_can_errno;
}

void kfsw_csp_clear_counters(void)
{
	csp_dbg_buffer_out = 0U;
	csp_dbg_conn_out = 0U;
	csp_dbg_conn_ovf = 0U;
	csp_dbg_conn_noroute = 0U;
	csp_dbg_inval_reply = 0U;
	csp_dbg_errno = 0U;
	csp_dbg_can_errno = 0U;
}

const char *kfsw_csp_error_name(uint8_t code)
{
	switch (code) {
	case 0U:
		return "none";
	case CSP_DBG_ERR_CORRUPT_BUFFER:
		return "corrupt buffer";
	case CSP_DBG_ERR_MTU_EXCEEDED:
		return "MTU exceeded";
	case CSP_DBG_ERR_ALREADY_FREE:
		return "buffer already free";
	case CSP_DBG_ERR_REFCOUNT:
		return "buffer reference count";
	case CSP_DBG_ERR_INVALID_RTABLE_ENTRY:
		return "invalid route entry";
	case CSP_DBG_ERR_UNSUPPORTED:
		return "unsupported";
	case CSP_DBG_ERR_INVALID_BIND_PORT:
		return "invalid bind port";
	case CSP_DBG_ERR_PORT_ALREADY_IN_USE:
		return "port in use";
	case CSP_DBG_ERR_ALREADY_CLOSED:
		return "connection already closed";
	case CSP_DBG_ERR_INVALID_POINTER:
		return "invalid pointer";
	case CSP_DBG_ERR_CLOCK_SET_FAIL:
		return "clock set refused";
	default:
		return "unknown";
	}
}

const char *kfsw_csp_can_error_name(uint8_t code)
{
	switch (code) {
	case 0U:
		return "none";
	case CSP_DBG_CAN_ERR_FRAME_LOST:
		return "frame lost";
	case CSP_DBG_CAN_ERR_RX_OVF:
		return "receive overflow";
	case CSP_DBG_CAN_ERR_RX_OUT:
		return "no receive buffer";
	case CSP_DBG_CAN_ERR_SHORT_BEGIN:
		return "short first frame";
	case CSP_DBG_CAN_ERR_INCOMPLETE:
		return "incomplete packet";
	case CSP_DBG_CAN_ERR_UNKNOWN:
		return "unknown frame";
	default:
		return "unknown";
	}
}

#if CONFIG_KFSW_CSP_CLOCK_RTC
/*
 * RTC-backed versions of libcsp's weak clock hooks, so the time survives a
 * reset. They are in this file so the linker pulls them in.
 */
void csp_clock_get_time(csp_timestamp_t *time)
{
	int64_t seconds = 0;

	if (time == NULL) {
		return;
	}
	time->tv_nsec = 0U;
	/* An unset clock reads as zero. */
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
	/* Same hook as libcsp, so the shell and remote callers see the same time. */
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

	/* libcsp does not mask a destination; out of range it goes to another node. */
	if ((clock == NULL) || (node == 0U) || (node >= KFSW_CSP_BROADCAST_ADDRESS)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -ENETDOWN;
	}

	/* Zero seconds means read only; the other node answers with its time. */
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
		/* Don't send an unset clock: zero means read on the wire. */
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

int kfsw_csp_interface_stats_read(uint16_t node, const char *name, uint32_t timeout_ms,
				  struct kfsw_csp_interface_stats *stats)
{
	struct csp_cmp_if_stats_msg message = {0};
	size_t length;

	BUILD_ASSERT(KFSW_CSP_INTERFACE_NAME_SIZE == CSP_CMP_ROUTE_IFACE_LEN);
	if ((name == NULL) || (stats == NULL) || (timeout_ms == 0U) ||
	    (node >= (1UL << csp_id_get_host_bits()))) {
		return -EINVAL;
	}
	length = strnlen(name, sizeof(message.interface));
	if ((length == 0U) || (length == sizeof(message.interface))) {
		return -EINVAL;
	}
	if (!initialized || !router_running) {
		return -ENETDOWN;
	}
	memcpy(message.interface, name, length + 1U);
	if (csp_cmp_if_stats(node, timeout_ms, &message) != CSP_ERR_NONE) {
		return -ETIMEDOUT;
	}
	if ((message.type != CSP_CMP_REPLY) || (message.code != CSP_CMP_IF_STATS) ||
	    (memcmp(message.interface, name, length + 1U) != 0)) {
		return -EBADMSG;
	}

	struct kfsw_csp_interface_stats result = {
		.tx_packets = be32toh(message.tx),
		.rx_packets = be32toh(message.rx),
		.tx_errors = be32toh(message.tx_error),
		.rx_errors = be32toh(message.rx_error),
		.dropped_packets = be32toh(message.drop),
		.auth_errors = be32toh(message.autherr),
		.frame_errors = be32toh(message.frame),
		.tx_bytes = be32toh(message.txbytes),
		.rx_bytes = be32toh(message.rxbytes),
		.interrupts = be32toh(message.irq),
	};
	memcpy(result.name, name, length + 1U);
	*stats = result;
	return 0;
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

int kfsw_csp_ping(uint16_t node, uint32_t timeout_ms, size_t payload_size, uint32_t *round_trip_us)
{
	const unsigned int host_bits = csp_id_get_host_bits();
	uint64_t start_us;
	int elapsed_ms;

	if (!initialized || !router_running || round_trip_us == NULL ||
	    node >= (1UL << host_bits) || payload_size > CSP_BUFFER_SIZE) {
		return CSP_ERR_INVAL;
	}

	*round_trip_us = 0;
	/* libcsp counts in system ticks, 10 ms on native_sim; time it here instead. */
	start_us = kfsw_time_monotonic_us();
	elapsed_ms = csp_ping(node, timeout_ms, (unsigned int)payload_size, CSP_O_CRC32);
	if (elapsed_ms < 0) {
		return CSP_ERR_TIMEDOUT;
	}

	*round_trip_us = (uint32_t)MIN(kfsw_time_monotonic_us() - start_us, UINT32_MAX);
	return CSP_ERR_NONE;
}
