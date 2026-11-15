#include "folio_power.h"

#include <stdio.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include <sys/time.h>
#include "esp_private/sleep_cpu.h"
#endif

#include "epdiy.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "read_pico_pmu.h"

static uint32_t s_unix_time;
static int64_t s_time_anchor;
static int64_t s_next_clock_retry;
static int64_t s_start_time;
static bool s_available;
static bool s_key_down_wakes;
static bool s_status_valid;
static bool s_shutdown_pending;
static bool s_shutdown_prepared;
static bool s_shutdown_ready_sent;
static uint16_t s_shutdown_event_id;
// Last monotonic time the update gate saw a readable, safe battery. Only the
// gate uses it, and only to bridge an unreadable snapshot; see the header.
static int64_t s_update_safe_us;

#define UPDATE_GATE_GRACE_US INT64_C(3000000)

#define CLOCK_RETRY_US (60 * 1000000LL)
#define CLOCK_UTC_MIN UINT32_C(1704067200)
#define CLOCK_UTC_MAX UINT32_C(4102444799)

static bool read_clock_candidate(uint32_t* utc, int64_t* anchor) {
    // Require this TIME_GET's result, never an earlier successful snapshot.
    esp_err_t error = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    int64_t now = esp_timer_get_time();
    const pmu_snapshot_t* state = read_pico_pmu_get();
    if (error != ESP_OK || state == NULL || !state->time_ok || !state->time_synced ||
        state->last_op_code != PMU_CMD_TIME_GET || state->last_op_status != PMU_STATUS_OK ||
        state->unix_sec < CLOCK_UTC_MIN || state->unix_sec > CLOCK_UTC_MAX || now < 0) return false;
    *utc = state->unix_sec;
    *anchor = now;
    return true;
}

static void bootstrap_clock(void) {
    int64_t now = esp_timer_get_time();
    if (s_unix_time != 0 || now < 0 || now < s_next_clock_retry) return;
    uint32_t utc;
    int64_t anchor;
    if (read_clock_candidate(&utc, &anchor)) {
        // PMU seeds this boot once. Its slow RTC must not replace live holdover.
        s_unix_time = utc;
        s_time_anchor = anchor;
    }
    now = esp_timer_get_time();
    s_next_clock_retry = now >= 0 && now <= INT64_MAX - CLOCK_RETRY_US ? now + CLOCK_RETRY_US : INT64_MAX;
}

bool folio_power_rtc_time(uint32_t* utc) {
    if (utc == NULL) return false;
    *utc = 0;
    if (!s_available || s_shutdown_pending) return false;
    int64_t anchor;
    return read_clock_candidate(utc, &anchor);
}

void folio_power_init(void) {
    s_unix_time = 0;
    s_time_anchor = 0;
    s_next_clock_retry = INT64_MAX;
    s_status_valid = false;
    s_shutdown_pending = s_shutdown_prepared = s_shutdown_ready_sent = s_key_down_wakes = false;
    s_shutdown_event_id = 0;
    s_update_safe_us = 0;
    s_available = read_pico_pmu_ready();
    s_start_time = esp_timer_get_time();
    if (!s_available) return;
    // Boot events may include a low-battery shutdown. Never blindly drain them.
    if (folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW) return;
    (void)read_pico_pmu_report_ready();
    int calibrated_vcom;
    if (read_pico_pmu_vcom_get(&calibrated_vcom) == ESP_OK) epd_set_vcom(calibrated_vcom);
    s_next_clock_retry = 0;
    // The normal poll owns shutdown/FIFO priority before the first RTC sample.
    (void)folio_power_poll();
}

