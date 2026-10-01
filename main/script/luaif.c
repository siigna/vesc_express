/*
	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Firmware adapter for the Lua engine.
 *
 * This file and lispif.c are interchangeable: both implement the same small
 * set of entry points the rest of the firmware calls, and CMake picks one
 * according to SCRIPT_ENGINE. Nothing outside these two files knows which
 * language the board runs, which is why the build-time swap needs no changes
 * to commands.c, comm_can.c or main.c.
 *
 *   lispif_init                     main.c, at boot
 *   lispif_stop                     commands.c, before an upload
 *   lispif_process_cmd              commands.c, for COMM_LISP_*
 *   lispif_process_can{,2}          comm_can.c, per frame
 *   lispif_process_custom_app_data  commands.c
 *   lispif_print_prefix             commands.c, for the print prefix
 *   lispif_fw_name                  commands.c, in the fw version reply
 *
 * The names keep the lispif_ prefix on purpose. They are the firmware's
 * internal scripting interface rather than anything lisp-specific, and
 * renaming them across every caller would make the diff against upstream
 * larger for no benefit.
 */

#include "script_lua.h"
#include "script_pack.h"
#include "script_event.h"
#include "touch_core.h"
#include "lua_vesc_ext.h"

#include "lispif.h"
#include "commands.h"
#include "datatypes.h"
#include "flash_helper.h"
#include "mempools.h"
#include "buffer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "luaif";

/*
 * 12 KB of stack.
 *
 * Lua's parser is recursive descent, so compiling a deeply nested table or a
 * long chain of operators recurses in C, and the VM adds frames for every
 * nested call. The lisp engine runs in 2 KB because its evaluator keeps its
 * own explicit continuation stack in the heap instead; Lua does not, so this
 * cannot simply inherit that figure. 12 KB is comfortable for the scripts
 * this is meant to run and is affordable here only because dropping LispBM
 * frees far more; LUAI_MAXCCALLS also bounds the recursion to 200 frames.
 */
#define LUA_TASK_STACK		12288
#define LUA_TASK_PRIO		6

/*
 * Ceiling on script memory. Lua refuses allocations past this and raises a
 * catchable error, so a runaway script fails rather than starving the comms
 * stack. Sized generously for a board with PSRAM; the lisp engine's
 * equivalent budget on this target is a comparable order.
 */
#define LUA_MEM_LIMIT		(192 * 1024)

// VM instructions between hook calls. Small enough to stop a tight loop
// promptly, large enough that the hook is not a measurable tax.
#define LUA_HOOK_COUNT		2000

static script_lua_t *m_engine = NULL;
static TaskHandle_t m_task = NULL;
static SemaphoreHandle_t m_mutex = NULL;
static volatile bool m_running = false;
static volatile bool m_stop_req = false;
static volatile bool m_start_req = false;
static int m_restart_cnt = 0;
static script_blob_t m_blob;
static bool m_blob_valid = false;

/*
 * Empty, as the lisp engine leaves it. commands_printf_lisp uses this as the
 * format string for a prefix on every line and inserts it again after each
 * newline, so a non-empty value runs straight into the message -- "lua" plus
 * "tick 5" reads as "luatick 5". A script that wants its output labelled can
 * do it better than the firmware can.
 */
static char print_prefix[32] = "";
static char fw_name[32] = {0};

/*
 * Event queue, from the firmware's tasks to the engine task.
 *
 * Depth 32, one allocation at start up. A producer that fills this queue
 * drops the event and increments a counter rather than blocking: the
 * producers are the CAN and comms tasks, and stalling those because a script
 * is slow would turn a script problem into a comms problem. Dropping is
 * visible through vesc.events_dropped(), so a script that cannot keep up can
 * say so instead of quietly missing frames.
 */
#define LUA_EVENT_QUEUE_LEN	32

static QueueHandle_t m_events = NULL;
static volatile uint32_t m_dropped = 0;

/*
 * Which event kinds the script has handlers for, mirrored out of the engine
 * by the task that owns it.
 *
 * event_post runs on the CAN and comms tasks and must not touch m_engine at
 * all: the engine can be closed and freed while those tasks are between
 * reading the pointer and using it, which is a use-after-free. It also must
 * not take the mutex, because that would park a CAN frame behind a slow
 * script handler.
 *
 * So the engine task refreshes these after anything that could change them,
 * and producers read nothing else. A stale read costs one queued event
 * nobody wants, or one dropped event nobody was going to see.
 */
