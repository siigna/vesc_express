/*
 * Host tests for the Lua script engine.
 *
 * These run real Lua through the real engine, so the sandbox, the memory
 * ceiling, the interrupt hook and require() are checked by behaviour rather
 * than by reading the source. All four are safety properties on a device: a
 * script that can open a file, exhaust the heap, spin forever, or load a
 * string it was not flashed with is a script that can take the board out.
 *
 *   make -C main/script/test run
 */

#include "../script_lua.h"
#include "../script_pack.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks = 0;
static int failures = 0;

static char last_print[1024];
static int print_calls = 0;
static int stop_after_ticks = -1;
static int ticks = 0;

static void t_print(const char *msg) {
	snprintf(last_print, sizeof(last_print), "%s", msg);
	print_calls++;
}

static bool t_should_stop(void) {
	if (stop_after_ticks < 0) {
		return false;
	}
	return ++ticks > stop_after_ticks;
}

static void ok(const char *what, bool cond) {
	checks++;
	if (!cond) {
		failures++;
		printf("FAIL %s\n", what);
	}
}

static script_lua_t *open_engine(size_t limit, const script_blob_t *blob) {
	script_lua_cfg_t cfg = {
		.mem_limit = limit,
		.print = t_print,
		.should_stop = t_should_stop,
		.hook_count = 200,
		.blob = blob,
	};
	return script_lua_open(&cfg);
}

static bool run(script_lua_t *s, const char *src, char *err, size_t err_len) {
	return script_lua_run(s, src, (int32_t)strlen(src), "=test", err, err_len);
}

/*
 * End-to-end check against a container produced by tools/luapack.py.
 *
 * The layout is written by the packer in Python and read by script_pack.c in
 * C, so it is described in two places and can drift. This is the test that
 * notices: the packer builds a fixture, the parser reads it, and the engine
 * runs it.
 */
static int run_packed(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("FAIL cannot open %s\n", path);
		return 1;
	}
	static uint8_t blob[64 * 1024];
	size_t n = fread(blob, 1, sizeof(blob), f);
	fclose(f);

	script_blob_t p;
	ok("packed container parses", script_pack_parse(blob, (int32_t)n, &p));
	ok("packed container is lua", p.lang == SCRIPT_LANG_LUA);
	ok("packed container has both modules", p.num_imports == 2);

    script_lua_t *s = open_engine(0, &p);
	print_calls = 0;
	char err[512] = {0};
	bool r = script_lua_run(s, p.src, p.src_len, "=main", err, sizeof(err));
	ok("packed script runs", r);
	if (!r) {
		printf("     (error was: %s)\n", err);
	}
	ok("packed script printed", print_calls == 1);
	ok("packed script computed across modules",
			strcmp(last_print, "packed ok\t21") == 0);
	script_lua_close(s);
	return 0;
}