folio_power_event_t folio_power_poll(void) {
    if (!s_available) return FOLIO_POWER_NONE;
    if (s_shutdown_event_id != 0) return FOLIO_POWER_SHUTDOWN_NOW;
    folio_power_event_t event = FOLIO_POWER_NONE;
    for (unsigned count = 0; count < PMU_EVENT_FIFO_DEPTH; ++count) {
        if (read_pico_pmu_poll() != ESP_OK) {
            s_status_valid = false;
            break;
        }
        const pmu_snapshot_t* state = read_pico_pmu_get();
        s_status_valid = state != NULL && state->status_ok;
        if (s_status_valid && state->power_state == PMU_PWR_SHUTDOWN_PENDING) s_shutdown_pending = true;
        if (state == NULL || !state->pending_events) break;
        // The BSP may return ESP_OK for a failed PEEK. A non-empty but unreadable
        // FIFO cannot prove there is no wake key or shutdown behind this event.
        if (!state->event_ok || !state->event.event_id) {
            s_status_valid = false;
            break;
        }
        if (state->event.type == PMU_EVT_SHUTDOWN_REQUESTED) {
            s_shutdown_pending = true;
            s_shutdown_event_id = state->event.event_id;
            // PEEK is non-destructive. Retain the ID until files are closed and
            // SHUTDOWN_READY has been accepted, even during the boot quiet time.
            return FOLIO_POWER_SHUTDOWN_NOW;
        }
        // Ignore the release of the key that booted the board.
        bool boot_quiet = esp_timer_get_time() - s_start_time < 2 * 1000000LL;
        if (!boot_quiet && (state->event.type == PMU_EVT_KEY_SHORT || (s_key_down_wakes && state->event.type == PMU_EVT_KEY_DOWN)) && event == FOLIO_POWER_NONE) {
            event = FOLIO_POWER_LOCK;
        } else if (!boot_quiet && state->event.type == PMU_EVT_KEY_LONG) {
            event = FOLIO_POWER_OFF_REQUEST;
        }
        if (read_pico_pmu_event_ack(state->event.event_id) != ESP_OK) {
            s_status_valid = false;
            break;
        }
    }
    if (s_shutdown_pending) return FOLIO_POWER_SHUTDOWN_NOW;
    if (s_status_valid) bootstrap_clock();
    return event;
}

uint32_t folio_power_unix_time(void) {
    int64_t now = esp_timer_get_time();
    if (!s_available || s_unix_time == 0 || s_time_anchor < 0 || now < s_time_anchor) return 0;
    uint64_t value = (uint64_t)s_unix_time + (uint64_t)(now - s_time_anchor) / 1000000;
    return value <= CLOCK_UTC_MAX ? (uint32_t)value : 0;
}

static bool clock_write_allowed(void) {
    // Peek without acknowledging ordinary keys: the main loop still owns them.
    if(read_pico_pmu_poll()!=ESP_OK){s_status_valid=false;return false;}
    const pmu_snapshot_t* state=read_pico_pmu_get();
    s_status_valid=state && state->status_ok;
    if(!s_status_valid)return false;
    if(state->power_state==PMU_PWR_SHUTDOWN_PENDING)s_shutdown_pending=true;
    // An unreadable nonempty FIFO cannot rule out an urgent shutdown request.
    if(state->pending_events && (!state->event_ok || !state->event.event_id)) {
        s_status_valid=false;
        return false;
    }
    if(state->event_ok && state->pending_events && state->event.event_id &&
       state->event.type==PMU_EVT_SHUTDOWN_REQUESTED) {
        s_shutdown_pending=true;s_shutdown_event_id=state->event.event_id;
    }
    return !s_shutdown_pending;
}

bool folio_power_set_time(uint32_t utc_seconds) {
    if(!s_available || s_shutdown_pending || utc_seconds<CLOCK_UTC_MIN || utc_seconds>CLOCK_UTC_MAX)return false;
    if(!clock_write_allowed())return false;
    uint8_t payload[4];for(unsigned i=0;i<4;++i)payload[i]=(uint8_t)(utc_seconds>>(8*i));
    if(read_pico_pmu_cmd(PMU_CMD_TIME_SYNC,payload,sizeof(payload))!=ESP_OK)return false;
    if(!clock_write_allowed())return false;
    uint32_t actual;
    int64_t anchor;
    if(!read_clock_candidate(&actual,&anchor) || actual<utc_seconds || (uint64_t)actual-utc_seconds>5)return false;
    if(!clock_write_allowed())return false;
#ifdef ESP_PLATFORM
    struct timeval tv={.tv_sec=(time_t)actual};
    if(settimeofday(&tv,NULL)!=0)return false;
#endif
    // Commit only a verified result; failed writes/readback must preserve holdover.
    s_unix_time=actual;
    s_time_anchor=anchor;
    return true;
}

