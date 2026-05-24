#ifndef VIBAMIX_BLE_RADIO_H
#define VIBAMIX_BLE_RADIO_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

class BLERadio {
public:
    int init(const struct gpio_dt_spec *status_led);
    void start_advertising();

    // Called by file-scope BT_CONN_CB_DEFINE callbacks; not for external use.
    void handle_connected(struct bt_conn *conn, uint8_t err);
    void handle_disconnected(struct bt_conn *conn, uint8_t reason);
    void handle_recycled();

private:
    static void adv_work_handler(struct k_work *work);

    const struct gpio_dt_spec *m_led{nullptr};
    struct k_work m_adv_work;
};

#endif /* VIBAMIX_BLE_RADIO_H */
