/*
 * nrfdfu - Nordic DFU Upgrade Utility
 *
 * Copyright (C) 2020 Bruno Randolf (br1@einfach.org)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "dfu_ble.h"
#include "log.h"
#include "util.h"

#ifndef BLE_SUPPORT

bool ble_enter_dfu(const char* address, enum BLE_ATYPE atype)
{
	return false;
}
bool ble_write_ctrl(uint8_t* req, size_t len)
{
	return false;
}
bool ble_write_data(uint8_t* req, size_t len)
{
	return false;
}
const uint8_t* ble_read(void)
{
	return NULL;
}
void ble_fini(void)
{
}

#else

#include <blzlib.h>
#include <blzlib_util.h>

#define DFU_SERVICE_UUID	 "0000fe59-0000-1000-8000-00805f9b34fb"
#define DFU_CONTROL_UUID	 "8EC90001-F315-4F60-9FB8-838830DAEA50"
#define DFU_DATA_UUID		 "8EC90002-F315-4F60-9FB8-838830DAEA50"
#define DFU_BUTTONLESS_UUID	 "8EC90003-F315-4F60-9FB8-838830DAEA50"
#define SERVICE_CHANGED_UUID "2A05"
#define CONNECT_MAX_TRY		 3

static bool buttonless_noti = false;
static bool control_noti = false;
static blz* ctx = NULL;
static blz_dev* dev = NULL;
static blz_char* cp = NULL;
static blz_char* dp = NULL;

static uint8_t recv_buf[200];

void buttonless_notify_handler(const uint8_t* data, size_t len, blz_char* ch,
							   void* user)
{
	if (data[2] != 0x01) {
		LOG_ERR("Unexpected response (%zd) %x %x %x", len, data[0], data[1],
				data[2]);
	}
	buttonless_noti = true;
}

void control_notify_handler(const uint8_t* data, size_t len, blz_char* ch,
							void* user)
{
	memcpy(recv_buf, data, len);
	control_noti = true;

	if (conf.loglevel >= LL_DEBUG) {
		dump_data("RX: ", data, len);
	}
}

bool ble_enter_dfu(const char* interface, const char* address,
				   enum BLE_ATYPE atype)
{
	ctx = blz_init(interface);
	if (ctx == NULL) {
		LOG_ERR("Could not initialize BLE interface '%s'", interface);
		return false;
	}

	int trynum = 0;
	do {
		LOG_NOTI("Connecting to %s (%s)...", address, blz_addr_type_str(atype));
		dev = blz_connect(ctx, address, atype);
		if (dev == NULL) {
			LOG_ERR("Could not connect to %s", address);
			sleep(5);
		}
	} while (dev == NULL && ++trynum < CONNECT_MAX_TRY);

	if (trynum >= CONNECT_MAX_TRY) {
		LOG_ERR("Gave up connecting to %s", address);
		return false;
	}

	blz_serv* srv = blz_get_serv_from_uuid(dev, DFU_SERVICE_UUID);
	if (srv == NULL) {
		LOG_ERR("DFU Service not found");
		return false;
	}

	blz_char* bch = blz_get_char_from_uuid(srv, DFU_BUTTONLESS_UUID);
	if (bch == NULL) {
		LOG_ERR("Could not find buttonless DFU UUID");
		/* try to find characteristics of DfuTarg */
		dp = blz_get_char_from_uuid(srv, DFU_DATA_UUID);
		cp = blz_get_char_from_uuid(srv, DFU_CONTROL_UUID);
		if (dp != NULL && cp != NULL) {
			goto dfutarg_found;
		} else {
			return false;
		}
	}

	bool b = blz_char_indicate_start(bch, buttonless_notify_handler, NULL);
	if (!b) {
		LOG_ERR("Could not start buttonless notification");
		return false;
	}

	LOG_NOTI("Enter DFU Bootloader");

	uint8_t buf = 0x01;
	b = blz_char_write(bch, &buf, 1);
	if (!b) {
		LOG_ERR("Could not write buttonless");
		return false;
	}

	/* wait until notification is received with confirmation */
	blz_loop_timeout(ctx, &buttonless_noti, 10000000);

	/* we don't disconnect here, because the device will reset and enter
	 * bootloader and appear under a new MAC and the connection times out */
	blz_disconnect(dev);

	/* connect to DfuTarg: increase MAC address by one */
	uint8_t* mac = blz_string_to_mac_s(address);
	mac[0]++;
	const char* macs = blz_mac_to_string_s(mac);

	LOG_NOTI("Connecting to DfuTarg (%s)...", macs);
	dev = blz_connect(ctx, macs, atype);
	if (dev == NULL) {
		LOG_ERR("Could not connect DfuTarg");
		return false;
	}

	srv = blz_get_serv_from_uuid(dev, DFU_SERVICE_UUID);
	if (srv == NULL) {
		LOG_ERR("DFU Service not found");
		return false;
	}

	dp = blz_get_char_from_uuid(srv, DFU_DATA_UUID);
	cp = blz_get_char_from_uuid(srv, DFU_CONTROL_UUID);
	if (dp == NULL || cp == NULL) {
		LOG_ERR("Could not find DFU UUIDs");
		dp = cp = NULL;
		return false;
	}

dfutarg_found:

	LOG_NOTI("DFU characteristics found");
	b = blz_char_notify_start(cp, control_notify_handler, NULL);
	if (!b) {
		LOG_ERR("Could not start CP notification");
		return false;
	}
	return true;
}

bool ble_write_ctrl(uint8_t* req, size_t len)
{
	if (conf.loglevel >= LL_DEBUG) {
		dump_data("CP: ", req, len);
	}
	return blz_char_write(cp, req, len);
}

bool ble_write_data(uint8_t* req, size_t len)
{
	if (conf.loglevel >= LL_DEBUG) {
		dump_data("TX: ", req, len);
	}
	return blz_char_write_cmd(dp, req, len);
}

const uint8_t* ble_read(void)
{
	/* wait until notification is received */
	control_noti = false;
	blz_loop_timeout(ctx, &control_noti, 10000000);

	if (control_noti == false) {
		LOG_ERR("BLE waiting for notification failed");
		return NULL;
	}

	return recv_buf;
}

void ble_fini(void)
{
	blz_disconnect(dev);
	blz_fini(ctx);
}

#endif
