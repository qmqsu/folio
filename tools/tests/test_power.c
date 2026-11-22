#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "folio_power.h"
#include "read_pico_pmu.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#ifdef ESP_PLATFORM
#include <sys/time.h>
#ifndef FOLIO_POWER_TEST_TIME_MOCK
#error "ESP_PLATFORM power tests require the pure-memory settimeofday mock"
#endif
#endif

static pmu_snapshot_t state;
static pmu_event_t queue[PMU_EVENT_FIFO_DEPTH];
static unsigned queue_count, ack_count, ready_count, host_ready_count, request_count, clock_reads, poll_count;
static unsigned touch_count, ioe_count, sensor_count, wake_count, off_count;
static unsigned clock_writes;
static uint32_t last_clock_write;
static int clock_readback_delta;
static uint16_t ready_id;
static int64_t now, awake_us;
// Just past the production 3 s unreadable bridge; keep in step with folio_power.c.
#define UPDATE_GATE_TEST_GRACE_US INT64_C(3000001)
static int rtc_rate_ppm;
static bool shutdown_during_clock_read;
static uint16_t clock_response_status;
static bool available, fail_poll, fail_refresh, fail_clock, fail_command, fail_peripherals, shutdown_during_sleep;
static bool shutdown_ack_before_ready;
static bool fail_clock_write, ignore_clock_write, shutdown_during_sync;
static bool rtc_running, ioe_sleeping, fail_ioe_wake, fail_ioe_sleep, shutdown_on_ioe_wake;
static uint32_t rtc_epoch;
static int64_t rtc_anchor;
static unsigned refresh_count, refresh_scenario, key_after_light_calls, wake_fail_count;
static unsigned fail_clock_on_sleep, recover_clock_on_sleep;
static uint32_t refreshed_utc[512]; // Holds every minute refresh in the simulated 6-hour lock.
static uint64_t timer_us, timer_history[1024];
static int64_t expected_deadline, key_at, shutdown_at, fail_poll_at, refresh_cost;
static bool key_state_only, key_on_cleanup, shutdown_on_cleanup;
static unsigned clock_jump_count;
static unsigned fifo_fault; // 1=failed PEEK, 2=zero event ID, 3=failed ACK.
static unsigned sync_fifo_fault; // Inject unreadable PEEK or zero ID during TIME_SYNC.
static unsigned boot_fault_after_ready; // 1/2=urgent event/state, 3/4=unreadable FIFO.
static bool fail_event_ack;
enum { FAULT_NONE, FAULT_SENSOR_DOWN, FAULT_TOUCH_SLEEP, FAULT_GPIO_CONFIG,
       FAULT_GPIO_ENABLE, FAULT_SLEEP_GPIO, FAULT_TIMER, FAULT_GPIO_DISABLE,
       FAULT_DISABLE_GPIO, FAULT_DISABLE_TIMER, FAULT_TOUCH_WAKE, FAULT_SENSOR_UP };
static unsigned fault;
static unsigned timer_count, disable_timer_count, disable_gpio_count;
static read_pico_handle_t hardware = {.touch_ready = true, .sensor_ready = true};
#ifdef ESP_PLATFORM
static bool fail_system_time, check_system_time_order;
static unsigned system_time_calls;
static uint32_t expected_old_system_anchor;
static int64_t system_time_cost;
static struct timeval last_system_time;

int folio_power_test_settimeofday(const struct timeval* value, const void* zone) {
    assert(value != NULL && zone == NULL && value->tv_usec == 0);
    ++system_time_calls;
    last_system_time = *value;
    // The runtime anchor must still be the old one while libc is being called.
    if (check_system_time_order) assert(folio_power_unix_time() == expected_old_system_anchor);
    now += system_time_cost;
    awake_us += system_time_cost;
    return fail_system_time ? -1 : 0;
}
#endif

