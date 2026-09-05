#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>

#include <csp/csp.h>
#include <csp/drivers/can_zephyr.h>

#include <kfsw/comms/can.h>

/* No logging here. This layer sits below the log service and must not reach up
 * into it, so a failure is returned and the composition reports it.
 */

#define KFSW_CAN_NODE DT_CHOSEN(kfsw_csp_can)

BUILD_ASSERT(DT_NODE_EXISTS(KFSW_CAN_NODE),
	     "KFSW_CSP_CAN requires a kfsw,csp-can chosen node naming a CAN controller");

BUILD_ASSERT(sizeof(CONFIG_KFSW_CSP_CAN_INTERFACE_NAME) <= KFSW_CAN_INTERFACE_NAME_SIZE,
	     "the CAN interface name must fit libcsp's nine-character parser limit");

static const struct device *const can_device = DEVICE_DT_GET(KFSW_CAN_NODE);

static csp_iface_t *can_interface;
static uint32_t applied_bitrate = CONFIG_KFSW_CSP_CAN_BITRATE;

bool kfsw_can_bitrate_supported(uint32_t bitrate)
{
	switch (bitrate) {
	case KFSW_CAN_BITRATE_125K:
	case KFSW_CAN_BITRATE_250K:
	case KFSW_CAN_BITRATE_500K:
	case KFSW_CAN_BITRATE_800K:
	case KFSW_CAN_BITRATE_1M:
		return true;
	default:
		return false;
	}
}

int kfsw_can_open(void)
{
	int result;

	if (can_interface != NULL) {
		return 0;
	}
	if (!device_is_ready(can_device)) {
		return -ENODEV;
	}
	if (!kfsw_can_bitrate_supported(applied_bitrate)) {
		return -EINVAL;
	}

	/* A filter address of zero with a zero mask accepts every frame, which
	 * is what a routing node wants: libcsp decides what belongs to it from
	 * the CFP header, and a controller-level filter would drop traffic this
	 * node is meant to forward.
	 */
	result = csp_can_open_and_add_interface(can_device, CONFIG_KFSW_CSP_CAN_INTERFACE_NAME,
						CONFIG_KFSW_CSP_ADDRESS, applied_bitrate, 0U, 0U,
						&can_interface);
	if (result != CSP_ERR_NONE) {
		can_interface = NULL;
		return -EIO;
	}

	can_interface->netmask = CONFIG_KFSW_CSP_CAN_PREFIX_LENGTH;
	return 0;
}

void kfsw_can_get_info(struct kfsw_can_info *info)
{
	if (info == NULL) {
		return;
	}
	memset(info, 0, sizeof(*info));
	info->bitrate = applied_bitrate;
	if (can_interface == NULL) {
		return;
	}
	(void)strncpy(info->interface_name, CONFIG_KFSW_CSP_CAN_INTERFACE_NAME,
		      sizeof(info->interface_name) - 1U);
	info->address = (uint16_t)CONFIG_KFSW_CSP_ADDRESS;
	info->prefix_length = (uint8_t)CONFIG_KFSW_CSP_CAN_PREFIX_LENGTH;
	info->ready = true;
}

int kfsw_can_set_bitrate(uint32_t bitrate)
{
	int result;

	if (!kfsw_can_bitrate_supported(bitrate)) {
		return -EINVAL;
	}
	if (bitrate == applied_bitrate) {
		return 0;
	}
	if (can_interface == NULL) {
		/* Nothing is open yet, so the value is simply what the next
		 * open will use. Refusing here would make the parameter
		 * unsettable on a node whose bus has not come up.
		 */
		applied_bitrate = bitrate;
		return 0;
	}

	result = csp_can_stop(can_interface);
	if (result != CSP_ERR_NONE) {
		return -EIO;
	}
	can_interface = NULL;

	applied_bitrate = bitrate;
	return kfsw_can_open();
}
