#include <soc.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control/renesas_ra_cgc.h>
#include "fsp_common_api.h"
#include "r_usb_device.h"
#include "r_usb_device_api.h"
#include "zephyr/toolchain.h"

#include <zephyr/logging/log.h>

#define LOG_DECLARE

LOG_MODULE_REGISTER(udc_renesas_ra, CONFIG_USB_RENESAS_LOG_LEVEL);


#include "device/dcd.h"


#define CONFIG_UDC_RENESAS_RA_MAX_QMESSAGES						64

#define RHPORT 0

enum udc_bus_speed {
	/** Device is probably not connected */
	UDC_BUS_UNKNOWN,
	/** Device is connected to a full speed bus */
	UDC_BUS_SPEED_FS,
	/** Device is connected to a high speed bus  */
	UDC_BUS_SPEED_HS,
	/** Device is connected to a super speed bus */
	UDC_BUS_SPEED_SS,
};

enum
{
	EDPT_CTRL_OUT = 0x00,
	EDPT_CTRL_IN  = 0x80
};


struct udc_ep_stat {
	/** Endpoint is enabled */
	uint32_t enabled : 1;
	/** Endpoint is halted (returning STALL PID) */
	uint32_t halted : 1;
	/** Last submitted PID is DATA1 */
	uint32_t data1 : 1;
	/** If double buffering is supported, last used buffer is odd */
	uint32_t odd : 1;
	/** Endpoint is busy */
	uint32_t busy : 1;
};

struct data_item {
	void *fifo_reserved;
	const uint8_t *data;
	uint32_t len;
};

/* Endpoint state */
struct usb_dc_ep_state {
    uint16_t ep_mps;    /* Endpoint max packet size */
    uint8_t ep_type;    /* Endpoint type */
    struct udc_ep_stat stat;
	struct k_fifo fifo;
	uint8_t msg_buffer[5 * sizeof(struct data_item)];
};

struct udc_renesas_ra_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device **clocks;
	size_t num_of_clocks;
	size_t num_of_eps;
    struct usb_dc_ep_state *in_ep;  /*!< IN endpoint parameters*/
    struct usb_dc_ep_state *out_ep; /*!< OUT endpoint parameters */
	void (*make_thread)(const struct device *dev);
	int speed_idx;
};


struct udc_renesas_work {
	const struct device *dev;
	struct k_work work;
};

/* Driver state */
struct udc_renesas_ra_data {
    volatile uint8_t dev_addr;
	struct k_work_q udc_work_q;
	struct udc_renesas_work work;
	struct st_usbd_instance_ctrl udc;
	struct st_usbd_cfg udc_cfg;
};

enum udc_renesas_ra_event_type {
	/* An event generated by the HAL driver */
	UDC_RENESAS_RA_EVT_HAL,
	/* Shim driver event to trigger next transfer */
	UDC_RENESAS_RA_EVT_XFER,
	/* Let controller perform status stage */
	UDC_RENESAS_RA_EVT_STATUS,
};

struct udc_renesas_ra_evt {
	enum udc_renesas_ra_event_type type;
	usbd_event_t hal_evt;
	uint8_t ep;
};

K_MSGQ_DEFINE(drv_msgq, sizeof(struct udc_renesas_ra_evt), CONFIG_UDC_RENESAS_RA_MAX_QMESSAGES,
			sizeof(uint32_t));

static ALWAYS_INLINE struct usb_dc_ep_state *
udc_get_ep_cfg(const struct udc_renesas_ra_config *config, const uint8_t ep_addr) {
  if ((tu_edpt_dir(ep_addr) == TUSB_DIR_IN))
    return config->in_ep + tu_edpt_number(ep_addr);
  else
    return config->out_ep + tu_edpt_number(ep_addr);
}

static void udc_renesas_ra_event_handler(usbd_callback_arg_t *p_args)
{
	int ret = k_msgq_put(&drv_msgq, &p_args->event, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("Failed to put event to message queue: %d", ret);
	}
}