static void push(uint8_t type, uint16_t id) {
    assert(queue_count < PMU_EVENT_FIFO_DEPTH);
    queue[queue_count++] = (pmu_event_t){.type = type, .event_id = id};
}
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(unsigned ticks) { now += (int64_t)ticks * 1000; awake_us += (int64_t)ticks * 1000; }
void epd_set_vcom(int millivolts) { assert(millivolts == 1290); }
void epd_poweroff(void) { ++off_count; }
void read_pico_epd_settle(void) {}
esp_err_t sc7a20h_power_down(void* sensor) { (void)sensor; ++sensor_count; return fail_peripherals || fault==FAULT_SENSOR_DOWN ? ESP_FAIL : ESP_OK; }
esp_err_t cst836u_set_mode(void* touch, int mode) {
    (void)touch; assert(mode == CST836U_MODE_DEEPSLEEP); ++touch_count;
    return fail_peripherals || fault==FAULT_TOUCH_SLEEP ? ESP_FAIL : ESP_OK;
}
esp_err_t cst836u_wake(void* touch) { (void)touch; ++wake_count; return fault==FAULT_TOUCH_WAKE ? ESP_FAIL : ESP_OK; }
esp_err_t read_pico_ioe_sleep(bool rails_off) { (void)rails_off; ++ioe_count;ioe_sleeping=true;return fail_peripherals || fail_ioe_sleep ? ESP_FAIL : ESP_OK; }
esp_err_t read_pico_ioe_wake(void) {
    ++wake_count;
    if(shutdown_on_ioe_wake){push(PMU_EVT_SHUTDOWN_REQUESTED,907);state.power_state=PMU_PWR_SHUTDOWN_PENDING;shutdown_on_ioe_wake=false;}
    if(fail_ioe_wake){++wake_fail_count;return ESP_FAIL;}
    ioe_sleeping=false;return ESP_OK;
}
bool read_pico_pmu_ready(void) { return available; }
const pmu_snapshot_t* read_pico_pmu_get(void) { return &state; }
esp_err_t read_pico_pmu_poll(void) {
    ++poll_count;
    if (fail_poll) return ESP_FAIL;
    state.pending_events = queue_count;
    state.event = queue_count ? queue[0] : (pmu_event_t){0};
    return ESP_OK;
}
esp_err_t read_pico_pmu_refresh(void) { return fail_refresh ? ESP_FAIL : read_pico_pmu_poll(); }
esp_err_t read_pico_pmu_report_ready(void) {
    ++host_ready_count;
    if(boot_fault_after_ready==1)push(PMU_EVT_SHUTDOWN_REQUESTED,921);
    if(boot_fault_after_ready==1 || boot_fault_after_ready==2)state.power_state=PMU_PWR_SHUTDOWN_PENDING;
    if(boot_fault_after_ready==3 || boot_fault_after_ready==4) {
        push(PMU_EVT_KEY_SHORT,boot_fault_after_ready==4?0:922);
        state.event_ok=boot_fault_after_ready!=3;
    }
    return ESP_OK;
}
esp_err_t read_pico_pmu_report_sleep(void) {
    if (shutdown_during_sleep) {
        push(PMU_EVT_SHUTDOWN_REQUESTED, 777);
        state.power_state = PMU_PWR_SHUTDOWN_PENDING;
    }
    return ESP_FAIL;
}
esp_err_t read_pico_pmu_vcom_get(int* vcom) { *vcom = 1290; return ESP_OK; }
esp_err_t read_pico_pmu_event_ack(uint16_t id) {
    assert(queue_count > 0 && queue[0].event_id == id);
    if (fail_event_ack) return ESP_FAIL;
    if (queue[0].type == PMU_EVT_SHUTDOWN_REQUESTED && ready_count == 0) shutdown_ack_before_ready = true;
    ++ack_count;
    memmove(queue, queue + 1, (--queue_count) * sizeof(*queue));
    return read_pico_pmu_poll(); // Official ACK also refreshes the snapshot.
}
esp_err_t read_pico_pmu_cmd(uint16_t code, const uint8_t* payload, uint8_t length) {
    state.last_op_code=code;state.last_op_status=PMU_STATUS_OK;
    if (code == PMU_CMD_TIME_GET) {
        assert(length == 0 && payload == NULL); ++clock_reads;
        state.last_op_status=clock_response_status;
        if(rtc_running && !fail_clock && clock_response_status==PMU_STATUS_OK) {
            int64_t elapsed=now-rtc_anchor;
            state.unix_sec=rtc_epoch+(uint32_t)((elapsed+elapsed*rtc_rate_ppm/1000000)/1000000);
        }
        if(shutdown_during_clock_read){push(PMU_EVT_SHUTDOWN_REQUESTED,915);state.power_state=PMU_PWR_SHUTDOWN_PENDING;shutdown_during_clock_read=false;}
        // Keep the old snapshot on read failure, matching the official BSP.
        return fail_clock ? ESP_FAIL : ESP_OK;
    } else if(code==PMU_CMD_TIME_SYNC) {
        assert(length==4 && payload);++clock_writes;
        last_clock_write=(uint32_t)payload[0]|((uint32_t)payload[1]<<8)|((uint32_t)payload[2]<<16)|((uint32_t)payload[3]<<24);
        if(fail_clock_write)return ESP_FAIL;
        if(!ignore_clock_write){
            state.unix_sec=(uint32_t)((int64_t)last_clock_write+clock_readback_delta);state.time_synced=true;state.time_ok=true;
            if(rtc_running){rtc_epoch=state.unix_sec;rtc_anchor=now;}
        }
        if(shutdown_during_sync){push(PMU_EVT_SHUTDOWN_REQUESTED,901);state.power_state=PMU_PWR_SHUTDOWN_PENDING;}
        if(sync_fifo_fault){push(PMU_EVT_KEY_SHORT,sync_fifo_fault==2?0:918);state.event_ok=sync_fifo_fault!=1;}
        return ESP_OK;
    } else if (code == PMU_CMD_HOST_REQUEST_OFF) {
        assert(length == 3 && payload[0] == 0); ++request_count;
        push(PMU_EVT_SHUTDOWN_REQUESTED, 0x1234);
        state.power_state = PMU_PWR_SHUTDOWN_PENDING;
    } else {
        assert(code == PMU_CMD_SHUTDOWN_READY && length == 2);
        ready_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        if (!fail_command) ++ready_count;
    }
    return fail_command ? ESP_FAIL : ESP_OK;
}
static unsigned light_calls;
static int light_scenario;
bool sc7a20h_powered(void* s){(void)s;return true;}
esp_err_t sc7a20h_power_up(void* s){(void)s;++wake_count;return fault==FAULT_SENSOR_UP ? ESP_FAIL : ESP_OK;}
void read_pico_clear_ioe_int(void){}
esp_err_t gpio_config(const gpio_config_t* c){assert(c->pin_bit_mask==(1ULL<<41));return fault==FAULT_GPIO_CONFIG ? ESP_FAIL : ESP_OK;}
esp_err_t gpio_wakeup_enable(int p,int l){assert(p==41 && l==GPIO_INTR_LOW_LEVEL);return fault==FAULT_GPIO_ENABLE ? ESP_FAIL : ESP_OK;}
esp_err_t gpio_wakeup_disable(int p){assert(p==41);return fault==FAULT_GPIO_DISABLE ? ESP_FAIL : ESP_OK;}
esp_err_t esp_sleep_enable_gpio_wakeup(void){return fault==FAULT_SLEEP_GPIO ? ESP_FAIL : ESP_OK;}
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t u){
    assert(u>0&&u<=30000000);timer_us=u;
    assert(timer_count<sizeof(timer_history)/sizeof(timer_history[0]));timer_history[timer_count++]=u;
    return fault==FAULT_TIMER ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_sleep_disable_wakeup_source(int s){
    if(s==ESP_SLEEP_WAKEUP_TIMER){
        ++disable_timer_count;
        if(key_on_cleanup)push(PMU_EVT_KEY_DOWN,910);
        if(shutdown_on_cleanup){push(PMU_EVT_SHUTDOWN_REQUESTED,911);state.power_state=PMU_PWR_SHUTDOWN_PENDING;}
        if(fault==FAULT_DISABLE_TIMER)return ESP_FAIL;
    }else if(s==ESP_SLEEP_WAKEUP_GPIO){
        ++disable_gpio_count;if(fault==FAULT_DISABLE_GPIO)return ESP_FAIL;
    }else assert(false);
    return ESP_OK;
}
esp_err_t esp_light_sleep_start(void){
    ++light_calls;assert(ioe_sleeping);
    if(expected_deadline)assert(now<expected_deadline && timer_us<=(uint64_t)(expected_deadline-now));
    if(light_scenario==4)return ESP_ERR_SLEEP_REJECT; // Repeated rejected entry must not become a timeout.
    now+=(int64_t)timer_us;
    if(key_at && now>=key_at){
        if(key_state_only)state.key_state=1;else push(PMU_EVT_KEY_DOWN,912);
        key_at=0;
    }
    if(fifo_fault && expected_deadline && now>=expected_deadline){
        // Match the real BSP: STATUS/poll succeed, but FIFO PEEK or ACK can fail.
        push(PMU_EVT_BATTERY_SAMPLE_READY,fifo_fault==2?0:914);
        if(fifo_fault==1)state.event_ok=false;
        if(fifo_fault==3)fail_event_ack=true;
        fifo_fault=0;
    }
    if(shutdown_at && now>=shutdown_at){push(PMU_EVT_SHUTDOWN_REQUESTED,913);state.power_state=PMU_PWR_SHUTDOWN_PENDING;shutdown_at=0;}
    if(fail_poll_at && now>=fail_poll_at)fail_poll=true;
    if(light_calls==fail_clock_on_sleep)fail_clock=true;
    if(light_calls==recover_clock_on_sleep)fail_clock=false;
    if(light_scenario==2)return ESP_FAIL;
    if(light_scenario==3){push(PMU_EVT_KEY_DOWN,891);return ESP_ERR_SLEEP_REJECT;}
    if(light_scenario==1){push(PMU_EVT_SHUTDOWN_REQUESTED,888);state.power_state=PMU_PWR_SHUTDOWN_PENDING;}
    else if(light_calls==key_after_light_calls)push(PMU_EVT_KEY_DOWN,889); // Timer wake alone must stay locked.
    return ESP_OK;
}
static void reset(void) {
    light_calls=0;light_scenario=0;
    state = (pmu_snapshot_t){.status_ok = true, .event_ok = true, .power_state = PMU_PWR_RUNNING,
        .flags = PMU_STATUS_BATTERY_VALID, .soc_permille = 825, .charge_state = PMU_CHARGE_CHARGING};
    available = true; now = awake_us = 0; rtc_rate_ppm=0;
    shutdown_during_clock_read=false;clock_response_status=PMU_STATUS_OK;
    queue_count = ack_count = ready_count = host_ready_count = request_count = clock_reads = poll_count = 0;
    touch_count = ioe_count = sensor_count = wake_count = off_count = 0; ready_id = 0;
    fail_poll = fail_refresh = fail_clock = fail_command = fail_peripherals = shutdown_during_sleep = shutdown_ack_before_ready = false;
    clock_writes=0;last_clock_write=0;clock_readback_delta=0;
    fail_clock_write=ignore_clock_write=shutdown_during_sync=false;
    rtc_running=ioe_sleeping=fail_ioe_wake=fail_ioe_sleep=shutdown_on_ioe_wake=false;
    rtc_epoch=0;rtc_anchor=0;refresh_count=refresh_scenario=wake_fail_count=0;key_after_light_calls=2;
    fail_clock_on_sleep=recover_clock_on_sleep=0;memset(refreshed_utc,0,sizeof(refreshed_utc));
    timer_us=0;timer_count=disable_timer_count=disable_gpio_count=0;
    expected_deadline=key_at=shutdown_at=fail_poll_at=0;refresh_cost=1200000;
    key_state_only=key_on_cleanup=shutdown_on_cleanup=false;fault=FAULT_NONE;clock_jump_count=0;
    fifo_fault=sync_fifo_fault=boot_fault_after_ready=0;fail_event_ack=false;
#ifdef ESP_PLATFORM
    fail_system_time=check_system_time_order=false;
    system_time_calls=expected_old_system_anchor=0;system_time_cost=0;
    last_system_time=(struct timeval){0};
#endif
}

static void init_running(void) { reset(); folio_power_init(); now += 3000000; }

static void lock_refresh(void* context) {
    assert(context==&refresh_count && !ioe_sleeping && !folio_power_shutdown_pending());
    assert(refresh_count<sizeof(refreshed_utc)/sizeof(refreshed_utc[0]));refreshed_utc[refresh_count]=folio_power_unix_time();
    ++refresh_count;
    if(refresh_scenario==1){push(PMU_EVT_SHUTDOWN_REQUESTED,906);state.power_state=PMU_PWR_SHUTDOWN_PENDING;}
    if(refresh_scenario==2)push(PMU_EVT_KEY_DOWN,908);
    if(refresh_scenario==3)fail_ioe_sleep=true;
    if(refresh_scenario==4)fail_poll=true;
    if((refresh_scenario==5 || refresh_scenario==6) && !clock_jump_count){
        assert(folio_power_set_time(refresh_scenario==5 ? 4102444000u : 1704067200u));++clock_jump_count;
    }
    now+=refresh_cost;awake_us+=refresh_cost; // Rendering time counts toward the same monotonic deadline.
}
static void calibrated_lock(void) {
    init_running();
    assert(folio_power_set_time(1704067258)); // 00:00:58 UTC.
    rtc_running=true;rtc_epoch=1704067258;rtc_anchor=now;
}
static void test_lock_refresh(void) {
    calibrated_lock();key_after_light_calls=4;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK);
    assert(refresh_count==2 && light_calls==4 && timer_history[0]==2000000);
    assert(wake_count==5 && !ioe_sleeping && disable_timer_count==1 && disable_gpio_count==1);
    assert(!request_count && !ready_count); // Timer wakes never shut down or unlock.

    // Unknown clock still updates battery/state once a minute, not every poll.
    init_running();key_after_light_calls=3;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK);
    assert(refresh_count==1 && light_calls==3 && timer_count==3);

    // An unavailable RTC cannot revoke an established runtime clock while locked.
    calibrated_lock();key_after_light_calls=4;fail_clock_on_sleep=3;
    unsigned reads=clock_reads;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK);
    assert(refresh_count==2 && refreshed_utc[0]!=0 && refreshed_utc[1]>refreshed_utc[0]);
    assert(folio_power_unix_time()!=0 && clock_reads==reads);

    // A key arriving at the same minute boundary beats the refresh.
    calibrated_lock();key_after_light_calls=1;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK && refresh_count==0);
    // A shutdown arriving at the same boundary is returned before drawing.
    calibrated_lock();light_scenario=1;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_ERR_INVALID_STATE);
    assert(refresh_count==0 && wake_count==0 && !ready_count && queue_count==1);
    // Shutdown arriving during IOE wake must also skip the drawing callback.
    calibrated_lock();shutdown_on_ioe_wake=true;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_ERR_INVALID_STATE);
    assert(refresh_count==0 && wake_count==1 && light_calls==1 && !ready_count && queue_count==1);

    // Events during rendering are serviced before any further light sleep.
    calibrated_lock();refresh_scenario=1;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_ERR_INVALID_STATE);
    assert(refresh_count==1 && light_calls==1 && wake_count==1 && !ready_count && queue_count==1);
    assert(folio_power_finish_shutdown(&hardware)==ESP_ERR_TIMEOUT && ready_id==906 && !shutdown_ack_before_ready);
    calibrated_lock();refresh_scenario=2;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK);
    assert(refresh_count==1 && light_calls==1 && wake_count==4 && !ioe_sleeping);

    calibrated_lock();fail_ioe_wake=true;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_FAIL);
    assert(refresh_count==0 && light_calls==1 && wake_fail_count==2);
    calibrated_lock();refresh_scenario=3;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_FAIL);
    assert(refresh_count==1 && light_calls==1 && !ioe_sleeping);
    calibrated_lock();refresh_scenario=4;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_FAIL);
    assert(refresh_count==1 && light_calls==1 && !ioe_sleeping);
}

