/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2026, ralph_irving@hotmail.com
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Portable output stall watchdog.
//
// The output device drivers (PortAudio callback, ALSA/Pulse threads) refresh
// output.updated every time they successfully hand samples to the device. If
// that timestamp stops advancing while we should be streaming, the output path
// has gone silent even though the rest of the process looks healthy - the
// classic symptom of a PortAudio/CoreAudio stream handle that went stale across
// a macOS sleep/wake cycle, or a USB device that dropped out.
//
// This file holds only the portable timer logic. The decision of *whether* to
// recover lives here (and is unit tested - see test_watchdog.c); the platform
// specific business of actually tearing down and reopening the device is left
// to the output driver (see output_pa.c / slimproto.c wiring).

#include "squeezelite.h"

#include <signal.h>

static log_level loglevel;

// recovery threshold in milliseconds; 0 disables the watchdog
static u32_t watchdog_timeout_ms = 0;

// consecutive failed reopens within one stall episode before we give up on
// in-process recovery and ask to be restarted; 0 disables that escalation
static unsigned watchdog_restart_after = 0;

// escalation state: how many reopens we've forced in the current episode, and
// when the last one happened (to tell a fresh episode from a continuing one)
static unsigned consecutive_reopens = 0;
static u32_t    last_reopen_ms = 0;

// set asynchronously from a signal handler (SIGUSR1) to force a recovery for
// testing; sig_atomic_t so it is safe to touch from the handler
static volatile sig_atomic_t manual_trigger = 0;

void output_watchdog_init(log_level level, u32_t timeout_ms, unsigned restart_after) {
	loglevel = level;
	watchdog_timeout_ms = timeout_ms;
	watchdog_restart_after = restart_after;

	if (timeout_ms) {
		if (restart_after) {
			LOG_INFO("output watchdog enabled, timeout %u ms, restart after %u failed reopens", timeout_ms, restart_after);
		} else {
			LOG_INFO("output watchdog enabled, timeout %u ms, self-restart disabled", timeout_ms);
		}
	} else {
		LOG_INFO("output watchdog disabled");
	}
}

u32_t output_watchdog_timeout(void) {
	return watchdog_timeout_ms;
}

// Record that the watchdog has just forced a reopen and return how many it has
// forced in a row within this stall episode. A long gap since the last reopen
// (more than two timeout windows) means the previous episode recovered, so the
// count starts fresh. Clock going backwards is treated as a fresh episode too.
unsigned output_watchdog_note_reopen(u32_t now_ms) {
	if (last_reopen_ms == 0 || now_ms < last_reopen_ms ||
		(now_ms - last_reopen_ms) > 2 * watchdog_timeout_ms) {
		consecutive_reopens = 0;
	}
	last_reopen_ms = now_ms;
	return ++consecutive_reopens;
}

bool output_watchdog_should_restart(unsigned consecutive) {
	return watchdog_restart_after > 0 && consecutive >= watchdog_restart_after;
}

void output_watchdog_reset(void) {
	consecutive_reopens = 0;
	last_reopen_ms = 0;
}

// Pure decision function: returns true if a stall recovery should be triggered.
// Kept free of globals and locks so it can be exercised directly by the unit
// test. now_ms / last_update_ms are gettime_ms() values (wrap handled by the
// caller never feeding deltas anywhere near 49 days).
bool output_watchdog_stalled(output_state state, bool error_opening, bool reopen_pending,
							 u32_t last_update_ms, u32_t now_ms, u32_t timeout_ms) {

	// watchdog disabled
	if (timeout_ms == 0) {
		return false;
	}

	// a recovery is already scheduled, or the device is known to be down and is
	// already being reopened elsewhere - don't pile on
	if (reopen_pending || error_opening) {
		return false;
	}

	// only watch while audio should actually be flowing; when off or stopped the
	// device may legitimately be idle (or closed) and not writing
	if (state == OUTPUT_OFF || state == OUTPUT_STOPPED) {
		return false;
	}

	// no write has been recorded yet - nothing to measure against
	if (last_update_ms == 0) {
		return false;
	}

	// clock skew / wrap - ignore this tick rather than fire spuriously
	if (now_ms < last_update_ms) {
		return false;
	}

	return (now_ms - last_update_ms) > timeout_ms;
}

// Async-signal-safe: only touches manual_trigger. Wired to SIGUSR1 so a stuck
// output path can be recovered (and the reopen path exercised) on demand.
void output_watchdog_trigger(void) {
	manual_trigger = 1;
}

// Consume the manual trigger flag. Returns true once per SIGUSR1.
bool output_watchdog_triggered(void) {
	if (manual_trigger) {
		manual_trigger = 0;
		return true;
	}
	return false;
}
