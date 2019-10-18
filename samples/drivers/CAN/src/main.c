/*
 * Copyright (c) 2018 Alexander Wachter
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <kernel.h>
#include <sys/printk.h>
#include <device.h>
#include <drivers/can.h>
#include <drivers/gpio.h>
#include <misc/byteorder.h>

#define RX_THREAD_STACK_SIZE 512
#define RX_THREAD_PRIORITY 2
#define LED_MSG_ID 0x10
#define COUNTER_MSG_ID 0x12345
#define SET_LED 1
#define RESET_LED 0
#define SLEEP_TIME K_MSEC(250)

K_THREAD_STACK_DEFINE(rx_thread_stack, RX_THREAD_STACK_SIZE);

struct k_thread rx_thread_data;
struct zcan_work rx_work;

CAN_DEFINE_MSGQ(counter_msgq, 2);

void tx_irq_callback(u32_t error_flags, void *arg)
{
	char *sender = (char *)arg;

	if (error_flags) {
		printk("Callback! error-code: %d\nSender: %s\n",
		       error_flags, sender);
	}
}

void rx_thread(void *can_dev_param, void *arg1, void *arg2)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	const struct zcan_filter filter = {
		.id_type = CAN_EXTENDED_IDENTIFIER,
		.rtr = CAN_DATAFRAME,
		.ext_id = COUNTER_MSG_ID,
		.rtr_mask = 1,
		.ext_id_mask = CAN_EXT_ID_MASK
	};
	struct device *can_dev = can_dev_param;
	struct zcan_frame msg;
	int filter_id;

	filter_id = can_attach_msgq(can_dev, &counter_msgq, &filter);
	printk("Counter filter id: %d\n", filter_id);

	while (1) {
		k_msgq_get(&counter_msgq, &msg, K_FOREVER);

		if (msg.dlc != 2U) {
			printk("Wrong data length: %u\n", msg.dlc);
			continue;
		}

		printk("Counter received: %u\n",
		       sys_be16_to_cpu(UNALIGNED_GET((u16_t *)&msg.data)));
	}
}

void change_led(struct zcan_frame *msg, void *led_dev_param)
{
	struct device *led_dev = (struct device *)led_dev_param;

#if defined(DT_ALIAS_LED0_GPIOS_PIN) && defined(DT_ALIAS_LED0_GPIOS_CONTROLLER)

	if (!led_dev_param) {
		printk("No LED GPIO device\n");
		return;
	}

	switch (msg->data[0]) {
	case SET_LED:
		gpio_pin_write(led_dev, DT_ALIAS_LED0_GPIOS_PIN, 1);
		break;
	case RESET_LED:
		gpio_pin_write(led_dev, DT_ALIAS_LED0_GPIOS_PIN, 0);
		break;
	}
#else
	printk("LED %s\n", msg->data[0] == SET_LED ? "ON" : "OFF");
#endif
}

void main(void)
{
	const struct zcan_filter change_led_filter = {
		.id_type = CAN_STANDARD_IDENTIFIER,
		.rtr = CAN_DATAFRAME,
		.std_id = LED_MSG_ID,
		.rtr_mask = 1,
		.std_id_mask = CAN_STD_ID_MASK
	};
	struct zcan_frame change_led_frame = {
		.id_type = CAN_STANDARD_IDENTIFIER,
		.rtr = CAN_DATAFRAME,
		.std_id = LED_MSG_ID,
		.dlc = 1
	};
	struct zcan_frame counter_frame = {
		.id_type = CAN_EXTENDED_IDENTIFIER,
		.rtr = CAN_DATAFRAME,
		.ext_id = COUNTER_MSG_ID,
		.dlc = 2
	};
	u8_t toggle = 1;
	u16_t counter = 0;
	struct device *led_gpio_dev = NULL;
	struct device *can_dev;
	k_tid_t rx_tid;
	int ret;

	/* Usually the CAN device is either called CAN_0 or CAN_1, depending
	 * on the SOC. Let's check both and take the first valid one.
	 */
	can_dev = device_get_binding("CAN_0");
	if (!can_dev) {
		can_dev = device_get_binding("CAN_1");
	}

	if (!can_dev) {
		printk("CAN: Device driver not found.\n");
		return;
	}

#ifdef CONFIG_LOOPBACK_MODE
	can_configure(can_dev, CAN_LOOPBACK_MODE, 125000);
#endif

#if defined(DT_ALIAS_LED0_GPIOS_PIN) && defined(DT_ALIAS_LED0_GPIOS_CONTROLLER)
	led_gpio_dev = device_get_binding(DT_ALIAS_LED0_GPIOS_CONTROLLER);
	if (!led_gpio_dev) {
		printk("LED: Device driver not found.\n");
		return;
	}

	ret = gpio_pin_configure(led_gpio_dev, DT_ALIAS_LED0_GPIOS_PIN,
				 GPIO_DIR_OUT);
	if (ret < 0) {
		printk("Error setting LED pin to output mode [%d]", ret);
	}
#endif

	ret = can_attach_workq(can_dev, &k_sys_work_q, &rx_work, change_led,
			       led_gpio_dev, &change_led_filter);
	if (ret == CAN_NO_FREE_FILTER) {
		printk("Error, no filter available!\n");
		return;
	}

	printk("Change LED filter ID: %d\n", ret);

	rx_tid = k_thread_create(&rx_thread_data, rx_thread_stack,
				 K_THREAD_STACK_SIZEOF(rx_thread_stack),
				 rx_thread, can_dev, NULL, NULL,
				 RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	if (!rx_tid) {
		printk("ERROR spawning rx thread\n");
	}

	printk("Finished init.\n");

	while (1) {
		change_led_frame.data[0] = toggle++ & 0x01 ? SET_LED : RESET_LED;
		/* This sending call is none blocking. */
		can_send(can_dev, &change_led_frame, K_FOREVER, tx_irq_callback,
			 "LED change");
		k_sleep(SLEEP_TIME);

		UNALIGNED_PUT(sys_cpu_to_be16(counter),
			      (u16_t *)&counter_frame.data[0]);
		counter++;
		/* This sending call is blocking until the message is sent. */
		can_send(can_dev, &counter_frame, K_MSEC(100), NULL, NULL);
		k_sleep(SLEEP_TIME);
	}
}
