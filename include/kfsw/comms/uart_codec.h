#ifndef KFSW_COMMS_UART_CODEC_H
#define KFSW_COMMS_UART_CODEC_H

#include <csp/csp_interface.h>

/** Optional packet protection for one named KISS interface. */
struct kfsw_uart_codec {
	/** Transform a packet before KISS framing; negative errno rejects TX. */
	int (*encode)(csp_packet_t *packet);
	/** Restore a packet before routing. 0 routes, 1 consumes, negative drops. */
	int (*decode)(csp_packet_t *packet);
};

/** Register a static codec before CSP initialization. One registration only. */
int kfsw_uart_codec_register(const char *interface_name, const struct kfsw_uart_codec *codec);

/** Send a codec control payload directly on its bound interface. Thread context only. */
int kfsw_uart_codec_control(uint16_t peer, const uint8_t *data, size_t size);

#endif
