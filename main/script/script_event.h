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

#ifndef MAIN_SCRIPT_SCRIPT_EVENT_H_
#define MAIN_SCRIPT_SCRIPT_EVENT_H_

#include <stdint.h>

/*
 * Events handed from the firmware's own tasks to the script engine.
 *
 * A CAN frame arrives on the CAN task and app data on a comms task, while
 * the interpreter belongs to the engine task. An interpreter is not
 * reentrant, so a producer must never call into it: it copies the event into
 * a queue and returns. Everything in this struct is therefore plain bytes,
 * fixed size and copyable, with no pointers into the producer's buffers --
 * those buffers are reused the moment the producer returns.
 *
 * Keeping the payload fixed also means the queue is allocated once at start
 * up. A producer that had to allocate could fail or block, and both are worse
 * than dropping a frame.
 */

// Chosen so a whole CAN frame always fits with room for app data that is
// worth delivering. Anything longer is truncated, and the truncation is
// reported rather than hidden: see script_lua_events_dropped.
#define SCRIPT_EVENT_PAYLOAD	64

typedef enum {
	SCRIPT_EV_NONE = 0,
	SCRIPT_EV_CAN_SID,	// Standard id, bus 1
	SCRIPT_EV_CAN_EID,	// Extended id, bus 1
	SCRIPT_EV_CAN2_SID,	// Standard id, bus 2
	SCRIPT_EV_CAN2_EID,	// Extended id, bus 2
	SCRIPT_EV_APP_DATA,	// COMM_CUSTOM_APP_DATA
	SCRIPT_EV_TIMER,	// Periodic, posted by the engine task itself
} script_event_type_t;

typedef struct {
	uint8_t type;		// script_event_type_t
	uint32_t id;		// CAN id, unused otherwise
	uint16_t len;		// Bytes valid in data
	uint8_t truncated;	// Non-zero if the source was longer than data
	uint8_t data[SCRIPT_EVENT_PAYLOAD];
} script_event_t;

#endif /* MAIN_SCRIPT_SCRIPT_EVENT_H_ */
