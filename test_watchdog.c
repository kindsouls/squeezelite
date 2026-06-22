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

// Unit test for the portable output watchdog timer logic (output_watchdog.c).
// Builds standalone - no audio backend, threads or frameworks required.
//   make test          (or: cc -o test_watchdog test_watchdog.c output_watchdog.c && ./test_watchdog)

#include "squeezelite.h"

// logging stubs so output_watchdog.c links without the rest of squeezelite
const char *logtime(void) { return "test"; }
void logprint(const char *fmt, ...) { (void)fmt; }

static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL line %d: %s\n", __LINE__, #cond); \
		++failures; \
	} \
} while (0)

int main(void) {
	const u32_t T = 5000; // 5 second threshold

	// init / accessor
	output_watchdog_init(lINFO, T, 3);
	CHECK(output_watchdog_timeout() == T);
	output_watchdog_init(lINFO, 0, 3);
	CHECK(output_watchdog_timeout() == 0);

	// disabled (timeout 0): never fires, even when clearly stalled
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, false, 1000, 100000, 0));

	// healthy: recent write, well within threshold
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, false, 10000, 12000, T));

	// stalled: no write for longer than threshold while running
	CHECK( output_watchdog_stalled(OUTPUT_RUNNING, false, false, 10000, 16000, T));

	// stalled while buffering (callback died before reaching RUNNING - the
	// post-wake "track dispatched but never plays" case)
	CHECK( output_watchdog_stalled(OUTPUT_BUFFER, false, false, 10000, 16000, T));

	// other active states are watched too
	CHECK( output_watchdog_stalled(OUTPUT_PAUSE_FRAMES, false, false, 10000, 16000, T));
	CHECK( output_watchdog_stalled(OUTPUT_START_AT, false, false, 10000, 16000, T));

	// boundary: exactly at the threshold does not fire (strictly greater)
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, false, 10000, 15000, T));
	// one ms past the threshold fires
	CHECK( output_watchdog_stalled(OUTPUT_RUNNING, false, false, 10000, 15001, T));

	// suppressed while a reopen is already pending
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, true, 10000, 60000, T));
	// suppressed while the device is in an error/reopen state (monitor thread owns it)
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, true, false, 10000, 60000, T));

	// not armed when off or stopped (device may legitimately be idle / closed)
	CHECK(!output_watchdog_stalled(OUTPUT_OFF, false, false, 10000, 60000, T));
	CHECK(!output_watchdog_stalled(OUTPUT_STOPPED, false, false, 10000, 60000, T));

	// no reference yet (never written) - don't fire
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, false, 0, 60000, T));

	// clock went backwards - ignore rather than fire spuriously
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, false, 60000, 50000, T));

	// a fresh write resets the condition
	CHECK(!output_watchdog_stalled(OUTPUT_RUNNING, false, false, 16000, 16100, T));

	// manual trigger (SIGUSR1): false until set, true exactly once, then cleared
	CHECK(!output_watchdog_triggered());
	output_watchdog_trigger();
	CHECK( output_watchdog_triggered());
	CHECK(!output_watchdog_triggered());

	// escalation: consecutive in-episode reopens + restart decision
	output_watchdog_init(lINFO, T, 3);           // timeout 5s, restart after 3 reopens
	output_watchdog_reset();
	CHECK(output_watchdog_note_reopen(10000) == 1);
	CHECK(!output_watchdog_should_restart(1));
	CHECK(output_watchdog_note_reopen(15000) == 2);   // gap == timeout: same episode
	CHECK(!output_watchdog_should_restart(2));
	CHECK(output_watchdog_note_reopen(20000) == 3);   // threshold reached
	CHECK( output_watchdog_should_restart(3));
	CHECK( output_watchdog_should_restart(4));
	// a gap longer than two timeout windows is a fresh episode -> count restarts
	CHECK(output_watchdog_note_reopen(40000) == 1);
	// clock going backwards is treated as a fresh episode too
	CHECK(output_watchdog_note_reopen(100) == 1);
	// explicit reset clears the running count
	output_watchdog_note_reopen(50000);
	output_watchdog_reset();
	CHECK(output_watchdog_note_reopen(60000) == 1);
	// self-restart disabled (restart_after = 0) never escalates
	output_watchdog_init(lINFO, T, 0);
	CHECK(!output_watchdog_should_restart(99));

	if (failures) {
		printf("watchdog tests: %d FAILED\n", failures);
		return 1;
	}
	printf("watchdog tests: all passed\n");
	return 0;
}