static volatile uint8_t m_want_can = 0;
static volatile uint8_t m_want_app = 0;
static volatile uint8_t m_want_timer = 0;
static volatile uint8_t m_want_touch = 0;
static volatile uint32_t m_timer_period = 0;

// Called only from the engine task, with the engine alive.
static void refresh_wants(void) {
	if (m_engine) {
		m_want_can = script_lua_wants(m_engine, SCRIPT_EV_CAN_SID) ? 1 : 0;
		m_want_app = script_lua_wants(m_engine, SCRIPT_EV_APP_DATA) ? 1 : 0;
		m_want_timer = script_lua_wants(m_engine, SCRIPT_EV_TIMER) ? 1 : 0;
		m_want_touch = script_lua_wants(m_engine, SCRIPT_EV_TOUCH) ? 1 : 0;
		m_timer_period = script_lua_timer_period(m_engine);
	} else {
		m_want_can = 0;
		m_want_app = 0;
		m_want_timer = 0;
		m_want_touch = 0;
		m_timer_period = 0;
	}
}

static bool want_event(script_event_type_t type) {
	switch (type) {
	case SCRIPT_EV_CAN_SID:
	case SCRIPT_EV_CAN_EID:
	case SCRIPT_EV_CAN2_SID:
	case SCRIPT_EV_CAN2_EID:
		return m_want_can != 0;
	case SCRIPT_EV_APP_DATA:
		return m_want_app != 0;
	case SCRIPT_EV_TIMER:
		return m_want_timer != 0;
	case SCRIPT_EV_TOUCH:
		return m_want_touch != 0;
	default:
		return false;
	}
}

/*
 * Post from a producer task. Never blocks, never touches the interpreter.
 *
 * The check against script_lua_wants means traffic nothing has subscribed to
 * costs a queue-depth test rather than a copy and a wakeup. On a busy CAN bus
 * that is the difference between a script costing nothing and a script
 * costing every frame.
 */
static void event_post(script_event_type_t type, uint32_t id,
		const uint8_t *data, int len) {
	if (!m_events || !want_event(type)) {
		return;
	}

	script_event_t ev = {0};
	ev.type = (uint8_t)type;
	ev.id = id;

	if (len < 0) {
		len = 0;
	}
	if (len > SCRIPT_EVENT_PAYLOAD) {
		ev.truncated = 1;
		len = SCRIPT_EVENT_PAYLOAD;
	}
	ev.len = (uint16_t)len;
	if (data && len > 0) {
		memcpy(ev.data, data, (size_t)len);
	}

	if (xQueueSend(m_events, &ev, 0) != pdTRUE) {
		m_dropped++;
	}
}

/*
 * Touch sink for the driver core.
 *
 * Runs on the core's event task, so it is a producer like the CAN one: it
 * copies and returns, and never reaches for the interpreter. The core calls
 * touch_wants first and skips the read entirely when nothing is subscribed,
 * so an unregistered handler costs one flag test per poll rather than an I2C
 * transaction.
 */
static bool touch_wants(void) {
	return m_want_touch != 0;
}

static void touch_sink(bool pressed, const touch_point_t *point, touch_part_t part) {
	(void)part;

	script_event_touch_t t = {
			.pressed = pressed ? 1 : 0,
			.track_id = pressed ? point->track_id : 0,
			.x = pressed ? point->x : 0,
			.y = pressed ? point->y : 0,
			.strength = pressed ? point->strength : 0,
	};

	event_post(SCRIPT_EV_TOUCH, 0, (const uint8_t *)&t, (int)sizeof(t));
}

uint32_t luaif_events_dropped(void) {
	return m_dropped;
}

// -------------------------------------------------------------- callbacks ---

static void engine_print(const char *msg) {
	commands_printf_lisp("%s", msg);

	/*
	 * Also to the console. commands_printf_lisp delivers over whichever comm
	 * port last received a packet, so on a board with no active port -- or
	 * while the engine runs a script at boot, before anything has connected
	 * -- script output goes nowhere. Mirroring it here means a console build
	 * shows what a script said, which is the difference between "the script
	 * printed nothing" and "the print did not get delivered".
	 */
	ESP_LOGI(TAG, "print: %s", msg);
}

static bool engine_should_stop(void) {
	return m_stop_req;
}

/*
 * Called from the instruction hook. Yielding here is what keeps a
 * compute-heavy script from starving equal-priority tasks: FreeRTOS is
 * preemptive, but a script spinning at this priority still denies the
 * scheduler the chance to run anything else on this core between ticks.
 */
