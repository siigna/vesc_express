/*
	Copyright 2026 Stephen Bouche

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
 * A small ring of the most recent log lines.
 *
 * commands_printf sends a COMM_PRINT packet out whichever port last spoke to
 * the board, so a message printed before anything connected goes nowhere, and
 * a message printed to a port that has since gone away is lost. Everything
 * said during bring-up -- which panel came up, whether touch answered,
 * whether the radio attached -- falls into exactly that window.
 *
 * So the lines are also kept here. Three producers feed it: commands_printf
 * and its lisp variant, the ESP-IDF log through esp_log_set_vprintf, and the
 * script engine's print. A script can read it back, which is what lets a
 * display show its own boot log instead of needing a serial cable to find out
 * why it came up wrong.
 *
 * Fixed storage, oldest dropped. The count of what was dropped is kept and
 * reported, because a log that silently loses its beginning is worse than one
 * that says it did.
 */

#ifndef MAIN_LOG_RING_H_
#define MAIN_LOG_RING_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Lines kept, and the most of one line that is kept. A line longer than this
// is truncated rather than split, because a split line reads as two events.
// Enough for the ESP-IDF bring-up chatter plus what the firmware and the
// script say over it. The display's own steps are kept separately by
// lib/boot_log.lua as well, so they survive the ring wrapping during a noisy
// radio attach.
#define LOG_RING_LINES		64
#define LOG_RING_LINE_LEN	96

void log_ring_init(void);

/*
 * Append one line. A trailing newline is stripped and embedded newlines split
 * the text into separate entries, so a producer that formats several lines in
 * one call still reads back as several.
 *
 * Safe to call from any task. Never blocks: if the lock is held, the line is
 * counted as dropped rather than waiting, because the callers are diagnostic
 * paths and one of them is the memory-pressure path in commands_printf.
 */
void log_ring_add(const char *text);

// As above, with an explicit length, for text that is not NUL terminated.
void log_ring_addn(const char *text, size_t len);

/*
 * Copy out up to max lines, oldest first, into out[i] of at least
 * LOG_RING_LINE_LEN bytes each. Returns how many were written.
 *
 * Oldest first because that is reading order for a boot log.
 */
int log_ring_read(char (*out)[LOG_RING_LINE_LEN], int max);

/*
 * Stop and resume accepting lines.
 *
 * Dumping the ring prints it, and printing feeds it: the `log` terminal
 * command pushed a copy of its own output over the 64 lines it was there to
 * show, so the second read of a board only ever saw the first read. Suspend
 * around a dump and the ring survives being looked at.
 *
 * Nests, so a dump inside a dump cannot resume early.
 */
void log_ring_suspend(void);
void log_ring_resume(void);

// How many lines are held, and how many were dropped for want of room.
int log_ring_count(void);
uint32_t log_ring_dropped(void);

/*
 * Install the ESP-IDF log hook. Separate from init so a build can keep the
 * ring without redirecting esp_log, and so the hook is installed once the
 * console is up rather than before it.
 *
 * The previous handler is kept and still called, so this adds a copy rather
 * than taking the log away from the console.
 */
void log_ring_hook_esp_log(void);

#endif /* MAIN_LOG_RING_H_ */
