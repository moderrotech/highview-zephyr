/*
 * Copyright (c) 2016 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr.h>
#include <arch/cpu.h>
#include <sys/byteorder.h>
#include <logging/log.h>
#include <sys/util.h>

#include <device.h>
#include <init.h>
#include <drivers/uart.h>

#include <net/buf.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/buf.h>
#include <bluetooth/hci_raw.h>



// ********************************************************************************************** begin
// Kai: code for upgrading 52840 firmware

#include <drivers/gpio.h>
#include <dfu/mcuboot.h>
#include <lte_uart_dfu.h>

#define MY_GPIO_HOST_UPGRADE_52840	22

static void my_check_upgrade(void)
{
	int gpio_status;
	const struct device *gpio_dev = device_get_binding("GPIO_0");
	if (gpio_dev == 0) 
		printk("ERROR: %s(): GPIO bind\n", __FUNCTION__);
	gpio_pin_configure(gpio_dev, MY_GPIO_HOST_UPGRADE_52840, GPIO_INPUT);
	k_sleep(K_MSEC(100));
	gpio_status = gpio_pin_get_raw(gpio_dev, MY_GPIO_HOST_UPGRADE_52840);
	if (gpio_status == 1)
	{
		printk("start upgrade...\n");
		lte_uart_dfu_start();		// this function won't return
	}
}

// **********************************************************************************************  end



#define LOG_MODULE_NAME hci_uart
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

static const struct device *hci_uart_dev;
static K_THREAD_STACK_DEFINE(tx_thread_stack, CONFIG_BT_HCI_TX_STACK_SIZE);
static struct k_thread tx_thread_data;
static K_FIFO_DEFINE(tx_queue);

/* RX in terms of bluetooth communication */
static K_FIFO_DEFINE(uart_tx_queue);

#define H4_CMD 0x01
#define H4_ACL 0x02
#define H4_SCO 0x03
#define H4_EVT 0x04

/* Receiver states. */
#define ST_IDLE 0	/* Waiting for packet type. */
#define ST_HDR 1	/* Receiving packet header. */
#define ST_PAYLOAD 2	/* Receiving packet payload. */
#define ST_DISCARD 3	/* Dropping packet. */

/* Length of a discard/flush buffer.
 * This is sized to align with a BLE HCI packet:
 * 1 byte H:4 header + 32 bytes ACL/event data
 * Bigger values might overflow the stack since this is declared as a local
 * variable, smaller ones will force the caller to call into discard more
 * often.
 */
#define H4_DISCARD_LEN 33

static int h4_read(const struct device *uart, uint8_t *buf, size_t len)
{
	int rx = uart_fifo_read(uart, buf, len);

	LOG_DBG("read %d req %d", rx, len);

	return rx;
}

static bool valid_type(uint8_t type)
{
	return (type == H4_CMD) | (type == H4_ACL);
}

/* Function assumes that type is validated and only CMD or ACL will be used. */
static uint32_t get_len(const uint8_t *hdr_buf, uint8_t type)
{
	return (type == BT_BUF_CMD) ?
		((const struct bt_hci_cmd_hdr *)hdr_buf)->param_len :
		sys_le16_to_cpu(((const struct bt_hci_acl_hdr *)hdr_buf)->len);
}

/* Function assumes that type is validated and only CMD or ACL will be used. */
static int hdr_len(uint8_t type)
{
	return (type == H4_CMD) ?
		sizeof(struct bt_hci_cmd_hdr) : sizeof(struct bt_hci_acl_hdr);
}

static void rx_isr(void)
{
	static struct net_buf *buf;
	static int remaining;
	static uint8_t state;
	static uint8_t type;
	static uint8_t hdr_buf[MAX(sizeof(struct bt_hci_cmd_hdr),
			sizeof(struct bt_hci_acl_hdr))];
	int read;

	do {
		switch (state) {
		case ST_IDLE:
			/* Get packet type */
			read = h4_read(hci_uart_dev, &type, sizeof(type));
			/* since we read in loop until no data is in the fifo,
			 * it is possible that read = 0.
			 */
			if (read) {
				if (valid_type(type)) {
					/* Get expected header size and switch
					 * to receiving header.
					 */
					remaining = hdr_len(type);
					state = ST_HDR;
				} else {
					LOG_WRN("Unknown header %d", type);
				}
			}
			break;
		case ST_HDR:
			read = h4_read(hci_uart_dev,
				       &hdr_buf[hdr_len(type) - remaining],
				       remaining);
			remaining -= read;
			if (remaining == 0) {
				/* Header received. Allocate buffer and get
				 * payload length. If allocation fails leave
				 * interrupt. On failed allocation state machine
				 * is reset.
				 */
				buf = bt_buf_get_tx(BT_BUF_H4, K_NO_WAIT,
						    &type, sizeof(type));
				if (!buf) {
					state = ST_IDLE;
					return;
				}

				remaining = get_len(hdr_buf, type);

				net_buf_add_mem(buf, hdr_buf, hdr_len(type));
				if (remaining > net_buf_tailroom(buf)) {
					LOG_ERR("Not enough space in buffer");
					net_buf_unref(buf);
					state = ST_DISCARD;
				} else {
					state = ST_PAYLOAD;
				}

			}
			break;
		case ST_PAYLOAD:
			read = h4_read(hci_uart_dev, net_buf_tail(buf),
				       remaining);
			buf->len += read;
			remaining -= read;
			if (remaining == 0) {
				/* Packet received */
				LOG_DBG("putting RX packet in queue.");
				net_buf_put(&tx_queue, buf);
				state = ST_IDLE;
			}
			break;
		case ST_DISCARD:
		{
			uint8_t discard[H4_DISCARD_LEN];
			size_t to_read = MIN(remaining, sizeof(discard));

			read = h4_read(hci_uart_dev, discard, to_read);
			remaining -= read;
			if (remaining == 0) {
				state = ST_IDLE;
			}

			break;

		}
		default:
			read = 0;
			__ASSERT_NO_MSG(0);
			break;

		}
	} while (read);
}