static void start_timed_lock(uint16_t minutes, bool clocked) {
    if(clocked)calibrated_lock();else init_running();
    key_after_light_calls=0;
    expected_deadline=now+(int64_t)minutes*60*1000000;
}

static void test_timed_lock(void) {
    const uint16_t durations[]={30,180,360};
    bool timed_out=true;
    // Pure-memory esp_timer/light-sleep mocks advance time; this is not a real 6h wait.
    // No process-global/boot idle countdown: a fresh duration starts at lock entry.
    init_running();now+=12LL*60*60*1000000;key_after_light_calls=0;
    expected_deadline=now+30LL*60*1000000;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,30,&timed_out)==ESP_OK && timed_out);
    assert(now==expected_deadline && !ioe_sleeping && !request_count && !ready_count);
    for(unsigned i=0;i<sizeof(durations)/sizeof(durations[0]);++i) {
        for(unsigned clocked=0;clocked<2;++clocked) {
            start_timed_lock(durations[i],clocked!=0);timed_out=false;
            assert(folio_power_lock_timed(&hardware,clocked?lock_refresh:NULL,&refresh_count,durations[i],&timed_out)==ESP_OK);
            assert(timed_out && now==expected_deadline && !ioe_sleeping && !folio_power_shutdown_pending());
            assert(!request_count && !ready_count && disable_timer_count==1 && disable_gpio_count==1);
            assert(clocked ? refresh_count>0 : refresh_count==0);
            assert(timer_history[timer_count-1]<30000000); // Last sleep clips to the deadline.
        }
    }
    // 0 disables it altogether, even past the largest selectable duration.
    init_running();key_after_light_calls=750;int64_t entered=now;timed_out=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,0,&timed_out)==ESP_OK && !timed_out);
    assert(now-entered>360LL*60*1000000 && !request_count && !ready_count);
    for(unsigned i=0;i<sizeof(durations)/sizeof(durations[0]);++i) {
        uint16_t minutes=durations[i];
        // Both key event and raw key-down status win exactly at every deadline.
        for(unsigned raw_key=0;raw_key<2;++raw_key) {
            start_timed_lock(minutes,false);key_at=expected_deadline;key_state_only=raw_key!=0;timed_out=true;
            assert(folio_power_lock_timed(&hardware,NULL,NULL,minutes,&timed_out)==ESP_OK && !timed_out);
            assert(now==expected_deadline && !ioe_sleeping && !request_count && !ready_count);
        }
        // Wake before the deadline, then re-enter: no residual time carries over.
        start_timed_lock(minutes,false);key_after_light_calls=1;
        assert(folio_power_lock_timed(&hardware,NULL,NULL,minutes,&timed_out)==ESP_OK && !timed_out);
        assert(now<expected_deadline);
        key_after_light_calls=0;expected_deadline=now+(int64_t)minutes*60*1000000;
        // Reset the observation buffer, not the production power state or mock time.
        timer_count=0;
        assert(folio_power_lock_timed(&hardware,NULL,NULL,minutes,&timed_out)==ESP_OK && timed_out && now==expected_deadline);
        assert(!request_count && !ready_count);
        // Emergency beats auto-off and a simultaneous key; files are still open.
        start_timed_lock(minutes,false);key_at=shutdown_at=expected_deadline;
        assert(folio_power_lock_timed(&hardware,NULL,NULL,minutes,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out);
        assert(now==expected_deadline && folio_power_shutdown_pending());
        assert(queue_count==1 && !ready_count && !request_count && !wake_count);
    }
    // The last cleanup and IOE restoration can also receive an event.
    start_timed_lock(5,false);key_on_cleanup=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_OK && !timed_out);
    start_timed_lock(5,false);shutdown_on_cleanup=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out && !wake_count);
    start_timed_lock(5,false);shutdown_on_ioe_wake=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out && wake_count==1);

    // A slow redraw consumes the original budget; never sleep again after it expires.
    start_timed_lock(5,true);refresh_cost=300000000;
    assert(folio_power_lock_timed(&hardware,lock_refresh,&refresh_count,5,&timed_out)==ESP_OK && timed_out);
    assert(refresh_count==1 && light_calls==1 && now>expected_deadline && !ioe_sleeping);
    start_timed_lock(5,true);refresh_cost=300000000;refresh_scenario=2;
    assert(folio_power_lock_timed(&hardware,lock_refresh,&refresh_count,5,&timed_out)==ESP_OK && !timed_out && light_calls==1);
    start_timed_lock(5,true);refresh_cost=300000000;refresh_scenario=1;
    assert(folio_power_lock_timed(&hardware,lock_refresh,&refresh_count,5,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out && light_calls==1);
    // Forward/backward RTC synchronization does not alter the monotonic deadline.
    for(unsigned scenario=5;scenario<=6;++scenario) {
        start_timed_lock(5,true);refresh_scenario=scenario;
        assert(folio_power_lock_timed(&hardware,lock_refresh,&refresh_count,5,&timed_out)==ESP_OK && timed_out);
        assert(now==expected_deadline && clock_jump_count==1);
    }
    start_timed_lock(5,true);fail_clock_on_sleep=2;
    assert(folio_power_lock_timed(&hardware,lock_refresh,&refresh_count,5,&timed_out)==ESP_OK && timed_out && now==expected_deadline);

    // A successful STATUS/poll does not imply that the FIFO was readable/ACKed.
    // Inject each partial failure at the deadline, not merely during setup.
    for(unsigned f=1;f<=3;++f) {
        start_timed_lock(5,false);fifo_fault=f;timed_out=true;
        assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_FAIL && !timed_out);
        assert(now==expected_deadline && queue_count==1 && !ack_count && !ioe_sleeping);
        assert(!ready_count && !request_count && disable_timer_count==1 && disable_gpio_count==1);
        // Even an unreadable event in front of it cannot hide pending shutdown STATUS.
        start_timed_lock(5,false);fifo_fault=f;shutdown_at=expected_deadline;
        assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out);
        assert(folio_power_shutdown_pending() && queue_count==2 && !ack_count && !wake_count);
        assert(!ready_count && !request_count && disable_timer_count==1 && disable_gpio_count==1);
    }
    // Empty FIFO with event_ok=false is normal in the real BSP, not a PEEK failure.
    start_timed_lock(5,false);state.event_ok=false;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_OK && timed_out);
    assert(now==expected_deadline && !ioe_sleeping && !ready_count && !request_count);

    // Every setup/cleanup error leaves timed_out false and attempts both source cleanups.
    for(unsigned f=FAULT_SENSOR_DOWN;f<=FAULT_SENSOR_UP;++f) {
        start_timed_lock(5,false);fault=f;timed_out=true;
        assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_FAIL && !timed_out);
        assert(disable_timer_count==1 && disable_gpio_count==1 && !request_count && !ready_count);
    }
    start_timed_lock(5,false);fail_ioe_sleep=true;timed_out=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_FAIL && !timed_out);
    start_timed_lock(5,false);fail_ioe_wake=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_FAIL && !timed_out);
    start_timed_lock(5,false);fail_poll_at=expected_deadline;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_FAIL && !timed_out);
    start_timed_lock(5,false);light_scenario=2;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_FAIL && !timed_out);
    start_timed_lock(5,false);light_scenario=4;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_ERR_TIMEOUT && !timed_out && light_calls==21);
    assert(disable_timer_count==1 && disable_gpio_count==1);
    start_timed_lock(5,false);state.key_state=1;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_ERR_TIMEOUT && !timed_out && !light_calls);
    init_running();timed_out=true;
    assert(folio_power_lock_timed(NULL,NULL,NULL,5,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out && !timer_count);
    reset();available=false;folio_power_init();timed_out=true;
    assert(folio_power_lock_timed(&hardware,NULL,NULL,5,&timed_out)==ESP_ERR_INVALID_STATE && !timed_out);
}

