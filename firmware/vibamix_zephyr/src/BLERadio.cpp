#include "BLERadio.h"
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/printk.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

// Singleton pointer — lets file-scope BT callbacks dispatch to the instance.
static BLERadio *s_instance;

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (s_instance) { s_instance->handle_connected(conn, err); }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (s_instance) { s_instance->handle_disconnected(conn, reason); }
}

static void on_recycled(void)
{
	if (s_instance) { s_instance->handle_recycled(); }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = on_connected,
	.disconnected = on_disconnected,
	.recycled     = on_recycled,
};

void BLERadio::adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
	}
}

int BLERadio::init(const struct gpio_dt_spec *status_led)
{
	m_led = status_led;
	s_instance = this;
	k_work_init(&m_adv_work, adv_work_handler);

	int err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return err;
	}
	printk("Bluetooth initialized\n");
	return 0;
}

void BLERadio::start_advertising()
{
	k_work_submit(&m_adv_work);
}

void BLERadio::handle_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err 0x%02x)\n", err);
		return;
	}
	printk("Connected\n");
	if (m_led) { gpio_pin_set_dt(m_led, 1); }
}

void BLERadio::handle_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason 0x%02x)\n", reason);
	if (m_led) { gpio_pin_set_dt(m_led, 0); }
	start_advertising();
}

void BLERadio::handle_recycled()
{
	start_advertising();
}
