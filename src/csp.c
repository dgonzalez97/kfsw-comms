#include <zephyr/kernel.h>

#include <csp/csp.h>
#include <csp/csp_id.h>
#include <csp/drivers/usart.h>
#include <csp/interfaces/csp_if_lo.h>

#include <kfsw/comms/csp.h>

static bool initialized;
static bool router_running;

static void kfsw_csp_router(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		(void)csp_route_work();
	}
}

K_THREAD_DEFINE(kfsw_csp_router_thread, CONFIG_KFSW_CSP_ROUTER_STACK_SIZE,
		kfsw_csp_router, NULL, NULL, NULL,
		CONFIG_KFSW_CSP_ROUTER_PRIORITY, 0, SYS_FOREVER_MS);

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

	/* libcsp creates LOOP during csp_init(); give it this node's address. */
	csp_if_lo.addr = CONFIG_KFSW_CSP_ADDRESS;

#if CONFIG_KFSW_CSP_KISS_UART
	csp_iface_t *host_interface = NULL;
	const csp_usart_conf_t usart_config = {
		.device = CONFIG_KFSW_CSP_KISS_UART_DEVICE,
		.baudrate = 115200,
		.databits = 8,
		.stopbits = 1,
		.paritysetting = 0,
	};

	result = csp_usart_open_and_add_kiss_interface(&usart_config, "KISS",
						       CONFIG_KFSW_CSP_ADDRESS,
						       &host_interface);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	/* Route every non-local destination directly over the host link. */
	result = csp_rtable_set(0, 0, host_interface, CSP_NO_VIA_ADDRESS);
	if (result != CSP_ERR_NONE) {
		return result;
	}
#endif

	/* Expose only libcsp's standard ping service in this first increment. */
	result = csp_bind_callback(csp_service_handler, CSP_PING);
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

void kfsw_csp_visit_interfaces(kfsw_csp_interface_visitor_t visitor,
			       void *context)
{
	csp_iface_t *interface;

	if (!initialized || visitor == NULL) {
		return;
	}

	for (interface = csp_iflist_get(); interface != NULL;
	     interface = interface->next) {
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

	visitor_context->keep_visiting =
		visitor_context->visitor(&info, visitor_context->context);
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

int kfsw_csp_ping(uint16_t node, uint32_t timeout_ms, size_t payload_size,
		  uint32_t *round_trip_ms)
{
	const unsigned int host_bits = csp_id_get_host_bits();
	int elapsed_ms;

	if (!initialized || !router_running || round_trip_ms == NULL ||
	    node >= (1UL << host_bits) || payload_size > CSP_BUFFER_SIZE) {
		return CSP_ERR_INVAL;
	}

	*round_trip_ms = 0;
	elapsed_ms = csp_ping(node, timeout_ms, (unsigned int)payload_size,
			      CSP_O_CRC32);
	if (elapsed_ms < 0) {
		return CSP_ERR_TIMEDOUT;
	}

	*round_trip_ms = (uint32_t)elapsed_ms;
	return CSP_ERR_NONE;
}