static void test_timed_lock_large_integers(void) {
    const struct { uint16_t minutes; int64_t microseconds; } durations[] = {
        {30, INT64_C(1800000000)}, {180, INT64_C(10800000000)}, {360, INT64_C(21600000000)}
    };
    bool timed_out;
    for (unsigned i = 0; i < sizeof(durations)/sizeof(durations[0]); ++i) {
        const int64_t duration = durations[i].microseconds;
        assert((int64_t)durations[i].minutes * 60 * 1000000 == duration);
        // Cross 32-bit microseconds, then exercise full-duration addition near INT64_MAX.
        const int64_t starts[] = {INT64_C(4294967295) - 90000000, INT64_MAX - duration - 1000000};
        for (unsigned j = 0; j < sizeof(starts)/sizeof(starts[0]); ++j) {
            init_running(); now = starts[j]; key_after_light_calls = 0;
            expected_deadline = now + duration;
            assert(folio_power_lock_timed(&hardware, NULL, NULL, durations[i].minutes, &timed_out) == ESP_OK);
            assert(timed_out && now == expected_deadline && now - starts[j] == duration);
            assert(!ioe_sleeping && !request_count && !ready_count);
            assert(timer_count > 0 && disable_timer_count == 1 && disable_gpio_count == 1);
        }
        // The existing overflow guard saturates the deadline instead of wrapping.
        init_running(); now = INT64_MAX - duration / 2; key_after_light_calls = 0;
        expected_deadline = INT64_MAX;
        assert(folio_power_lock_timed(&hardware, NULL, NULL, durations[i].minutes, &timed_out) == ESP_OK);
        assert(timed_out && now == INT64_MAX && !ioe_sleeping && !request_count && !ready_count);
        assert(disable_timer_count == 1 && disable_gpio_count == 1);
    }
    puts("Timed lock integer simulation: 30/180/360 minute conversion, 32-bit crossing, near-INT64_MAX and saturated deadlines pass");
}

