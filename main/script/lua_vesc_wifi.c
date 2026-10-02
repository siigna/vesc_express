/*
	Copyright 2025 Stephen Bouche

	This file is part of the ESCargot firmware.

	The ESCargot firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The ESCargot firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * WiFi bindings for the Lua engine.
 *
 * Everything here goes through comm_wifi, which is engine-neutral already --
 * unlike the lisp wifi extensions, which keep their own state.
 *
 * Connecting does not wait. comm_wifi_change_network starts the attempt and
 * returns; the connection is up when the status says so. The lisp binding
 * offers both: with a wait flag it parks its own context until the IP event
 * fires, and without one it returns the same bool this does. So this is the
 * existing no-wait mode rather than a new behaviour -- a script polls
 * vesc.wifi_status(), which is what a Lua script with a timer wants anyway:
 *
 *   vesc.wifi_connect("ssid", "password")
 *   vesc.on_timer(500, function()
 *     if vesc.wifi_status() == "connected" then ... end
 *   end)
 *
 * Two groups are deliberately absent.
 *
 * Blocking, and needing async the engine does not have: wifi-scan-networks,
 * wifi-ftm-measure. Both park the interpreter context with
 * lbm_block_ctx_from_extension and are resumed from an event handler or a
 * worker task. A C binding that waited inside the call would stall the engine
 * instead. A non-blocking scan is possible -- start it, collect results later
 * -- but that is new firmware logic, not a binding, so it is not smuggled in
 * here.
 *
 * TCP: tcp-connect, tcp-close, tcp-status, tcp-send and tcp-recv. Those are
 * not blocked on async -- tcp-recv already has a MSG_DONTWAIT path -- but on
 * ownership. The socket registry they share lives inside
 * lispif_wifi_extensions.c, which a lua build excludes, so binding them means
 * extracting that registry into a neutral core first, the way main/touch now
 * works. That is a refactor, and it belongs in its own change.
 */

#include "lua_vesc_ext.h"

#include "script_lua.h"
#include "lauxlib.h"

#include "comm_wifi.h"
#include "datatypes.h"

#include "esp_wifi.h"

#include <stdio.h>
#include <string.h>

/*
 * Same guard the lisp wifi extensions carry, and for the same reason: on a
 * build with no wifi there is no esp_wifi_set_max_tx_power to link against.
 * On the ESP32-P4 wifi is a separate co-processor reached through
 * esp_wifi_remote, and a board that does not fit one -- the Waveshare P4
 * Touch LCD 4.3 sets CONFIG_ESP_WIFI_REMOTE_IS_DISABLED -- has neither the
 * local nor the remote symbol. Without this the lua build failed to link
 * while the lisp build was fine, because their file compiles to nothing here.
 *
 * Nothing is registered when wifi is absent, so vesc.wifi_status is simply
 * not there -- the same as the lisp extensions not existing.
 */
#if CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_WIFI_REMOTE_ENABLED

// The lisp side refuses these calls unless wifi is enabled, and most of them
// unless it is in station mode. Same rule here, reported as a Lua error so a
// script can pcall it.
static void require_station(lua_State *L, const char *what) {
	if (comm_wifi_get_mode() != WIFI_MODE_STATION) {
		luaL_error(L, "%s: wifi is not in station mode", what);
	}
}

static void require_enabled(lua_State *L, const char *what) {
	if (comm_wifi_get_mode() == WIFI_MODE_DISABLED) {
		luaL_error(L, "%s: wifi is disabled", what);
	}
}

/*
 * vesc.wifi_status() -> "connected", "connecting" or "disconnected"
 *
 * Strings rather than the lisp symbols, which are the same three words.
 */
static int l_wifi_status(lua_State *L) {
	require_enabled(L, "wifi_status");

	if (comm_wifi_is_connecting()) {
		lua_pushstring(L, "connecting");
	} else if (comm_wifi_is_connected()) {
		lua_pushstring(L, "connected");
	} else {
		lua_pushstring(L, "disconnected");
	}

	return 1;
}