static void renesas_ra_thread_handler(struct k_work *work)
{
	struct udc_renesas_work *udc_work = CONTAINER_OF(work, struct udc_renesas_work, work);
	const struct device *dev = udc_work->dev;

	LOG_DBG("Driver %s thread started", dev->name);

	while (true) {
		usbd_event_t event;

		k_msgq_get(&drv_msgq, &event, K_FOREVER);

		switch (event.event_id) {
			case USBD_EVENT_BUS_RESET:
				dcd_event_bus_reset(RHPORT, TUSB_SPEED_HIGH, false);
				break;

			case USBD_EVENT_VBUS_RDY:
				dcd_connect(RHPORT);
				break;

			case USBD_EVENT_VBUS_REMOVED:
				dcd_disconnect(RHPORT);
				break;

			case USBD_EVENT_SUSPEND:
				dcd_event_bus_signal(RHPORT, DCD_EVENT_SUSPEND, false);
				break;

			case USBD_EVENT_RESUME:
				dcd_event_bus_signal(RHPORT, DCD_EVENT_RESUME, false);
				break;

			case USBD_EVENT_SOF:
				dcd_event_sof(RHPORT, event.sof.frame_count, false);
				break;

			case USBD_EVENT_XFER_COMPLETE:
				dcd_event_xfer_complete(RHPORT, event.xfer_complete.ep_addr, event.xfer_complete.len, event.xfer_complete.result, false);
				break;

			case USBD_EVENT_SETUP_RECEIVED:
				dcd_event_setup_received(RHPORT, (uint8_t *)&event.setup_received, false);
				break;
			default:
				LOG_ERR("Unknown event: %d", event.event_id);
				break;
		}
	}
}

extern void usb_device_isr(void);

static void udc_renesas_ra_interrupt_handler(void *arg)
{
	ARG_UNUSED(arg);
    usb_device_isr();
}

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {

	ARG_UNUSED(rhport);
	ARG_UNUSED(rh_init);

	int ret;

	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;

	if (FSP_SUCCESS != R_USBD_Open(&data->udc, &data->udc_cfg)) {
		LOG_ERR("Failed to open device: %s", dev->name);
		return false;
	}

	tusb_desc_endpoint_t control_out_ep_desc;
	tusb_desc_endpoint_t control_in_ep_desc;

	control_out_ep_desc.bDescriptorType = TUSB_XFER_CONTROL;
	control_out_ep_desc.bEndpointAddress = EDPT_CTRL_OUT;
	control_out_ep_desc.bInterval = 0;
	control_out_ep_desc.bLength = sizeof(usbd_desc_endpoint_t);
	control_out_ep_desc.wMaxPacketSize = 64;
	control_out_ep_desc.bmAttributes.xfer = TUSB_XFER_CONTROL;

	if(!dcd_edpt_open(EDPT_CTRL_OUT, &control_out_ep_desc)) {
		LOG_ERR("Failed to enable control endpoint");
		return false;
	}


	control_in_ep_desc.bDescriptorType = TUSB_XFER_CONTROL;
	control_in_ep_desc.bEndpointAddress = EDPT_CTRL_IN;
	control_in_ep_desc.bInterval = 0;
	control_in_ep_desc.bLength = sizeof(usbd_desc_endpoint_t);
	control_in_ep_desc.wMaxPacketSize = 64;
	control_in_ep_desc.bmAttributes.xfer = TUSB_XFER_CONTROL;


	if(!dcd_edpt_open(EDPT_CTRL_IN, &control_in_ep_desc)) {
		LOG_ERR("Failed to enable control endpoint");
		return false;
	}

	dcd_int_enable(RHPORT);

	if (FSP_SUCCESS != R_USBD_Connect(&data->udc)) {
		return -EIO;
	}
	
	ret = k_work_submit_to_queue(&data->udc_work_q, &data->work.work);

	if (ret < 0) {
		LOG_ERR("Failed to submit work to queue: %d", ret);
		return false;
	} else {
		LOG_DBG("Enable device %p", dev);
		return true;
	}
}


