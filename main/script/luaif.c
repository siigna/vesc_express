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

static char print_prefix[32] = "lua";
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
 * Post from a producer task. Never blocks, never touches the interpreter.
 *
 * The check against script_lua_wants means traffic nothing has subscribed to
 * costs a queue-depth test rather than a copy and a wakeup. On a busy CAN bus
 * that is the difference between a script costing nothing and a script
 * costing every frame.
 */
static void event_post(script_event_type_t type, uint32_t id,
		const uint8_t *data, int len) {
	if (!m_events || !m_engine) {
		return;
	}
	if (!script_lua_wants(m_engine, (int)type)) {
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

uint32_t luaif_events_dropped(void) {
	return m_dropped;
}

// -------------------------------------------------------------- callbacks ---

static void engine_print(const char *msg) {
	commands_printf_lisp("%s", msg);
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
		return false;
	}

	lua_vesc_ext_register(m_engine);
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
		return false;
	}

	if (!script_pack_parse(data, (int32_t)size, &m_blob)) {
		return false;
	}

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
	uint32_t period = m_engine ? script_lua_timer_period(m_engine) : 0;

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
		if (m_start_req) {
			m_start_req = false;
			m_stop_req = false;

			lock();
			if (load_blob() && engine_open()) {
				m_restart_cnt++;
				unlock();
				run_loaded();
			} else {
				unlock();
			}
		}

		if (m_stop_req) {
			lock();
			engine_close();
			unlock();
			m_stop_req = false;
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
	xTaskCreate(lua_task, "lua", LUA_TASK_STACK, NULL, LUA_TASK_PRIO, &m_task);

	m_start_req = true;
	ESP_LOGI(TAG, "Lua engine started");
}

void lispif_stop(void) {
	m_stop_req = true;

	// Wait for the script to unwind. The hook raises an error at the next
	// check, so this is bounded by LUA_HOOK_COUNT instructions rather than by
	// the script's own behaviour -- an infinite loop stops just as promptly
	// as a cooperative one.
	for (int i = 0; i < 200 && m_running; i++) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}

	lock();
	engine_close();
	unlock();
}

int lispif_get_restart_cnt(void) {
	return m_restart_cnt;
}

bool lispif_restart(bool print, bool load_code) {
	(void)load_code;
	if (print) {
		commands_printf_lisp("Restarting Lua");
	}
	lispif_stop();
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