/*
 * vesc.wifi_connect(ssid [, password]) -> bool
 *
 * Starts the attempt; poll wifi_status(). Closes every open TCP socket, as
 * comm_wifi documents. An over-long ssid or password is refused here rather
 * than silently trimmed, which is what comm_wifi would do with it.
 */
static int l_wifi_connect(lua_State *L) {
	require_station(L, "wifi_connect");

	size_t ssid_len = 0;
	const char *ssid = luaL_checklstring(L, 1, &ssid_len);
	size_t pass_len = 0;
	const char *password = luaL_optlstring(L, 2, "", &pass_len);

	if (ssid_len >= 32) {
		return luaL_error(L, "wifi_connect: ssid is longer than 31 characters");
	}
	if (pass_len >= 64) {
		return luaL_error(L, "wifi_connect: password is longer than 63 characters");
	}

	lua_pushboolean(L, comm_wifi_change_network(ssid, password));
	return 1;
}

// vesc.wifi_connect_last() -> bool. Reconnects the configured network.
static int l_wifi_connect_last(lua_State *L) {
	require_station(L, "wifi_connect_last");
	lua_pushboolean(L, comm_wifi_reconnect_network());
	return 1;
}

static int l_wifi_disconnect(lua_State *L) {
	require_station(L, "wifi_disconnect");
	lua_pushboolean(L, comm_wifi_disconnect_network());
	return 1;
}

/*
 * vesc.wifi_auto_reconnect([enable]) -> the value before the call
 *
 * Returning the previous value is what the lisp binding does, and it is what
 * makes saving and restoring the setting possible in one call.
 */
static int l_wifi_auto_reconnect(lua_State *L) {
	require_station(L, "wifi_auto_reconnect");

	bool previous = comm_wifi_get_auto_reconnect();

	if (!lua_isnoneornil(L, 1)) {
		comm_wifi_set_auto_reconnect(lua_toboolean(L, 1));
	}

	lua_pushboolean(L, previous);
	return 1;
}

// vesc.wifi_max_tx_power([dbm_quarters]) -> the value now set.
static int l_wifi_max_tx_power(lua_State *L) {
	require_station(L, "wifi_max_tx_power");

	int8_t power = 0;
	if (esp_wifi_get_max_tx_power(&power) != ESP_OK) {
		return luaL_error(L, "wifi_max_tx_power: could not read the current power");
	}

	if (lua_isnoneornil(L, 1)) {
		lua_pushinteger(L, power);
		return 1;
	}

	int8_t requested = (int8_t)luaL_checkinteger(L, 1);
	if (esp_wifi_set_max_tx_power(requested) != ESP_OK) {
		return luaL_error(L, "wifi_max_tx_power: %d was refused", (int)requested);
	}

	lua_pushinteger(L, requested);
	return 1;
}

/*
 * vesc.wifi_ip() -> "a.b.c.d", or nil when there is no address yet.
 *
 * Not in the lisp extensions at all, and the first thing a script wants after
 * wifi_status says connected.
 */
static int l_wifi_ip(lua_State *L) {
	require_enabled(L, "wifi_ip");

	esp_ip4_addr_t ip = comm_wifi_get_ip();
	if (ip.addr == 0) {
		lua_pushnil(L);
		return 1;
	}

	char buf[16];
	snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
			(int)(ip.addr & 0xFF),
			(int)((ip.addr >> 8) & 0xFF),
			(int)((ip.addr >> 16) & 0xFF),
			(int)((ip.addr >> 24) & 0xFF));
	lua_pushstring(L, buf);
	return 1;
}

// vesc.wifi_mode() -> "disabled", "station", "access-point" or a number for
// anything a later firmware adds.
static int l_wifi_mode(lua_State *L) {
	WIFI_MODE mode = comm_wifi_get_mode();

	switch (mode) {
	case WIFI_MODE_DISABLED:
		lua_pushstring(L, "disabled");
		break;
	case WIFI_MODE_STATION:
		lua_pushstring(L, "station");
		break;
	case WIFI_MODE_ACCESS_POINT:
		lua_pushstring(L, "access-point");
		break;
	default:
		lua_pushinteger(L, (int)mode);
		break;
	}

	return 1;
}

