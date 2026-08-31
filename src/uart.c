#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/util.h>

#include <csp/csp.h>
#include <csp/csp_id.h>
#include <csp/csp_iflist.h>
#include <csp/csp_rtable.h>
#include <csp/drivers/usart.h>
#include <csp/interfaces/csp_if_kiss.h>

#include <kfsw/comms/csp.h>
#include <kfsw/comms/uart.h>

#include "uart_internal.h"

#define KFSW_UART_TEST_PAYLOAD_SIZE 128U
#define KFSW_UART_IRQ_RX_CHUNK_SIZE 32U
#define KFSW_CSP_KISS_UARTS_COMPAT kfsw_csp_kiss_uarts

struct kfsw_uart_config {
	const struct device *device;
	const char *interface_name;
	struct uart_config serial;
	uint16_t address;
	uint16_t prefix_length;
};

struct kfsw_uart_context {
	const struct kfsw_uart_config *config;
	csp_iface_t *interface;
#if CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
	csp_iface_t interrupt_interface;
	csp_kiss_interface_data_t kiss_data;
#endif
};

#define KFSW_UART_NODE(child_id) DT_PHANDLE(child_id, uart)
#define KFSW_UART_SERIAL_CONFIG(node_id)                                                           \
	{                                                                                          \
		.baudrate = DT_PROP(node_id, current_speed),                                       \
		.parity = DT_ENUM_IDX(node_id, parity),                                            \
		.stop_bits = DT_ENUM_IDX(node_id, stop_bits),                                      \
		.data_bits = DT_PROP(node_id, data_bits) - 5,                                      \
		.flow_ctrl = DT_PROP(node_id, hw_flow_control) ? UART_CFG_FLOW_CTRL_RTS_CTS        \
							       : UART_CFG_FLOW_CTRL_NONE,          \
	}

#if DT_HAS_COMPAT_STATUS_OKAY(KFSW_CSP_KISS_UARTS_COMPAT)

#define KFSW_CSP_UARTS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(KFSW_CSP_KISS_UARTS_COMPAT)
#define KFSW_UART_ASSERT_CHILD(child_id)                                                           \
	BUILD_ASSERT(DT_NODE_HAS_STATUS(KFSW_UART_NODE(child_id), okay),                           \
		     "each K-FSW CSP KISS UART must be enabled");                                  \
	BUILD_ASSERT(DT_NODE_HAS_PROP(KFSW_UART_NODE(child_id), current_speed),                    \
		     "each K-FSW CSP KISS UART must define current-speed");                        \
	BUILD_ASSERT(sizeof(DT_PROP(child_id, interface_name)) <= CSP_IFLIST_NAME_MAX,             \
		     "CSP interface names must contain at most nine route-safe characters");

DT_FOREACH_CHILD_STATUS_OKAY(KFSW_CSP_UARTS_NODE, KFSW_UART_ASSERT_CHILD)

#define KFSW_UART_CONFIG_ENTRY(child_id)                                                           \
	{                                                                                          \
		.device = DEVICE_DT_GET(KFSW_UART_NODE(child_id)),                                 \
		.interface_name = DT_PROP(child_id, interface_name),                               \
		.serial = KFSW_UART_SERIAL_CONFIG(KFSW_UART_NODE(child_id)),                       \
		.address = DT_PROP_OR(child_id, address, CONFIG_KFSW_CSP_ADDRESS),                 \
		.prefix_length = DT_PROP(child_id, prefix_length),                                 \
	}

static const struct kfsw_uart_config uart_configs[] = {
	DT_FOREACH_CHILD_STATUS_OKAY_SEP(KFSW_CSP_UARTS_NODE, KFSW_UART_CONFIG_ENTRY, (, ))};

BUILD_ASSERT(ARRAY_SIZE(uart_configs) > 0,
	     "a kfsw,csp-kiss-uarts node must have at least one enabled child");

#else

#define KFSW_CSP_UART_NODE DT_CHOSEN(kfsw_csp_uart)

BUILD_ASSERT(DT_HAS_CHOSEN(kfsw_csp_uart),
	     "KFSW_CSP_KISS_UART requires a kfsw,csp-uart chosen node or "
	     "kfsw,csp-kiss-uarts container");
BUILD_ASSERT(DT_NODE_HAS_STATUS(KFSW_CSP_UART_NODE, okay),
	     "the chosen K-FSW CSP UART must be enabled");