bool folio_power_datetime(struct tm* local, int timezone_minutes) {
    if (local == NULL) return false;
    *local = (struct tm){0};
    uint32_t utc = folio_power_unix_time();
    if (utc == 0 || timezone_minutes < -720 || timezone_minutes > 840) return false;
    int64_t seconds = (int64_t)utc + (int64_t)timezone_minutes * 60;
    time_t timestamp = (time_t)seconds;
    // A 32-bit time_t must fail instead of wrapping into 1901 after 2038.
    if ((int64_t)timestamp != seconds) return false;
    return gmtime_r(&timestamp, local) != NULL;
}

void folio_power_status(char* text, size_t capacity, int timezone_minutes) {
    if (text == NULL || capacity == 0) return;
    char clock_text[8] = "--:--";
    struct tm clock;
    if (folio_power_datetime(&clock, timezone_minutes)) {
        snprintf(clock_text, sizeof(clock_text), "%02d:%02d", clock.tm_hour, clock.tm_min);
    }
    const pmu_snapshot_t* state = s_available ? read_pico_pmu_get() : NULL;
    if (s_status_valid && state != NULL && state->status_ok &&
        (state->flags & PMU_STATUS_BATTERY_VALID) != 0 && state->soc_permille <= 1000) {
        snprintf(text, capacity, "%s  %u%%%s", clock_text, (unsigned)((state->soc_permille + 5) / 10),
                 state->charge_state == PMU_CHARGE_CHARGING ? "+" : "");
    } else {
        snprintf(text, capacity, "%s  --%%", clock_text);
    }
}

bool folio_power_shutdown_pending(void) { return s_shutdown_pending; }

bool folio_power_update_safe(void) {
    const pmu_snapshot_t* state = s_available ? read_pico_pmu_get() : NULL;
    return s_status_valid && !s_shutdown_pending && state && state->status_ok &&
        state->power_state != PMU_PWR_SHUTDOWN_PENDING &&
        (state->flags & PMU_STATUS_BATTERY_VALID) &&
        !(state->flags & (PMU_STATUS_BATTERY_LOW | PMU_STATUS_BATTERY_CRITICAL)) &&
        state->soc_permille >= 200 && state->soc_permille <= 1000;
}

bool folio_power_unsafe_confirmed(void) {
    if (s_shutdown_pending) return true;
    const pmu_snapshot_t* state = s_available ? read_pico_pmu_get() : NULL;
    if (!s_status_valid || !state || !state->status_ok) return false;
    return state->power_state == PMU_PWR_SHUTDOWN_PENDING ||
        !(state->flags & PMU_STATUS_BATTERY_VALID) ||
        (state->flags & (PMU_STATUS_BATTERY_LOW | PMU_STATUS_BATTERY_CRITICAL)) ||
        state->soc_permille < 200 || state->soc_permille > 1000;
}
bool folio_power_update_gate(void) {
    if (folio_power_update_safe()) {
        s_update_safe_us = esp_timer_get_time();
        return true;
    }
    // A readable status that is unsafe must refuse immediately and must also
    // cancel any bridge. Only an unreadable snapshot may ride out the next
    // poll: an OTA worker erasing flash can make one I2C poll fail without the
    // battery having changed.
    if (s_status_valid || !s_available) { s_update_safe_us = 0; return false; }
    int64_t now = esp_timer_get_time();
    return s_update_safe_us > 0 && now >= s_update_safe_us &&
        now - s_update_safe_us <= UPDATE_GATE_GRACE_US;
}

