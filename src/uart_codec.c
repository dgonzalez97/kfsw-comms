#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <kfsw/comms/csp.h>
#include <kfsw/comms/uart_codec.h>

static const struct kfsw_uart_codec *codec;
static char selected[CSP_IFLIST_NAME_MAX];
static csp_iface_t *bound;
static nexthop_t original_tx;
K_MSGQ_DEFINE(codec_packets, sizeof(csp_packet_t *), CONFIG_KFSW_CSP_UART_CODEC_QUEUE_DEPTH,
	      sizeof(void *));
BUILD_ASSERT(CONFIG_KFSW_CSP_UART_CODEC_PRIORITY > CONFIG_KFSW_CSP_ROUTER_PRIORITY &&
		     CONFIG_KFSW_CSP_UART_CODEC_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
	     "The UART codec must run below the CSP router");
BUILD_ASSERT(CONFIG_KFSW_CSP_UART_CODEC_QUEUE_DEPTH + 4 <= CSP_BUFFER_COUNT,
	     "Reserve packet buffers outside the codec queue");

static void receive_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		csp_packet_t *packet;

		k_msgq_get(&codec_packets, &packet, K_FOREVER);
		int result = codec->decode(packet);

		if (result == 0) {
			csp_qfifo_write(packet, bound, NULL);
		} else {
			if (result < 0) {
				bound->drop++;
			}
			csp_buffer_free(packet);
		}
	}
}

K_THREAD_DEFINE(kfsw_uart_codec_rx, CONFIG_KFSW_CSP_UART_CODEC_STACK_SIZE, receive_worker, NULL,
		NULL, NULL, CONFIG_KFSW_CSP_UART_CODEC_PRIORITY, 0, SYS_FOREVER_MS);

static int transmit(csp_iface_t *iface, uint16_t via, csp_packet_t *packet, int from_me)
{
	int result = codec->encode(packet);

	/* libcsp frees rejected TX packets. Successful next-hop calls own them. */
	return result == 0 ? original_tx(iface, via, packet, from_me) : CSP_ERR_TX;
}

int kfsw_uart_codec_register(const char *name, const struct kfsw_uart_codec *implementation)
{
	struct kfsw_csp_info info;
	kfsw_csp_get_info(&info);
	if (name == NULL || implementation == NULL || implementation->encode == NULL ||
	    implementation->decode == NULL || name[0] == '\0' || strlen(name) >= sizeof(selected)) {
		return -EINVAL;
	}
	if (codec != NULL || info.initialized) {
		return -EALREADY;
	}
	strcpy(selected, name);
	codec = implementation;
	return 0;
}

int kfsw_uart_codec_check(void)
{
	return codec != NULL && bound == NULL ? -ENODEV : 0;
}

int kfsw_uart_codec_attach(csp_iface_t *iface)
{
	if (codec == NULL || strcmp(iface->name, selected) != 0) {
		return 0;
	}
	if (bound != NULL) {
		return -EALREADY;
	}
	bound = iface;
	original_tx = iface->nexthop;
	iface->nexthop = transmit;
	k_thread_start(kfsw_uart_codec_rx);
	return 0;
}

/* Only the maintained KISS decoder calls this entry point. The compile-time
 * redirect leaves its framing and CRC checks intact and avoids vendor edits.
 */
void kfsw_uart_codec_receive(csp_packet_t *packet, csp_iface_t *iface, void *task_woken)
{
	if (codec == NULL || strcmp(iface->name, selected) != 0) {
		csp_qfifo_write(packet, iface, task_woken);
		return;
	}
	if (k_msgq_put(&codec_packets, &packet, K_NO_WAIT) != 0) {
		iface->drop++;
		if (task_woken != NULL) {
			csp_buffer_free_isr(packet);
		} else {
			csp_buffer_free(packet);
		}
	}
}

int kfsw_uart_codec_control(uint16_t peer, const uint8_t *data, size_t size)
{
	if (bound == NULL || data == NULL || size > CSP_BUFFER_SIZE - sizeof(uint32_t) ||
	    k_is_in_isr()) {
		return -EINVAL;
	}
	csp_packet_t *packet = csp_buffer_get(size);

	if (packet == NULL) {
		return -ENOBUFS;
	}
	packet->id = (csp_id_t){.src = bound->addr, .dst = peer};
	packet->length = (uint16_t)size;
	memcpy(packet->data, data, size);
	int result = original_tx(bound, CSP_NO_VIA_ADDRESS, packet, 1);

	if (result != CSP_ERR_NONE) {
		csp_buffer_free(packet);
		bound->tx_error++;
		return -EIO;
	}
	bound->tx++;
	bound->txbytes += (uint32_t)size;
	return 0;
}