static void engine_tick(void) {
	taskYIELD();
}

// ------------------------------------------------------------------- run ---

static void lock(void) {
	if (m_mutex) {
		xSemaphoreTake(m_mutex, portMAX_DELAY);
	}
}

static void unlock(void) {
	if (m_mutex) {
		xSemaphoreGive(m_mutex);
	}
}

static void engine_close(void) {
	if (m_engine) {
		script_lua_close(m_engine);
		m_engine = NULL;
	}
	m_running = false;
}

static bool engine_open(void) {
	engine_close();

	script_lua_cfg_t cfg = {
		.mem_limit = LUA_MEM_LIMIT,
		.print = engine_print,
		.should_stop = engine_should_stop,
		.on_tick = engine_tick,
		.hook_count = LUA_HOOK_COUNT,
		.blob = m_blob_valid ? &m_blob : NULL,
	};

	m_engine = script_lua_open(&cfg);
	if (!m_engine) {
		commands_printf_lisp("Lua: could not create an interpreter");
		ESP_LOGE(TAG, "script_lua_open failed (ceiling %d bytes, free heap %u)",
				(int)LUA_MEM_LIMIT, (unsigned)esp_get_free_heap_size());
		return false;
	}

	ESP_LOGI(TAG, "interpreter up, %u bytes held", (unsigned)script_lua_mem_used(m_engine));

	lua_vesc_ext_register(m_engine);
	lua_vesc_io_register(m_engine);
	lua_vesc_disp_register(m_engine);
	lua_vesc_disp_load_register(m_engine);
	lua_vesc_font_register(m_engine);
	lua_vesc_touch_register(m_engine);
	lua_vesc_bms_register(m_engine);
	lua_vesc_wifi_register(m_engine);
	script_lua_install_events(m_engine);
	return true;
}

/*
 * Read the container out of flash.
 *
 * The pointers handed to the engine point into flash and stay valid as long
 * as nothing erases the partition, which is why lispif_stop is called before
 * an upload: running a script while the flash it lives in is being rewritten
 * would hand the interpreter bytes that change under it.
 */
static bool load_blob(void) {
	m_blob_valid = false;

	const uint8_t *data = flash_helper_code_data_raw(CODE_IND_LISP);
	int size = flash_helper_code_size_raw(CODE_IND_LISP);
	if (!data || size <= SCRIPT_HEADER_SIZE) {
		/*
		 * Logged, not silent. An engine that declines to start and says
		 * nothing is indistinguishable from one that started and does
		 * nothing, and on a board whose only channel is the packet protocol
		 * -- which needs the engine to be useful -- that is a dead end. This
		 * cost a session's worth of guessing before it was added.
		 */
		ESP_LOGE(TAG, "no script partition (data=%p size=%d)", data, size);
		return false;
	}

	if (!script_pack_parse(data, (int32_t)size, &m_blob)) {
		ESP_LOGE(TAG, "script container did not parse; first bytes "
				"%02x %02x %02x %02x %02x %02x %02x %02x",
				data[0], data[1], data[2], data[3],
				data[4], data[5], data[6], data[7]);
		return false;
	}

	ESP_LOGI(TAG, "script: %d source bytes, %d imports, lang %d",
			(int)m_blob.src_len, (int)m_blob.num_imports, (int)m_blob.lang);

	/*
	 * A lisp container on a Lua build is reported rather than fed to the
	 * parser. Both languages share the package store, so this is a normal
	 * thing to encounter, and "syntax error near (" would be a poor way to
	 * explain it.
	 */
	if (m_blob.lang != SCRIPT_LANG_LUA) {
		commands_printf_lisp(
				"This script is marked as LispBM and this firmware runs Lua. "
				"Pack it with tools/luapack.py, or flash a LispBM build.");
		return false;
	}

	m_blob_valid = true;
	return true;
}

static void run_loaded(void) {
	if (!m_blob_valid || !m_engine) {
		return;
	}

	char err[256];
	m_running = true;
	bool ok = script_lua_run(m_engine, m_blob.src, m_blob.src_len, "=main",
			err, sizeof(err));
	m_running = false;

	if (!ok) {
		commands_printf_lisp("Lua error: %s", err);
		// Also to the console: on a board where the packet protocol is the
		// only channel, an error explaining why the script died cannot only
		// be delivered over that channel.
		ESP_LOGE(TAG, "script error: %s", err);
	} else {
		ESP_LOGI(TAG, "script ran to completion");
	}
}