int main(int argc, char **argv) {
	// Basics: a chunk runs, print reaches the host.
	{
		script_lua_t *s = open_engine(0, NULL);
		ok("engine opens", s != NULL);
		print_calls = 0;
		ok("chunk runs", run(s, "print('hello', 42)", NULL, 0));
		ok("print was routed", print_calls == 1);
		ok("print formatting matches lua",
				strcmp(last_print, "hello\t42") == 0);
		script_lua_close(s);
	}

	// Arithmetic and the standard libraries that are supposed to be present.
	{
		script_lua_t *s = open_engine(0, NULL);
		ok("string library present", run(s, "assert(('x'):rep(3) == 'xxx')", NULL, 0));
		ok("table library present",
				run(s, "local t={3,1,2} table.sort(t) assert(t[1]==1)", NULL, 0));
		ok("math library present", run(s, "assert(math.floor(2.7) == 2)", NULL, 0));
		ok("coroutines present",
				run(s, "local c=coroutine.create(function() coroutine.yield(1) end)"
					" local _,v = coroutine.resume(c) assert(v==1)", NULL, 0));
		script_lua_close(s);
	}

	// The sandbox. Each of these must be absent, not merely restricted.
	{
		script_lua_t *s = open_engine(0, NULL);
		static const char *const forbidden[] = {
			"io", "os", "package", "debug", "dofile", "loadfile", "load",
			"collectgarbage", "require('io')", NULL,
		};
		for (const char *const *n = forbidden; *n; n++) {
			char src[128];
			snprintf(src, sizeof(src), "return %s", *n);
			char err[256];
			bool got = run(s, src, err, sizeof(err));
			// Either the global is nil (so the chunk returns nil harmlessly)
			// or calling it errors. What must not happen is it working.
			char check[160];
			snprintf(check, sizeof(check), "%s is not reachable", *n);
			if (got) {
				// Ran without error: confirm it evaluated to nil.
				snprintf(src, sizeof(src), "assert(%s == nil)", *n);
				got = run(s, src, err, sizeof(err));
				ok(check, got);
			} else {
				ok(check, true);
			}
		}
		script_lua_close(s);
	}

	// A syntax error is reported, not fatal.
	{
		script_lua_t *s = open_engine(0, NULL);
		char err[256] = {0};
		ok("syntax error refused", !run(s, "this is not lua", err, sizeof(err)));
		ok("syntax error has a message", strlen(err) > 0);
		ok("engine still usable after a syntax error",
				run(s, "local x = 1", NULL, 0));
		script_lua_close(s);
	}

	// A runtime error is caught rather than escaping the call.
	{
		script_lua_t *s = open_engine(0, NULL);
		char err[256] = {0};
		ok("runtime error caught", !run(s, "error('boom')", err, sizeof(err)));
		ok("runtime error message kept", strstr(err, "boom") != NULL);
		ok("engine still usable after a runtime error",
				run(s, "local x = 1", NULL, 0));
		script_lua_close(s);
	}

	// The memory ceiling. A script allocating without bound must fail its own
	// allocation and leave the engine alive.
	{
		script_lua_t *s = open_engine(256 * 1024, NULL);
		ok("engine opens with a ceiling", s != NULL);
		char err[256] = {0};
		bool r = run(s,
				"local t = {} local i = 0"
				" while true do i = i + 1 t[i] = string.rep('x', 1024) end",
				err, sizeof(err));
		ok("unbounded allocation refused", !r);
		ok("stayed under the ceiling", script_lua_mem_peak(s) <= 256 * 1024);
		ok("engine alive after running out of memory",
				run(s, "local x = 1", NULL, 0));
		script_lua_close(s);
	}

	// Memory accounting returns to roughly where it started once garbage is
	// collected, which is what makes the reported figure worth showing.
	{
		script_lua_t *s = open_engine(0, NULL);
		size_t before = script_lua_mem_used(s);
		ok("allocates while working",
				run(s, "local t = {} for i=1,2000 do t[i] = i end", NULL, 0));
		lua_gc(script_lua_state(s), LUA_GCCOLLECT, 0);
		lua_gc(script_lua_state(s), LUA_GCCOLLECT, 0);
		size_t after = script_lua_mem_used(s);
		ok("memory is reclaimed", after < before + 64 * 1024);
		script_lua_close(s);
	}

	// The interrupt hook. An infinite loop must be stoppable.
	{
		script_lua_t *s = open_engine(0, NULL);
		stop_after_ticks = 5;
		ticks = 0;
		char err[256] = {0};
		ok("infinite loop is interrupted",
				!run(s, "while true do end", err, sizeof(err)));
		ok("interruption says why", strstr(err, "stopped") != NULL);
		stop_after_ticks = -1;
		ok("engine usable after being interrupted",
				run(s, "local x = 1", NULL, 0));
		script_lua_close(s);
	}

	// A tight loop that is not asked to stop must complete.
	{
		script_lua_t *s = open_engine(0, NULL);
		stop_after_ticks = -1;
		ok("bounded loop completes",
				run(s, "local n = 0 for i=1,200000 do n = n + i end assert(n > 0)",
					NULL, 0));
		script_lua_close(s);
	}

	// require() against a real container built the way the packer builds one.
	{
		uint8_t blob[512];
		int32_t n = 0;
		memset(blob, 0, sizeof(blob));
		// header
		n = 8;
		const char *src = "local m = require('mod') assert(m.val == 7) print('ok')";
		memcpy(blob + n, src, strlen(src) + 1);
		n += (int32_t)strlen(src) + 1;
		blob[6] = 0; blob[7] = SCRIPT_FLAG_LANG_LUA;
		// import table: one entry
		int32_t tbl = n;
		blob[tbl] = 0; blob[tbl + 1] = 1;
		n += 2;
		memcpy(blob + n, "mod", 4);
		n += 4;
		const char *mod = "return {val = 7}";
		int32_t payload_at = n + 8;
		int32_t off = payload_at - 8;
		blob[n++] = (uint8_t)(off >> 24); blob[n++] = (uint8_t)(off >> 16);
		blob[n++] = (uint8_t)(off >> 8);  blob[n++] = (uint8_t)off;
		int32_t ml = (int32_t)strlen(mod);
		blob[n++] = (uint8_t)(ml >> 24); blob[n++] = (uint8_t)(ml >> 16);
		blob[n++] = (uint8_t)(ml >> 8);  blob[n++] = (uint8_t)ml;
		memcpy(blob + n, mod, (size_t)ml);
		n += ml;

		script_blob_t p;
		ok("test container parses", script_pack_parse(blob, n, &p));
		ok("container is lua", p.lang == SCRIPT_LANG_LUA);
		ok("container has one import", p.num_imports == 1);

		script_lua_t *s = open_engine(0, &p);
		print_calls = 0;
		char err[256] = {0};
		ok("require resolves from the container",
				script_lua_run(s, p.src, p.src_len, "=main", err, sizeof(err)));
		if (failures && strlen(err)) {
			printf("     (error was: %s)\n", err);
		}
		ok("required module ran", print_calls == 1);

		// Requiring the same module twice must not run it again.
		ok("require caches",
				run(s, "local a = require('mod') local b = require('mod')"
					" assert(a == b)", NULL, 0));

		// A module that was not bundled must fail clearly.
		memset(err, 0, sizeof(err));
		ok("missing module refused",
				!run(s, "require('absent')", err, sizeof(err)));
		ok("missing module names itself", strstr(err, "absent") != NULL);
		script_lua_close(s);
	}

	// require() with no container at all.
	{
		script_lua_t *s = open_engine(0, NULL);
		char err[256] = {0};
		ok("require without a container refused",
				!run(s, "require('anything')", err, sizeof(err)));
		script_lua_close(s);
	}

	// Bindings registered into the vesc table are reachable.
	{
		script_lua_t *s = open_engine(0, NULL);
		lua_State *L = script_lua_state(s);
		lua_getglobal(L, "vesc");
		ok("vesc table exists", lua_istable(L, -1));
		lua_pop(L, 1);
		script_lua_close(s);
	}

	// Event dispatch. These are the paths a CAN frame takes to a handler, and
	// the point of testing them here is that the alternative is testing them
	// on a moving vehicle.
	{
		script_lua_t *s = open_engine(0, NULL);
		script_lua_install_events(s);

		script_event_t none = {.type = SCRIPT_EV_CAN_SID, .id = 5, .len = 1};
		ok("no handler is not an error",
				script_lua_dispatch(s, &none, NULL, 0));
		ok("wants() false with nothing registered",
				!script_lua_wants(s, SCRIPT_EV_CAN_SID));

		ok("handler registers", run(s,
				"seen = {} "
				"vesc.on_can(function(id, data, is_ext, bus) "
				"  seen.id = id seen.data = data seen.ext = is_ext seen.bus = bus "
				"end)", NULL, 0));
		ok("wants() true once registered",
				script_lua_wants(s, SCRIPT_EV_CAN_SID));

		script_event_t ev = {.type = SCRIPT_EV_CAN_SID, .id = 0x123, .len = 3};
		memcpy(ev.data, "abc", 3);
		char err[256] = {0};
		ok("can frame dispatched", script_lua_dispatch(s, &ev, err, sizeof(err)));
		ok("handler saw the id", run(s, "assert(seen.id == 0x123)", NULL, 0));
		ok("handler saw the payload as a string",
				run(s, "assert(seen.data == 'abc')", NULL, 0));
		ok("standard id reports not extended",
				run(s, "assert(seen.ext == false)", NULL, 0));
		ok("bus 1 reported", run(s, "assert(seen.bus == 1)", NULL, 0));

		// A payload with a zero byte in it must survive: CAN carries binary,
		// and a C string would stop at the first NUL.
		script_event_t bin = {.type = SCRIPT_EV_CAN_EID, .id = 7, .len = 4};
		memcpy(bin.data, "a\0bc", 4);
		ok("binary payload dispatched", script_lua_dispatch(s, &bin, err, sizeof(err)));
		ok("payload keeps embedded NUL and length",
				run(s, "assert(#seen.data == 4 and seen.data:byte(2) == 0)", NULL, 0));
		ok("extended id reports extended", run(s, "assert(seen.ext == true)", NULL, 0));

		// Bus 2 is a different event type but the same handler.
		script_event_t b2 = {.type = SCRIPT_EV_CAN2_SID, .id = 9, .len = 1};
		b2.data[0] = 1;
		ok("bus 2 dispatched", script_lua_dispatch(s, &b2, err, sizeof(err)));
		ok("bus 2 reported", run(s, "assert(seen.bus == 2)", NULL, 0));

		// A handler that raises is reported but stays registered: one bad
		// frame must not unsubscribe a vehicle from its own CAN traffic.
		ok("failing handler installs",
				run(s, "vesc.on_can(function() error('handler boom') end)", NULL, 0));
		memset(err, 0, sizeof(err));
		ok("failing handler reported",
				!script_lua_dispatch(s, &ev, err, sizeof(err)));
		ok("failure message kept", strstr(err, "handler boom") != NULL);
		ok("handler still registered after raising",
				script_lua_wants(s, SCRIPT_EV_CAN_SID));

		// Unregistering.
		ok("handler clears", run(s, "vesc.on_can(nil)", NULL, 0));
		ok("wants() false after clearing",
				!script_lua_wants(s, SCRIPT_EV_CAN_SID));

		// A non-function is refused at registration rather than at dispatch.
		ok("non-function handler refused",
				!run(s, "vesc.on_can(42)", NULL, 0));

		script_lua_close(s);
	}

	// App data and the timer.
	{
		script_lua_t *s = open_engine(0, NULL);
		script_lua_install_events(s);

		ok("app data handler registers", run(s,
				"got = nil vesc.on_app_data(function(d) got = d end)", NULL, 0));
		script_event_t ev = {.type = SCRIPT_EV_APP_DATA, .len = 5};
		memcpy(ev.data, "hello", 5);
		ok("app data dispatched", script_lua_dispatch(s, &ev, NULL, 0));
		ok("app data arrived", run(s, "assert(got == 'hello')", NULL, 0));

		ok("no timer period by default", script_lua_timer_period(s) == 0);
		ok("timer registers", run(s,
				"ticks = 0 vesc.on_timer(50, function() ticks = ticks + 1 end)",
				NULL, 0));
		ok("timer period reported", script_lua_timer_period(s) == 50);

		script_event_t t = {.type = SCRIPT_EV_TIMER};
		ok("timer dispatched", script_lua_dispatch(s, &t, NULL, 0));
		ok("timer dispatched again", script_lua_dispatch(s, &t, NULL, 0));
		ok("timer ran twice", run(s, "assert(ticks == 2)", NULL, 0));

		// A zero or negative period is clamped, not rejected: asking for zero
		// means "as fast as you can", and honouring that literally would
		// starve the rest of the core.
		ok("zero period accepted", run(s, "vesc.on_timer(0, function() end)", NULL, 0));
		ok("zero period clamped", script_lua_timer_period(s) == 1);

		script_lua_close(s);
	}

	// An unknown event type is ignored rather than mis-dispatched.
	{
		script_lua_t *s = open_engine(0, NULL);
		script_lua_install_events(s);
		script_event_t ev = {.type = 200, .len = 1};
		ok("unknown event type ignored", script_lua_dispatch(s, &ev, NULL, 0));
		ok("wants() false for unknown type", !script_lua_wants(s, 200));
		script_lua_close(s);
	}

	if (argc > 1) {
		run_packed(argv[1]);
	} else {
		printf("(no packed fixture given; skipping the end-to-end check)\n");
	}

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