BUILD_ASSERT(DT_NODE_HAS_PROP(KFSW_CSP_UART_NODE, current_speed),
	     "the chosen K-FSW CSP UART must define current-speed");

static const struct kfsw_uart_config uart_configs[] = {
	{
		.device = DEVICE_DT_GET(KFSW_CSP_UART_NODE),
		.interface_name = "KISS",
		.serial = KFSW_UART_SERIAL_CONFIG(KFSW_CSP_UART_NODE),
		.address = CONFIG_KFSW_CSP_ADDRESS,
		.prefix_length = 0,
	},
};

#endif

#if !CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
BUILD_ASSERT(CONFIG_CSP_UART_RX_THREAD_NUM >= ARRAY_SIZE(uart_configs),
	     "CONFIG_CSP_UART_RX_THREAD_NUM must cover every polling KISS/UART interface");
#endif

static struct kfsw_uart_context uart_contexts[ARRAY_SIZE(uart_configs)];
static bool opened;

static bool route_safe_name(const char *name)
{
	size_t length = strnlen(name, CSP_IFLIST_NAME_MAX + 1U);

	if (length == 0U || length >= CSP_IFLIST_NAME_MAX) {
		return false;
	}

	for (size_t i = 0U; i < length; ++i) {
		const char character = name[i];

		if (!((character >= 'A' && character <= 'Z') ||
		      (character >= 'a' && character <= 'z') ||
		      (character >= '0' && character <= '9') || character == '_' ||
		      character == '-')) {
			return false;
		}
	}

	return true;
}

static int preflight(void)
{
	for (size_t i = 0U; i < ARRAY_SIZE(uart_configs); ++i) {
		const struct kfsw_uart_config *config = &uart_configs[i];

		if (!route_safe_name(config->interface_name) ||
		    config->address > csp_id_get_max_nodeid() ||
		    config->prefix_length > csp_id_get_host_bits() ||
		    !device_is_ready(config->device) ||
		    csp_iflist_get_by_name(config->interface_name) != NULL) {
			return CSP_ERR_INVAL;
		}

		for (size_t earlier = 0U; earlier < i; ++earlier) {
			if (strcmp(config->interface_name, uart_configs[earlier].interface_name) ==
				    0 ||
			    config->device == uart_configs[earlier].device) {
				return CSP_ERR_INVAL;
			}
		}
	}

	return CSP_ERR_NONE;
}

static int configure_device(const struct kfsw_uart_config *config)
{
	/*
	 * Native PTYs intentionally return ENOSYS because baud and framing are
	 * properties of the external bridge rather than the simulated device.
	 */
	int result = uart_configure(config->device, &config->serial);

	if (result != 0 && result != -ENOSYS) {
		return CSP_ERR_DRIVER;
	}

	return CSP_ERR_NONE;
}

#if CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
static int interrupt_tx(void *driver_data, const uint8_t *data, size_t data_length)
{
	struct kfsw_uart_context *context = driver_data;

	for (size_t i = 0U; i < data_length; ++i) {
		uart_poll_out(context->config->device, data[i]);
	}

	return CSP_ERR_NONE;
}

static void interrupt_rx(const struct device *device, void *user_data)
{
	struct kfsw_uart_context *context = user_data;
	uint8_t data[KFSW_UART_IRQ_RX_CHUNK_SIZE];
	int task_woken = 0;
	int received;

	if (!uart_irq_update(device)) {
		return;
	}

	if (uart_err_check(device) > 0) {
		context->interrupt_interface.rx_error++;
	}

	if (!uart_irq_rx_ready(device)) {
		return;
	}

	do {
		received = uart_fifo_read(device, data, sizeof(data));
		if (received > 0) {
			csp_kiss_rx(&context->interrupt_interface, data, (size_t)received,
				    &task_woken);
		}
	} while (received > 0);
}

static int open_interrupt(struct kfsw_uart_context *context)
{
	int result;

	context->interrupt_interface.name = context->config->interface_name;
	context->interrupt_interface.addr = context->config->address;
	context->interrupt_interface.netmask = context->config->prefix_length;
	context->interrupt_interface.driver_data = context;
	context->interrupt_interface.interface_data = &context->kiss_data;
	context->kiss_data.tx_func = interrupt_tx;

	result = uart_irq_callback_user_data_set(context->config->device, interrupt_rx, context);
	if (result != 0) {
		return CSP_ERR_DRIVER;
	}

	result = csp_kiss_add_interface(&context->interrupt_interface);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	context->interface = &context->interrupt_interface;
	uart_irq_err_enable(context->config->device);
	uart_irq_rx_enable(context->config->device);
	return CSP_ERR_NONE;
}
#endif

