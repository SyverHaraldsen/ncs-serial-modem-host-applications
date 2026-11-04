/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <nrf_fuel_gauge.h>

#include "app_common.h"
#include "lp803448_model.h"

LOG_MODULE_REGISTER(battery, CONFIG_APP_BATTERY_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_BATTERY_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_BATTERY_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* nPM1300 BCHGCHARGESTATUS bits */
#define CHG_STATUS_COMPLETE_MASK BIT(1)
#define CHG_STATUS_TC_MASK       BIT(2)
#define CHG_STATUS_CC_MASK       BIT(3)
#define CHG_STATUS_CV_MASK       BIT(4)

/* Private channel message types for the module's own sampling routine. */
enum priv_battery_msg_type {
	BATTERY_PRIV_SAMPLE,
};

struct priv_battery_msg {
	enum priv_battery_msg_type type;
};

ZBUS_CHAN_DEFINE(battery_priv_chan,
		 struct priv_battery_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

ZBUS_MSG_SUBSCRIBER_DEFINE(battery);

#define CHANNEL_LIST(X) X(battery_priv_chan, struct priv_battery_msg)

#define MAX_MSG_SIZE MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST)

#define ADD_OBSERVERS(_chan, _type) ZBUS_CHAN_ADD_OBS(_chan, battery, 0);

CHANNEL_LIST(ADD_OBSERVERS)

static void sample_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample_work_handler);

static int read_sensors(const struct device *charger, float *voltage, float *current,
			float *temp, int32_t *chg_status, bool *vbus);
static void update_charge_state(int32_t chg_status, int32_t *prev);
static int fuel_gauge_setup(const struct device *charger);
static void battery_sample(const struct device *charger, int64_t *ref_time,
			   int32_t *prev_chg_status);
static void battery_thread(void);

static int read_sensors(const struct device *charger, float *voltage, float *current,
			float *temp, int32_t *chg_status, bool *vbus)
{
	int err;
	struct sensor_value val = {0};

	err = sensor_sample_fetch(charger);
	if (err) {
		return err;
	}

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
	if (err) {
		return err;
	}
	*voltage = sensor_value_to_float(&val);

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &val);
	if (err) {
		return err;
	}
	*temp = sensor_value_to_float(&val);

	err = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &val);
	if (err) {
		return err;
	}
	/* Fuel gauge wants charge-positive; the sensor API is discharge-negative. */
	*current = -sensor_value_to_float(&val);

	err = sensor_channel_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
				 &val);
	if (err) {
		return err;
	}
	*chg_status = val.val1;

	struct sensor_value vbus_val;

	err = sensor_attr_get(charger, (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      (enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
			      &vbus_val);
	*vbus = (err == 0) && (vbus_val.val1 != 0);

	return 0;
}

static void update_charge_state(int32_t chg_status, int32_t *prev)
{
	union nrf_fuel_gauge_ext_state_info_data ext;
	int err;

	if (chg_status == *prev) {
		return;
	}
	*prev = chg_status;

	if (chg_status & CHG_STATUS_COMPLETE_MASK) {
		ext.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & CHG_STATUS_TC_MASK) {
		ext.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & CHG_STATUS_CC_MASK) {
		ext.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
	} else if (chg_status & CHG_STATUS_CV_MASK) {
		ext.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		ext.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}

	err = nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
					      &ext);
	if (err) {
		LOG_WRN("nrf_fuel_gauge_ext_state_update, error: %d", err);
	}
}

static int fuel_gauge_setup(const struct device *charger)
{
	int err;
	int32_t chg_status;
	bool vbus;
	struct nrf_fuel_gauge_init_parameters params = { .model = &battery_model };
	struct sensor_value desired;
	float soc;

	err = read_sensors(charger, &params.v0, &params.i0, &params.t0, &chg_status, &vbus);
	if (err) {
		return err;
	}

	/* The Memfault --wrap on nrf_fuel_gauge_init() marks the gauge ready. */
	err = nrf_fuel_gauge_init(&params, NULL);
	if (err) {
		return err;
	}

	err = nrf_fuel_gauge_process(params.v0, params.i0, params.t0, 0.0f, &soc, NULL);
	if (err) {
		return err;
	}

	/* Seed charge-current limits for time-to-full prediction. */
	if (sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT,
				&desired) == 0) {
		float limit = sensor_value_to_float(&desired);
		union nrf_fuel_gauge_ext_state_info_data ext = { .charge_current_limit = limit };

		err = nrf_fuel_gauge_ext_state_update(
			NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT, &ext);
		if (err) {
			return err;
		}
		ext.charge_term_current = limit / 10.0f;
		err = nrf_fuel_gauge_ext_state_update(NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
						      &ext);
		if (err) {
			return err;
		}
	}

	return 0;
}

