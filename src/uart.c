#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <csp/csp.h>
#include <csp/csp_rtable.h>
#include <csp/drivers/usart.h>
#include <csp/interfaces/csp_if_kiss.h>

#include <kfsw/comms/csp.h>
#include <kfsw/comms/uart.h>

#include "uart_internal.h"

#define KFSW_UART_TEST_PAYLOAD_SIZE 128U
#define KFSW_UART_IRQ_RX_CHUNK_SIZE 32U

static const struct device *uart_device;
static csp_iface_t *uart_interface;

#if CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
struct kfsw_uart_interrupt_context {
	char name[CSP_IFLIST_NAME_MAX + 1];
	csp_iface_t interface;
	csp_kiss_interface_data_t kiss_data;
	const struct device *device;
};

static struct kfsw_uart_interrupt_context interrupt_context;

static int kfsw_uart_interrupt_tx(void *driver_data, const uint8_t *data,
				  size_t data_length)
{
	struct kfsw_uart_interrupt_context *context = driver_data;

	for (size_t i = 0; i < data_length; i++) {
		uart_poll_out(context->device, data[i]);
	}

	return CSP_ERR_NONE;
}

static void kfsw_uart_interrupt_rx(const struct device *device,
				   void *user_data)
{
	struct kfsw_uart_interrupt_context *context = user_data;
	uint8_t data[KFSW_UART_IRQ_RX_CHUNK_SIZE];
	int errors;
	int task_woken = 0;
	int received;

	if (!uart_irq_update(device)) {
		return;
	}

	errors = uart_err_check(device);
	if (errors > 0) {
		context->interface.rx_error++;
	}

	if (!uart_irq_rx_ready(device)) {
		return;
	}

	do {
		received = uart_fifo_read(device, data, sizeof(data));
		if (received > 0) {
			csp_kiss_rx(&context->interface, data, (size_t)received,
				    &task_woken);
		}
	} while (received > 0);
}

static int kfsw_uart_open_interrupt(uint16_t address,
				    csp_iface_t **return_interface)
{
	int result;

	memset(&interrupt_context, 0, sizeof(interrupt_context));
	strncpy(interrupt_context.name, "KISS",
		sizeof(interrupt_context.name) - 1);
	interrupt_context.interface.name = interrupt_context.name;
	interrupt_context.interface.addr = address;
	interrupt_context.interface.driver_data = &interrupt_context;
	interrupt_context.interface.interface_data =
		&interrupt_context.kiss_data;
	interrupt_context.kiss_data.tx_func = kfsw_uart_interrupt_tx;
	interrupt_context.device = uart_device;

	result = uart_irq_callback_user_data_set(
		uart_device, kfsw_uart_interrupt_rx, &interrupt_context);
	if (result != 0) {
		return CSP_ERR_DRIVER;
	}

	result = csp_kiss_add_interface(&interrupt_context.interface);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	uart_interface = &interrupt_context.interface;
	uart_irq_err_enable(uart_device);
	uart_irq_rx_enable(uart_device);

	if (return_interface != NULL) {
		*return_interface = uart_interface;
	}

	return CSP_ERR_NONE;
}
#endif

static int kfsw_uart_configure(void)
{
	const struct uart_config config = {
		.baudrate = CONFIG_KFSW_CSP_UART_BAUDRATE,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int result;

	uart_device = device_get_binding(CONFIG_KFSW_CSP_KISS_UART_DEVICE);
	if (uart_device == NULL || !device_is_ready(uart_device)) {
		return CSP_ERR_DRIVER;
	}

	/*
	 * The pinned libcsp Zephyr USART backend does not apply csp_usart_conf_t.
	 * Configure physical UARTs here. Native PTYs intentionally return ENOSYS
	 * because baud and framing are properties of the external PTY bridge.
	 */
	result = uart_configure(uart_device, &config);
	if (result != 0 && result != -ENOSYS) {
		return CSP_ERR_DRIVER;
	}

	return CSP_ERR_NONE;
}

int kfsw_uart_open(uint16_t address, csp_iface_t **return_interface)
{
#if !CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
	const csp_usart_conf_t usart_config = {
		.device = CONFIG_KFSW_CSP_KISS_UART_DEVICE,
		.baudrate = CONFIG_KFSW_CSP_UART_BAUDRATE,
		.databits = 8,
		.stopbits = 1,
		.paritysetting = 0,
	};
#endif
	int result;

	if (uart_interface != NULL) {
		if (return_interface != NULL) {
			*return_interface = uart_interface;
		}
		return CSP_ERR_NONE;
	}

	result = kfsw_uart_configure();
	if (result != CSP_ERR_NONE) {
		return result;
	}

#if CONFIG_KFSW_CSP_UART_INTERRUPT_DRIVEN
	return kfsw_uart_open_interrupt(address, return_interface);
#else
	result = csp_usart_open_and_add_kiss_interface(
		&usart_config, "KISS", address, &uart_interface);
	if (result != CSP_ERR_NONE) {
		uart_interface = NULL;
		return result;
	}

	if (return_interface != NULL) {
		*return_interface = uart_interface;
	}

	return CSP_ERR_NONE;
#endif
}

void kfsw_uart_get_info(struct kfsw_uart_info *info)
{
	if (info == NULL) {
		return;
	}

	memset(info, 0, sizeof(*info));
	info->device_name = CONFIG_KFSW_CSP_UART_NAME;
	info->baudrate = CONFIG_KFSW_CSP_UART_BAUDRATE;
	info->ready = uart_device != NULL && device_is_ready(uart_device) &&
		      uart_interface != NULL;
	info->interface_started = uart_interface != NULL;
	info->interface_name =
		uart_interface != NULL ? uart_interface->name : "unavailable";
	info->node = CONFIG_KFSW_CSP_ADDRESS;
	info->peer = CONFIG_KFSW_CSP_UART_PEER_ADDRESS;

	if (uart_interface != NULL) {
		info->tx_packets = uart_interface->tx;
		info->rx_packets = uart_interface->rx;
		info->tx_errors = uart_interface->tx_error;
		info->rx_errors = uart_interface->rx_error;
		info->dropped_packets = uart_interface->drop;
		info->frame_errors = uart_interface->frame;
	}
}

int kfsw_uart_test(uint32_t timeout_ms,
		   struct kfsw_uart_test_result *test_result)
{
	csp_route_t *route;
	uint32_t round_trip_ms;
	int result;

	if (test_result == NULL || uart_interface == NULL) {
		return CSP_ERR_INVAL;
	}

	route = csp_rtable_find_route(CONFIG_KFSW_CSP_UART_PEER_ADDRESS);
	if (route == NULL || route->iface != uart_interface) {
		return CSP_ERR_NOTSUP;
	}

	result = kfsw_csp_ping(CONFIG_KFSW_CSP_UART_PEER_ADDRESS, timeout_ms,
				KFSW_UART_TEST_PAYLOAD_SIZE, &round_trip_ms);
	if (result != CSP_ERR_NONE) {
		return result;
	}

	test_result->peer = CONFIG_KFSW_CSP_UART_PEER_ADDRESS;
	test_result->interface_name = uart_interface->name;
	test_result->round_trip_ms = round_trip_ms;

	return CSP_ERR_NONE;
}