void dcd_int_enable(uint8_t rhport) {
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = dev->data;

	#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
	if (data->udc_cfg.hs_irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_enable(data->udc_cfg.hs_irq);
	}
#endif

	if (data->udc_cfg.irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_enable(data->udc_cfg.irq);
	}

	if (data->udc_cfg.irq_r != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_enable(data->udc_cfg.irq_r);
	}
}

void dcd_int_disable(uint8_t rhport) {
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = dev->data;

	#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
	if (data->udc_cfg.hs_irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_disable(data->udc_cfg.hs_irq);
	}
#endif

	if (data->udc_cfg.irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_disable(data->udc_cfg.irq);
	}

	if (data->udc_cfg.irq_r != (IRQn_Type)BSP_IRQ_DISABLED) {
		irq_disable(data->udc_cfg.irq_r);
	}
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr) {

	ARG_UNUSED(rhport);

	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	/* The USB controller will automatically perform a response to the SET_ADRRESS request. */
	LOG_DBG("Set new address %u for %s", dev_addr, dev->name);

	return;
}

void dcd_remote_wakeup(uint8_t rhport)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;

	if (FSP_SUCCESS != R_USBD_RemoteWakeup(&data->udc)) {
		LOG_ERR("Remote wakeup from %s failed", dev->name);
	} else {
		LOG_DBG("Remote wakeup from %s", dev->name);
	}

	return;
}

void dcd_connect(uint8_t rhport)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;

	R_USBD_Connect(&data->udc);
}

void dcd_disconnect(uint8_t rhport)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;

	R_USBD_Disconnect(&data->udc);
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
	R_USB_HS0->INTENB0_b.SOFE = en ? 1: 0;
	LOG_DBG("SOF %s", en ? "enabled" : "disabled");
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const * ep_desc)
{
    const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));

	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;
	struct udc_renesas_ra_config *config = (struct udc_renesas_ra_config *)dev->config;

    usbd_desc_endpoint_t ep;
    uint8_t ep_idx = tu_edpt_number(ep_desc->bEndpointAddress);



    if (tu_edpt_dir(ep_desc->bEndpointAddress) == TUSB_DIR_OUT) {
		if (config->out_ep[ep_idx].stat.enabled == true)
			return true;
        config->out_ep[ep_idx].ep_mps = tu_edpt_packet_size(ep_desc);
        config->out_ep[ep_idx].ep_type = tu_desc_type(ep_desc);
        config->out_ep[ep_idx].stat.enabled = true;
		k_fifo_init(&config->out_ep[ep_idx].fifo);
    } else {
		if (config->in_ep[ep_idx].stat.enabled == true)
			return true;
        config->in_ep[ep_idx].ep_mps = tu_edpt_packet_size(ep_desc);
        config->in_ep[ep_idx].ep_type = tu_desc_type(ep_desc);
        config->in_ep[ep_idx].stat.enabled = true;
		k_fifo_init(&config->in_ep[ep_idx].fifo);
    }

	if (ep_idx == 0) {
		return true;
	}

	ep.bLength = sizeof(tusb_desc_endpoint_t);
	ep.bDescriptorType = tu_desc_type(ep_desc);
	ep.bEndpointAddress = ep_desc->bEndpointAddress;
	ep.Attributes.sync = ep_desc->bmAttributes.sync;
	ep.Attributes.xfer = ep_desc->bmAttributes.xfer;
	ep.wMaxPacketSize = ep_desc->wMaxPacketSize;
	ep.bInterval = ep_desc->bInterval;

	if (FSP_SUCCESS != R_USBD_EdptOpen(&data->udc, &ep)) {
		LOG_ERR("Enable ep 0x%02x failed", ep_desc->bEndpointAddress);
		return false;
	}


	LOG_DBG("Enable ep 0x%02x", ep_desc->bEndpointAddress);

	return true;
}