esp_err_t folio_power_finish_shutdown(read_pico_handle_t* hardware) {
    if (hardware == NULL || !s_available || !s_shutdown_pending) return ESP_ERR_INVALID_STATE;
    if (!s_shutdown_prepared) {
        s_shutdown_prepared = true;
        epd_poweroff();
        if (hardware->sensor_ready) (void)sc7a20h_power_down(hardware->sensor);
        if (hardware->touch_ready) (void)cst836u_set_mode(hardware->touch, CST836U_MODE_DEEPSLEEP);
        // Peripheral errors must not consume the PMU's ten-second deadline.
        (void)read_pico_ioe_sleep(true);
    }
    if (s_shutdown_event_id == 0) (void)folio_power_poll();
    if (s_shutdown_event_id == 0) return ESP_ERR_NOT_FOUND;
    if (!s_shutdown_ready_sent) {
        const uint8_t ready[] = {(uint8_t)s_shutdown_event_id, (uint8_t)(s_shutdown_event_id >> 8)};
        esp_err_t error = read_pico_pmu_cmd(PMU_CMD_SHUTDOWN_READY, ready, sizeof(ready));
        if (error != ESP_OK) return error;
        s_shutdown_ready_sent = true;
        // ACK only after readiness was accepted. ACK can fail if EN already fell.
        (void)read_pico_pmu_event_ack(s_shutdown_event_id);
    }
    // No HOST_READY/IOE wake rollback: it could cancel an in-flight power cut.
    // Successful commands should drop ESP_EN after about 100 ms; never infer
    // physical power-off solely from an accepted command response.
    vTaskDelay(pdMS_TO_TICKS(150));
    return ESP_ERR_TIMEOUT;
}