static void tx_isr(void)
{
	static struct net_buf *buf;
	int len;

	if (!buf) {
		buf = net_buf_get(&uart_tx_queue, K_NO_WAIT);
		if (!buf) {
			uart_irq_tx_disable(hci_uart_dev);
			return;
		}
	}

	len = uart_fifo_fill(hci_uart_dev, buf->data, buf->len);
	net_buf_pull(buf, len);
	if (!buf->len) {
		net_buf_unref(buf);
		buf = NULL;
	}
}

static void bt_uart_isr(const struct device *unused, void *user_data)
{
	ARG_UNUSED(unused);
	ARG_UNUSED(user_data);

	if (!(uart_irq_rx_ready(hci_uart_dev) ||
	      uart_irq_tx_ready(hci_uart_dev))) {
		LOG_DBG("spurious interrupt");
	}

	if (uart_irq_tx_ready(hci_uart_dev)) {
		tx_isr();
	}

	if (uart_irq_rx_ready(hci_uart_dev)) {
		rx_isr();
	}
}



//************************************************************ added by Kai begin

// firmware version
#define KAI_VERSION_MAJOR		6
#define KAI_VERSION_MINOR		0
#define KAI_VERSION_BUILD		200


// It used to work by modifying vs_read_version_info() in hci.c. However it works for Zephyr BLE controller only, not for SoftDevice Controller. That's why we need the following code.
static inline void my_hci_op_vs_read_version_info(struct net_buf *buf)
{
	// check if it is BT_HCI_OP_VS_READ_VERSION_INFO (0xfc01)
	{
		uint8_t *p2 = buf->data;
		if (	buf->len != 3 || 		// length of total
			p2[0] != 1 || 		// opcode-1
			p2[1] != 0xfc || 		// opcode-2
			p2[2] != 0)			// length of payload
			return;
	}
	
	static uint8_t cmd[24];
	uint8_t *p3 = cmd;
	*p3 = 0x04;					p3 ++;		// type: H4_EVT
	*p3 = 0x0e;					p3 ++;		// event: BT_HCI_EVT_CMD_COMPLETE
								p3 ++;		// length of payload
	*p3 = 0x01;					p3 ++;		// ncmd (payload starts here)
	*p3 = 0x01;					p3 ++;		// opcode: BT_HCI_OP_VS_READ_VERSION_INFO
	*p3 = 0xfc;					p3 ++;		// opcode
	*p3 = 0x00;					p3 ++;		// bt_hci_rp_vs_read_version_info -- status
	*p3 = 0x00;					p3 ++;		// hw_platform - 1
	*p3 = 0x00;					p3 ++;		// hw_platform - 2
	*p3 = 0x00;					p3 ++;		// hw_variant - 1
	*p3 = 0x00;					p3 ++;		// hw_variant - 2
	*p3 = 0x00;					p3 ++;		// fw_variant
	*p3 = KAI_VERSION_MAJOR;	p3 ++;		// fw_version
	*p3 = KAI_VERSION_MINOR;	p3 ++;		// fw_revision - 1
	*p3 = 0x00;					p3 ++;		// fw_revision - 2
	*p3 = KAI_VERSION_BUILD;		p3 ++;		// fw_build - 1
	*p3 = 0x00;					p3 ++;		// fw_build - 2
	*p3 = 0x00;					p3 ++;		// fw_build - 3
	*p3 = 0x00;					p3 ++;		// fw_build - 4
	uint32_t cmd_len = (uint32_t) (p3 - cmd);
	cmd[2] = cmd_len - 3;
	int sent_len = uart_fifo_fill(hci_uart_dev, cmd, cmd_len);
	printk("%s() sent %d bytes\n", __FUNCTION__, sent_len);
}

//************************************************************ added by Kai end