void dcd_edpt_close(uint8_t rhport, uint8_t ep_addr)
{
    const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));

	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;
	struct udc_renesas_ra_config *config = (struct udc_renesas_ra_config *)dev->config;

	uint8_t ep_idx = tu_edpt_number(ep_addr);

	if (tu_edpt_dir(ep_addr) == TUSB_DIR_OUT) {
		config->out_ep[ep_idx].stat.enabled = false;
	} else {
		config->in_ep[ep_idx].stat.enabled = false;
	}

	if (FSP_SUCCESS != R_USBD_EdptClose(&data->udc, ep_addr)) {
		LOG_DBG("Disable ep 0x%02x failed", ep_addr);
	} else {
		LOG_DBG("Disable ep 0x%02x", ep_addr);
	}

    return;
}

void dcd_edpt_close_all(uint8_t rhport)
{
	dcd_int_disable(rhport);
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;
	struct udc_renesas_ra_config *config = (struct udc_renesas_ra_config *)dev->config;
	for (uint8_t i = 1; i < config->num_of_eps; i++) {
		if (config->in_ep[i].stat.enabled) {
			R_USBD_EdptClose(&data->udc, tu_edpt_addr(i, TUSB_DIR_IN));
			config->in_ep[i].stat.enabled = false;
		}
		if (config->out_ep[i].stat.enabled) {
			R_USBD_EdptClose(&data->udc, tu_edpt_addr(i, TUSB_DIR_OUT));
			config->out_ep[i].stat.enabled = false;
		}
	}
	dcd_int_enable(rhport);

}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *dev_data = (struct udc_renesas_ra_data *)dev->data;
	int err;


	if (!buffer && total_bytes) {
	    return false;
	}

	LOG_DBG("D%sH: ep: %d data: %p len: %d", ((tu_edpt_dir(ep_addr) == TUSB_DIR_IN) ? "->" : "<-"),
		tu_edpt_number(ep_addr), buffer, total_bytes);

	err = R_USBD_XferStart(&dev_data->udc, ep_addr, (uint8_t*)buffer, total_bytes);
	if (err != FSP_SUCCESS) {
		LOG_ERR("Failed to start write on ep 0x%02x", ep_addr);
		return false;
	}

    return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;

	LOG_DBG("Set halt ep 0x%02x", ep_addr);

	if (FSP_SUCCESS != R_USBD_EdptStall(&data->udc, ep_addr)) {
		LOG_DBG("Set halt ep 0x%02x failed", ep_addr);
	}

	return;
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));
	struct udc_renesas_ra_data *data = (struct udc_renesas_ra_data *)dev->data;

	LOG_DBG("Clear halt ep 0x%02x", ep_addr);

	if (FSP_SUCCESS != R_USBD_EdptClearStall(&data->udc, ep_addr)) {
		LOG_DBG("Clear halt ep 0x%02x failed", ep_addr);
	}

    return;
}

static int udc_renesas_ra_driver_preinit(const struct device *dev)
{
	int err;
	const struct udc_renesas_ra_config *config = dev->config;
	struct udc_renesas_ra_data *data = dev->data;

#if !USBHS_PHY_CLOCK_SOURCE_IS_XTAL
	if (data->udc_cfg.usb_speed == USBD_SPEED_HS) {
		LOG_ERR("High-speed operation is not supported in case PHY clock source is not "
			"XTAL");
		return -ENOTSUP;
	}
#endif

	if (config->speed_idx == UDC_BUS_SPEED_HS) {
		if (!(data->udc_cfg.usb_speed == USBD_SPEED_HS ||
		      data->udc_cfg.usb_speed == USBD_SPEED_FS)) {
			LOG_ERR("USBHS module only support high-speed and full-speed device");
			return -ENOTSUP;
		}
	} else {
		/* config->speed_idx == UDC_BUS_SPEED_FS */
		if (data->udc_cfg.usb_speed != USBD_SPEED_FS) {
			LOG_ERR("USBFS module only support full-speed device");
			return -ENOTSUP;
		}
	}


#if USBHS_PHY_CLOCK_SOURCE_IS_XTAL
	if (config->speed_idx == UDC_BUS_SPEED_HS) {
		if (BSP_CFG_XTAL_HZ == 0) {
			LOG_ERR("XTAL clock should be provided");
			return -EINVAL;
		}

		goto finishi_clk_check;
	}
#endif

	for (size_t i = 0; i < config->num_of_clocks; i++) {
		const struct device *clock_dev = *(config->clocks + i);
		const struct clock_control_ra_pclk_cfg *clock_cfg = clock_dev->config;
		uint32_t clk_src_rate;
		uint32_t clock_rate;

		if (!device_is_ready(clock_dev)) {
			LOG_ERR("%s is not ready", clock_dev->name);
			return -ENODEV;
		}

		clk_src_rate = R_BSP_SourceClockHzGet(clock_cfg->clk_src);
		clock_rate = clk_src_rate / clock_cfg->clk_div;

		if (strcmp(clock_dev->name, "uclk") == 0 && clock_rate != MHZ(48)) {
			LOG_ERR("Setting for uclk should be 48Mhz");
			return -ENOTSUP;
		}

#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
		if (strcmp(clock_dev->name, "u60clk") == 0 && clock_rate != MHZ(60)) {
			LOG_ERR("Setting for u60clk should be 60Mhz");
			return -ENOTSUP;
		}
#endif
	}

finishi_clk_check:

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		return err;
	}