/* Owns the fuel gauge: init once, then keep it fed. Memfault reads SoC/SoH via
 * nrf_fuel_gauge_soc_get()/soh_get() on its own heartbeat.
 */
static void battery_sample(const struct device *charger, int64_t *ref_time,
			   int32_t *prev_chg_status)
{
	int err;
	float voltage;
	float current;
	float temp;
	float soc;
	int32_t chg_status;
	bool vbus;

	err = read_sensors(charger, &voltage, &current, &temp, &chg_status, &vbus);
	if (err) {
		LOG_ERR("read_sensors, error: %d", err);
		return;
	}

	err = nrf_fuel_gauge_ext_state_update(
		vbus ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
		     : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED, NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_ext_state_update, error: %d", err);
	}
	update_charge_state(chg_status, prev_chg_status);

	float delta = (float)k_uptime_delta(ref_time) / 1000.0f;

	err = nrf_fuel_gauge_process(voltage, current, temp, delta, &soc, NULL);
	if (err) {
		LOG_ERR("nrf_fuel_gauge_process, error: %d", err);
		return;
	}

	LOG_DBG("SoC %d%%, %d mV, %s", (int)soc, (int)(voltage * 1000),
		vbus ? "VBUS" : "battery");
}

static void sample_schedule(void)
{
	int err;

	err = k_work_reschedule(&sample_work,
				K_SECONDS(CONFIG_APP_BATTERY_SAMPLE_INTERVAL_SECONDS));
	if (err < 0) {
		LOG_ERR("k_work_reschedule sample_work, error: %d", err);
		FATAL_ERROR();
	}
}

/* Moves the sampling tick from the system workqueue to the battery thread. */
static void sample_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	struct priv_battery_msg msg = { .type = BATTERY_PRIV_SAMPLE };
	int err;

	err = zbus_chan_pub(&battery_priv_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub battery_priv_chan, error: %d", err);
		FATAL_ERROR();
	}
}

static void battery_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Battery watchdog expired, channel: %d, thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	FATAL_ERROR_WATCHDOG_TIMEOUT();
}

static void battery_thread(void)
{
	int err;
	int task_wdt_id;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	int64_t ref_time;
	int32_t prev_chg_status = -1;
	const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_charger));
	const uint32_t wdt_timeout_ms =
		(CONFIG_APP_BATTERY_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_BATTERY_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait = K_MSEC(wdt_timeout_ms - execution_time_ms);

	if (!device_is_ready(charger)) {
		LOG_ERR("Charger device not ready");
		FATAL_ERROR();
	}

	err = fuel_gauge_setup(charger);
	if (err) {
		LOG_ERR("fuel_gauge_setup, error: %d", err);
		FATAL_ERROR();
	}
	ref_time = k_uptime_get();

	/* fuel_gauge_setup() took the first sample, so the next one is due after an interval. */
	sample_schedule();

	task_wdt_id = task_wdt_add(wdt_timeout_ms, battery_wdt_callback,
				   (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		FATAL_ERROR();
		return;
	}

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&battery, &chan, msg_buf, zbus_wait);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			FATAL_ERROR();
			return;
		}

		if (chan == &battery_priv_chan) {
			const struct priv_battery_msg *msg =
				(const struct priv_battery_msg *)msg_buf;

			if (msg->type == BATTERY_PRIV_SAMPLE) {
				battery_sample(charger, &ref_time, &prev_chg_status);
				sample_schedule();
			}
		} else {
			LOG_WRN("Unhandled message in battery thread");
		}
	}
}

K_THREAD_DEFINE(battery_tid, CONFIG_APP_BATTERY_THREAD_STACK_SIZE, battery_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