static void tx_thread(void *p1, void *p2, void *p3)
{
	while (1) {
		struct net_buf *buf;
		int err;

		/* Wait until a buffer is available */
		buf = net_buf_get(&tx_queue, K_FOREVER);


		// added by Kai
		my_hci_op_vs_read_version_info(buf);


		/* Pass buffer to the stack */
		err = bt_send(buf);
		if (err) {
			LOG_ERR("Unable to send (err %d)", err);
			net_buf_unref(buf);
		}

		/* Give other threads a chance to run if tx_queue keeps getting
		 * new data all the time.
		 */
		k_yield();
	}
}

static int h4_send(struct net_buf *buf)
{
	LOG_DBG("buf %p type %u len %u", buf, bt_buf_get_type(buf),
		    buf->len);

	net_buf_put(&uart_tx_queue, buf);
	uart_irq_tx_enable(hci_uart_dev);

	return 0;
}

#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER)
void bt_ctlr_assert_handle(char *file, uint32_t line)
{
	uint32_t len = 0U, pos = 0U;

	/* Disable interrupts, this is unrecoverable */
	(void)irq_lock();

	uart_irq_rx_disable(hci_uart_dev);
	uart_irq_tx_disable(hci_uart_dev);

	if (file) {
		while (file[len] != '\0') {
			if (file[len] == '/') {
				pos = len + 1;
			}
			len++;
		}
		file += pos;
		len -= pos;
	}

	uart_poll_out(hci_uart_dev, H4_EVT);
	/* Vendor-Specific debug event */
	uart_poll_out(hci_uart_dev, 0xff);
	/* 0xAA + strlen + \0 + 32-bit line number */
	uart_poll_out(hci_uart_dev, 1 + len + 1 + 4);
	uart_poll_out(hci_uart_dev, 0xAA);

	if (len) {
		while (*file != '\0') {
			uart_poll_out(hci_uart_dev, *file);
			file++;
		}
		uart_poll_out(hci_uart_dev, 0x00);
	}

	uart_poll_out(hci_uart_dev, line >> 0 & 0xff);
	uart_poll_out(hci_uart_dev, line >> 8 & 0xff);
	uart_poll_out(hci_uart_dev, line >> 16 & 0xff);
	uart_poll_out(hci_uart_dev, line >> 24 & 0xff);

	while (1) {
	}
}
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER */

static int hci_uart_init(const struct device *unused)
{
	LOG_DBG("");

	/* Derived from DT's bt-c2h-uart chosen node */
	hci_uart_dev = device_get_binding(CONFIG_BT_CTLR_TO_HOST_UART_DEV_NAME);
	if (!hci_uart_dev) {
		return -EINVAL;
	}

	uart_irq_rx_disable(hci_uart_dev);
	uart_irq_tx_disable(hci_uart_dev);

	uart_irq_callback_set(hci_uart_dev, bt_uart_isr);

	uart_irq_rx_enable(hci_uart_dev);

	return 0;
}

DEVICE_INIT(hci_uart, "hci_uart", &hci_uart_init, NULL, NULL,
	    APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

void main(void)
{


// ********************************************************************************************** begin
// Kai: code for upgrading 52840 firmware

	printk("BLE app v%u.%u.%u\n", KAI_VERSION_MAJOR, KAI_VERSION_MINOR, KAI_VERSION_BUILD);

	// mark application upgrade success (automatically check if needed inside this function)
	boot_write_img_confirmed();
	
	my_check_upgrade();

// ********************************************************************************************** end



	/* incoming events and data from the controller */
	static K_FIFO_DEFINE(rx_queue);
	int err;

	LOG_DBG("Start");
	__ASSERT(hci_uart_dev, "UART device is NULL");

	/* Enable the raw interface, this will in turn open the HCI driver */
	bt_enable_raw(&rx_queue);

	if (IS_ENABLED(CONFIG_BT_WAIT_NOP)) {
		/* Issue a Command Complete with NOP */
		int i;

		const struct {
			const uint8_t h4;
			const struct bt_hci_evt_hdr hdr;
			const struct bt_hci_evt_cmd_complete cc;
		} __packed cc_evt = {
			.h4 = H4_EVT,
			.hdr = {
				.evt = BT_HCI_EVT_CMD_COMPLETE,
				.len = sizeof(struct bt_hci_evt_cmd_complete),
			},
			.cc = {
				.ncmd = 1,
				.opcode = sys_cpu_to_le16(BT_OP_NOP),
			},
		};

		for (i = 0; i < sizeof(cc_evt); i++) {
			uart_poll_out(hci_uart_dev,
				      *(((const uint8_t *)&cc_evt)+i));
		}
	}

	/* Spawn the TX thread and start feeding commands and data to the
	 * controller
	 */
	k_thread_create(&tx_thread_data, tx_thread_stack,
			K_THREAD_STACK_SIZEOF(tx_thread_stack), tx_thread,
			NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "HCI uart TX");

	while (1) {
		struct net_buf *buf;

		buf = net_buf_get(&rx_queue, K_FOREVER);
		err = h4_send(buf);
		if (err) {
			LOG_ERR("Failed to send");
		}
	}
}