#if DT_HAS_COMPAT_STATUS_OKAY(renesas_ra_usbhs)
	if (data->udc_cfg.hs_irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		R_ICU->IELSR[data->udc_cfg.hs_irq] = ELC_EVENT_USBHS_USB_INT_RESUME;
	}
#endif

	if (data->udc_cfg.irq != (IRQn_Type)BSP_IRQ_DISABLED) {
		R_ICU->IELSR[data->udc_cfg.irq] = ELC_EVENT_USBFS_INT;
	}

	if (data->udc_cfg.irq_r != (IRQn_Type)BSP_IRQ_DISABLED) {
		R_ICU->IELSR[data->udc_cfg.irq_r] = ELC_EVENT_USBFS_RESUME;
	}

	config->make_thread(dev);
	LOG_INF("Device %p (max. speed %d)", dev, data->udc_cfg.usb_speed);

    return 0;
}

#define DT_DRV_COMPAT renesas_ra_udc

#define USB_RENESAS_RA_MODULE_NUMBER(id) (DT_REG_ADDR(id) == R_USB_FS0_BASE ? 0 : 1)

#define USB_RENESAS_RA_MODULE_NUMBER(id) (DT_REG_ADDR(id) == R_USB_FS0_BASE ? 0 : 1)

#define USB_RENESAS_RA_IRQ_GET(id, name, cell)                                                     \
	COND_CODE_1(DT_IRQ_HAS_NAME(id, name), (DT_IRQ_BY_NAME(id, name, cell)),                   \
		    ((IRQn_Type) BSP_IRQ_DISABLED))

#define USB_RENESAS_RA_MAX_SPEED_IDX(id)                                                           \
	(DT_NODE_HAS_COMPAT(id, renesas_ra_usbhs) ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS)

#define USB_RENESAS_RA_SPEED_IDX(id)                                                               \
	(DT_NODE_HAS_COMPAT(id, renesas_ra_usbhs)                                                  \
		 ? DT_ENUM_IDX_OR(id, maximum_speed, UDC_BUS_SPEED_HS)                             \
		 : DT_ENUM_IDX_OR(id, maximum_speed, UDC_BUS_SPEED_FS))

#define USB_RENESAS_RA_IRQ_CONNECT(idx, n)                                                         \
	IRQ_CONNECT(DT_IRQ_BY_IDX(DT_INST_PARENT(n), idx, irq),                                    \
		    DT_IRQ_BY_IDX(DT_INST_PARENT(n), idx, priority),                               \
		    udc_renesas_ra_interrupt_handler, DEVICE_DT_INST_GET(n), 0)

#define USB_RENESAS_RA_CLOCKS_GET(idx, id)                                                         \
	DEVICE_DT_GET_OR_NULL(DT_PHANDLE_BY_IDX(id, phys_clock, idx))