static int open_context(struct kfsw_uart_context *context)
{
#if CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
	return open_interrupt(context);
#else
	const csp_usart_conf_t usart_config = {
		.device = context->config->device->name,
		.baudrate = context->config->serial.baudrate,
		.databits = context->config->serial.data_bits + 5,
		.stopbits = context->config->serial.stop_bits == UART_CFG_STOP_BITS_2 ? 2 : 1,
		.paritysetting = context->config->serial.parity,
	};
	int result = csp_usart_open_and_add_kiss_interface(
		&usart_config, context->config->interface_name, context->config->address,
		&context->interface);

	if (result == CSP_ERR_NONE) {
		context->interface->netmask = context->config->prefix_length;
	}
	return result;
#endif
}

int kfsw_uart_open_all(void)
{
	int result;

	if (opened) {
		return CSP_ERR_NONE;
	}

	result = preflight();
	if (result != CSP_ERR_NONE) {
		return result;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(uart_configs); ++i) {
		result = configure_device(&uart_configs[i]);
		if (result != CSP_ERR_NONE) {
			return result;
		}
	}

	for (size_t i = 0U; i < ARRAY_SIZE(uart_configs); ++i) {
		uart_contexts[i].config = &uart_configs[i];
		result = open_context(&uart_contexts[i]);
		if (result != CSP_ERR_NONE) {
			return result;
		}
	}

	opened = true;
	return CSP_ERR_NONE;
}

size_t kfsw_uart_count(void)
{
	return ARRAY_SIZE(uart_configs);
}

const char *kfsw_uart_first_interface_name(void)
{
	return uart_configs[0].interface_name;
}

static void get_info_at(size_t index, struct kfsw_uart_info *info)
{
	const struct kfsw_uart_config *config = &uart_configs[index];
	csp_iface_t *interface = uart_contexts[index].interface;

	memset(info, 0, sizeof(*info));
	info->device_name = config->device->name;
	info->baudrate = config->serial.baudrate;
	info->ready = device_is_ready(config->device) && interface != NULL;
	info->interface_started = interface != NULL;
	info->interface_name = interface != NULL ? interface->name : config->interface_name;
	info->node = config->address;
	info->prefix_length = config->prefix_length;
	info->peer = CONFIG_KFSW_CSP_UART_PEER_ADDRESS;

	if (interface != NULL) {
		info->tx_packets = interface->tx;
		info->rx_packets = interface->rx;
		info->tx_errors = interface->tx_error;
		info->rx_errors = interface->rx_error;
		info->dropped_packets = interface->drop;
		info->frame_errors = interface->frame;
	}
}

void kfsw_uart_get_info(struct kfsw_uart_info *info)
{
	if (info != NULL) {
		get_info_at(0U, info);
	}
}

void kfsw_uart_visit(kfsw_uart_visitor_t visitor, void *context)
{
	if (visitor == NULL) {
		return;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(uart_configs); ++i) {
		struct kfsw_uart_info info;

		get_info_at(i, &info);
		if (!visitor(&info, context)) {
			break;
		}
	}
}

static bool managed_interface(const csp_iface_t *interface)
{
	for (size_t i = 0U; i < ARRAY_SIZE(uart_contexts); ++i) {
		if (uart_contexts[i].interface == interface) {
			return true;
		}
	}

	return false;
}

int kfsw_uart_test_peer(uint16_t peer, uint32_t timeout_ms,
			struct kfsw_uart_test_result *test_result)
{
	csp_route_t *route;
	uint32_t round_trip_ms;
	int result;

	if (test_result == NULL || !opened) {
		return CSP_ERR_INVAL;
	}

	route = csp_rtable_find_route(peer);
	if (route == NULL || !managed_interface(route->iface)) {
		return CSP_ERR_NOTSUP;
	}

	result = kfsw_csp_ping(peer, timeout_ms, KFSW_UART_TEST_PAYLOAD_SIZE, &round_trip_ms);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	test_result->peer = peer;
	test_result->interface_name = route->iface->name;
	test_result->round_trip_ms = round_trip_ms;
	return CSP_ERR_NONE;
}

int kfsw_uart_test(uint32_t timeout_ms, struct kfsw_uart_test_result *test_result)
{
	return kfsw_uart_test_peer(CONFIG_KFSW_CSP_UART_PEER_ADDRESS, timeout_ms, test_result);
}
