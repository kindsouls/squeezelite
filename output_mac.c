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

// macOS specific output hardening: native sleep/wake detection (IOKit) and
// CoreAudio device-list change detection. CoreAudio can tear down and rebuild
// the underlying device object for a USB DAC across a sleep/wake cycle or a USB
// re-enumeration, leaving PortAudio holding a handle that still looks valid at
// the API level but no longer delivers samples to hardware. Rather than trust
// that handle, we proactively schedule a clean reopen of the output stream when
// the machine wakes or the device list changes.
//
// All of the real device work is done on the slimproto control thread via the
// existing output.pa_reopen path - here we only observe events and ask for a
// reopen, so we never touch PortAudio from these callback threads.

#include "squeezelite.h"

#if OSX && PORTAUDIO

#include <AvailabilityMacros.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

// kAudioObjectPropertyElementMaster was renamed to ...Main in the macOS 12 SDK.
// Both are enum constants (not macros), so gate on the SDK version: older SDKs
// only know Master, newer ones deprecate it in favour of Main.
#if !defined(MAC_OS_VERSION_12_0)
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

// ignore device-list churn bursts (USB re-enumeration fires several events)
#define MAC_DEVCHANGE_DEBOUNCE_MS 3000

static log_level loglevel;

extern struct outputstate output;
extern struct buffer *outputbuf;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

static io_connect_t          root_port = 0;
static IONotificationPortRef notify_port = NULL;
static io_object_t           notifier = 0;
static CFRunLoopRef          power_runloop = NULL;
static thread_type           power_thread;
static bool                  power_thread_started = false;
static bool                  ca_listener = false;
static u32_t                 last_devchange = 0;

// Ask the control thread to tear down and reopen the output stream. Reuses the
// existing pa_reopen flag so the device work happens on the slimproto thread
// under the output lock - we must not call into PortAudio from here.
static void request_reopen(void) {
	LOCK;
	output.pa_reopen = true;
	UNLOCK;
	wake_controller();
}

static void power_callback(void *refcon, io_service_t service, natural_t type, void *arg) {
	(void)refcon;
	(void)service;

	switch (type) {
	case kIOMessageCanSystemSleep:
		// Idle sleep request - we have no objection. Must acknowledge or the
		// system stalls for ~30s waiting for us.
		IOAllowPowerChange(root_port, (long)arg);
		break;
	case kIOMessageSystemWillSleep:
		// Forced/lid/idle sleep is now committed. Nothing to tear down here -
		// we rebuild the stream on wake - just acknowledge and log loudly so
		// this lines up with the server side timeline.
		LOG_WARN("macOS sleep detected (will sleep) - output device will be reopened on wake");
		IOAllowPowerChange(root_port, (long)arg);
		break;
	case kIOMessageSystemWillPowerOn:
		LOG_INFO("macOS wake starting (will power on)");
		break;
	case kIOMessageSystemHasPoweredOn:
		LOG_WARN("macOS wake detected (has powered on) - reopening output device");
		request_reopen();
		break;
	default:
		break;
	}
}

static const AudioObjectPropertyAddress devices_address = {
	kAudioHardwarePropertyDevices,
	kAudioObjectPropertyScopeGlobal,
	kAudioObjectPropertyElementMain
};

static OSStatus devices_changed(AudioObjectID object, UInt32 num_addr,
								const AudioObjectPropertyAddress *addr, void *client) {
	bool act;
	u32_t now = gettime_ms();

	(void)object;
	(void)num_addr;
	(void)addr;
	(void)client;

	LOCK;
	// Only act while audio should be flowing, and debounce the burst of events
	// that a single USB re-enumeration produces. error_opening / pa_reopen mean
	// a reopen is already in flight (e.g. PortAudio's own stream-finished hook
	// fired first) so we leave it alone.
	act = (output.state != OUTPUT_OFF && output.state != OUTPUT_STOPPED) &&
		  !output.error_opening && !output.pa_reopen &&
		  !(last_devchange && now >= last_devchange && now - last_devchange < MAC_DEVCHANGE_DEBOUNCE_MS);
	if (act) {
		output.pa_reopen = true;
		last_devchange = now;
	}
	UNLOCK;

	if (act) {
		LOG_WARN("CoreAudio device list changed - rebinding output device '%s'", output.device);
		wake_controller();
	} else {
		LOG_INFO("CoreAudio device list changed (no action: output idle, reopening, or debounced)");
	}

	return noErr;
}

static void *power_thread_fn(void *arg) {
	(void)arg;

	power_runloop = CFRunLoopGetCurrent();

	root_port = IORegisterForSystemPower(NULL, &notify_port, power_callback, &notifier);
	if (root_port) {
		CFRunLoopAddSource(power_runloop, IONotificationPortGetRunLoopSource(notify_port),
						   kCFRunLoopCommonModes);
		LOG_INFO("registered for macOS sleep/wake notifications");
	} else {
		LOG_WARN("IORegisterForSystemPower failed - sleep/wake detection disabled");
	}

	CFRunLoopRun();

	return NULL;
}

void output_mac_init(log_level level) {
	loglevel = level;

	// CoreAudio device-list change listener (USB re-enumeration / device churn).
	// Delivered on a CoreAudio-managed thread; the callback only flags a reopen.
	if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devices_address,
									   devices_changed, NULL) == noErr) {
		ca_listener = true;
		LOG_INFO("registered for CoreAudio device list changes");
	} else {
		LOG_WARN("failed to register CoreAudio device list listener");
	}

	// IOKit power notifications need a CFRunLoop, so run them on their own thread.
	if (pthread_create(&power_thread, NULL, power_thread_fn, NULL) == 0) {
		power_thread_started = true;
	} else {
		LOG_WARN("failed to start macOS power notification thread");
	}
}

void output_mac_close(void) {
	if (ca_listener) {
		AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &devices_address,
										  devices_changed, NULL);
		ca_listener = false;
	}

	if (power_runloop) {
		CFRunLoopStop(power_runloop);
	}

	if (power_thread_started) {
		pthread_join(power_thread, NULL);
		power_thread_started = false;
	}

	if (notify_port) {
		if (notifier) {
			IODeregisterForSystemPower(&notifier);
			notifier = 0;
		}
		if (root_port) {
			IOServiceClose(root_port);
			root_port = 0;
		}
		IONotificationPortDestroy(notify_port);
		notify_port = NULL;
	}
}

#endif // OSX && PORTAUDIO