#define UDC_RENESAS_RA_DEVICE_DEFINE(n)                                                            \
	PINCTRL_DT_DEFINE(DT_INST_PARENT(n));                                                      \
																								\
                                                                                                   \
	static const struct device *udc_renesas_ra_clock_dev_##n[] = {                             \
		LISTIFY(DT_PROP_LEN_OR(DT_INST_PARENT(n), phys_clock, 0),                          \
			USB_RENESAS_RA_CLOCKS_GET, (,), DT_INST_PARENT(n))                         \
	};                                                                                         \
																							\
	K_THREAD_STACK_DEFINE(udc_renesas_ra_stack_##n, CONFIG_TINYUSB_RENESAS_THREAD_STACK_SIZE);         \
                                                                                                   \
                                                                                                   \
	static void udc_renesas_ra_make_thread_##n(const struct device *dev)                       \
	{                                                                                          \
		struct udc_renesas_ra_data *data = dev->data;                           \
																					\
		data->work.dev = dev;										  \
		k_work_queue_init(&data->udc_work_q);													\
		k_work_init(&data->work.work, renesas_ra_thread_handler);									\
                                                                                                   \
		k_work_queue_start(&data->udc_work_q, udc_renesas_ra_stack_##n,                      \
				K_THREAD_STACK_SIZEOF(udc_renesas_ra_stack_##n),                   \
				K_PRIO_COOP(CONFIG_TINYUSB_RENESAS_THREAD_PRIO), NULL);             \
		k_thread_name_set(&data->udc_work_q.thread, dev->name);                                  \
	}                                                                                          \
                                                                                                   \
	static struct usb_dc_ep_state ep_cfg_in##n[DT_PROP(DT_INST_PARENT(n), num_bidir_endpoints)]; \
	static struct usb_dc_ep_state                                                                \
		ep_cfg_out##n[DT_PROP(DT_INST_PARENT(n), num_bidir_endpoints)];                    \
                                                                                                   \
	static const struct udc_renesas_ra_config udc_renesas_ra_config_##n = {                    \
		.pcfg = PINCTRL_DT_DEV_CONFIG_GET(DT_INST_PARENT(n)),                              \
		.clocks = udc_renesas_ra_clock_dev_##n,                                            \
		.num_of_clocks = DT_PROP_LEN_OR(DT_INST_PARENT(n), phys_clock, 0),                 \
		.num_of_eps = DT_PROP(DT_INST_PARENT(n), num_bidir_endpoints),                     \
		.in_ep = ep_cfg_in##n,                                                         \
		.out_ep = ep_cfg_out##n,                                                       \
		.make_thread = udc_renesas_ra_make_thread_##n,                                     \
		.speed_idx = USB_RENESAS_RA_MAX_SPEED_IDX(DT_INST_PARENT(n)),                      \
	};                                                                                         \
                                                                                                   \
	static struct udc_renesas_ra_data udc_priv_##n = {                                         \
		.udc_cfg =                                                                         \
			{                                                                          \
				.module_number = USB_RENESAS_RA_MODULE_NUMBER(DT_INST_PARENT(n)),  \
				.usb_speed = USB_RENESAS_RA_SPEED_IDX(DT_INST_PARENT(n)),          \
				.irq = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_i, irq),    \
				.irq_r = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_r, irq),  \
				.hs_irq =                                                          \
					USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbhs_ir, irq),  \
				.ipl = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_i,          \
							      priority),                           \
				.ipl_r = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbfs_r,        \
								priority),                         \
				.hsipl = USB_RENESAS_RA_IRQ_GET(DT_INST_PARENT(n), usbhs_ir,       \
								priority),                         \
				.p_context = DEVICE_DT_INST_GET(n),                                \
				.p_callback = udc_renesas_ra_event_handler,                        \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
                                                                                   \
	int udc_renesas_ra_driver_preinit##n(const struct device *dev)                             \
	{                                                                                          \
		LISTIFY(DT_NUM_IRQS(DT_INST_PARENT(n)), USB_RENESAS_RA_IRQ_CONNECT, (;), n);       \
		return udc_renesas_ra_driver_preinit(dev);                                         \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, udc_renesas_ra_driver_preinit##n, NULL, &udc_priv_##n,            \
			      &udc_renesas_ra_config_##n, POST_KERNEL,                             \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(UDC_RENESAS_RA_DEVICE_DEFINE)