// vesc.wifi_start() / vesc.wifi_stop(). The radio, not the connection.
static int l_wifi_start(lua_State *L) {
	(void)L;
	esp_wifi_start();
	return 0;
}

static int l_wifi_stop(lua_State *L) {
	(void)L;
	esp_wifi_stop();
	return 0;
}

// vesc.wifi_get_chan() -> 1..14
static int l_wifi_get_chan(lua_State *L) {
	uint8_t prim = 0;
	wifi_second_chan_t second;

	if (esp_wifi_get_channel(&prim, &second) == ESP_ERR_WIFI_NOT_INIT) {
		return luaL_error(L, "wifi_get_chan: wifi is not initialised");
	}

	lua_pushinteger(L, prim);
	return 1;
}

/*
 * vesc.wifi_set_chan(1..14)
 *
 * Sets the country code to JP first, as the lisp binding does, because the
 * default regulatory domain refuses the upper channels. That is a side effect
 * worth knowing about rather than a detail: it is a deliberate override of
 * the channel policy, and it stays set.
 */
static int l_wifi_set_chan(lua_State *L) {
	int ch = (int)luaL_checkinteger(L, 1);

	if (ch < 1 || ch > 14) {
		return luaL_error(L, "wifi_set_chan: %d is outside 1..14", ch);
	}

	wifi_country_t country = {
			.cc = {'J', 'P', '\0'},
			.schan = 1,
			.nchan = 14,
			.policy = WIFI_COUNTRY_POLICY_MANUAL,
	};
	esp_wifi_set_country(&country);

	if (esp_wifi_set_channel((uint8_t)ch, 0) == ESP_ERR_WIFI_NOT_INIT) {
		return luaL_error(L, "wifi_set_chan: wifi is not initialised");
	}

	lua_pushboolean(L, 1);
	return 1;
}

// vesc.wifi_get_bw() -> 20 or 40, in MHz, as the lisp binding reports it.
static int l_wifi_get_bw(lua_State *L) {
	wifi_bandwidth_t bwt = WIFI_BW_HT20;

	if (esp_wifi_get_bandwidth(WIFI_IF_AP, &bwt) == ESP_ERR_WIFI_NOT_INIT) {
		return luaL_error(L, "wifi_get_bw: wifi is not initialised");
	}

	lua_pushinteger(L, bwt == WIFI_BW_HT20 ? 20 : 40);
	return 1;
}

// vesc.wifi_set_bw(20 | 40)
static int l_wifi_set_bw(lua_State *L) {
	int bw = (int)luaL_checkinteger(L, 1);

	if (bw != 20 && bw != 40) {
		return luaL_error(L, "wifi_set_bw: %d is not 20 or 40", bw);
	}

	if (esp_wifi_set_bandwidth(WIFI_IF_AP,
			bw == 40 ? WIFI_BW_HT40 : WIFI_BW_HT20) == ESP_ERR_WIFI_NOT_INIT) {
		return luaL_error(L, "wifi_set_bw: wifi is not initialised");
	}

	lua_pushboolean(L, 1);
	return 1;
}

static const luaL_Reg wifi_funcs[] = {
	{"wifi_start", l_wifi_start},
	{"wifi_stop", l_wifi_stop},
	{"wifi_get_chan", l_wifi_get_chan},
	{"wifi_set_chan", l_wifi_set_chan},
	{"wifi_get_bw", l_wifi_get_bw},
	{"wifi_set_bw", l_wifi_set_bw},
	{"wifi_status", l_wifi_status},
	{"wifi_mode", l_wifi_mode},
	{"wifi_connect", l_wifi_connect},
	{"wifi_connect_last", l_wifi_connect_last},
	{"wifi_disconnect", l_wifi_disconnect},
	{"wifi_auto_reconnect", l_wifi_auto_reconnect},
	{"wifi_max_tx_power", l_wifi_max_tx_power},
	{"wifi_ip", l_wifi_ip},
	{NULL, NULL},
};

void lua_vesc_wifi_register(script_lua_t *s) {
	script_lua_register(s, wifi_funcs);
}

#else

void lua_vesc_wifi_register(script_lua_t *s) {
	(void)s;
}

#endif