// Light sleep leaves the ESP/PSRAM powered, unlike PMU SOFT_SLEEP which drops EN.
esp_err_t folio_power_lock(read_pico_handle_t* hardware) {
    return folio_power_lock_timed(hardware,NULL,NULL,0,NULL);
}
esp_err_t folio_power_lock_refresh(read_pico_handle_t* hardware, void (*refresh)(void*), void* context) {
    return folio_power_lock_timed(hardware,refresh,context,0,NULL);
}
esp_err_t folio_power_lock_timed(read_pico_handle_t* hardware, void (*refresh)(void*), void* context,
                                 uint16_t auto_off_minutes, bool* timed_out) {
    if(timed_out)*timed_out=false;
    if(!hardware || !s_available)return ESP_ERR_INVALID_STATE;
    const int64_t started=esp_timer_get_time();
    const int64_t duration=(int64_t)auto_off_minutes*60*1000000;
    const int64_t deadline=started<=INT64_MAX-duration?started+duration:INT64_MAX;
    bool expired=false;
    if(folio_power_poll()==FOLIO_POWER_SHUTDOWN_NOW)return ESP_ERR_INVALID_STATE;
#ifdef ESP_PLATFORM
    static bool retention_ready;
    if(!retention_ready)retention_ready=sleep_cpu_configure(true)==ESP_OK;
#endif
    epd_poweroff();
    bool sensor_on=hardware->sensor_ready && sc7a20h_powered(hardware->sensor);
    esp_err_t error=sensor_on?sc7a20h_power_down(hardware->sensor):ESP_OK;
    if(error==ESP_OK && hardware->touch_ready)error=cst836u_set_mode(hardware->touch,CST836U_MODE_DEEPSLEEP);
    if(error==ESP_OK)error=read_pico_ioe_sleep(false); // Keep CW_INT as an input.
    gpio_config_t io={.pin_bit_mask=1ULL<<READ_PICO_IOE_INT_GPIO,.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    if(error==ESP_OK)error=gpio_config(&io);
    if(error==ESP_OK)error=gpio_wakeup_enable(READ_PICO_IOE_INT_GPIO,GPIO_INTR_LOW_LEVEL);
    if(error==ESP_OK)error=esp_sleep_enable_gpio_wakeup();
    int64_t released=0;
    int64_t refreshed_at=started;
    uint32_t displayed_minute=folio_power_unix_time()/60;
    unsigned short_wakes=0;
    while(error==ESP_OK) {
        // Clear IOE before polling, so a PMU event between poll and sleep stays armed.
        read_pico_clear_ioe_int();
        folio_power_event_t event=folio_power_poll();
        if(s_shutdown_pending){error=ESP_ERR_INVALID_STATE;break;}
        if(!s_status_valid){error=ESP_FAIL;break;}
        const pmu_snapshot_t* state=read_pico_pmu_get();
        if(s_key_down_wakes && (event==FOLIO_POWER_LOCK || event==FOLIO_POWER_OFF_REQUEST || (state->key_state & 1)))break;
        if(!s_key_down_wakes) {
            int64_t now=esp_timer_get_time();
            if(state->key_state & 1)released=0;
            else if(!released)released=now;
            if(released && now-released>=80000)s_key_down_wakes=true;
            if(now-started>3000000){error=ESP_ERR_TIMEOUT;break;}
            vTaskDelay(pdMS_TO_TICKS(20));continue;
        }
        uint32_t utc=folio_power_unix_time();
        int64_t now=esp_timer_get_time();
        // A queued PMU request or wake key was checked first, even at the deadline.
        if(auto_off_minutes && now>=deadline){expired=true;break;}
        if(refresh && (utc/60!=displayed_minute || (!utc && now-refreshed_at>=60000000))) {
            error=read_pico_ioe_wake();
            if(error!=ESP_OK)break;
            event=folio_power_poll();
            if(s_shutdown_pending){error=ESP_ERR_INVALID_STATE;break;}
            if(!s_status_valid){error=ESP_FAIL;break;}
            if(event==FOLIO_POWER_LOCK || event==FOLIO_POWER_OFF_REQUEST || (read_pico_pmu_get()->key_state & 1))break;
            if(auto_off_minutes && esp_timer_get_time()>=deadline){expired=true;break;}
            utc=folio_power_unix_time();
            refresh(context);
            epd_poweroff();
            // Rendering takes time; a key or PMU deadline may arrive meanwhile.
            // Never hide that event behind another 30-second timer sleep.
            event=folio_power_poll();
            if(s_shutdown_pending){error=ESP_ERR_INVALID_STATE;break;}
            if(!s_status_valid){error=ESP_FAIL;break;}
            if(event==FOLIO_POWER_LOCK || event==FOLIO_POWER_OFF_REQUEST || (read_pico_pmu_get()->key_state & 1))break;
            if(auto_off_minutes && esp_timer_get_time()>=deadline){expired=true;break;}
            error=read_pico_ioe_sleep(false);
            if(error!=ESP_OK)break;
            displayed_minute=utc/60;refreshed_at=esp_timer_get_time();
            continue;
        }
        uint64_t sleep_us=30000000; // PMU fallback, even without a UI refresh.
        if(refresh && utc) {
            uint64_t minute_us=(uint64_t)(60-utc%60)*1000000;
            if(minute_us<sleep_us)sleep_us=minute_us;
        }
        // Recalculate every sleep, including the time spent rendering and polling.
        int64_t before=esp_timer_get_time();
        if(auto_off_minutes) {
            if(before>=deadline)continue; // Recheck PMU/key priority before returning.
            uint64_t remaining=(uint64_t)(deadline-before);
            if(remaining<sleep_us)sleep_us=remaining;
        }
        error=esp_sleep_enable_timer_wakeup(sleep_us);
        if(error!=ESP_OK)break;
        error=esp_light_sleep_start();
        if(error==ESP_ERR_SLEEP_REJECT)error=ESP_OK; // A key may arrive immediately before entry.
        if(esp_timer_get_time()-before<20000)++short_wakes;else short_wakes=0;
        if(short_wakes>20){error=ESP_ERR_TIMEOUT;break;}
    }
    esp_err_t cleanup=gpio_wakeup_disable(READ_PICO_IOE_INT_GPIO);
    esp_err_t disabled=esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    if(cleanup==ESP_OK)cleanup=disabled;
    disabled=esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if(cleanup==ESP_OK)cleanup=disabled;
    if(error==ESP_OK)error=cleanup;
    // A failure must not revive peripherals during a new cut. Files remain caller-owned.
    folio_power_event_t final_event=folio_power_poll();
    if(s_shutdown_pending){s_key_down_wakes=false;return ESP_ERR_INVALID_STATE;}
    if(!s_status_valid && error==ESP_OK)error=ESP_FAIL;
    if(final_event==FOLIO_POWER_LOCK || final_event==FOLIO_POWER_OFF_REQUEST ||
       (s_status_valid && (read_pico_pmu_get()->key_state & 1)))expired=false;
    esp_err_t restored=read_pico_ioe_wake();
    final_event=folio_power_poll();
    if(s_shutdown_pending){s_key_down_wakes=false;return ESP_ERR_INVALID_STATE;}
    if(!s_status_valid && error==ESP_OK)error=ESP_FAIL;
    if(final_event==FOLIO_POWER_LOCK || final_event==FOLIO_POWER_OFF_REQUEST ||
       (s_status_valid && (read_pico_pmu_get()->key_state & 1)))expired=false;
    if(hardware->touch_ready && cst836u_wake(hardware->touch)!=ESP_OK)restored=ESP_FAIL;
    if(sensor_on && sc7a20h_power_up(hardware->sensor)!=ESP_OK)restored=ESP_FAIL;
    if(error==ESP_OK)error=restored;
    // esp_timer already includes light sleep; do not re-anchor from the PMU RTC.
    final_event=folio_power_poll();
    if(s_shutdown_pending)error=ESP_ERR_INVALID_STATE;
    else if(!s_status_valid && error==ESP_OK)error=ESP_FAIL;
    if(final_event==FOLIO_POWER_LOCK || final_event==FOLIO_POWER_OFF_REQUEST ||
       (s_status_valid && (read_pico_pmu_get()->key_state & 1)))expired=false;
    s_key_down_wakes=false;
    s_start_time=esp_timer_get_time(); // Ignore the wake press release, never an emergency.
    if(timed_out && error==ESP_OK)*timed_out=expired;
    return error;
}

esp_err_t folio_power_suspend(read_pico_handle_t* hardware, bool power_off) {
    if (hardware == NULL || !s_available) return ESP_ERR_INVALID_STATE;
    if (folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW) return folio_power_finish_shutdown(hardware);
    epd_poweroff();
    if (hardware->sensor_ready) (void)sc7a20h_power_down(hardware->sensor);
    esp_err_t error = ESP_OK;
    if (hardware->touch_ready) error = cst836u_set_mode(hardware->touch, CST836U_MODE_DEEPSLEEP);
    if (error == ESP_OK) error = read_pico_ioe_sleep(true);
    if (error == ESP_OK) {
        if (power_off) {
            const uint8_t request[] = {0, 1, 0};
            error = read_pico_pmu_cmd(PMU_CMD_HOST_REQUEST_OFF, request, sizeof(request));
            // Also inspect the queue after a lost response: REQUEST_OFF may have
            // reached the PMU and already started its deadline.
            for (unsigned i = 0; i < 50; ++i) {
                if (folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW) return folio_power_finish_shutdown(hardware);
                if (error != ESP_OK) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (error == ESP_OK) error = ESP_ERR_TIMEOUT;
        } else {
            error = read_pico_pmu_report_sleep();
        }
        if (error == ESP_OK) {
            // PMU controls ESP32 EN; it should reset/hold the host before this expires.
            vTaskDelay(pdMS_TO_TICKS(1500));
            error = ESP_ERR_TIMEOUT;
        }
    }
    if (folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW) return folio_power_finish_shutdown(hardware);
    (void)read_pico_ioe_wake();
    if (hardware->touch_ready) (void)cst836u_wake(hardware->touch);
    (void)read_pico_pmu_report_ready();
    // Preserve any shutdown that arrived while recovering from failed sleep.
    (void)folio_power_poll();
    return error;
}