static void test_set_time(void) {
    init_running();
    assert(!folio_power_set_time(0) && !folio_power_set_time(1704067199u) && !folio_power_set_time(4102444800u));
    assert(clock_writes==0);
    assert(folio_power_set_time(1704067200u) && last_clock_write==1704067200u && folio_power_unix_time()==1704067200u);
    assert(folio_power_set_time(4102444799u) && last_clock_write==4102444799u && folio_power_unix_time()==4102444799u);
    unsigned reads=clock_reads;
    fail_clock_write=true;assert(!folio_power_set_time(1793102400u) && clock_reads==reads);fail_clock_write=false;
    fail_clock=true;assert(!folio_power_set_time(1793102400u) && folio_power_unix_time()==4102444799u);fail_clock=false;
    ignore_clock_write=true;state.time_synced=false;assert(!folio_power_set_time(1793102400u));ignore_clock_write=false;
    clock_readback_delta=5;assert(folio_power_set_time(1793102400u));
    clock_readback_delta=6;assert(!folio_power_set_time(1793102400u));
    clock_readback_delta=-1;assert(!folio_power_set_time(1793102400u));clock_readback_delta=0;
    unsigned writes=clock_writes;fail_poll=true;assert(!folio_power_set_time(1793102400u) && clock_writes==writes);fail_poll=false;
    state.status_ok=false;assert(!folio_power_set_time(1793102400u) && clock_writes==writes);state.status_ok=true;
    // Time sync must not consume a user's queued short-press event.
    push(PMU_EVT_KEY_SHORT,902);unsigned acknowledgements=ack_count;
    assert(folio_power_set_time(1793102400u) && ack_count==acknowledgements && queue_count==1);
    assert(folio_power_poll()==FOLIO_POWER_LOCK && queue_count==0);
    // A newly arrived shutdown, not yet observed by the UI, wins before SYNC.
    init_running();push(PMU_EVT_SHUTDOWN_REQUESTED,903);state.power_state=PMU_PWR_SHUTDOWN_PENDING;
    writes=clock_writes;reads=clock_reads;
    assert(!folio_power_set_time(1793102400u) && clock_writes==writes && clock_reads==reads);
    assert(folio_power_shutdown_pending() && ack_count==0 && folio_power_poll()==FOLIO_POWER_SHUTDOWN_NOW);
    // A shutdown while SYNC completes must skip the follow-up TIME_GET.
    init_running();shutdown_during_sync=true;reads=clock_reads;
    assert(!folio_power_set_time(1793102400u) && clock_writes==1 && clock_reads==reads);
    assert(folio_power_shutdown_pending() && queue_count==1 && ack_count==0);
    reset();available=false;folio_power_init();assert(!folio_power_set_time(1793102400u) && !clock_writes);
}

static void test_clock(void) {
    struct tm local;
    char status[64];
    reset(); folio_power_init();
    assert(clock_reads == 1 && folio_power_unix_time() == 0);
    assert(!folio_power_datetime(&local, 480));
    folio_power_status(status, sizeof(status), 480); assert(strncmp(status, "--:--", 5) == 0);

    reset(); state.time_synced = true; state.time_ok = true;
    state.unix_sec = 1767196799u; // 2025-12-31 15:59:59 UTC.
    folio_power_init();
    assert(folio_power_unix_time() == 1767196799u && clock_reads == 1);
    assert(folio_power_datetime(&local, 480));
    assert(local.tm_year == 125 && local.tm_mon == 11 && local.tm_mday == 31 && local.tm_hour == 23);
    now += 2000000;
    assert(folio_power_datetime(&local, 480));
    assert(local.tm_year == 126 && local.tm_mon == 0 && local.tm_mday == 1 && local.tm_hour == 0 && local.tm_sec == 1);
    assert(clock_reads == 1); // Accessors only use the monotonic anchor.
    assert(!folio_power_datetime(NULL, 480));
    assert(!folio_power_datetime(&local, 1000000000));

    now = 59000000; folio_power_poll(); assert(clock_reads == 1);
    now = 60000000; state.unix_sec = 1767225600u; folio_power_poll();
    assert(clock_reads == 1 && folio_power_unix_time() == 1767196859u);
    // PMU invalidation and failures do not erase the successful bootstrap.
    now = 120000000; state.time_synced = false; folio_power_poll();
    assert(clock_reads == 1 && folio_power_unix_time() == 1767196919u);
    now = 180000000; state.time_synced = true; fail_clock = true; folio_power_poll();
    assert(clock_reads == 1 && folio_power_unix_time() == 1767196979u);
    now = 240000000; fail_clock = false;
    assert(folio_power_set_time(2208988800u));
    if (sizeof(time_t) >= 8) {
        assert(folio_power_datetime(&local, 0) && local.tm_year == 140 && local.tm_mon == 0 && local.tm_mday == 1);
    } else assert(!folio_power_datetime(&local, 0));

    now = 300000000;assert(folio_power_set_time(4102444799u));
    now += 2000000; assert(folio_power_unix_time() == 0 && !folio_power_datetime(&local, 0));
    now = 299000000; assert(folio_power_unix_time() == 0); // Monotonic rollback invalidates interpolation.
    unsigned reads=clock_reads;folio_power_poll();assert(folio_power_unix_time()==0 && clock_reads==reads);
    assert(folio_power_set_time(1767225600u) && folio_power_unix_time()==1767225600u);

    // A due bootstrap retry never takes priority over a PMU shutdown deadline.
    reset();folio_power_init();now=60000000;
    unsigned before = clock_reads; push(PMU_EVT_SHUTDOWN_REQUESTED, 900);
    assert(folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW && clock_reads == before && ack_count == 0);
}

static void test_clock_init_priority(void) {
    unsigned failures=0;
    for(unsigned kind=1;kind<=4;++kind) {
        reset();state.time_synced=true;state.time_ok=true;state.unix_sec=1794144000u;
        boot_fault_after_ready=kind;folio_power_init();
        if(clock_reads || folio_power_unix_time()!=0) {
            fprintf(stderr,"After HOST_READY fault=%u: TIME_GET count=%u runtime=%u (expected no read/anchor)\n",kind,clock_reads,folio_power_unix_time());
            ++failures;
        }
        assert(host_ready_count==1 && clock_writes==0 && ack_count==0);
        assert(queue_count==(kind==2?0u:1u));
        assert(folio_power_shutdown_pending()==(kind<=2));
#ifdef ESP_PLATFORM
        assert(system_time_calls==0);
#endif
    }
    assert(failures==0);
    puts("Clock init priority: after-HOST_READY urgent event/state and unreadable FIFO skip TIME_GET/bootstrap without ACK");
}

static void test_clock_fifo_guard(void) {
    const uint32_t epoch=1794144000u;
    for(unsigned kind=1;kind<=2;++kind) {
        init_running();assert(folio_power_set_time(epoch));now+=1500000;
        push(PMU_EVT_KEY_SHORT,kind==2?0:918);state.event_ok=kind!=1;
        unsigned writes=clock_writes,reads=clock_reads,acks=ack_count;
        assert(!folio_power_set_time(epoch+3600));
        assert(clock_writes==writes && clock_reads==reads && ack_count==acks && queue_count==1);
        assert(folio_power_unix_time()==epoch+1 && !folio_power_shutdown_pending());

        init_running();assert(folio_power_set_time(epoch));now+=1500000;sync_fifo_fault=kind;
        writes=clock_writes;reads=clock_reads;acks=ack_count;
        assert(!folio_power_set_time(epoch+3600));
        assert(clock_writes==writes+1 && clock_reads==reads && ack_count==acks && queue_count==1);
        assert(folio_power_unix_time()==epoch+1 && !folio_power_shutdown_pending());
    }
    init_running();push(PMU_EVT_KEY_SHORT,0);state.event_ok=false;state.power_state=PMU_PWR_SHUTDOWN_PENDING;
    assert(!folio_power_set_time(epoch) && !clock_writes && !ack_count && folio_power_shutdown_pending());
    puts("Clock FIFO guard: unreadable PEEK/zero ID before or during SYNC blocks writes/commit without ACK; urgent state wins");
}

