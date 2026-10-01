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

#include "log_ring.h"

#include <string.h>
#include <stdio.h>

#ifndef LOG_RING_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#endif

static char m_lines[LOG_RING_LINES][LOG_RING_LINE_LEN];
static int m_head;		// where the next line goes
static int m_count;
static uint32_t m_dropped;
static bool m_init;
static int m_suspend;

/*
 * A spinlock rather than a mutex. The producers include the ESP-IDF log hook,
 * which may run from a task holding other locks, and commands_printf's
 * out-of-memory path. Both want a bounded, non-blocking append more than they
 * want the line.
 */
#ifndef LOG_RING_HOST_TEST
static portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()		portENTER_CRITICAL(&m_lock)
#define UNLOCK()	portEXIT_CRITICAL(&m_lock)
#else
#define LOCK()		do {} while (0)
#define UNLOCK()	do {} while (0)
#endif

void log_ring_init(void) {
	LOCK();
	m_head = 0;
	m_count = 0;
	m_dropped = 0;
	m_suspend = 0;
	m_init = true;
	UNLOCK();
}

// One line, already split and trimmed. Caller holds the lock.
static void push(const char *text, size_t len) {
	if (len == 0) {
		return;
	}
	if (len >= LOG_RING_LINE_LEN) {
		len = LOG_RING_LINE_LEN - 1;
	}

	memcpy(m_lines[m_head], text, len);
	m_lines[m_head][len] = '\0';

	m_head = (m_head + 1) % LOG_RING_LINES;
	if (m_count < LOG_RING_LINES) {
		m_count++;
	} else {
		// The ring is full, so this push overwrote the oldest line.
		m_dropped++;
	}
}

void log_ring_addn(const char *text, size_t len) {
	if (!m_init || !text || len == 0) {
		return;
	}

	LOCK();

	if (m_suspend > 0) {
		UNLOCK();
		return;
	}

	// Split on newlines: a producer that formats several lines in one call
	// should read back as several, and vprintf-style callers routinely do.
	size_t start = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i == len || text[i] == '\n') {
			size_t n = i - start;
			// Strip a carriage return, which the ESP-IDF log emits.
			if (n > 0 && text[start + n - 1] == '\r') {
				n--;
			}
			push(text + start, n);
			start = i + 1;
		}
	}

	UNLOCK();
}

void log_ring_add(const char *text) {
	if (!text) {
		return;
	}
	log_ring_addn(text, strlen(text));
}

int log_ring_read(char (*out)[LOG_RING_LINE_LEN], int max) {
	if (!out || max <= 0) {
		return 0;
	}

	LOCK();

	int n = m_count < max ? m_count : max;
	// Oldest first. With a full ring the oldest is at head; with a partial
	// one it is at 0. Taking the newest n means starting that far back from
	// head either way.
	int first = (m_head - n + LOG_RING_LINES) % LOG_RING_LINES;

	for (int i = 0; i < n; i++) {
		int idx = (first + i) % LOG_RING_LINES;
		memcpy(out[i], m_lines[idx], LOG_RING_LINE_LEN);
	}

	UNLOCK();
	return n;
}

void log_ring_suspend(void) {
	LOCK();
	m_suspend++;
	UNLOCK();
}

void log_ring_resume(void) {
	LOCK();
	if (m_suspend > 0) {
		m_suspend--;
	}
	UNLOCK();
}

int log_ring_count(void) {
	LOCK();
	int n = m_count;
	UNLOCK();
	return n;
}

uint32_t log_ring_dropped(void) {
	LOCK();
	uint32_t n = m_dropped;
	UNLOCK();
	return n;
}

#ifndef LOG_RING_HOST_TEST
static vprintf_like_t m_prev_vprintf;

static int log_vprintf(const char *fmt, va_list args) {
	// Formatted twice when there is a previous handler, because va_list is
	// consumed by use. Copying it is the portable way and the only cost is
	// one extra format of a diagnostic line.
	va_list copy;
	va_copy(copy, args);

	char buf[LOG_RING_LINE_LEN * 2];
	int len = vsnprintf(buf, sizeof(buf), fmt, copy);
	va_end(copy);

	if (len > 0) {
		log_ring_addn(buf, (size_t)len < sizeof(buf) ? (size_t)len
				: sizeof(buf) - 1);
	}

	if (m_prev_vprintf) {
		return m_prev_vprintf(fmt, args);
	}
	return len;
}

void log_ring_hook_esp_log(void) {
	// Kept and still called, so this adds a copy rather than taking the log
	// away from the console.
	vprintf_like_t prev = esp_log_set_vprintf(log_vprintf);
	if (prev != log_vprintf) {
		m_prev_vprintf = prev;
	}
}
#else
void log_ring_hook_esp_log(void) {
}
#endif