/*
 * Drain the queue and run the timer, then sleep.
 *
 * The wait on the queue is what idles this task: with no events and no timer
 * it blocks for 100 ms at a time rather than spinning. With a timer it waits
 * at most until the timer is next due, so a 20 ms timer is served to within a
 * tick without polling at 20 ms when nothing is registered.
 */
static void drain_events(void) {
	// From the mirror, not from the engine: this runs before the lock is
	// taken, and the engine may be closed between a read and a use.
	uint32_t period = m_want_timer ? m_timer_period : 0;

	TickType_t wait = pdMS_TO_TICKS(100);
	if (period > 0) {
		TickType_t p = pdMS_TO_TICKS(period);
		if (p < wait) {
			wait = p;
		}
	}
	if (wait < 1) {
		wait = 1;
	}

	script_event_t ev;
	if (m_events && xQueueReceive(m_events, &ev, wait) == pdTRUE) {
		char err[256];
		lock();
		if (m_engine) {
			m_running = true;
			bool ok = script_lua_dispatch(m_engine, &ev, err, sizeof(err));
			m_running = false;
			if (!ok) {
				commands_printf_lisp("Lua handler error: %s", err);
			}
		}
		unlock();
	}

	// Timer, checked against the tick count rather than counted in sleeps, so
	// a slow handler does not make the period drift.
	if (period > 0) {
		static TickType_t next = 0;
		TickType_t now = xTaskGetTickCount();
		if (next == 0 || (int32_t)(now - next) >= 0) {
			next = now + pdMS_TO_TICKS(period);

			script_event_t t = {.type = SCRIPT_EV_TIMER};
			char err[256];
			lock();
			if (m_engine) {
				m_running = true;
				bool ok = script_lua_dispatch(m_engine, &t, err, sizeof(err));
				m_running = false;
				if (!ok) {
					commands_printf_lisp("Lua timer error: %s", err);
				}
			}
			unlock();
		}
	}
}

static void lua_task(void *arg) {
	(void)arg;

	for (;;) {
		// Stop first: a restart sets both flags, and closing before opening
		// is what makes restart mean restart rather than "open a second
		// interpreter and leak the first".
		if (m_stop_req) {
			lock();
			engine_close();
			refresh_wants();
			unlock();
			m_stop_req = false;
		}

		if (m_start_req) {
			m_start_req = false;

			lock();
			bool loaded = load_blob();
			bool opened = loaded && engine_open();
			unlock();

			if (opened) {
				m_restart_cnt++;
				run_loaded();
				// Handlers are registered by the script itself, so the
				// mirrors can only be correct once it has run.
				lock();
				refresh_wants();
				unlock();
			} else {
				ESP_LOGE(TAG, "not starting: blob %s, interpreter %s",
						loaded ? "ok" : "unusable",
						opened ? "ok" : "not created");
			}
		}

		drain_events();
	}
}

// ------------------------------------------------------------- interface ---

void lispif_init(void) {
	if (m_mutex) {
		return;
	}

	m_mutex = xSemaphoreCreateMutex();
	m_events = xQueueCreate(LUA_EVENT_QUEUE_LEN, sizeof(script_event_t));

	// The engine task exists for the life of the firmware; starting and
	// stopping a script means opening and closing an interpreter inside it,
	// not creating and destroying a task, so a script cannot leak one.
	// Registered once, for the life of the firmware. The core only calls it
	// while a script has a handler, and the want flag is cleared when the
	// engine closes, so there is nothing to tear down per script.
	touch_core_set_event_cb(touch_sink, touch_wants);

	xTaskCreate(lua_task, "lua", LUA_TASK_STACK, NULL, LUA_TASK_PRIO, &m_task);

	m_start_req = true;
	ESP_LOGI(TAG, "Lua engine started");
}

/*
 * Stop a script.
 *
 * This runs on the comms task, and it does not close the interpreter itself.
 * Only the engine task does that, because it is the only task that can know
 * it is not currently inside the interpreter. An earlier version closed it
 * from here after waiting for a flag, and that crashed the board on every
 * restart: the engine task reads m_engine outside the lock on its way into a
 * dispatch, so freeing it from another task is a use-after-free that shows up
 * as SW_CPU_RESET with a saved PC in the middle of the engine.
 */