static void test_clock_drift(void) {
    const int rates[]={-20000,-10000,0,10000,20000};
    const uint32_t epoch=1794144000u;
    for(unsigned explicit_sync=0;explicit_sync<2;++explicit_sync) {
        for(unsigned r=0;r<sizeof(rates)/sizeof(rates[0]);++r) {
            reset();state.time_synced=true;state.time_ok=true;state.unix_sec=epoch;
            rtc_running=true;rtc_epoch=epoch;rtc_rate_ppm=rates[r];folio_power_init();
            if(explicit_sync)assert(folio_power_set_time(epoch));
            unsigned reads=clock_reads,writes=clock_writes;
            for(unsigned second=60;second<=86400;second+=60) {
                now=(int64_t)second*1000000;folio_power_poll();
                uint32_t actual=folio_power_unix_time();
                if(actual!=epoch+second)fprintf(stderr,"RTC rate=%d ppm at %us: runtime=%u expected=%u\n",rates[r],second,actual,epoch+second);
                assert(actual==epoch+second);
            }
            assert(clock_reads==reads && clock_writes==writes);
            uint32_t raw;
            assert(folio_power_rtc_time(&raw));
            assert(raw==epoch+(uint32_t)(86400LL*(1000000+rates[r])/1000000));
            assert(folio_power_unix_time()==epoch+86400 && clock_writes==writes);
        }
    }
    puts("Clock drift: bootstrap/explicit sync hold over for 24h at RTC -2/-1/0/+1/+2 percent");
}

static void test_clock_transaction(void) {
    const uint32_t epoch=1794144000u;
    init_running();assert(folio_power_set_time(epoch));
    now+=1500000;
    fail_clock_write=true;assert(!folio_power_set_time(epoch+3600));
    assert(folio_power_unix_time()==epoch+1);fail_clock_write=false;
    fail_clock=true;assert(!folio_power_set_time(epoch+3600));
    assert(folio_power_unix_time()==epoch+1);fail_clock=false;
    clock_readback_delta=6;assert(!folio_power_set_time(epoch+3600));
    assert(folio_power_unix_time()==epoch+1);
    clock_readback_delta=-1;assert(!folio_power_set_time(epoch+3600));
    assert(folio_power_unix_time()==epoch+1);clock_readback_delta=0;
    ignore_clock_write=true;state.time_synced=false;assert(!folio_power_set_time(epoch+3600));
    assert(folio_power_unix_time()==epoch+1);ignore_clock_write=false;
    now+=500000;assert(folio_power_unix_time()==epoch+2); // Failed writes retain fractional anchor age.
    assert(folio_power_set_time(epoch+3600));now+=60000000;folio_power_poll();
    assert(folio_power_unix_time()==epoch+3660);
    assert(folio_power_set_time(epoch-3600));now+=60000000;folio_power_poll();
    assert(folio_power_unix_time()==epoch-3540);
    shutdown_during_clock_read=true;uint32_t before=folio_power_unix_time();
    assert(!folio_power_set_time(epoch));assert(folio_power_unix_time()==before);
    assert(folio_power_shutdown_pending() && queue_count==1 && ack_count==0);
    puts("Clock transaction: failed persistence/readback preserves anchor; explicit forward/backward sync and shutdown priority pass");
}

static void test_clock_bootstrap(void) {
    const uint32_t epoch=1794144000u;
    reset();folio_power_init();assert(clock_reads==1 && folio_power_unix_time()==0);
    state.time_synced=true;state.time_ok=true;state.unix_sec=epoch;
    now=59999999;folio_power_poll();assert(clock_reads==1 && folio_power_unix_time()==0);
    now=60000000;fail_clock=true;folio_power_poll();assert(clock_reads==2 && folio_power_unix_time()==0);
    fail_clock=false;now=119999999;folio_power_poll();assert(clock_reads==2);
    now=120000000;folio_power_poll();assert(clock_reads==3 && folio_power_unix_time()==epoch);
    now=180000000;state.time_synced=false;fail_clock=true;folio_power_poll();
    assert(clock_reads==3 && folio_power_unix_time()==epoch+60);
    // Valid flags do not make an unsupported or absent epoch acceptable.
    const uint32_t invalid[]={0,1704067199u,4102444800u,UINT32_MAX};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        reset();state.time_synced=true;state.time_ok=true;state.unix_sec=invalid[i];folio_power_init();
        assert(folio_power_unix_time()==0 && clock_reads==1);
    }
    for(unsigned flags=0;flags<3;++flags) {
        reset();state.time_ok=(flags&1)!=0;state.time_synced=(flags&2)!=0;state.unix_sec=epoch;
        folio_power_init();assert(folio_power_unix_time()==0);
    }
    reset();state.time_synced=true;state.time_ok=true;state.unix_sec=epoch;
    clock_response_status=PMU_STATUS_ACCEPTED;folio_power_init();assert(folio_power_unix_time()==0);
    puts("Clock bootstrap: bounded retry, failed/stale/invalid samples, valid recovery and no post-bootstrap revocation pass");
}

static void test_clock_diagnostic(void) {
    const uint32_t epoch=1794144000u;
    init_running();assert(folio_power_set_time(epoch));now+=121500000;
    state.unix_sec=epoch-1000;push(PMU_EVT_KEY_SHORT,916);
    unsigned polls=poll_count,acks=ack_count,reads=clock_reads,writes=clock_writes;
    uint32_t raw=123;
    assert(!folio_power_rtc_time(NULL) && clock_reads==reads);
    assert(folio_power_rtc_time(&raw) && raw==epoch-1000 && clock_reads==reads+1);
    assert(folio_power_unix_time()==epoch+121 && poll_count==polls && ack_count==acks && queue_count==1 && clock_writes==writes);
    fail_clock=true;raw=123;assert(!folio_power_rtc_time(&raw) && raw==0);fail_clock=false;
    clock_response_status=PMU_STATUS_ACCEPTED;raw=123;assert(!folio_power_rtc_time(&raw) && raw==0);
    clock_response_status=PMU_STATUS_OK;
    state.time_ok=false;raw=123;assert(!folio_power_rtc_time(&raw) && raw==0);state.time_ok=true;
    state.time_synced=false;raw=123;assert(!folio_power_rtc_time(&raw) && raw==0);state.time_synced=true;
    const uint32_t invalid[]={0,1704067199u,4102444800u,UINT32_MAX};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){state.unix_sec=invalid[i];raw=123;assert(!folio_power_rtc_time(&raw) && raw==0);}
    assert(folio_power_unix_time()==epoch+121 && poll_count==polls && ack_count==acks && queue_count==1 && clock_writes==writes);
    now+=500000;assert(folio_power_unix_time()==epoch+122);
    assert(folio_power_poll()==FOLIO_POWER_LOCK && queue_count==0); // Diagnostic reads did not eat the key.
    push(PMU_EVT_SHUTDOWN_REQUESTED,917);assert(folio_power_poll()==FOLIO_POWER_SHUTDOWN_NOW);
    reads=clock_reads;polls=poll_count;raw=123;
    assert(!folio_power_rtc_time(&raw) && raw==0 && clock_reads==reads && poll_count==polls);

    // A diagnostic sample never seeds an unset clock or postpones its retry.
    reset();folio_power_init();state.time_synced=true;state.time_ok=true;state.unix_sec=epoch;
    now=1000000;assert(folio_power_rtc_time(&raw) && raw==epoch && folio_power_unix_time()==0);
    reads=clock_reads;now=59999999;folio_power_poll();assert(clock_reads==reads && folio_power_unix_time()==0);
    now=60000000;folio_power_poll();assert(clock_reads==reads+1 && folio_power_unix_time()==epoch);
    reset();available=false;folio_power_init();raw=123;
    assert(!folio_power_rtc_time(&raw) && raw==0 && clock_reads==0 && poll_count==0);
    puts("Clock diagnostic: fresh/raw-only reads, zeroed failures, no anchor/retry/event/write side effects pass");
}

