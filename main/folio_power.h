#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "read_pico_init.h"

typedef enum {
    FOLIO_POWER_NONE,
    FOLIO_POWER_LOCK,
    FOLIO_POWER_OFF_REQUEST,
    FOLIO_POWER_SHUTDOWN_NOW,
} folio_power_event_t;

void folio_power_init(void);
folio_power_event_t folio_power_poll(void);
// No I/O: this boot's RTC seed or explicit sync plus sleep-inclusive esp_timer.
// Zero/false means no valid anchor, monotonic rollback, or outside 2024..2099 UTC.
// Poll retries an unset clock at most once/minute; a valid anchor is not replaced.
// After reboot, accuracy again depends on the PMU RTC until explicit clock sync.
uint32_t folio_power_unix_time(void);
// UI thread only. Read fresh PMU UTC for diagnostics; false clears *utc to zero.
// No runtime re-anchoring, event polling/ACK, retry scheduling or RTC write.
bool folio_power_rtc_time(uint32_t* utc);
// UI thread only; persist UTC, verify readback, then replace the runtime anchor.
// Failure preserves the old runtime anchor; PMU persistence may already have changed.
bool folio_power_set_time(uint32_t utc_seconds);
bool folio_power_datetime(struct tm* local, int timezone_minutes);
void folio_power_status(char* text, size_t capacity, int timezone_minutes);
// A PMU deadline is active: finish saving promptly, without waiting for the user.
bool folio_power_shutdown_pending(void);
// UI thread only, cached PMU status; no I2C, event polling or acknowledgement.
// Unknown/failed status, <20%, low/critical battery and pending shutdown refuse.
bool folio_power_update_safe(void);
// Online-update gate. Same verdict as folio_power_update_safe(), except that a
// status that cannot be read for a few seconds does not revoke an install that
// was already confirmed safe: no I2C on this path, so a worker holding flash or
// the bus can make one poll fail. A readable UNSAFE status still refuses at once.
bool folio_power_update_gate(void);
// True only when the PMU positively reports an unsafe state (or a shutdown is
// pending). An unreadable snapshot is not evidence of a low battery: during an
// OTA flash erase the cache is off long enough for several polls to fail, so
// mid-install decisions must not treat that as unsafe.
bool folio_power_unsafe_confirmed(void);
// Best-effort peripheral sleep, then SHUTDOWN_READY with the retained event ID.
// Call only after closing files/syncing the card; failed saves must not block this.
// A return means the host has not yet lost power; retry without resuming card I/O.
esp_err_t folio_power_finish_shutdown(read_pico_handle_t* hardware);
// Radio must be stopped and writes synced. Keeps RAM/files and returns ESP_OK on key wake.
// A pending PMU shutdown is returned to the caller, which must close files before READY.
esp_err_t folio_power_lock(read_pico_handle_t* hardware);
// Optional UI refresh while still locked. Called on minute changes with IOE awake.
// It must return promptly and must not count as reading activity.
esp_err_t folio_power_lock_refresh(read_pico_handle_t* hardware, void (*refresh)(void*), void* context);
// Real lock only, not previews/idle: monotonic time starts at this call (0 disables).
// Optional timed_out is cleared on entry; true only with ESP_OK after IOE restoration.
// PMU shutdown wins, then a simultaneous wake key, then the deadline. No power cut here.
// On timeout the caller must draw, save/close files and unmount before suspend(true).
esp_err_t folio_power_lock_timed(read_pico_handle_t* hardware, void (*refresh)(void*), void* context,
                                 uint16_t auto_off_minutes, bool* timed_out);
// Saves and SD unmount must finish before this call. Returns only if power-down fails.
esp_err_t folio_power_suspend(read_pico_handle_t* hardware, bool power_off);
