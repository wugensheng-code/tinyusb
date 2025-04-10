#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>


#define LOG_DECLARE

LOG_MODULE_REGISTER(tinyusb, CONFIG_TINYUSB_LOG_LEVEL);

int usb_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	log_generic(LOG_LEVEL_DBG, fmt, args);
	va_end(args);
	return 0;
}


#include "tusb.h"

struct tinyusb_data {
	k_tid_t tid;
	struct k_thread thread;
	K_KERNEL_STACK_MEMBER(stack, CONFIG_TINYUSB_THREAD_STACK_SIZE);
} tinyusb;

static void usb_thread(void)
{
	while (1) {
		tud_task();
	}
}


static int usb_init(void)
{
	const struct device *const dev = DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0));

	tinyusb.tid = k_thread_create(&tinyusb.thread, tinyusb.stack,
		K_KERNEL_STACK_SIZEOF(tinyusb.stack),
		(k_thread_entry_t)usb_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(CONFIG_TINYUSB_THREAD_PRIO), 0, K_NO_WAIT);

	if (IS_ENABLED(CONFIG_THREAD_NAME)) {
		(void)k_thread_name_set(tinyusb.tid, "tinyusb");
	}
	
	if (!device_is_ready(dev)) {
		LOG_ERR("%s not ready, unable to initialize tinyusb", dev->name);
		return -EIO;
	}


	tusb_init();


	return 0;
}
SYS_INIT_NAMED(tinyusb, usb_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);