static void test_clock_wake(void) {
    for(unsigned mode=0;mode<3;++mode) {
        calibrated_lock();rtc_rate_ppm=mode==0?-20000:20000;key_after_light_calls=0;
        if(mode==2)fail_clock_on_sleep=2;
        int64_t start=now,awake_start=awake_us;uint32_t epoch=folio_power_unix_time();
        key_at=start+360000000;
        unsigned reads=clock_reads,writes=clock_writes;
        assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK);
        assert(now-start>=300000000 && awake_us-awake_start<20000000);
        uint32_t expected=epoch+(uint32_t)((now-start)/1000000);
        if(folio_power_unix_time()!=expected)fprintf(stderr,"Wake runtime=%u expected=%u, RTC rate=%d ppm\n",folio_power_unix_time(),expected,rtc_rate_ppm);
        assert(folio_power_unix_time()==expected && clock_reads==reads && clock_writes==writes);
        assert(refresh_count>0 && !ioe_sleeping && !request_count && !ready_count);
    }
    // No initial clock: a successful bounded retry may seed it while locked.
    reset();rtc_running=true;rtc_epoch=1794144000u;state.time_synced=true;state.time_ok=true;
    fail_clock=true;folio_power_init();assert(folio_power_unix_time()==0 && clock_reads==1);
    now=3000000;key_after_light_calls=0;key_at=now+360000000;recover_clock_on_sleep=1;
    assert(folio_power_lock_refresh(&hardware,lock_refresh,&refresh_count)==ESP_OK);
    assert(clock_reads==2 && folio_power_unix_time()==rtc_epoch+(uint32_t)(now/1000000));
    assert(refresh_count>0 && !ioe_sleeping && !clock_writes);
    puts("Clock wake: slow/fast/unreadable PMU holdover and unset-clock recovery across light sleep pass");
}

#ifdef ESP_PLATFORM
static void test_platform_system_time(void) {
    const uint32_t epoch=1794144000u;
    init_running();assert(system_time_calls==0);
    assert(folio_power_set_time(epoch) && system_time_calls==1);
    assert(last_system_time.tv_sec==(time_t)epoch && last_system_time.tv_usec==0);

    now+=1500000;check_system_time_order=true;expected_old_system_anchor=epoch+1;
    fail_system_time=true;system_time_cost=250000;
    assert(!folio_power_set_time(epoch+3600) && system_time_calls==2);
    assert(last_system_time.tv_sec==(time_t)(epoch+3600));
    assert(folio_power_unix_time()==epoch+1);
    now+=250000;assert(folio_power_unix_time()==epoch+2); // Original fractional age survived libc failure.

    fail_system_time=false;expected_old_system_anchor=epoch+2;clock_readback_delta=3;
    assert(folio_power_set_time(epoch+7200) && system_time_calls==3);
    assert(last_system_time.tv_sec==(time_t)(epoch+7203) && last_system_time.tv_usec==0);
    assert(folio_power_unix_time()==epoch+7203); // Only committed after the mock returned successfully.
    now+=750000;assert(folio_power_unix_time()==epoch+7204); // Includes time spent in the mock.
    check_system_time_order=false;

    // Every pre-libc failure/urgent boundary must leave the system clock alone.
    for(unsigned mode=0;mode<7;++mode) {
        init_running();assert(folio_power_set_time(epoch));now+=1500000;
        unsigned calls=system_time_calls;
        if(mode==0)fail_clock_write=true;
        else if(mode==1)fail_clock=true;
        else if(mode==2)clock_readback_delta=6;
        else if(mode==3){push(PMU_EVT_SHUTDOWN_REQUESTED,920);state.power_state=PMU_PWR_SHUTDOWN_PENDING;}
        else if(mode==4)shutdown_during_sync=true;
        else if(mode==5)shutdown_during_clock_read=true;
        else {push(PMU_EVT_KEY_SHORT,0);state.event_ok=false;}
        assert(!folio_power_set_time(epoch+3600));
        assert(system_time_calls==calls && folio_power_unix_time()==epoch+1 && ack_count==0);
        now+=500000;assert(folio_power_unix_time()==epoch+2);
        if(mode>=3 && mode<=5)assert(folio_power_shutdown_pending());
    }
    // Failure without a prior anchor must not publish the candidate either.
    init_running();fail_system_time=true;check_system_time_order=true;expected_old_system_anchor=0;
    assert(!folio_power_set_time(epoch) && system_time_calls==1 && folio_power_unix_time()==0);
    puts("ESP_PLATFORM settimeofday mock: UTC argument, failure/fractional holdover, commit-after-success and no libc write on early/urgent failures passed");
}
#endif

