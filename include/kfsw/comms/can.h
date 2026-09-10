#ifndef KFSW_COMMS_CAN_H
#define KFSW_COMMS_CAN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * CSP over CAN.
 *
 * A CAN frame carries at most eight bytes, so libcsp fragments a CSP packet
 * across several of them with its own protocol (CFP). That is libcsp's job;
 * this file owns which controller is used, at what bitrate, and when it starts.
 *
 * The controller comes from the `kfsw,csp-can` chosen node, so reusable code
 * names no board and no peripheral. The same path serves a real controller on
 * a board and Zephyr's native-linux CAN device on a host, which is what lets a
 * ground node reach a USB adapter without a second implementation.
 */

/** Longest interface name libcsp's text parser accepts, plus a terminator. */
#define KFSW_CAN_INTERFACE_NAME_SIZE 10U

/** Bitrates an operator may select. Anything else is refused. */
#define KFSW_CAN_BITRATE_125K 125000U
#define KFSW_CAN_BITRATE_250K 250000U
#define KFSW_CAN_BITRATE_500K 500000U
#define KFSW_CAN_BITRATE_800K 800000U
#define KFSW_CAN_BITRATE_1M 1000000U

/** What the CAN link is doing, for the shell and the board table. */
struct kfsw_can_info {
	/** Interface name as libcsp knows it, empty when not opened. */
	char interface_name[KFSW_CAN_INTERFACE_NAME_SIZE];
	/** Bitrate currently applied, in bits per second. */
	uint32_t bitrate;
	/** CSP address this interface answers for. */
	uint16_t address;
	/** Prefix length of the address, in bits. */
	uint8_t prefix_length;
	/** True once the controller is open and handed to libcsp. */
	bool ready;
};

/**
 * @brief Open the chosen CAN controller and register it with libcsp.
 *
 * Called by the CSP lifecycle after the router exists and before it starts.
 * Returns 0 on success, a negative errno if the controller is missing or
 * refuses the configured bitrate.
 */
int kfsw_can_open(void);

/**
 * @brief Read what the CAN link is doing.
 */
void kfsw_can_get_info(struct kfsw_can_info *info);

/**
 * @brief Whether a bitrate is one an operator may select.
 *
 * Exposed so a parameter validator can reject a bitrate before the
 * controller is opened.
 */
bool kfsw_can_bitrate_supported(uint32_t bitrate);

/**
 * @brief Apply a new bitrate, restarting the controller.
 *
 * This function only reports whether the controller accepted the change.
 *
 * Returns 0 on success, -EINVAL for an unsupported bitrate, -ENODEV when no
 * controller is open.
 */
int kfsw_can_set_bitrate(uint32_t bitrate);

#ifdef __cplusplus
}
#endif

#endif /* KFSW_COMMS_CAN_H */