void lispif_stop(void) {
	if (!m_task) {
		return;
	}

	m_stop_req = true;

	// The instruction hook raises at its next check, so this is bounded by
	// LUA_HOOK_COUNT instructions rather than by the script's behaviour: an
	// infinite loop stops as promptly as a cooperative one.
	for (int i = 0; i < 300 && (m_engine || m_running); i++) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

int lispif_get_restart_cnt(void) {
	return m_restart_cnt;
}

bool lispif_restart(bool print, bool load_code) {
	(void)load_code;

	/*
	 * Deliberately does not print.
	 *
	 * This runs on whichever comms task delivered the packet, and the UART
	 * one is created with a 3 kB stack (comm_uart.c). commands_printf_lisp
	 * runs sprintf and then vsnprintf -- full newlib printf, which wants one
	 * to two kilobytes of stack each on RISC-V -- so a courtesy message here
	 * overflows that task's stack and resets the board.
	 *
	 * It presented as "the Lua engine never prints anything": every restart
	 * crashed before the script could produce output, and with the console
	 * disabled on this board the reset was silent. A coredump named it in one
	 * read. The lisp engine's lispif_restart does not print either, which is
	 * why it never showed this.
	 *
	 * A restart is observable through GET_STATS and through the script's own
	 * output, so nothing is lost.
	 */
	(void)print;

	lispif_stop();		// waits for the engine task to close the interpreter
	m_start_req = true;
	return true;
}

char *lispif_print_prefix(void) {
	print_prefix[sizeof(print_prefix) - 1] = 0;
	return print_prefix;
}

char *lispif_fw_name(void) {
	fw_name[sizeof(fw_name) - 1] = 0;
	return fw_name;
}

/*
 * Frame and app-data hooks. These run on the CAN and comms tasks, so they
 * copy into the queue and return -- a lua_State is not reentrant and the
 * engine task may be inside it. The interpreter is entered only from
 * drain_events, on the engine task.
 */
void lispif_process_can(uint32_t can_id, uint8_t *data8, int len, bool is_ext) {
	event_post(is_ext ? SCRIPT_EV_CAN_EID : SCRIPT_EV_CAN_SID, can_id, data8, len);
}

void lispif_process_can2(uint32_t can_id, uint8_t *data8, int len, bool is_ext) {
	event_post(is_ext ? SCRIPT_EV_CAN2_EID : SCRIPT_EV_CAN2_SID, can_id, data8, len);
}

void lispif_process_custom_app_data(unsigned char *data, unsigned int len) {
	event_post(SCRIPT_EV_APP_DATA, 0, data, (int)len);
}

void lispif_process_rmsg(int slot, unsigned char *data, unsigned int len) {
	(void)slot; (void)data; (void)len;
}

void lispif_disable_all_events(void) {
}

void lispif_lock_lbm(void) {
	lock();
}

void lispif_unlock_lbm(void) {
	unlock();
}

void lispif_stop_lib(void) {
}

bool lispif_is_eval_task(void) {
	return xTaskGetCurrentTaskHandle() == m_task;
}

void lispif_free(void *ptr) {
	(void)ptr;
}

void lispif_add_ext_load_callback(void (*p_func)(bool)) {
	(void)p_func;
}

void lispif_add_dyn_load_callback(bool (*p_func)(const char *, const char **)) {
	(void)p_func;
}

// ----------------------------------------------------------------- stats ---

/*
 * Report numeric globals, matching what the lisp engine sends so VESC Tool's
 * binding display works unchanged. print_all false means only names starting
 * "vt", which is the convention the tool uses for values it plots.
 */
static void append_globals(uint8_t *buf, int32_t *ind, bool print_all) {
	lua_State *L = script_lua_state(m_engine);
	if (!L) {
		return;
	}

	lua_pushglobaltable(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		if (*ind > 300) {
			lua_pop(L, 2);
			break;
		}

		if (lua_type(L, -2) == LUA_TSTRING && lua_isnumber(L, -1)) {
			const char *name = lua_tostring(L, -2);
			bool wanted = print_all ||
					((name[0] == 'v' || name[0] == 'V') &&
							(name[1] == 't' || name[1] == 'T'));
			if (wanted && strlen(name) < 40) {
				strcpy((char *)(buf + *ind), name);
				*ind += (int32_t)strlen(name) + 1;
				buffer_append_float32_auto(buf, (float)lua_tonumber(L, -1), ind);
			}
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

void lispif_process_cmd(unsigned char *data, unsigned int len,
		void(*reply_func)(unsigned char *data, unsigned int len)) {
	COMM_PACKET_ID packet_id = (COMM_PACKET_ID)data[0];
	data++;
	len--;

	switch (packet_id) {
	case COMM_LISP_SET_RUNNING: {
		bool ok = false;

		if (len >= 1 && data[0]) {
			lispif_restart(true, true);
			ok = true;
		} else {
			lispif_stop();
			ok = true;
		}

		int32_t ind = 0;
		uint8_t buf[2];
		buf[ind++] = (uint8_t)packet_id;
		buf[ind++] = ok ? 1 : 0;
		reply_func(buf, (unsigned int)ind);
	} break;

	case COMM_LISP_GET_STATS: {
		if (!m_engine) {
			break;
		}

		bool print_all = len > 0 ? (bool)data[0] : true;

		float cpu_use = 0.0;
		float mem_use = 0.0;

		size_t used = script_lua_mem_used(m_engine);
		mem_use = 100.0f * (float)used / (float)LUA_MEM_LIMIT;

		uint32_t time_tot = 0;
#ifdef portALT_GET_RUN_TIME_COUNTER_VALUE
		portALT_GET_RUN_TIME_COUNTER_VALUE(time_tot);
#else
		time_tot = portGET_RUN_TIME_COUNTER_VALUE();
#endif
		if (time_tot > 0 && m_task) {
			TaskStatus_t stat;
			vTaskGetInfo(m_task, &stat, pdFALSE, 0);
			static uint32_t time_last = 0;
			static uint32_t time_task_last = 0;
			if (time_tot > time_last) {
				cpu_use = 100.0f *
						(float)(stat.ulRunTimeCounter - time_task_last) /
						(float)(time_tot - time_last);
			}
			time_last = time_tot;
			time_task_last = stat.ulRunTimeCounter;
		}

		uint8_t *buf = mempools_get_packet_buffer();
		int32_t ind = 0;
		buf[ind++] = (uint8_t)packet_id;
		buffer_append_float16(buf, cpu_use, 1e2, &ind);
		// The lisp engine reports cons-cell heap and its own memory pool
		// separately. Lua has one allocator, so the same figure is sent for
		// both rather than inventing a second number.
		buffer_append_float16(buf, mem_use, 1e2, &ind);
		buffer_append_float16(buf, mem_use, 1e2, &ind);
		buffer_append_float16(buf, 0, 1e2, &ind);
		buf[ind++] = '\0';

		lock();
		append_globals(buf, &ind, print_all);
		unlock();

		reply_func(buf, (unsigned int)ind);
		mempools_free_packet_buffer(buf);
	} break;

	case COMM_LISP_REPL_CMD: {
		/*
		 * Rate limited the way the lisp engine limits it. VESC Tool's REPL
		 * sends on every keystroke in some versions, and each command here
		 * compiles a chunk.
		 */
		static TickType_t last = 0;
		TickType_t now = xTaskGetTickCount();
		if (last != 0 && (now - last) < pdMS_TO_TICKS(500)) {
			break;
		}
		last = now;

		if (!m_engine) {
			lock();
			if (!engine_open()) {
				unlock();
				break;
			}
			unlock();
		}

		char expr[512];
		size_t n = len < sizeof(expr) - 1 ? len : sizeof(expr) - 1;
		memcpy(expr, data, n);
		expr[n] = '\0';

		/*
		 * Bare expressions are the point of a REPL, so try the chunk wrapped
		 * in a return first and fall back to running it as a statement. That
		 * is what the standalone interpreter does, and without it `1 + 1`
		 * would be a syntax error.
		 */
		char wrapped[540];
		snprintf(wrapped, sizeof(wrapped), "return %s", expr);

		char err[256];
		lock();
		lua_State *L = script_lua_state(m_engine);
		bool ok = false;
		if (L && luaL_loadbuffer(L, wrapped, strlen(wrapped), "=repl") == LUA_OK) {
			if (lua_pcall(L, 0, 1, 0) == LUA_OK) {
				if (!lua_isnil(L, -1)) {
					commands_printf_lisp("%s", luaL_tolstring(L, -1, NULL));
					lua_pop(L, 1);
				}
				lua_pop(L, 1);
				ok = true;
			} else {
				commands_printf_lisp("Lua error: %s", lua_tostring(L, -1));
				lua_pop(L, 1);
				ok = true;
			}
		} else {
			if (L) {
				lua_pop(L, 1);	// the failed chunk's error message
			}
			ok = script_lua_run(m_engine, expr, (int32_t)n, "=repl",
					err, sizeof(err));
			if (!ok) {
				commands_printf_lisp("Lua error: %s", err);
			}
		}
		unlock();
	} break;

	default:
		break;
	}
}