static void test_update_power_gate(void) {
    init_running();unsigned polls=poll_count,acks=ack_count,reads=clock_reads;
    assert(folio_power_update_safe());
    state.soc_permille=199;assert(!folio_power_update_safe());
    state.soc_permille=200;assert(folio_power_update_safe());
    state.soc_permille=1001;assert(!folio_power_update_safe());
    state.soc_permille=1000;assert(folio_power_update_safe());
    state.flags|=PMU_STATUS_BATTERY_LOW;assert(!folio_power_update_safe());
    state.flags=PMU_STATUS_BATTERY_VALID|PMU_STATUS_BATTERY_CRITICAL;assert(!folio_power_update_safe());
    state.flags=0;assert(!folio_power_update_safe());
    state.flags=PMU_STATUS_BATTERY_VALID;state.status_ok=false;assert(!folio_power_update_safe());
    state.status_ok=true;assert(folio_power_update_safe());
    assert(poll_count==polls && ack_count==acks && clock_reads==reads); // Getter does no I/O.
    fail_poll=true;(void)folio_power_poll();assert(!folio_power_update_safe());
    fail_poll=false;(void)folio_power_poll();assert(folio_power_update_safe());
    state.power_state=PMU_PWR_SHUTDOWN_PENDING;assert(!folio_power_update_safe());
    (void)folio_power_poll();state.power_state=0;assert(!folio_power_update_safe());
    reset();available=false;folio_power_init();assert(!folio_power_update_safe());

    // The online gate bridges an unreadable poll but never a readable refusal.
    init_running();
    assert(folio_power_update_gate());
    fail_poll=true;(void)folio_power_poll();
    assert(!folio_power_update_safe());assert(folio_power_update_gate());
    now+=UPDATE_GATE_TEST_GRACE_US;(void)folio_power_poll();
    assert(!folio_power_update_gate());                       // Bridge expired, still unreadable.
    fail_poll=false;(void)folio_power_poll();
    assert(folio_power_update_gate());                        // Next good poll restores it.
    state.soc_permille=150;assert(!folio_power_update_gate()); // Readable unsafe refuses.
    fail_poll=true;(void)folio_power_poll();
    assert(!folio_power_update_gate());                       // No bridge after an unsafe reading.
    fail_poll=false;(void)folio_power_poll();state.soc_permille=1000;
    assert(folio_power_update_gate());
    // Mid-install decisions: only a readable unsafe report may stop the write.
    init_running();
    assert(!folio_power_unsafe_confirmed());
    fail_poll=true;(void)folio_power_poll();
    assert(!folio_power_unsafe_confirmed());          // Unreadable is not unsafe.
    now+=INT64_C(30000000);(void)folio_power_poll();
    assert(!folio_power_unsafe_confirmed());          // Still unreadable, still not unsafe.
    fail_poll=false;(void)folio_power_poll();
    assert(!folio_power_unsafe_confirmed());
    state.flags=PMU_STATUS_BATTERY_VALID|PMU_STATUS_BATTERY_LOW;
    assert(folio_power_unsafe_confirmed());           // A readable low battery is.
    state.flags=PMU_STATUS_BATTERY_VALID;state.soc_permille=150;
    assert(folio_power_unsafe_confirmed());
    state.soc_permille=1000;assert(!folio_power_unsafe_confirmed());
    state.flags=PMU_STATUS_BATTERY_VALID|PMU_STATUS_BATTERY_CRITICAL;
    assert(folio_power_unsafe_confirmed());
    state.flags=PMU_STATUS_BATTERY_VALID;state.power_state=PMU_PWR_SHUTDOWN_PENDING;
    assert(folio_power_unsafe_confirmed());
    state.power_state=0;(void)folio_power_poll();state.power_state=0;
    reset();available=false;folio_power_init();assert(!folio_power_update_gate());
    puts("Cached update power gate: valid >=20%, low/critical/unknown/failed-poll/pending refusal, 3 s unreadable bridge, readable-refusal precedence and no I/O passed");
}
int main(void) {
    const char* clock_case=getenv("FOLIO_POWER_CLOCK_CASE");
    if(clock_case) {
        if(!strcmp(clock_case,"drift"))test_clock_drift();
        else if(!strcmp(clock_case,"transaction"))test_clock_transaction();
        else if(!strcmp(clock_case,"bootstrap"))test_clock_bootstrap();
        else if(!strcmp(clock_case,"wake"))test_clock_wake();
        else if(!strcmp(clock_case,"diagnostic"))test_clock_diagnostic();
        else if(!strcmp(clock_case,"fifo"))test_clock_fifo_guard();
        else if(!strcmp(clock_case,"init"))test_clock_init_priority();
        else assert(!"Unknown FOLIO_POWER_CLOCK_CASE");
        return 0;
    }
#ifdef ESP_PLATFORM
    test_platform_system_time();
#endif
    test_clock_drift();test_clock_transaction();test_clock_bootstrap();test_clock_wake();test_clock_diagnostic();test_clock_fifo_guard();test_clock_init_priority();
    test_clock();
    test_set_time();
    test_update_power_gate();
    test_lock_refresh();
    test_timed_lock();
    test_timed_lock_large_integers();
    init_running();unsigned r=host_ready_count;
    assert(folio_power_lock(&hardware)==ESP_OK && light_calls==2 && !request_count && !ready_count);
    assert(host_ready_count==r && wake_count==3);
    push(PMU_EVT_KEY_SHORT,890);assert(folio_power_poll()==FOLIO_POWER_NONE); // Wake release cannot relock.
    init_running();light_scenario=1;assert(folio_power_lock(&hardware)==ESP_ERR_INVALID_STATE);
    assert(folio_power_shutdown_pending() && !ready_count && !ack_count && !wake_count); // Files still open.
    assert(folio_power_finish_shutdown(&hardware)==ESP_ERR_TIMEOUT && ready_id==888);
    init_running();light_scenario=2;assert(folio_power_lock(&hardware)==ESP_FAIL && wake_count==3);
    init_running();light_scenario=3;assert(folio_power_lock(&hardware)==ESP_OK && light_calls==1);

    char status[64];
    reset(); folio_power_init();
    folio_power_status(status, sizeof(status), 480); assert(strstr(status, "83%+") != NULL);
    state.flags = 0; folio_power_status(status, sizeof(status), 480); assert(strstr(status, "--%") != NULL);
    state.flags = PMU_STATUS_BATTERY_VALID; state.status_ok = false; folio_power_poll();
    folio_power_status(status, sizeof(status), 480); assert(strstr(status, "--%") != NULL);
    state.status_ok = true; fail_poll = true; folio_power_poll();
    folio_power_status(status, sizeof(status), 480); assert(strstr(status, "--%") != NULL);

    // Boot-key release is ignored; an emergency in the same boot FIFO is retained.
    reset(); push(PMU_EVT_KEY_SHORT, 1); push(PMU_EVT_SHUTDOWN_REQUESTED, 2); folio_power_init();
    assert(folio_power_shutdown_pending() && ack_count == 1 && queue_count == 1 && host_ready_count == 0);
    assert(folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW && ack_count == 1);
    fail_peripherals = true; fail_command = true;
    assert(folio_power_finish_shutdown(&hardware) == ESP_FAIL && ack_count == 1 && ready_id == 2);
    assert(touch_count == 1 && ioe_count == 1 && sensor_count == 1 && off_count == 1);
    fail_command = false;
    assert(folio_power_finish_shutdown(&hardware) == ESP_ERR_TIMEOUT);
    assert(ready_count == 1 && ack_count == 2 && !shutdown_ack_before_ready && request_count == 0 && wake_count == 0);
    folio_power_finish_shutdown(&hardware); assert(ready_count == 1 && touch_count == 1);

    init_running(); push(PMU_EVT_KEY_SHORT, 10); assert(folio_power_poll() == FOLIO_POWER_LOCK);
    push(PMU_EVT_KEY_SHORT, 11); push(PMU_EVT_KEY_LONG, 12); assert(folio_power_poll() == FOLIO_POWER_OFF_REQUEST);
    push(PMU_EVT_KEY_LONG, 13); push(PMU_EVT_SHUTDOWN_REQUESTED, 14);
    assert(folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW && queue_count == 1);

    // A valid pending STATUS is urgent even if the FIFO is temporarily unreadable.
    init_running(); state.power_state = PMU_PWR_SHUTDOWN_PENDING; state.event_ok = false;
    assert(folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW);
    assert(folio_power_finish_shutdown(&hardware) == ESP_ERR_NOT_FOUND && ready_count == 0);
    state.event_ok = true; push(PMU_EVT_SHUTDOWN_REQUESTED, 31);
    folio_power_finish_shutdown(&hardware); assert(ready_id == 31 && ready_count == 1);

    // Voluntary power-off uses the same ID and READY-before-ACK path.
    init_running(); assert(folio_power_suspend(&hardware, true) == ESP_ERR_TIMEOUT);
    assert(request_count == 1 && ready_count == 1 && ready_id == 0x1234 && !shutdown_ack_before_ready && wake_count == 0);
    // A lost REQUEST_OFF response must still discover the PMU's active deadline.
    init_running(); fail_command = true; assert(folio_power_suspend(&hardware, true) == ESP_FAIL);
    assert(folio_power_shutdown_pending() && request_count == 1 && ack_count == 0 && wake_count == 0);

    // Sleep failure recovery cannot drain or cancel a newly arrived shutdown.
    init_running(); shutdown_during_sleep = true; unsigned initial_ready = host_ready_count;
    assert(folio_power_suspend(&hardware, false) == ESP_ERR_TIMEOUT);
    assert(ready_id == 777 && host_ready_count == initial_ready && wake_count == 0 && !shutdown_ack_before_ready);
    init_running(); assert(folio_power_suspend(&hardware, false) == ESP_FAIL);
    assert(wake_count == 2 && !folio_power_shutdown_pending());
    reset(); available = false; folio_power_init();
    assert(folio_power_poll() == FOLIO_POWER_NONE && folio_power_suspend(&hardware, false) == ESP_ERR_INVALID_STATE);
    puts("PMU priority, simulated timed lock 30/180/360 and disabled beyond 6h, wake cancellation/re-entry, key/deadline/refresh races, FIFO PEEK/zero-ID/ACK failures, cleanup, RTC/TIME_SYNC, READY/ACK and sleep recovery passed (mock time, not a real 6h wait)");
    return 0;
}
