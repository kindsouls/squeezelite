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
| **macOS (recommended)** | `make -f Makefile.brew` | Homebrew libs + CLT SDK. See below. |
| Linux / ALSA (default) | `make` | Default backend is ALSA. |
| Linux / PortAudio | `make OPTS="-DPORTAUDIO"` | |
| macOS (bundled libs) | `make -f Makefile.osx` / `Makefile.m1` | Only if you have the bundled static libs in `include/`,`libm1/` **and** full Xcode. |
| Unit tests | `make test` | Builds & runs `test_watchdog` (no audio backend needed). |

Common feature `OPTS`: `-DDSD -DRESAMPLE -DFFMPEG -DALAC -DOPUS -DVISEXPORT
-DUSE_SSL -DLINKALL` (see the `OPT_*` vars in `Makefile`).

### Build on macOS with Homebrew (verified on arm64, macOS 15, Command Line Tools)

The stock `Makefile.osx` / `Makefile.m1` need bundled static libs and a full Xcode
SDK. `Makefile.brew` instead links the Homebrew libraries and the Command Line Tools
SDK — the usual setup on a personal Mac. One time:

```
brew install portaudio flac libvorbis libogg opus opusfile mpg123 faad2 libsoxr ffmpeg openssl@3
make -f Makefile.brew            # -> ./squeezelite (arm64 or Intel)
```

> **Heads-up:** install those libs *explicitly* (as above). `brew install`/`brew
> autoremove` will delete codec libs that are only present as orphaned auto-installed
> dependencies, which breaks the dynamically-linked binary. Explicitly-installed
> formulae are kept. If a future `brew autoremove` removes one, reinstall it and
> re-run `make -f Makefile.brew`.

Feature set built by `Makefile.brew`: PortAudio output, FFmpeg (wma/alac/aac),
native Opus, soxr resampling, DSD, visualizer export, TLS, ogg metadata; MP3 via
mpg123. Edit `OPTS` in `Makefile.brew` to change it.

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

### How it's fixed (layered, loudest-wins)

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

Layers 1–3 converge on the **existing** `output.pa_reopen` → `_pa_open()` path, which
runs on the slimproto control thread under the output lock. The native hooks (which
run on other threads) only *flag* a reopen and `wake_controller()`; they never call
into PortAudio directly.

When reopening **isn't** enough, two escalations keep a wedged client from imposing on
the server (the original failure made LMS spin on `Waiting for queue to drain`):

4. **Exit for a supervised restart (device present but wedged).** If the device still
   opens (`error_opening` clear) but repeated reopens don't restore sample flow — e.g.
   PortAudio is holding a device object CoreAudio rebuilt on wake; only a fresh
   `Pa_Initialize()` clears it — then after `<restart>` consecutive failed reopens the
   process `exit()`s with code `OUTPUT_WATCHDOG_EXIT_CODE` (69). Exiting closes the
   SlimProto socket (so the server immediately sees the player drop and stops looping)
   and lets the supervisor (launchd) hand us a clean process. The consecutive-reopen
   counting is episode-aware and unit-tested (`output_watchdog_note_reopen` /
   `output_watchdog_should_restart`).

5. **Report "stopped" to the server (device absent).** If the device can't be opened
   at all for `OUTPUT_WATCHDOG_DEVGONE_MS` (10 s) while we should be playing — DAC
   unplugged/removed, where a restart would only *flap* — we transition the output to
   `OUTPUT_STOPPED` and send `STMu`, so LMS stops feeding/waiting, then let the
   existing monitor thread keep probing for the device's return. `error_opening` is the
   present-vs-absent signal that routes between #4 (present → restart) and #5
   (absent → report stopped), so the two never fight.

### CLI flag

```
-k <timeout>[:<restart>]   Output stall watchdog (PortAudio builds).
    <timeout>  reopen the output device if no samples are written for this many
               seconds. Default 5; 0 disables the watchdog entirely.
    <restart>  after this many consecutive failed reopens, exit (code 69) for a
               supervised restart. Default 3; 0 disables self-restart (reopen only).
```

Examples: `-k 5:3` (default), `-k 10` (10 s, default restart), `-k 5:0` (reopen
forever, never self-restart), `-k 0` (watchdog off). All existing flags are unchanged.

See `examples/squeezelite.plist` for a launchd LaunchAgent that supervises the process
(`KeepAlive`/`SuccessfulExit=false` restarts on the watchdog's non-zero exit but not on
a clean stop; `ThrottleInterval` prevents a tight loop; `ProcessType=Interactive` keeps
App Nap from throttling audio). It must be a LaunchAgent, not a LaunchDaemon, so it runs
in the GUI session where CoreAudio and the IOKit/CoreAudio notifications are available.

### Log lines (grep these to correlate with server-side stalls)

Emitted at **WARN** level, so visible at the default log level (no `-d` needed):

```
macOS sleep detected (will sleep) - output device will be reopened on wake
macOS wake detected (has powered on) - reopening output device
CoreAudio device list changed - rebinding output device '<dev>'
output watchdog: no device writes for <N> ms (state <S>) - forcing output device reopen
manual output recovery trigger received (SIGUSR1) - forcing output device reopen
output recovery exhausted - exiting for supervised restart (exit 69)
output device unavailable for <N> ms - reporting stopped to server
```

Setup confirmations (`registered for macOS sleep/wake notifications`, `output
watchdog enabled, timeout <N> ms, restart after <R> failed reopens`, etc.) are
INFO — enable with `-d output=info`.

### Files

| File | Role |
| --- | --- |
| `output_watchdog.c` | Portable watchdog timer + stall decision + episode-aware reopen counter + restart decision + SIGUSR1 trigger flag. No platform/audio deps. |
| `output_mac.c` | macOS-only (`#if OSX`): IOKit power notifications + CoreAudio device-list listener. |
| `test_watchdog.c` | Standalone unit test for the stall decision, reopen counter, restart decision, and trigger flag. |
| `output_pa.c` | Resets `output.updated` on successful (re)open; starts/stops the macOS hooks; `output_pa_active()` guard. |
| `slimproto.c` | Control-loop wiring: manual trigger, watchdog reopen, #4 exit-for-restart, #5 report-stopped. |
| `main.c` | `-k <timeout>[:<restart>]` parsing, `output_watchdog_init`, `SIGUSR1` handler. |
| `squeezelite.h` | Declarations + `OUTPUT_WATCHDOG_*` defaults / exit code. |
| `examples/squeezelite.plist` | Sample launchd LaunchAgent that supervises and restarts the process. |

### Why it won't false-trigger

The watchdog only arms when the device should be live and the callback should be
running. It is suppressed when: the timeout is 0; a reopen is already pending
(`output.pa_reopen`); the device is in an error/probe state (`output.error_opening`,
handled by the existing monitor thread); the output is `OUTPUT_OFF`/`OUTPUT_STOPPED`
(idle, possibly closed by `-C`); no write has been recorded yet; or the clock went
backwards. In normal playback the callback fires every few ms, so a 5 s threshold has
a wide margin. After any reopen, `output.updated` is reset, giving a fresh grace
window.

The **self-restart** (#4) is just as conservative: the reopen counter only advances on
genuine repeat stalls within one episode (a gap longer than two timeout windows, or the
clock going backwards, starts a fresh episode), and it resets the moment the device
goes absent (handing off to #5) or recovers. With defaults that's ~3 failed reopens
over ~15 s before exiting — and `ThrottleInterval` in the plist bounds restarts to one
per 10 s even in the worst case. **Report-stopped** (#5) only fires after a sustained
10 s outage while actively playing, and once per outage.

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
