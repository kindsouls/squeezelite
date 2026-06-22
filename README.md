## This fork — macOS sleep/wake & stream-stall hardening

A fork of [ralph-irving/squeezelite](https://github.com/ralph-irving/squeezelite)
hardened for running as a background service on macOS (PortAudio/CoreAudio). It fixes
a silent failure where the audio output stalls after the Mac sleeps/wakes or a USB DAC
re-enumerates — the SlimProto control connection reconnects fine, but samples stop
flowing, the player's `playPoint` freezes, and the server loops on
`nextChunk: Waiting for queue to drain` until LMS itself becomes unresponsive.

Added (all isolated behind `#if OSX` / `#if PORTAUDIO`; ALSA/Linux and the
SlimProto/sync paths are unchanged):

- **Output stall watchdog** — reopens the output device if no samples are written for a
  configurable timeout. New flag **`-k <seconds>`** (default 5, `0` disables).
- **Native sleep/wake detection** (IOKit) — proactively reopens the stream on wake.
- **CoreAudio device-list handling** — rebinds when the output device disappears/reappears.
- **Loud, greppable recovery logging**, plus **`kill -USR1 <pid>`** to force a reopen for testing.

### Build on macOS (Homebrew)

The stock `Makefile.osx` / `Makefile.m1` need bundled static libs and a full Xcode SDK.
On a typical Mac, build against Homebrew libraries with `Makefile.brew`:

```sh
brew install portaudio flac libvorbis libogg opus opusfile mpg123 faad2 libsoxr ffmpeg openssl@3
make -f Makefile.brew          # -> ./squeezelite
make test                      # run the watchdog unit tests
```

Install those libraries *explicitly* (as above): `brew autoremove` will otherwise delete
codec libs that are only present as orphaned dependencies and break the dynamically
linked binary.

See **[CLAUDE.md](CLAUDE.md)** for the full design, log-line reference, and the manual
sleep/wake test procedure.

---

This program is free software: you can redistribute it and/or modify<br>
it under the terms of the GNU General Public License as published by<br>
the Free Software Foundation, either version 3 of the License, or<br>
(at your option) any later version.<br>
<br>
This program is distributed in the hope that it will be useful,<br>
but WITHOUT ANY WARRANTY; without even the implied warranty of<br>
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the<br>
GNU General Public License for more details.<br>
<br>
You should have received a copy of the GNU General Public License<br>
along with this program.  If not, see <http://www.gnu.org/licenses/>.<br>
<br>
Additional permission under GNU GPL version 3 section 7<br>
<br>
If you modify this program, or any covered work, by linking or
combining it with OpenSSL (or a modified version of that library),
containing parts covered by the terms of The OpenSSL Project, the
licensors of this program grant you additional permission to convey
the resulting work. {Corresponding source for a non-source form of
such a combination shall include the source code for the parts of
OpenSSL used as well as that of the covered work.}<br>
<br>
Contains dsd2pcm library Copyright 2009, 2011 Sebastian Gesemann which<br>
is subject to its own license.<br>
<br>
Contains the Daphile Project full dsd patch Copyright 2013-2017 Daphile,<br>
which is subject to its own license.<br>
<br>
Option to allow server side upsampling for PCM streams (-W) from<br>
squeezelite-R2 (c) Marco Curti 2015, marcoc1712@gmail.com.<br>
<br>
This software uses libraries from the FFmpeg project under<br>
the LGPLv2.1 and its source can be downloaded from<br>
https://sourceforge.net/projects/lmsclients/files/source/<br>
