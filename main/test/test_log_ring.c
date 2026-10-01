/*
 * Host tests for the log ring.
 *
 * Compiled from the firmware source with LOG_RING_HOST_TEST, which stubs the
 * FreeRTOS spinlock and the esp_log hook. Everything else -- the wrap, the
 * newline splitting, the truncation and the dropped count -- is the code that
 * ships.
 *
 * Worth testing off-target because the read is index arithmetic over a ring
 * that is read both partially full and wrapped, and because the thing it
 * exists for is diagnosing a board that cannot be diagnosed any other way. A
 * log that silently loses its beginning is worse than no log.
 */

#include "../log_ring.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int fails;

static void ok(const char *what, int cond) {
	checks++;
	if (!cond) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

static void eq_str(const char *what, const char *got, const char *want) {
	checks++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
	}
}

static void eq_int(const char *what, int got, int want) {
	checks++;
	if (got != want) {
		fails++;
		printf("FAIL %s: got %d want %d\n", what, got, want);
	}
}

int main(void) {
	static char out[LOG_RING_LINES][LOG_RING_LINE_LEN];

	// Nothing before init, and nothing after it.
	log_ring_add("lost");
	log_ring_init();
	eq_int("empty after init", log_ring_count(), 0);
	eq_int("nothing to read", log_ring_read(out, LOG_RING_LINES), 0);
	eq_int("nothing dropped", (int)log_ring_dropped(), 0);

	// A partially full ring reads oldest first, which is reading order for a
	// boot log.
	log_ring_add("one");
	log_ring_add("two");
	log_ring_add("three");
	eq_int("three held", log_ring_count(), 3);
	eq_int("three read", log_ring_read(out, LOG_RING_LINES), 3);
	eq_str("oldest first", out[0], "one");
	eq_str("then",         out[1], "two");
	eq_str("then newest",  out[2], "three");

	// Asking for fewer gives the newest ones, still oldest first: a page
	// showing the last n lines wants the end of the log, not the start.
	eq_int("two read", log_ring_read(out, 2), 2);
	eq_str("the second oldest of those", out[0], "two");
	eq_str("and the newest",             out[1], "three");

	// A trailing newline is not a second, empty line.
	log_ring_init();
	log_ring_add("trailing\n");
	eq_int("one line, not two", log_ring_count(), 1);
	log_ring_read(out, 1);
	eq_str("and the newline is gone", out[0], "trailing");

	// Embedded newlines split: a producer formatting several lines in one
	// call should read back as several, and vprintf callers routinely do.
	log_ring_init();
	log_ring_add("a\nb\nc");
	eq_int("split into three", log_ring_count(), 3);
	log_ring_read(out, 3);
	eq_str("first", out[0], "a");
	eq_str("last",  out[2], "c");

	// The ESP-IDF log ends lines with \r\n.
	log_ring_init();
	log_ring_add("crlf\r\n");
	eq_int("crlf is one line", log_ring_count(), 1);
	log_ring_read(out, 1);
	eq_str("with no carriage return", out[0], "crlf");

	// An empty line between two is kept as nothing rather than as an entry:
	// push ignores a zero length, so a blank line does not cost a slot.
	log_ring_init();
	log_ring_add("x\n\ny");
	eq_int("a blank line costs no slot", log_ring_count(), 2);

	// Longer than a slot is truncated, not split. A split line reads as two
	// events.
	log_ring_init();
	char big[LOG_RING_LINE_LEN * 2];
	memset(big, 'z', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	log_ring_add(big);
	eq_int("still one line", log_ring_count(), 1);
	log_ring_read(out, 1);
	eq_int("truncated to the slot", (int)strlen(out[0]), LOG_RING_LINE_LEN - 1);

	// Exactly full, then one more.
	log_ring_init();
	char buf[32];
	for (int i = 0; i < LOG_RING_LINES; i++) {
		snprintf(buf, sizeof(buf), "line%d", i);
		log_ring_add(buf);
	}
	eq_int("full", log_ring_count(), LOG_RING_LINES);
	eq_int("nothing dropped while filling", (int)log_ring_dropped(), 0);
	eq_int("reads the lot", log_ring_read(out, LOG_RING_LINES), LOG_RING_LINES);
	eq_str("oldest is the first written", out[0], "line0");

	log_ring_add("overflow");
	eq_int("still full", log_ring_count(), LOG_RING_LINES);
	eq_int("and one dropped", (int)log_ring_dropped(), 1);
	log_ring_read(out, LOG_RING_LINES);
	eq_str("the oldest is gone", out[0], "line1");
	eq_str("and the newest is the overflow",
			out[LOG_RING_LINES - 1], "overflow");

	// Wrapping all the way round.
	for (int i = 0; i < LOG_RING_LINES; i++) {
		snprintf(buf, sizeof(buf), "second%d", i);
		log_ring_add(buf);
	}
	eq_int("dropped counts every overwrite",
			(int)log_ring_dropped(), 1 + LOG_RING_LINES);
	log_ring_read(out, LOG_RING_LINES);
	eq_str("nothing of the first pass is left", out[0], "second0");

	// Reading more than is held is not an overrun.
	log_ring_init();
	log_ring_add("only");
	eq_int("asking for too many gives what there is",
			log_ring_read(out, LOG_RING_LINES), 1);

	// Defensive arguments. These are reachable from a binding.
	ok("a null read is zero",    log_ring_read(NULL, 4) == 0);
	ok("a zero max is zero",     log_ring_read(out, 0) == 0);
	ok("a negative max is zero", log_ring_read(out, -1) == 0);
	log_ring_add(NULL);
	log_ring_addn("x", 0);
	eq_int("a null or empty add is ignored", log_ring_count(), 1);

	printf("\n%d checks, %d failures\n", checks, fails);
	return fails > 0 ? 1 : 0;
}
