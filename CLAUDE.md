# CLAUDE.md

Guidance for working in this repository.

## What this is

Squeezelite — a lightweight, headless [Lyrion Music Server](https://lyrion.org/)
(formerly Logitech Media Server / LMS) client written in C. This tree is a fork of
[ralph-irving/squeezelite](https://github.com/ralph-irving/squeezelite) carrying a
**macOS sleep/wake & stream-stall hardening** patch set (see the dedicated section
below). On Linux it builds against ALSA; on macOS/Windows/BSD it builds against
PortAudio.

The control plane (SlimProto / sync) and the output plane (audio device) run on
separate threads. This patch set only touches the **output plane**; SlimProto and
sync handling are deliberately left alone.

## Build & run

The platform-specific makefiles set `CFLAGS`/`LDFLAGS`/`OPTS` and then `include
Makefile`. Feature flags are passed via `OPTS` (see the `OPT_*` vars in `Makefile`).

| Target | Command | Notes |
| --- | --- | --- |
| Linux / ALSA (default) | `make` | Default backend is ALSA. |
| Linux / PortAudio | `make OPTS="-DPORTAUDIO"` | |
| macOS Intel | `make -f Makefile.osx` | Needs bundled `./include` + `libportaudio.a`. |
| macOS arm64 (Apple Silicon) | `make -f Makefile.m1` | Needs bundled `./includem1` + `./libm1`. |
| Unit tests | `make test` | Builds & runs `test_watchdog` (no audio backend needed). |

Common feature `OPTS`: `-DDSD -DRESAMPLE -DFFMPEG -DALAC -DOPUS -DVISEXPORT
-DUSE_SSL -DLINKALL`. The macOS makefiles already enable a sensible set.

List devices and pick one with `-o`:

```
./squeezelite -l
./squeezelite -o "USB DAC [Core Audio]" -s 192.168.1.10
```

## macOS Sleep/Wake & Stream-Stall Hardening

### Problem this solves

After a Mac sleeps and wakes, the SlimProto control connection reconnects fine, but
the audio output path can silently die: CoreAudio tears down and rebuilds the USB
device object on wake, leaving PortAudio holding a stream handle that still looks
valid at the API level but no longer delivers samples. Squeezelite keeps "writing"
to a dead stream, `frames_played` freezes, and the server loops on
`nextChunk: Waiting for queue to drain` until LMS itself becomes unresponsive.
Squeezelite logs nothing — the failure is only visible from server-side timing.

### How it's fixed (three layers, loudest-wins)

1. **Output stall watchdog (priority fix, portable).** The PortAudio callback
   refreshes `output.updated` on every invocation. The slimproto control loop (which
   stays alive and runs every ~100 ms) checks: *if we should be streaming and
   `output.updated` hasn't advanced for longer than the threshold, the callback is
   dead → force a clean reopen.* This catches the freeze regardless of cause (sleep/
   wake, USB dropout, CoreAudio churn) and does not depend on the sleep/wake hook.
   The decision logic is a pure function (`output_watchdog_stalled`) with unit tests.

2. **Native sleep/wake detection (macOS).** `IORegisterForSystemPower` on a dedicated
   CFRunLoop thread. On `kIOMessageSystemHasPoweredOn` (wake) we proactively schedule
   a stream reopen instead of trusting the existing handle. On
   `kIOMessageSystemWillSleep` we log and acknowledge the power change.

3. **CoreAudio device-list changes (macOS).** A `kAudioHardwarePropertyDevices`
   listener detects USB re-enumeration (device disappear/reappear) and schedules a
   rebind, debounced and gated to active playback to avoid churn on unrelated device
   changes.

All three converge on the **existing** `output.pa_reopen` → `_pa_open()` path, which
runs on the slimproto control thread under the output lock. The native hooks (which
run on other threads) only *flag* a reopen and `wake_controller()`; they never call
into PortAudio directly.

### New CLI flag

```
-k <timeout>   Output stall watchdog: reopen the output device if no samples are
               written for <timeout> seconds. Default 5, 0 disables. (PortAudio builds)
```

All existing flags and their behavior are unchanged.

### Log lines (grep these to correlate with server-side stalls)

Emitted at **WARN** level, so visible at the default log level (no `-d` needed):

```
macOS sleep detected (will sleep) - output device will be reopened on wake
macOS wake detected (has powered on) - reopening output device
CoreAudio device list changed - rebinding output device '<dev>'
output watchdog: no device writes for <N> ms (state <S>) - forcing output device reopen
manual output recovery trigger received (SIGUSR1) - forcing output device reopen
```

Setup confirmations (`registered for macOS sleep/wake notifications`, `output
watchdog enabled, timeout <N> ms`, etc.) are INFO — enable with `-d output=info`.

### Files

| File | Role |
| --- | --- |
| `output_watchdog.c` | Portable watchdog timer + decision logic + SIGUSR1 trigger flag. No platform/audio deps. |
| `output_mac.c` | macOS-only (`#if OSX`): IOKit power notifications + CoreAudio device-list listener. |
| `test_watchdog.c` | Standalone unit test for the watchdog decision + trigger flag. |
| `output_pa.c` | Resets `output.updated` on successful (re)open; starts/stops the macOS hooks. |
| `slimproto.c` | Control-loop wiring: consumes the manual trigger, runs the watchdog, reopens. |
| `main.c` | `-k` flag parsing, `output_watchdog_init`, `SIGUSR1` handler. |
| `squeezelite.h` | Declarations + `OUTPUT_WATCHDOG_DEFAULT_MS`. |

### Why it won't false-trigger

The watchdog only arms when the device should be live and the callback should be
running. It is suppressed when: the timeout is 0; a reopen is already pending
(`output.pa_reopen`); the device is in an error/probe state (`output.error_opening`,
handled by the existing monitor thread); the output is `OUTPUT_OFF`/`OUTPUT_STOPPED`
(idle, possibly closed by `-C`); no write has been recorded yet; or the clock went
backwards. In normal playback the callback fires every few ms, so a 5 s threshold has
a wide margin. After any reopen, `output.updated` is reset, giving a fresh grace
window.

## Testing

### Unit tests (CI-friendly)

```
make test
```

Builds and runs `test_watchdog`, which exercises `output_watchdog_stalled` (threshold
boundary, reset-on-write, suppression conditions, clock-skew) and the SIGUSR1 trigger
flag — independent of any device-reopen code.

### Manual recovery trigger (no sleep/wake cycle needed)

Send `SIGUSR1` to a running instance to force the exact reopen path the watchdog and
wake hook use:

```
kill -USR1 $(pgrep -x squeezelite)
```

Expect a `manual output recovery trigger received (SIGUSR1) ...` line, then the normal
`opened device ...` reopen log. Audio should continue without a manual seek.

### Manual sleep/wake test procedure

1. Run with output logging: `squeezelite -o "<your DAC>" -s <LMS> -d output=info`
   (optionally `-k 5`).
2. Start playback (ideally synced with a second player) and confirm audio.
3. Sleep the Mac mid-track (`pmset sleepnow` or the Apple menu).
4. Wake it.
5. In the log, confirm `macOS sleep detected ...` then `macOS wake detected ...
   reopening output device`, followed by `opened device ...`.
6. Confirm audio **resumes on its own** (no manual seek) and the server's `playPoint`
   advances again / the `nextChunk: Waiting for queue to drain` loop clears.
7. To prove the watchdog backstop independently, repeat but with the IOKit hook's
   value disabled by relying solely on `-k` (e.g. pull the USB cable for >5 s and
   reinsert): expect `output watchdog: no device writes for ...` then a reopen.

## Architecture notes (for future work)

- **Output timing** the server sees comes from `output.frames_played_dmp`,
  `output.device_frames`, and `output.updated`, copied into `status` in the slimproto
  loop (`slimproto.c`) and sent via `sendSTAT`. A frozen `output.updated` is the
  client-side signature of the server-side stall.
- **PortAudio is callback-driven** (`pa_callback` in `output_pa.c`), unlike ALSA which
  has its own write thread. There is no dedicated PA output thread to watch; the
  watchdog watches the callback's heartbeat (`output.updated`) instead.
- **Reopen contract:** `_pa_open()` must be called on the control thread with the
  output lock held. Other threads request a reopen via `output.pa_reopen = true;
  wake_controller();`. Follow this pattern for any new recovery trigger.
- **Platform isolation:** macOS-specific code lives in `output_mac.c` behind
  `#if OSX`; the portable timer lives in `output_watchdog.c`. ALSA/Linux behavior is
  unchanged (the watchdog is only wired into the PortAudio reopen path).

## Constraints honored by this patch set

- macOS/PortAudio changes are isolated behind `#if OSX` / `#if PORTAUDIO`.
- All existing CLI flags keep their current behavior; `-k` is new.
- SlimProto / sync / control-plane reconnect logic is untouched.
- ALSA/Linux output behavior is unchanged.
- Targeted and reviewable: two new files + small, localized edits, matching existing
  tab-indented style.
