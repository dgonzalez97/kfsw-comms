#include <zephyr/kernel.h>

#include <string.h>

#include <csp/csp.h>
#include <csp/csp_cmp.h>
#include <csp/csp_id.h>
#include <csp/csp_iflist.h>
#include <csp/csp_interface.h>
#include <csp/csp_rtable.h>
#include <csp/interfaces/csp_if_lo.h>

#include <kfsw/comms/csp.h>

#if CONFIG_KFSW_CSP_KISS_UART
#include "uart_internal.h"
#endif

static bool initialized;
static bool router_running;

/*
 * Self-addressed traffic.
 *
 * libcsp's built-in loopback is taken in csp_send_direct() before the outgoing
 * source address is applied, so a packet a node sends to itself arrives with
 * source 0 and the reply is addressed to node 0. Carrying this node's address
 * on an ordinary interface instead puts self-addressed traffic through the
 * normal send path, which applies the source address like any other transmit.
 *
 * The interface subnet search transmits on every interface whose subnet
 * matches, so this can only be done when no other interface already covers this
 * node's address. A node that reaches itself through one of its own link
 * addresses keeps libcsp's loopback and its previous behaviour.
 */
static int self_interface_tx(csp_iface_t *iface, uint16_t via, csp_packet_t *packet, int from_me)
{
	ARG_UNUSED(via);
	ARG_UNUSED(from_me);

	/* Hand the packet back to the router, which delivers it locally. */
	csp_qfifo_write(packet, iface, NULL);
	return CSP_ERR_NONE;
}

static csp_iface_t self_interface = {
	.name = "SELF",
	.nexthop = self_interface_tx,
	.addr = CONFIG_KFSW_CSP_ADDRESS,
	.is_default = 0,
};

static bool address_covered_by_another_interface(void)
{
	return csp_iflist_get_by_subnet(CONFIG_KFSW_CSP_ADDRESS, NULL) != NULL;
}

static void configure_self_interface(void)
{
	if (address_covered_by_another_interface()) {
		/*
		 * Another interface already carries this address. Leave the
		 * built-in loopback owning it rather than transmitting a copy
		 * on that interface as well.
		 */
		csp_if_lo.addr = CONFIG_KFSW_CSP_ADDRESS;
		return;
	}

	self_interface.netmask = csp_id_get_host_bits();
	csp_iflist_add(&self_interface);
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

static int configure_routes(void)
{
	const char *const route_table = CONFIG_KFSW_CSP_ROUTE_TABLE;

	if (route_table[0] != '\0') {
		size_t expected_entries;
		int result = check_route_table(route_table, &expected_entries);

		if (result != CSP_ERR_NONE || expected_entries == 0U) {
			return result != CSP_ERR_NONE ? result : CSP_ERR_INVAL;
		}

		csp_rtable_clear();
		result = csp_rtable_load(route_table);
		if (result < 0 || (size_t)result != expected_entries) {
			/* Startup must never expose a partially loaded static table. */
			csp_rtable_clear();
			return result < 0 ? result : CSP_ERR_INVAL;
		}
		return CSP_ERR_NONE;
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
	csp_conf.revision = CONFIG_KFSW_CSP_REVISION;
	csp_init();

#if CONFIG_KFSW_CSP_KISS_UART
	result = kfsw_uart_open_all();
	if (result != CSP_ERR_NONE) {
		return result;
	}
#endif

	configure_self_interface();

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
	if (!initialized) {
		return CSP_ERR_INVAL;
	}

	if (router_running) {
		return CSP_ERR_NONE;
	}

	k_thread_start(kfsw_csp_router_thread);
	router_running = true;
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
	info->revision = CONFIG_KFSW_CSP_REVISION;
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
	if (!initialized) {
		return CSP_ERR_INVAL;
	}

	return check_route_table(route_table, entry_count);
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
