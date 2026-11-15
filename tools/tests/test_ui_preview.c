// Render the production native UI and reader into a host grayscale framebuffer.
// Hardware, persistence settings and EPUB preparation are mocked; reader,
// activity persistence, WeRead cache/mapping, PNG/JPEG cover decoding, glyph
// rendering, navigation and hit-testing are real. Network/service responses are
// simulated; test artwork and original text never enter the firmware binary.
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "../../main/book/folio_epub.h"
#include "../../main/book/folio_weread_cache.h"
#include "../../main/book/folio_font.h"
#include "../../main/book/folio_reader.h"
static folio_font_t* preview_font_open(const char* path, int size);
static folio_font_t* preview_font_open_builtin(int size);
static folio_font_t* preview_font_open_first_on_card(int size, char* path, uint32_t cap);
static folio_reader_t* preview_reader_open(const folio_book_t* book, folio_font_t* font, const folio_reader_options_t* options);
static bool preview_reader_save(folio_reader_t* reader);
static bool preview_reader_seek(folio_reader_t* reader, uint32_t anchor);
#include "../../main/book/folio_wallpaper.h"
#include "../../main/book/folio_cover.h"
#include "../../main/folio_storage.h"
static bool preview_wallpaper_decode(const char* path, unsigned width, unsigned height,
    folio_wallpaper_image_t* image, char* error, size_t cap);
static bool preview_wallpaper_cache_load(uint8_t** pixels, unsigned* width, unsigned* height);
static bool preview_wallpaper_draw(const folio_wallpaper_image_t* image, uint8_t* target, EpdRect rect);
static bool preview_legacy_wallpaper_draw(const char* path, uint8_t* target, EpdRect rect);
#include "esp_app_desc.h"
static esp_app_desc_t mock_app_description = {.version=FOLIO_TEST_APP_VERSION};
const esp_app_desc_t* esp_app_get_description(void) { return &mock_app_description; }
// The clock implementation is linked separately on hardware.
bool folio_power_rtc_time(uint32_t* utc);
#define CONFIG_FOLIO_USB_DEBUG 1
#define app_main folio_firmware_main
// Inject failures only at the application boundary; real font loading, resource
// validation, rasterization, reader reflow and file persistence stay linked.
#define folio_font_open preview_font_open
#define folio_font_open_builtin preview_font_open_builtin
#define folio_font_open_first_on_card preview_font_open_first_on_card
#define folio_reader_open_with_options preview_reader_open
#define folio_reader_save preview_reader_save
#define folio_reader_seek preview_reader_seek
#define folio_wallpaper_decode_file preview_wallpaper_decode
#define folio_storage_wallpaper_cache_load preview_wallpaper_cache_load
#define folio_wallpaper_draw_image preview_wallpaper_draw
#define folio_cover_draw_image preview_legacy_wallpaper_draw
#include "../../main/app_main.c"
#undef folio_cover_draw_image
#undef folio_wallpaper_draw_image
#undef folio_storage_wallpaper_cache_load
#undef folio_wallpaper_decode_file
#undef folio_reader_seek
#undef folio_reader_save
#undef folio_reader_open_with_options
#undef folio_font_open_first_on_card
#undef folio_font_open_builtin
#undef folio_font_open
#undef app_main

#define SCREEN_W 684
#define SCREEN_H 1216
static uint8_t screen[SCREEN_W * SCREEN_H];
static unsigned pixels_clipped, frames, settings_writes;
static uint32_t chapter_offsets[3];
static char session_path[256];
static bool fail_settings_write;
static folio_settings_t persisted_settings;
static bool fail_all_fonts, assert_single_font, fail_font_reader_save;
static const char* fail_font_paths[2];
static unsigned font_open_attempts, external_font_attempts, fail_reader_opens, reader_open_attempts, fail_reader_seeks;
static folio_font_t* preview_font_open(const char* path, int size) {
    ++font_open_attempts;
    if (path && !folio_font_is_builtin_path(path)) ++external_font_attempts;
    if (assert_single_font) assert(s_font == NULL);
    if (fail_all_fonts) return NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (path && fail_font_paths[i] && !strcmp(path, fail_font_paths[i])) return NULL;
    return folio_font_open(path, size);
}
static folio_font_t* preview_font_open_builtin(int size) { return preview_font_open(FOLIO_FONT_BUILTIN_PATH, size); }
static folio_font_t* preview_font_open_first_on_card(int size, char* path, uint32_t cap) {
    if (assert_single_font) assert(s_font == NULL);
    if (fail_all_fonts) return NULL;
    return folio_font_open_first_on_card(size, path, cap);
}
static folio_reader_t* preview_reader_open(const folio_book_t* book, folio_font_t* font, const folio_reader_options_t* options) {
    ++reader_open_attempts;
    if (fail_reader_opens) { --fail_reader_opens; return NULL; }
    return folio_reader_open_with_options(book, font, options);
}
static bool preview_reader_save(folio_reader_t* reader) {
    return !fail_font_reader_save && folio_reader_save(reader);
}
static bool preview_reader_seek(folio_reader_t* reader, uint32_t anchor) {
    if (fail_reader_seeks) { --fail_reader_seeks; return false; }
    return folio_reader_seek(reader, anchor);
}
// Observe production calls, never replace decode/cache bytes or draw results.
// PMU timing below is host fault injection, not an actual interrupt/TF test.
static unsigned wallpaper_decodes, wallpaper_cache_reads, wallpaper_cache_hits, legacy_wallpaper_draws;
static bool inside_wallpaper_cache, inside_wallpaper_draw, mock_shutdown_pending;
static unsigned shutdown_cache_checks, shutdown_draw_pixels, shutdown_injections, frames_at_shutdown;
static char last_legacy_wallpaper[256];
static void inject_wallpaper_shutdown(void) {
    assert(!mock_shutdown_pending);
    mock_shutdown_pending = true; ++shutdown_injections; frames_at_shutdown = frames;
}
static bool preview_wallpaper_decode(const char* path, unsigned width, unsigned height,
    folio_wallpaper_image_t* image, char* error, size_t cap) {
    ++wallpaper_decodes;
    return folio_wallpaper_decode_file(path, width, height, image, error, cap);
}
static bool preview_wallpaper_cache_load(uint8_t** pixels, unsigned* width, unsigned* height) {
    assert(!inside_wallpaper_cache); inside_wallpaper_cache = true; ++wallpaper_cache_reads;
    bool okay = folio_storage_wallpaper_cache_load(pixels, width, height);
    inside_wallpaper_cache = false;
    if (okay) ++wallpaper_cache_hits;
    return okay;
}
static bool preview_wallpaper_draw(const folio_wallpaper_image_t* image, uint8_t* target, EpdRect rect) {
    assert(!inside_wallpaper_draw); inside_wallpaper_draw = true;
    bool okay = folio_wallpaper_draw_image(image, target, rect);
    inside_wallpaper_draw = false;
    return okay;
}
static bool preview_legacy_wallpaper_draw(const char* path, uint8_t* target, EpdRect rect) {
    ++legacy_wallpaper_draws;
    struct stat info; assert(stat(path, &info) == 0 && info.st_size <= 2 * 1024 * 1024);
    snprintf(last_legacy_wallpaper, sizeof(last_legacy_wallpaper), "%s", path);
    return folio_cover_draw_image(path, target, rect);
}
static bool fail_card_sync;
static bool mock_card_unformatted, mock_card_unmounted;
static unsigned card_remount_calls, card_probe_reads;
static bool fail_transfer_stop;
static folio_transfer_status_t mock_transfer;
static folio_weread_status_t mock_wr;
static folio_network_status_t mock_network;
static bool mock_wifi_saved, mock_network_delay_connect;
static bool mock_online_worker, mock_wr_cancelled;
static folio_online_snapshot_t mock_online;
static folio_online_hooks_t mock_online_hooks;
static unsigned online_checks, online_downloads, online_installs, online_cancels;
static char mock_online_requested_channel[16];
static bool mock_online_cancel, mock_online_start_fail;
// UI simulation only: these mocks perform no network, signature or OTA work.
bool folio_online_update_init(const folio_online_hooks_t* hooks) {
    assert(!mock_online.busy);mock_online_hooks=*hooks;memset(&mock_online,0,sizeof(mock_online));return true;
}
bool folio_online_update_start_check(const char* channel,char* error,size_t cap) {
    snprintf(mock_online_requested_channel,sizeof(mock_online_requested_channel),"%s",channel?channel:"");
    // Match the real folio_remote_manifest_url contract; beta is not an alias.
    if(!channel || (strcmp(channel,"stable") && strcmp(channel,"preview"))) {
        if(error && cap)snprintf(error,cap,"更新通道无效。");
        return false;
    }
    if(error && cap)*error=0;
    assert(!mock_online.busy);++online_checks;
    memset(&mock_online,0,sizeof(mock_online));mock_online_cancel=false;
    mock_online.busy=true;mock_online.state=FOLIO_ONLINE_CHECKING;
    snprintf(mock_online.channel,sizeof(mock_online.channel),"%s",channel);return true;
}
bool folio_online_update_start_download(char* error,size_t cap) {
    (void)error;(void)cap;assert(!mock_online.busy && mock_online.state==FOLIO_ONLINE_AVAILABLE);
    ++online_downloads;mock_online_cancel=false;mock_online.busy=true;mock_online.state=FOLIO_ONLINE_DOWNLOADING;return true;
}
bool folio_online_update_start_install(char* error,size_t cap) {
    assert(!s_reader && !s_activity && !s_catalog && !mock_online.busy);
    assert(!strcmp(s_active_font,FOLIO_FONT_BUILTIN_PATH));
    assert(mock_online.state==FOLIO_ONLINE_READY);
    assert(mock_online_hooks.install_allowed(NULL,error,cap));
    ++online_installs;mock_online_cancel=false;
    if(mock_online_start_fail){snprintf(error,cap,"模拟任务启动失败");return false;}
    mock_online.busy=true;mock_online.state=FOLIO_ONLINE_INSTALLING;return true;
}
void folio_online_update_cancel(void){++online_cancels;mock_online_cancel=true;}
bool folio_online_update_busy(void){return mock_online.busy;}
bool folio_online_update_is_worker(void){return mock_online_worker;}
bool folio_online_update_cancel_requested(void){return mock_online_cancel || (mock_online_hooks.cancelled && mock_online_hooks.cancelled(NULL));}
void folio_online_update_get_snapshot(folio_online_snapshot_t* snapshot){*snapshot=mock_online;}
bool folio_network_saved_password(const char* name,char* password,size_t cap){(void)name;if(password && cap)snprintf(password,cap,"%s",mock_wifi_saved?"saved-password":"");return mock_wifi_saved;}
bool folio_network_forget(const char* name,char* error,size_t cap){(void)name;(void)error;(void)cap;mock_wifi_saved=false;return folio_network_stop();}
static bool fail_weread_stop, mock_wr_handoff;
static unsigned wr_starts, wr_stops, wr_commands, transfer_starts;
static folio_weread_action_t wr_last_action;
static size_t wr_last_index;
static char wr_last_value[16], mock_wr_path[256];
static uint32_t mock_wr_position;
static unsigned mock_https_station_calls, mock_native_connect_calls;
static bool mock_network_start_transport(char* error,size_t cap) {
    if(error && cap)*error=0;
    if(!mock_network.has_credentials)return false;
    mock_network.running=true;mock_network.connecting=mock_network_delay_connect;
    mock_network.connected=!mock_network_delay_connect;return true;
}
bool folio_network_start_station(char* error,size_t cap) {
    ++mock_https_station_calls;return mock_network_start_transport(error,cap);
}
void folio_network_get_status(folio_network_status_t* out){*out=mock_network;}
bool folio_network_stop(void){if(!folio_transfer_stop())return false;mock_network.running=mock_network.connected=mock_network.connecting=false;return true;}
bool folio_network_take_utc(uint32_t* utc){(void)utc;return false;}
bool folio_network_configure(const char* s,const char* p,char* e,size_t c){(void)s;(void)p;(void)e;(void)c;mock_network.has_credentials=true;return true;}
bool folio_network_connect(char* e,size_t c){++mock_native_connect_calls;return mock_network_start_transport(e,c);}
bool folio_network_scan_start(char* e,size_t c){(void)e;(void)c;return true;}
bool folio_network_scan_result(folio_network_ap_t* a,size_t c,size_t* n){(void)a;(void)c;(void)n;return false;}
bool folio_network_sync_time(char* e,size_t c){(void)e;(void)c;return true;}
void folio_network_cancel_time(void){}

bool folio_weread_start(char* error,size_t cap) {
    ++wr_starts;if(cap)*error=0;memset(&mock_wr,0,sizeof(mock_wr));++mock_wr.revision;
    snprintf(mock_wr.title,sizeof(mock_wr.title),"微信读书");
    if(!mock_network.has_credentials){
        mock_wr.state=FOLIO_WR_ERROR;
        snprintf(mock_wr.message,sizeof(mock_wr.message),"尚未配置 Wi-Fi。请打开网络设置，在传书网页连接 Wi-Fi 并校时。");return false;
    }
    mock_wr.running=true;mock_wr.state=FOLIO_WR_LOGIN;
    snprintf(mock_wr.qr_url,sizeof(mock_wr.qr_url),"https://weread.qq.com/test-only-offline-qr");return true;
}
bool folio_weread_start_sync(const char* path,char* error,size_t cap){
    bool okay=folio_weread_start(error,cap);
    snprintf(mock_wr.local_path,sizeof(mock_wr.local_path),"%s",path);return okay;
}
bool folio_weread_busy(void){return mock_wr.busy;}
bool folio_weread_stop(void){
    ++wr_stops;if(fail_weread_stop)return false;
    mock_wr.running=mock_wr.busy=false;mock_network.running=mock_network.connected=false;return true;
}
void folio_weread_get_status(folio_weread_status_t* out){*out=mock_wr;}
bool folio_weread_command(folio_weread_action_t action,size_t index,const char* value){
    ++wr_commands;wr_last_action=action;wr_last_index=index;
    snprintf(wr_last_value,sizeof(wr_last_value),"%s",value?value:"");return true;
}
void folio_weread_record(const char* path,uint32_t position,uint32_t percent,uint32_t seconds){
    (void)path;(void)position;(void)percent;(void)seconds;
}
bool folio_weread_flush(void){return true;}
bool folio_weread_io_cancelled(void){return mock_wr_cancelled;}
bool folio_weread_take_open(char* path,size_t cap,uint32_t* position){
    if(!mock_wr_handoff)return false;mock_wr_handoff=false;
    snprintf(path,cap,"%s",mock_wr_path);*position=mock_wr_position;return true;
}
bool folio_weread_local_position(const char* path,uint32_t* position){(void)path;(void)position;return false;}
static unsigned format_calls, restart_calls, update_calls;
bool folio_transfer_start(const folio_transfer_config_t* config,char* error,size_t cap) {
    ++transfer_starts;assert(!mock_wr.running);
    (void)config;if(cap)*error='\0';memset(&mock_transfer,0,sizeof(mock_transfer));mock_transfer.running=true;
    snprintf(mock_transfer.ssid,sizeof(mock_transfer.ssid),"Folio");
    snprintf(mock_transfer.ap_password,sizeof(mock_transfer.ap_password),"foliomuki");
    snprintf(mock_transfer.url,sizeof(mock_transfer.url),"http://192.168.4.1/");
    snprintf(mock_transfer.message,sizeof(mock_transfer.message),"等待接收书籍");return true;
}
bool folio_transfer_stop(void){if(fail_transfer_stop)return false;mock_transfer.running=false;mock_transfer.busy=false;return true;}
void folio_transfer_get_status(folio_transfer_status_t* out){*out=mock_transfer;}
bool folio_transfer_forget_wifi(void){return true;}
bool folio_update_available(void){return true;}
bool folio_update_inspect(folio_update_info_t* out,char* error,size_t cap) {
    (void)error;(void)cap;memset(out,0,sizeof(*out));snprintf(out->version,sizeof(out->version),"0.7.1-test");out->size=1024*1024;
    for(unsigned i=0;i<32;++i)out->sha256[i]=(uint8_t)i;out->token=1;return true;
}
bool folio_update_install(const folio_update_info_t* info,char* error,size_t cap){(void)info;(void)error;(void)cap;++update_calls;return true;}
bool folio_update_confirm_boot(bool ready){return ready;}
void esp_restart(void){++restart_calls;}
static unsigned card_sync_calls, emergency_replies, voluntary_off_calls, power_suspend_calls;
static int64_t mock_us = 1000000;
static uint32_t mock_utc = 1793021760; // 2026-10-26 21:36, Asia/Shanghai.
static bool mock_clock_valid = true;
static bool hide_embedded_cover;
static bool mock_empty_cover, mock_numbered_sections;
static const char* copyright_fixture = "版权信息\n书名：一页微光\n作者：页间\n版权所有，未经许可请勿转载。\n";

int epd_rotated_display_width(void) { return SCREEN_W; }
int epd_rotated_display_height(void) { return SCREEN_H; }
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* target) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) { ++pixels_clipped; return; }
    target[(size_t)y * SCREEN_W + x] = (uint8_t)((color >> 4) * 17);
    if (inside_wallpaper_draw && shutdown_draw_pixels && !--shutdown_draw_pixels)
        inject_wallpaper_shutdown();
}
void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* target) {
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x) epd_draw_pixel(x, y, color, target);
}
void epd_draw_rect(EpdRect rect, uint8_t color, uint8_t* target) {
    epd_fill_rect((EpdRect){rect.x, rect.y, rect.width, 1}, color, target);
    epd_fill_rect((EpdRect){rect.x, rect.y + rect.height - 1, rect.width, 1}, color, target);
    epd_fill_rect((EpdRect){rect.x, rect.y, 1, rect.height}, color, target);
    epd_fill_rect((EpdRect){rect.x + rect.width - 1, rect.y, 1, rect.height}, color, target);
}
void epd_hl_set_all_white(EpdiyHighlevelState* state) { memset(state->framebuffer, 255, sizeof(screen)); }
void epd_poweron(void) {}
void epd_poweroff(void) {}
void epd_clear(void) { memset(screen, 255, sizeof(screen)); }
float epd_ambient_temperature(void) { return 25; }
static uint8_t displayed[SCREEN_W*SCREEN_H];
static unsigned region_frames,full_frames;
static EpdRect last_region;
enum EpdDrawError epd_hl_update_screen(EpdiyHighlevelState* state, int mode, int temperature) {
    (void)mode; (void)temperature; ++frames;++full_frames;
    memcpy(displayed,state->framebuffer,sizeof(displayed));return EPD_DRAW_SUCCESS;
}
enum EpdDrawError epd_hl_update_area(EpdiyHighlevelState* state,int mode,int temperature,EpdRect r) {
    (void)mode;(void)temperature;++frames;++region_frames;last_region=r;
    for(int y=0;y<SCREEN_H;++y)for(int x=0;x<SCREEN_W;++x)
        if(x<r.x || x>=r.x+r.width || y<r.y || y>=r.y+r.height)
            assert(displayed[y*SCREEN_W+x]==state->framebuffer[y*SCREEN_W+x]);
    memcpy(displayed,state->framebuffer,sizeof(displayed));return EPD_DRAW_SUCCESS;
}
enum EpdDrawError epd_hl_update_screen_full(EpdiyHighlevelState* state, int mode, int temperature) {
    return epd_hl_update_screen(state, mode, temperature);
}
enum EpdDrawError epd_hl_update_screen_from_white(EpdiyHighlevelState* state, int mode, int temperature) {
    return epd_hl_update_screen(state, mode, temperature);
}
int64_t esp_timer_get_time(void) { return mock_us += 1000; }
esp_err_t read_pico_init(read_pico_handle_t* hardware) {
    memset(hardware, 0, sizeof(*hardware));
    hardware->framebuffer = screen; hardware->hl.framebuffer = screen;
    hardware->touch_ready = hardware->pmu_ready = true;
    return ESP_OK;
}
esp_err_t cst836u_read(void* touch, cst836u_touch_t* sample) {
    (void)touch; memset(sample, 0, sizeof(*sample)); return ESP_OK;
}
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    *info = (read_pico_sd_info_t){.mounted = !mock_card_unformatted && !mock_card_unmounted, .present=true,.needs_format=mock_card_unformatted,.free_bytes = UINT64_C(14000000000),
                               .capacity_bytes = UINT64_C(16000000000)};
    if(card_probe_reads) {
        --card_probe_reads;info->mounted=false;info->error=ESP_ERR_NOT_FINISHED;
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}
esp_err_t read_pico_sd_remount(void) {
    ++card_remount_calls;mock_card_unmounted=false;return ESP_OK;
}
esp_err_t read_pico_sd_sync(void) {
    // The real BSP unmounts here. Calling with retained objects is a regression.
    assert(!s_reader && !s_font && !s_activity && !s_catalog);
    ++card_sync_calls;return fail_card_sync?ESP_FAIL:ESP_OK;
}
esp_err_t read_pico_sd_format(void){++format_calls;return ESP_FAIL;}
void folio_power_init(void) {}
static bool mock_update_power_safe=true;
static unsigned power_polls, mock_power_event_at, mock_power_unsafe_at;
static folio_power_event_t mock_power_event;
folio_power_event_t folio_power_poll(void) {
    ++power_polls;
    if(mock_power_unsafe_at && power_polls>=mock_power_unsafe_at)mock_update_power_safe=false;
    if(mock_shutdown_pending)return FOLIO_POWER_SHUTDOWN_NOW;
    return power_polls==mock_power_event_at?mock_power_event:FOLIO_POWER_NONE;
}
bool folio_power_update_safe(void){return mock_update_power_safe && !mock_shutdown_pending;}
// The preview has no polling history, so the gate has no grace window here.
bool folio_power_update_gate(void){return mock_update_power_safe && !mock_shutdown_pending;}
// Preview has no polling history: a refusal is only modelled as a readable one.
bool folio_power_unsafe_confirmed(void){return !mock_update_power_safe || mock_shutdown_pending;}
bool folio_power_shutdown_pending(void) {
    // The 12th check is inside the real cache payload loop, after earlier reads.
    if (inside_wallpaper_cache && shutdown_cache_checks && !--shutdown_cache_checks)
        inject_wallpaper_shutdown();
    return mock_shutdown_pending;
}
// Production transfer dispatches cancellation to the main UI on this task.
bool folio_io_cancelled(void) { return folio_main_io_cancelled(); }
esp_err_t folio_power_finish_shutdown(read_pico_handle_t* hardware) {
    (void)hardware; assert(s_reader == NULL && s_font == NULL && s_activity == NULL); ++emergency_replies; return ESP_FAIL;
}
uint32_t folio_power_unix_time(void) { return mock_clock_valid ? mock_utc : 0; }
bool folio_power_rtc_time(uint32_t* utc) {
    if(!utc || !mock_clock_valid)return false;
    *utc=mock_utc;return true;
}
bool folio_power_set_time(uint32_t utc) {mock_utc=utc;mock_clock_valid=true;return true;}
bool folio_power_datetime(struct tm* local, int timezone_minutes) {
    if (!local || !mock_clock_valid) return false;
    time_t shifted = (time_t)((int64_t)mock_utc + timezone_minutes * 60);
    return gmtime_r(&shifted, local) != NULL;
}
static const char* mock_battery="82%";
void folio_power_status(char* text, size_t capacity, int timezone_minutes) {
    struct tm local;
    if (folio_power_datetime(&local, timezone_minutes))
        snprintf(text, capacity, "%02d:%02d  %s", local.tm_hour, local.tm_min,mock_battery);
    else snprintf(text, capacity, "--:--  %s",mock_battery);
}
static bool mock_lock_success, mock_lock_timed_out;
static uint16_t last_lock_minutes;
static unsigned lock_refresh_calls;
esp_err_t folio_power_lock(read_pico_handle_t* hardware){
    (void)hardware;++power_suspend_calls;
    if(mock_lock_success){mock_us+=300000000;mock_utc+=300;return ESP_OK;}return ESP_FAIL;
}
esp_err_t folio_power_lock_refresh(read_pico_handle_t* hardware, void (*refresh)(void*), void* context) {
    esp_err_t result = folio_power_lock(hardware);
    if (result == ESP_OK && refresh) { ++lock_refresh_calls; refresh(context); }
    return result;
}
esp_err_t folio_power_lock_timed(read_pico_handle_t* hardware, void (*refresh)(void*), void* context,
                                uint16_t minutes, bool* timed_out) {
    last_lock_minutes=minutes;
    esp_err_t result=folio_power_lock_refresh(hardware,refresh,context);
    if(timed_out)*timed_out=result==ESP_OK && minutes && mock_lock_timed_out;
    return result;
}
esp_err_t folio_power_suspend(read_pico_handle_t* hardware, bool off) {
    (void)hardware; ++power_suspend_calls; if (off) ++voluntary_off_calls; return ESP_FAIL;
}
void folio_settings_defaults(folio_settings_t* settings) {
    *settings = (folio_settings_t){.font_size = 38, .margin = 48, .line_gap = 10, .paragraph_gap = 12,
        .refresh_every = 8, .idle_minutes = 5, .timezone_minutes = 480, .justify = 1,
        .auto_resume = 0, .home_chart = 0, .auto_shutdown_minutes=30};
}
bool folio_settings_load(folio_settings_t* settings) { folio_settings_defaults(settings); return true; }
bool folio_settings_save(const folio_settings_t* settings) {
    ++settings_writes;
    if (fail_settings_write) return false;
    persisted_settings = *settings;
    return true;
}
bool folio_session_save(const char* path) { snprintf(session_path, sizeof(session_path), "%s", path); return true; }
bool folio_session_load(char* path, unsigned capacity) {
    if (!*session_path) return false;
    snprintf(path, capacity, "%s", session_path); return true;
}

bool folio_epub_prepare(const char* path, folio_epub_t* result, char* error, size_t error_cap) {
    (void)path; (void)error; (void)error_cap;
    memset(result, 0, sizeof(*result));
    snprintf(result->title, sizeof(result->title), "一页微光");
    snprintf(result->text_path, sizeof(result->text_path), "%s/body.txt", FOLIO_SD_ROOT);
    result->chapters = calloc(3, sizeof(*result->chapters));
    assert(result->chapters != NULL);
    result->chapter_count = 3;
    const char* titles[] = {"第一章 留一段安静的时间", "第二章 街道尽头的光", "第三章 在日常里慢慢走"};
    if(mock_numbered_sections){titles[0]="一 开车";titles[1]="2";titles[2]="3";}
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(result->chapters[i].title, sizeof(result->chapters[i].title), "%s", titles[i]);
        result->chapters[i].offset = chapter_offsets[i];
    }
    if (mock_empty_cover) {
        snprintf(result->text_path, sizeof(result->text_path), "%s/blank-cover.txt", FOLIO_SD_ROOT);
        const char* frontmatter_titles[] = {"封面", "版权信息", "第一章"};
        const uint32_t offsets[] = {0, 1, 1 + (uint32_t)strlen(copyright_fixture)};
        for (size_t i = 0; i < 3; ++i) {
            snprintf(result->chapters[i].title, sizeof(result->chapters[i].title), "%s", frontmatter_titles[i]);
            result->chapters[i].offset = offsets[i];
        }
    }
    return true;
}
bool folio_epub_metadata(const char* path,char* title,size_t title_cap,char* author,size_t author_cap,char* error,size_t error_cap) {
    (void)path;if(title_cap)*title='\0';if(author_cap)*author='\0';if(error_cap)*error='\0';return true;
}
void folio_epub_free(folio_epub_t* epub) { free(epub->chapters); memset(epub, 0, sizeof(*epub)); }
bool folio_epub_cover(const char* path, uint8_t** data, size_t* size, char* error, size_t capacity) {
    *data = NULL; *size = 0;
    if (capacity) error[0] = '\0';
    if (hide_embedded_cover || !strstr(path, "一页微光.epub") || access(path, R_OK) != 0) return false;
    FILE* file = fopen(FOLIO_TEST_FIXTURES "/ui-cover-light.png", "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); assert(length > 0 && length < 2 * 1024 * 1024);
    rewind(file); *data = malloc((size_t)length); assert(*data);
    assert(fread(*data, 1, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0); *size = (size_t)length;
    return true;
}

static void copy_file(const char* source, const char* destination) {
    FILE* input = fopen(source, "rb"); FILE* output = fopen(destination, "wb");
    assert(input && output);
    char bytes[4096]; size_t length;
    while ((length = fread(bytes, 1, sizeof(bytes), input)) != 0)
        assert(fwrite(bytes, 1, length, output) == length);
    assert(!ferror(input)); assert(fclose(input) == 0 && fclose(output) == 0);
}

static void fixture_books(void) {
    assert(mkdir(FOLIO_SD_ROOT, 0700) == 0 || errno == EEXIST);
    assert(mkdir(FOLIO_SD_ROOT "/books", 0700) == 0 || errno == EEXIST);
    FILE* file = fopen(FOLIO_SD_ROOT "/body.txt", "wb"); assert(file != NULL);
    // Original prose for visual QA: varied paragraph lengths and punctuation.
    const char* paragraphs[] = {
        "清晨的窗边有一小块阳光。我把椅子往前挪了挪，翻开昨天没有读完的那一页。楼下有人推着自行车经过，车铃响了一声，很快又安静下来。\n",
        "有些书适合在路上读，有些书则需要一个不被打扰的下午。并不一定要记住多少句子，只是让自己的注意力，在这一刻完整地留在这里。\n",
        "杯里的茶已经凉了。纸页上的故事还没有结束，我也不急着知道结局。读到一段喜欢的文字，便停下来，看一会儿窗外的树影。\n",
        "后来我发现，日常生活中那些细小的变化，往往要慢一点才能看见。新长出的叶子，门口换过的花，朋友说话时比往常多停顿的半秒。\n",
        "午后去买面包，沿着河边多走了一段。水面上没有什么特别的风景，但拐过街角时，忽然想起书里讲过的一件小事。\n",
        "回到家，我没有立刻把书收起来，而是在夹着书签的地方写下日期。下次再打开时，就会知道，这一页曾陪我度过一个普通而安静的星期天。\n",
        "阅读不是每天都要完成的任务。有时只读两页，有时一连读上很久。愿意在忙碌之中留出一点空白，已经是一种不错的开始。\n",
        "天色渐暗，房间里的物件失去了清晰的边缘。我合上书，准备去做晚饭。那句话还在心里，像一盏不需要很亮的小灯。\n"
    };
    for (unsigned chapter = 0; chapter < 3; ++chapter) {
        long offset = ftell(file); assert(offset >= 0); chapter_offsets[chapter] = (uint32_t)offset;
        for (unsigned i = 0; i < 16; ++i) assert(fputs(paragraphs[(i + chapter * 3) % 8], file) >= 0);
    }
    assert(fclose(file) == 0);
    const char* titles[] = {"一页微光.epub", "南方来信.txt", "山间日记.txt", "平凡的光.txt",
        "慢行笔记.txt", "把日子过成诗.txt", "旧城漫游.txt", "星夜手记.txt",
        "晨光里的植物.txt", "海边散步.txt", "雨后记事.txt", "风停在窗边.txt"};
    for (unsigned i = 0; i < sizeof(titles) / sizeof(titles[0]); ++i) {
        char path[512]; snprintf(path, sizeof(path), "%s/books/%s", FOLIO_SD_ROOT, titles[i]);
        file = fopen(path, "wb"); assert(file != NULL);
        assert(fputs(i == 0 ? "fixture EPUB source" : paragraphs[i % 8], file) >= 0);
        assert(fclose(file) == 0);
    }
    copy_file(FOLIO_TEST_FIXTURES "/ui-cover-letter.jpg", FOLIO_SD_ROOT "/books/南方来信.jpg");
    copy_file(FOLIO_TEST_FIXTURES "/ui-cover-mountain.png", FOLIO_SD_ROOT "/books/山间日记.png");
}
static char fixture_font_paths[3][256];
static void fixture_fonts(const char* source) {
    assert(mkdir(FOLIO_SD_ROOT "/fonts", 0700) == 0 || errno == EEXIST);
    const char* names[] = {"宋体.ttf", "阅读宋体.TTF", "常用字体.ttf"};
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(fixture_font_paths[i], sizeof(fixture_font_paths[i]), "%s/fonts/%s", FOLIO_SD_ROOT, names[i]);
        FILE* input = fopen(source, "rb"); FILE* output = fopen(fixture_font_paths[i], "wb");
        assert(input != NULL && output != NULL);
        char bytes[4096]; size_t n;
        while ((n = fread(bytes, 1, sizeof(bytes), input)) != 0) assert(fwrite(bytes, 1, n, output) == n);
        assert(!ferror(input)); assert(fclose(input) == 0 && fclose(output) == 0);
    }
    scan_fonts();
    assert(s_font_count == FOLIO_FONT_BUILTIN_COUNT + 3);
}
static void save_pgm(const char* directory, const char* name) {
    assert(pixels_clipped == 0);
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.pgm", directory, name);
    FILE* file = fopen(path, "wb"); assert(file != NULL);
    assert(fprintf(file, "P5\n%d %d\n255\n", SCREEN_W, SCREEN_H) > 0);
    assert(fwrite(screen, 1, sizeof(screen), file) == sizeof(screen));
    assert(fclose(file) == 0);
    printf("preview: %s\n", path);
}
static void tap_rect(EpdRect r) { handle_tap(r.x + r.width / 2, r.y + r.height / 2); }
static void tap_row(unsigned row) { handle_tap(200, LIST_TOP + (int)row * LIST_ROW + 48); }
static void open_tools(void) { handle_tap(342, 600); assert(s_view == VIEW_TOOLS); }
static void assert_body_bounds(void) {
    memset(screen, 255, sizeof(screen));
    (void)folio_font_set_size(s_font, s_settings.font_size);
    folio_reader_draw(s_reader, screen);
    unsigned body_pixels = 0;
    for (int y = 0; y < SCREEN_H; ++y) for (int x = 0; x < SCREEN_W; ++x) {
        if (screen[(size_t)y * SCREEN_W + x] == 255) continue;
        ++body_pixels;
        assert(y >= 40 && y < SCREEN_H - (s_settings.footer_hidden?24:64));
        assert(x >= s_settings.margin - 4 && x < SCREEN_W - s_settings.margin + 4);
    }
    assert(body_pixels > 0 && pixels_clipped == 0);
}

static void assert_centered_ink(EpdRect rect, int size, const char* value) {
    memset(screen, 255, sizeof(screen));
    centered_in(rect, size, INK, value);
    int left = SCREEN_W, top = SCREEN_H, right = 0, bottom = 0;
    unsigned count = 0;
    for (int y = 0; y < SCREEN_H; ++y) for (int x = 0; x < SCREEN_W; ++x) {
        if (screen[(size_t)y * SCREEN_W + x] == 255) continue;
        ++count;
        assert(x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height);
        if (x < left) left = x;
        if (y < top) top = y;
        if (x + 1 > right) right = x + 1;
        if (y + 1 > bottom) bottom = y + 1;
    }
    assert(count && pixels_clipped == 0);
    // Compare doubled centers, retaining odd-pixel geometry without floats.
    assert(abs(left + right - (2 * rect.x + rect.width)) <= 2);
    assert(abs(top + bottom - (2 * rect.y + rect.height)) <= 2);
}
static uint32_t screen_hash(EpdRect rect) {
    uint32_t hash=2166136261u;
    for(int y=rect.y;y<rect.y+rect.height;++y)for(int x=rect.x;x<rect.x+rect.width;++x)
        hash=(hash^screen[(size_t)y*SCREEN_W+x])*16777619u;
    return hash;
}
static void test_ui_audit_regressions(void) {
    folio_settings_t saved=s_settings;uint32_t utc=mock_utc;int64_t us=mock_us;
    unsigned failures=0;
#define AUDIT_CHECK(test, message) do { bool okay=(test); printf("UI audit %s: %s\n",okay?"PASS":"FAIL",message); if(!okay)++failures; } while(0)
    memset(screen,255,sizeof(screen));paragraph(72,200,72+text_width(26,"阅读"),26,42,3,INK,"阅读。");
    uint32_t wrapped=screen_hash((EpdRect){48,144,588,180});
    memset(screen,255,sizeof(screen));wr_paragraph(72,200,72+text_width(26,"阅读"),26,42,3,INK,"阅读。");
    AUDIT_CHECK(wrapped==screen_hash((EpdRect){48,144,588,180}),"shared paragraph keeps closing punctuation with preceding glyph");
    draw_update();uint32_t version=screen_hash((EpdRect){48,176,588,40});
    epd_fill_rect((EpdRect){48,176,588,40},PAPER,fb());
    char current[80];snprintf(current,sizeof(current),"当前版本  %s",esp_app_get_description()->version);
    label(48,200,636,26,MUTED,current);
    AUDIT_CHECK(version==screen_hash((EpdRect){48,176,588,40}),"update header uses running app descriptor");
    s_settings_tab=1;s_view=VIEW_SETTINGS;
    bool resume=s_settings.auto_resume;handle_tap(150,608);
    AUDIT_CHECK(s_settings.auto_resume!=resume,"resume toggle responds on left side of its visible row");
    s_settings_tab=2;tools_action(5);
    AUDIT_CHECK(s_settings_tab==0 && s_settings_return==VIEW_TOOLS,"menu typography entry always selects typography tab");
    mock_wr=(folio_weread_status_t){.state=FOLIO_WR_OTP,.running=true};s_wr_otp[0]=0;
    draw_weread();
    AUDIT_CHECK(screen[1096*SCREEN_W+342]==(RULE|(RULE>>4)),"incomplete OTP has disabled submit border");
    mock_wr=(folio_weread_status_t){0};s_view=VIEW_SYSTEM;
    struct tm morning={.tm_year=126,.tm_mon=9,.tm_mday=26,.tm_hour=9,.tm_min=40};
    s_settings.timezone_minutes=0;s_settings.reserved[0]|=0x10;
    assert(folio_clock_to_utc(&morning,0,&mock_utc));draw_time();
    uint32_t morning_card=screen_hash((EpdRect){48,240,588,208});
    mock_utc+=43200;draw_time();
    AUDIT_CHECK(morning_card!=screen_hash((EpdRect){48,240,588,208}),"12-hour display distinguishes morning and afternoon");
    enter_time();unsigned before_regions=region_frames,before_full=full_frames;int64_t idle=s_last_activity;
    mock_utc+=60;mock_us+=60000000;refresh_setup();
    AUDIT_CHECK(region_frames>before_regions && full_frames==before_full && s_last_activity==idle,"TIME minute tick refreshes locally without changing activity");
    s_settings=saved;mock_utc=utc;mock_us=us;s_view=VIEW_HOME;
    fflush(stdout);assert(failures==0);
#undef AUDIT_CHECK
}
static void test_clock_layouts(const char* directory) {
    folio_settings_t saved=s_settings;uint32_t utc=mock_utc;bool valid=mock_clock_valid;
    const char* battery=mock_battery;mock_clock_valid=true;mock_battery="100%+";
    s_settings.timezone_minutes=480;s_settings.reserved[0]|=0x10;s_settings.keep_page_on_lock=2;
    const int hours[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23};
    const char* periods[]={"凌晨","上午","下午","晚上"};
    uint32_t am_card=0,am_digits=0;
    for(unsigned i=0;i<sizeof(hours)/sizeof(hours[0]);++i){
        struct tm local={.tm_year=126,.tm_mon=10,.tm_mday=30,.tm_hour=hours[i],.tm_min=59};
        assert(folio_clock_to_utc(&local,s_settings.timezone_minutes,&mock_utc));
        char clock[24],expected[24];ui_clock(clock,sizeof(clock));
        snprintf(expected,sizeof(expected),"%d:59",hours[i]%12?hours[i]%12:12);assert(!strcmp(clock,expected));
        assert(!strcmp(ui_period(),periods[hours[i]/6]));
        assert_centered_ink((EpdRect){72,170,540,164},144,clock);
        int period_width=text_width(20,ui_period()),size=22;
        if(text_width(size,clock)+text_width(size,mock_battery)+period_width+14>178)size=20;
        assert(text_width(size,clock)+text_width(size,mock_battery)+period_width+14<=178);
        assert(636-text_width(48,clock)-12-text_width(24,ui_period())>=388);
        char date[80];snprintf(date,sizeof(date),"%s · 2026年11月30日 · 周一",ui_period());
        assert(text_width(28,date)<=540);
        draw_lock(false);
        int former_icon_x = 624 - text_width(26, mock_battery) - 48;
        for (int y = 78; y < 96; ++y) for (int x = former_icon_x; x < former_icon_x + 35; ++x)
            assert(screen[y * SCREEN_W + x] == 255); // Percentage stays; the battery pictogram is gone.
        if(hours[i]==0){am_card=screen_hash((EpdRect){72,350,540,50});am_digits=screen_hash((EpdRect){72,170,540,164});save_pgm(directory,"128-lock-midnight-12h");}
        if(hours[i]==12){
            assert(am_card!=screen_hash((EpdRect){72,350,540,50}) && am_digits==screen_hash((EpdRect){72,170,540,164}));
            save_pgm(directory,"129-lock-noon-12h");
        }
        if(hours[i]==0 || hours[i]==12){
            draw_home();save_pgm(directory,hours[i]?"131-home-noon-12h":"130-home-midnight-12h");
            enter_time();save_pgm(directory,hours[i]?"133-time-noon-12h":"132-time-midnight-12h");
            draw_reader();save_pgm(directory,hours[i]?"135-reading-noon-12h":"134-reading-midnight-12h");
        }
        if(hours[i]%6==0){
            char name[64];unsigned group=(unsigned)hours[i]/6;
            const char* slug[]={"dawn","morning","afternoon","evening"};
            draw_lock(false);snprintf(name,sizeof(name),"%u-lock-%s",137+group*4,slug[group]);save_pgm(directory,name);
            draw_home();snprintf(name,sizeof(name),"%u-home-%s",138+group*4,slug[group]);save_pgm(directory,name);
            enter_time();snprintf(name,sizeof(name),"%u-time-%s",139+group*4,slug[group]);save_pgm(directory,name);
            draw_reader();snprintf(name,sizeof(name),"%u-reading-%s",140+group*4,slug[group]);save_pgm(directory,name);
        }
    }
    s_settings.timezone_minutes=-480;
    for(unsigned group=0;group<4;++group){
        struct tm local={.tm_year=126,.tm_mon=10,.tm_mday=30,.tm_hour=(int)group*6};
        assert(folio_clock_to_utc(&local,-480,&mock_utc));assert(!strcmp(ui_period(),periods[group]));
    }
    // Observe all four transitions at the minute edge, including local midnight.
    int64_t saved_us=mock_us;
    for(unsigned group=0;group<4;++group){
        struct tm local={.tm_year=126,.tm_mon=10,.tm_mday=30,.tm_hour=(int)group*6+5,.tm_min=59};
        assert(folio_clock_to_utc(&local,-480,&mock_utc));enter_time();
        uint32_t controls=screen_hash((EpdRect){48,472,588,720});int64_t idle=s_last_activity;
        mock_utc+=60;mock_us+=60000000;refresh_setup();
        assert(!strcmp(ui_period(),periods[(group+1)%4]));
        assert(screen_hash((EpdRect){48,472,588,720})==controls && s_last_activity==idle);
    }
    mock_us=saved_us;
    s_settings.reserved[0]&=(uint8_t)~0x10;assert(!*ui_period());
    s_settings.reserved[0]|=0x10;mock_clock_valid=false;assert(!*ui_period());
    char unknown[24];ui_clock(unknown,sizeof(unknown));assert(!strcmp(unknown,"--:--"));
    s_settings=saved;mock_utc=utc;mock_clock_valid=valid;mock_battery=battery;show_view(VIEW_READING,false);
    puts("12h layout: four Chinese periods, all 24 hours, four minute-edge transitions, positive/negative zones, unset clock, 144px numbers and full-charge footer width passed");
}
static void test_centered_text(void) {
    const struct { EpdRect rect; int size; const char* text; } cases[] = {
        {{72, 180, 540, 96}, 32, "继续阅读"},
        {{123, 301, 271, 75}, 32, "Agjp 32%"},
        {{48, 420, 588, 96}, 26, "Folio 中文 100%"},
        {{72, 540, 179, 75}, 32, "阅读此刻，停留在书页之间"},
        {{72, 660, 127, 84}, 38, "AAAAAAAAAAAAAAAAAAAAAAAAg"},
        {{72, 780, 211, 79}, 22, "  - 12 / 128 +  "},
        {{72, 900, 83, 83}, 48, "Aa"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert_centered_ink(cases[i].rect, cases[i].size, cases[i].text);
    // The invisible descender at the end must not shift a truncated label.
    EpdRect short_rect = {72, 180, text_width(32, "A..."), 96};
    assert_centered_ink(short_rect, 32, "AAAAAAAAAAAAAAAAAAAAAAAAg");
    uint8_t* truncated = malloc(sizeof(screen)); assert(truncated);
    memcpy(truncated, screen, sizeof(screen));
    assert_centered_ink(short_rect, 32, "A...");
    assert(memcmp(truncated, screen, sizeof(screen)) == 0);
    free(truncated);
    const char* blanks[] = {"", "   ", "　"};
    for (unsigned i = 0; i < sizeof(blanks) / sizeof(blanks[0]); ++i) {
        memset(screen, 255, sizeof(screen));
        centered_in(short_rect, 32, INK, blanks[i]);
        for (size_t pixel = 0; pixel < sizeof(screen); ++pixel) assert(screen[pixel] == 255);
    }
    printf("centered UI text: Chinese, ASCII, mixed, truncation and blank actual-ink checks passed\n");
}
static void test_weread_wrapping(void) {
    const struct {const char* value;const char* expected;const char* width;} cases[]={
        {"阅读。","阅\n读。","阅读"},
        {"阅读。”","阅\n读。”","读。”"},
        {"阅读Wi-Fi。","阅读\nWi-Fi。","Wi-Fi。"},
        {"阅读Wi-Fi.","阅读\nWi-Fi.","Wi-Fi."},
        {"AAAAAAAAAAAAAAAAAA","AAAAAA\nAAAAAA\nAAAAAA","AAAAAA"},
    };
    uint8_t* actual=malloc(sizeof(screen));assert(actual);
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        int right=72+text_width(26,cases[i].width);
        memset(screen,255,sizeof(screen));wr_paragraph(72,200,right,26,42,3,INK,cases[i].value);
        memcpy(actual,screen,sizeof(screen));
        memset(screen,255,sizeof(screen));paragraph(72,200,right,26,42,3,INK,cases[i].expected);
        assert(memcmp(actual,screen,sizeof(screen))==0);
        // Font advance is the wrapping width; actual W ink may extend a pixel
        // left of its pen. Permit only the font's measured side bearings.
        int left_ink=0,right_ink=0;const char* glyph=cases[i].value;
        while(*glyph){uint32_t cp=folio_utf8_next(&glyph);int l,r;
            if(folio_font_glyph_bounds(s_font,cp,&l,NULL,&r,NULL)){
                if(l<left_ink)left_ink=l;r-=folio_font_advance(s_font,cp,0);if(r>right_ink)right_ink=r;
            }
        }
        for(int y=0;y<SCREEN_H;++y)for(int x=0;x<SCREEN_W;++x)
            if(screen[y*SCREEN_W+x]!=255)assert(x>=72+left_ink && x<right+right_ink && y>=160 && y<300);
    }
    // The line cap still applies when a final glyph and its punctuation move.
    int right=72+text_width(26,"阅读");
    memset(screen,255,sizeof(screen));wr_paragraph(72,200,right,26,42,1,INK,"阅读。");memcpy(actual,screen,sizeof(screen));
    memset(screen,255,sizeof(screen));paragraph(72,200,right,26,42,1,INK,"阅");assert(memcmp(actual,screen,sizeof(screen))==0);
    free(actual);assert(pixels_clipped==0);
    printf("WeRead text wrapping: closing punctuation, Wi-Fi tokens, oversized words, line caps and actual ink bounds passed\n");
}

static void advance_clock(unsigned seconds) {
    mock_us += (int64_t)seconds * 1000000;
    mock_utc += seconds;
}
static void tap_nav(unsigned index) { handle_tap(138 + (int)index * 204, 1168); }
static void fixture_activity(void) {
    // The demo history belongs only to this disposable fake card. Exercise the
    // public timing API rather than injecting private records or UI values.
    const unsigned minutes[] = {0, 12, 35, 6, 0, 18, 64, 9, 25, 0, 14, 48, 7, 21,
        30, 0, 5, 52, 16, 38, 11, 0, 22, 45, 8, 19, 62, 24};
    int32_t today = ((int64_t)mock_utc + 480 * 60) / 86400;
    int64_t mono = 1000000;
    uint64_t expected = 0;
    for (unsigned day = 0; day < sizeof(minutes) / sizeof(minutes[0]); ++day) {
        int64_t wall = (int64_t)(today - 27 + (int)day) * 86400 + 4 * 3600;
        folio_activity_update(s_activity, mono, wall, 480, true, true);
        for (unsigned minute = 0; minute < minutes[day]; ++minute) {
            mono += 60000000; wall += 60;
            folio_activity_update(s_activity, mono, wall, 480, true, true);
        }
        folio_activity_update(s_activity, mono, wall, 480, false, false);
        mono += 1000000; expected += minutes[day] * 60;
    }
    assert(folio_activity_total_seconds(s_activity) == expected);
    assert(folio_activity_day_seconds(s_activity, today) == 24 * 60);
    for (unsigned i = 3; i > 0; --i) {
        const folio_book_t* book = &s_books[i - 1];
        assert(folio_activity_touch_book(s_activity, book->path, book->title,
                                        i == 1 ? 37 : i * 21, i * 17, 0));
    }
    assert(folio_activity_save(s_activity));
    folio_activity_close(s_activity); s_activity = folio_activity_open();
    assert(s_activity && !*folio_activity_error(s_activity));
    assert(folio_activity_recent_count(s_activity) == 3);
    assert(folio_activity_total_seconds(s_activity) == expected);
}
static void test_heatmap_calendar(const char* directory) {
    // Keep the real persisted activity history, vary only the mocked RTC. These
    // dates cover Monday, Sunday and crossing from October into November.
    uint32_t saved_utc = mock_utc;
    const unsigned shifts[] = {0, 6, 7};
    const int weekdays[] = {1, 0, 1}, months[] = {9, 10, 10}, days[] = {26, 1, 2};
    const char* names[] = {"40-heatmap-monday", "41-heatmap-sunday", "42-heatmap-next-month"};
    assert(s_settings.home_chart == 0);
    for (unsigned test = 0; test < 3; ++test) {
        mock_utc = saved_utc + shifts[test] * 86400;
        struct tm local;
        assert(folio_power_datetime(&local, s_settings.timezone_minutes));
        assert(local.tm_wday == weekdays[test] && local.tm_mon == months[test] && local.tm_mday == days[test]);
        int32_t today = home_epoch_day(mock_utc);
        memset(screen, 0x55, sizeof(screen));
        home_heatmap(today);
        for (unsigned index = 0; index < 28; ++index) {
            EpdRect cell = home_heat_cell(index);
            assert(cell.width == cell.height && cell.width >= 8);
            uint32_t seconds = folio_activity_day_seconds(s_activity, today - 27 + (int32_t)index);
            uint8_t shade = seconds == 0 ? 255 : seconds < 15 * 60 ? 187 : seconds < 30 * 60 ? 102 : 0;
            assert(screen[(cell.y + cell.height / 2) * SCREEN_W + cell.x + cell.width / 2] == shade);
            // Rounded corners retain the original background; even zero-reading
            // days must be painted, so skipped future slots cannot pass above.
            assert(screen[cell.y * SCREEN_W + cell.x] == 0x55);
            assert(screen[cell.y * SCREEN_W + cell.x + cell.width/2] == (seconds ? shade : (RULE>>4)*17));
            for (unsigned previous = 0; previous < index; ++previous) {
                EpdRect other = home_heat_cell(previous);
                assert(cell.x >= other.x + other.width || other.x >= cell.x + cell.width ||
                       cell.y >= other.y + other.height || other.y >= cell.y + cell.height);
            }
        }
        // Render complete previews without changing the presentation/idle state.
        draw_home(); save_pgm(directory, names[test]);
    }
    mock_utc = saved_utc;
    draw_home();
}
static void test_lock_personalization(const char* directory) {
    folio_settings_t saved = s_settings;
    uint32_t saved_utc = mock_utc;
    bool saved_valid = mock_clock_valid;
    assert(s_view == VIEW_HOME_SETTINGS);
    handle_tap(542,620);
    assert(FOLIO_LOCK_STYLE(&s_settings) == 2);
    handle_tap(188,976);
    assert(s_view == VIEW_LOCK_PREVIEW && !s_lock_preview_off);
    const struct {
        int year, month, day, start, days;
        const char* name;
    } dates[] = {
        {2024, 2, 29, 3, 29, "106-lock-calendar-leap"},
        {2026, 3, 31, 6, 31, "107-lock-calendar-six-weeks"},
        {2025, 2, 28, 5, 28, "108-lock-calendar-february"}
    };
    for (unsigned i = 0; i < sizeof(dates)/sizeof(*dates); ++i) {
        struct tm local = {.tm_year=dates[i].year-1900,.tm_mon=dates[i].month-1,
                           .tm_mday=dates[i].day,.tm_hour=23,.tm_min=59};
        assert(folio_clock_to_utc(&local,480,&mock_utc)); mock_clock_valid = true;
        draw_lock(false);
        for (unsigned cell = 0; cell < 42; ++cell) {
            unsigned ink = 0;
            for (int y = 632+(int)(cell/7)*64; y < 684+(int)(cell/7)*64; ++y)
                for (int x = 76+(int)(cell%7)*80; x < 128+(int)(cell%7)*80; ++x)
                    ink += screen[y*SCREEN_W+x] < 240;
            bool expected = (int)cell >= dates[i].start && (int)cell < dates[i].start+dates[i].days;
            assert((ink > 0) == expected);
        }
        assert(text_width(144,"23:59") <= 540 && text_width(144,"12:00") <= 540);
        assert(folio_font_pixel_height(s_font) == 144);
        save_pgm(directory,dates[i].name);
    }
    unsigned prior_frames = frames;
    int64_t prior_idle = s_last_activity;
    s_lock_preview_minute = mock_utc / 60;
    refresh_lock_preview(); assert(frames == prior_frames);
    advance_clock(60); refresh_lock_preview();
    assert(frames == prior_frames+1 && s_last_activity == prior_idle);
    save_pgm(directory,"109-lock-calendar-next-month");
    mock_clock_valid = false; draw_lock(false);
    save_pgm(directory,"110-lock-time-unset");
    handle_tap(342,900); assert(s_view == VIEW_HOME_SETTINGS);
    mock_utc = saved_utc; mock_clock_valid = saved_valid;

    // A new card has an accessible empty gallery and a static shutdown fallback.
    handle_tap(342,884);
    assert(s_view == VIEW_WALLPAPERS && !s_wallpaper_count && !s_wallpaper_total);
    save_pgm(directory,"111-wallpapers-empty");
    tap_rect(back_rect()); assert(s_view == VIEW_HOME_SETTINGS);
    handle_tap(542,796); handle_tap(492,976);
    assert(s_view == VIEW_LOCK_PREVIEW && s_lock_preview_off);
    uint8_t* fallback = malloc(sizeof(screen)); uint8_t* wallpaper = malloc(sizeof(screen));
    assert(fallback && wallpaper); memcpy(fallback,screen,sizeof(screen));
    mock_utc += 86400; mock_clock_valid = false;
    prior_frames = frames; refresh_lock_preview(); assert(frames == prior_frames);
    draw_lock(true); assert(!memcmp(fallback,screen,sizeof(screen)));
    save_pgm(directory,"112-shutdown-static-fallback");
    handle_tap(342,900); assert(s_view == VIEW_HOME_SETTINGS);
    const char* first = FOLIO_SD_ROOT "/picture/01-mountain.png";
    const char* second = FOLIO_SD_ROOT "/picture/02-letter.jpg";
    copy_file(FOLIO_TEST_FIXTURES "/ui-cover-mountain.png",first);
    copy_file(FOLIO_TEST_FIXTURES "/ui-cover-letter.jpg",second);
    handle_tap(342,884);
    assert(s_view == VIEW_WALLPAPERS && s_wallpaper_count == 2 && s_wallpaper_total == 2);
    assert(!strcmp(s_wallpaper_path,first));
    save_pgm(directory,"113-wallpapers-list");
    tap_row(1);
    assert(s_view == VIEW_LOCK_PREVIEW && s_lock_preview_off && FOLIO_OFF_STYLE(&s_settings) == 2);
    char selected[256]; assert(folio_storage_wallpaper_load(selected,sizeof(selected)) && !strcmp(selected,second));
    assert(memcmp(fallback,screen,sizeof(screen))); memcpy(wallpaper,screen,sizeof(screen));
    save_pgm(directory,"114-shutdown-wallpaper-jpeg");
    mock_utc = saved_utc; mock_clock_valid = true;
    draw_lock(true); assert(!memcmp(wallpaper,screen,sizeof(screen)));
    handle_tap(342,900); assert(s_view == VIEW_WALLPAPERS);
    assert(!strcmp(s_wallpaper_path,second));
    tap_row(0); assert(s_view == VIEW_LOCK_PREVIEW);
    save_pgm(directory,"115-shutdown-wallpaper-png");
    assert(folio_storage_wallpaper_load(selected,sizeof(selected)) && !strcmp(selected,first));
    // Removing the selection falls back to another available image; an empty
    // or corrupt folder uses the same static image regardless of RTC state.
    assert(unlink(first) == 0);
    unsigned legacy_before = legacy_wallpaper_draws;
    draw_lock(true);
    assert(folio_storage_wallpaper_load_legacy(selected,sizeof(selected)) && !strcmp(selected,second));
    assert(legacy_wallpaper_draws == legacy_before+1 && !strcmp(last_legacy_wallpaper,second));
    assert(memcmp(fallback,screen,sizeof(screen)));
    // Cached area reduction and legacy nearest-neighbor need not be identical.
    // Compare with the actual named legacy source instead, retaining all pixels
    // outside its rectangle (including the original shutdown badge).
    memcpy(wallpaper,screen,sizeof(screen));
    epd_fill_rect((EpdRect){32,32,620,1040},PAPER,fb());
    assert(folio_cover_draw_image(second,fb(),(EpdRect){32,32,620,1040}));
    assert(!memcmp(wallpaper,screen,sizeof(screen)));
    assert(unlink(second) == 0); draw_lock(true);
    assert(!memcmp(fallback,screen,sizeof(screen)));
    save_pgm(directory,"116-shutdown-wallpaper-missing");
    FILE* corrupt = fopen(first,"wb"); assert(corrupt);
    assert(fputs("invalid image",corrupt) >= 0 && fclose(corrupt) == 0);
    draw_lock(true); assert(!memcmp(fallback,screen,sizeof(screen)));
    handle_tap(342,900); assert(s_view == VIEW_WALLPAPERS);
    tap_row(0); assert(s_view == VIEW_ERROR && s_error_return == VIEW_WALLPAPERS);
    save_pgm(directory,"117-wallpaper-invalid-image");
    assert(unlink(first) == 0); tap_rect(done_rect());
    assert(s_view == VIEW_WALLPAPERS); tap_rect(back_rect());
    assert(s_view == VIEW_HOME_SETTINGS);
    free(fallback); free(wallpaper);
    s_settings = saved; mock_utc = saved_utc; mock_clock_valid = saved_valid;
    show_view(VIEW_HOME_SETTINGS,true);
}

// Original PNG artwork generated entirely on the disposable host card. No
// downloaded assets, Pillow, external encoders, or firmware resources involved.
static void wallpaper_png_u32(uint8_t bytes[4], uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24); bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8); bytes[3] = (uint8_t)value;
}
static void wallpaper_png_chunk(FILE* file, const char* type, const uint8_t* data, size_t size) {
    assert(size <= UINT32_MAX);
    uint8_t number[4]; wallpaper_png_u32(number, (uint32_t)size);
    assert(fwrite(number, 1, 4, file) == 4 && fwrite(type, 1, 4, file) == 4);
    if (size) assert(fwrite(data, 1, size, file) == size);
    uLong crc = crc32(0, (const Bytef*)type, 4);
    if (size) crc = crc32(crc, data, (uInt)size);
    wallpaper_png_u32(number, (uint32_t)crc);
    assert(fwrite(number, 1, 4, file) == 4);
}
static void fixture_wallpaper_png(const char* path, unsigned width, unsigned height, bool alternate) {
    size_t row = 1 + (size_t)width * 3, size = row * height;
    uint8_t* raw = malloc(size); assert(raw);
    for (unsigned y = 0; y < height; ++y) {
        raw[(size_t)y * row] = 0; // PNG filter None; RGB8, non-interlaced.
        for (unsigned x = 0; x < width; ++x) {
            unsigned shade = 244 - y * 80 / height;
            int dx = (int)x - (int)width * 3 / 4, dy = (int)y - (int)height / 4;
            if ((int64_t)dx*dx + (int64_t)dy*dy < (int64_t)width*width/100) shade = 120;
            if (y > height/2 + (unsigned)abs((int)x-(int)width/3)/2) shade = 104;
            if (y > height*3/4 - x/5) shade = 48 + (x/40 % 3)*16;
            if (alternate) shade = ((x*5/width + y*7/height) % 2) ? 208 : 64;
            size_t at = (size_t)y * row + 1 + (size_t)x * 3;
            raw[at] = (uint8_t)shade; raw[at+1] = (uint8_t)(shade - 12); raw[at+2] = (uint8_t)(shade - 24);
        }
    }
    uLongf compressed_size = compressBound((uLong)size);
    uint8_t* compressed = malloc(compressed_size); assert(compressed);
    assert(compress2(compressed, &compressed_size, raw, (uLong)size, Z_NO_COMPRESSION) == Z_OK);
    free(raw);
    FILE* file = fopen(path, "wb"); assert(file);
    assert(fwrite("\x89PNG\r\n\x1a\n", 1, 8, file) == 8);
    uint8_t header[13] = {0}; wallpaper_png_u32(header, width); wallpaper_png_u32(header+4, height);
    header[8] = 8; header[9] = 2;
    wallpaper_png_chunk(file, "IHDR", header, sizeof(header));
    wallpaper_png_chunk(file, "IDAT", compressed, compressed_size);
    wallpaper_png_chunk(file, "IEND", NULL, 0);
    assert(fclose(file) == 0); free(compressed);
}
static uint8_t* wallpaper_file_bytes(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0); long length = ftell(file); assert(length > 0);
    rewind(file); *size = (size_t)length;
    uint8_t* bytes = malloc(*size); assert(bytes);
    assert(fread(bytes, 1, *size, file) == *size && fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0); return bytes;
}
static const char* wallpaper_state_paths[] = {
    FOLIO_SD_ROOT "/.folio/wallpaper.a", FOLIO_SD_ROOT "/.folio/wallpaper.b",
    FOLIO_SD_ROOT "/.folio/wallpaper-cache.a", FOLIO_SD_ROOT "/.folio/wallpaper-cache.b"
};
typedef struct { uint8_t* bytes[4]; size_t sizes[4]; } wallpaper_disk_state_t;
static wallpaper_disk_state_t wallpaper_disk_snapshot(void) {
    wallpaper_disk_state_t snapshot = {0};
    for (unsigned i = 0; i < 4; ++i)
        snapshot.bytes[i] = wallpaper_file_bytes(wallpaper_state_paths[i], &snapshot.sizes[i]);
    return snapshot;
}
static void wallpaper_assert_disk_unchanged(const wallpaper_disk_state_t* snapshot) {
    for (unsigned i = 0; i < 4; ++i) {
        size_t size; uint8_t* actual = wallpaper_file_bytes(wallpaper_state_paths[i], &size);
        assert(size == snapshot->sizes[i] && !memcmp(actual, snapshot->bytes[i], size)); free(actual);
    }
}
static void wallpaper_disk_snapshot_free(wallpaper_disk_state_t* snapshot) {
    for (unsigned i = 0; i < 4; ++i) free(snapshot->bytes[i]);
    memset(snapshot, 0, sizeof(*snapshot));
}
static unsigned wallpaper_active_slot(void) {
    // Read only the stable record magic/generation prefix, not a private struct.
    uint32_t prefix[2][2];
    for (unsigned i = 0; i < 2; ++i) {
        FILE* file = fopen(wallpaper_state_paths[i], "rb"); assert(file);
        assert(fread(prefix[i], 1, sizeof(prefix[i]), file) == sizeof(prefix[i]) && fclose(file) == 0);
        assert(prefix[i][0] == UINT32_C(0x32505746));
    }
    return (int32_t)(prefix[1][1] - prefix[0][1]) > 0 ? 1 : 0;
}
static uint8_t* wallpaper_assert_cache(const uint8_t* expected) {
    uint8_t* pixels = NULL; unsigned width = 0, height = 0;
    assert(folio_storage_wallpaper_cache_load(&pixels, &width, &height));
    assert(pixels && width == 620 && height == 1040);
    if (expected) assert(!memcmp(pixels, expected, 310u * 1040u));
    return pixels;
}
static void wallpaper_assert_selected(const char* expected, const folio_settings_t* settings) {
    char selected[256];
    assert(!strcmp(s_wallpaper_path, expected) && access(expected, R_OK) == 0);
    assert(folio_storage_wallpaper_load(selected, sizeof(selected)) && !strcmp(selected, expected));
    assert(!memcmp(&s_settings, settings, sizeof(*settings)));
}
static void tap_wallpaper_path(const char* path) {
    assert(s_view == VIEW_WALLPAPERS);
    for (size_t i = 0; i < s_wallpaper_count; ++i) if (!strcmp(s_file_entries[i].path, path)) {
        tap_row((unsigned)i); return;
    }
    assert(!"wallpaper is absent from the actual visible list");
}
static void test_wallpaper_pending_shutdown(bool during_cache, bool preview) {
    // Separate address spaces preserve emergency_shutdown's one-way files_closed
    // latch and the original later PMU regressions. No threads/devices involved.
    fflush(stdout); pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        assert(!mock_shutdown_pending && s_font && FOLIO_OFF_STYLE(&s_settings) == 2);
        s_power_initialized = true; s_ui_task = xTaskGetCurrentTaskHandle();
        unsigned before = frames, syncs = card_sync_calls, replies = emergency_replies;
        unsigned suspends = power_suspend_calls, offs = voluntary_off_calls;
        unsigned reads = wallpaper_cache_reads, hits = wallpaper_cache_hits;
        unsigned decodes = wallpaper_decodes, legacy = legacy_wallpaper_draws;
        uint8_t* shown = malloc(sizeof(displayed)); assert(shown); memcpy(shown, displayed, sizeof(displayed));
        if (during_cache) shutdown_cache_checks = 12;
        else shutdown_draw_pixels = 5000;
        if (preview) { s_lock_preview_off = true; show_view(VIEW_LOCK_PREVIEW, true); }
        else suspend_device(true);
        assert(mock_shutdown_pending && shutdown_injections == 1);
        assert(!shutdown_cache_checks && !shutdown_draw_pixels);
        assert(frames == before && frames == frames_at_shutdown && !memcmp(shown, displayed, sizeof(displayed)));
        assert(emergency_replies == replies+1 && card_sync_calls == syncs+1);
        assert(power_suspend_calls == suspends && voluntary_off_calls == offs);
        assert(!s_font && !s_reader && !s_activity && !s_catalog);
        assert(wallpaper_cache_reads == reads+1 && wallpaper_cache_hits == hits+(during_cache ? 0u : 1u));
        assert(wallpaper_decodes == decodes && legacy_wallpaper_draws == legacy);
        emergency_shutdown();
        assert(emergency_replies == replies+2 && card_sync_calls == syncs+1 && frames == before);
        printf("Wallpaper PMU host fault injection PASS: %s / %s; zero extra presentations, emergency first\n",
            preview ? "preview" : "suspend", during_cache ? "real cache payload read" : "real cached-image pixel draw");
        free(shown); fflush(stdout); _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!mock_shutdown_pending && !shutdown_injections && s_font);
}
static void test_large_wallpaper_integration(const char* directory) {
    assert(s_view == VIEW_HOME_SETTINGS && s_font);
    folio_settings_t saved = s_settings;
    s_settings.keep_page_on_lock = (s_settings.keep_page_on_lock & 3u) | (2u << 2);
    draw_lock(true);
    uint8_t* fallback = malloc(sizeof(screen)); uint8_t* rendered = malloc(sizeof(screen));
    assert(fallback && rendered); memcpy(fallback, screen, sizeof(screen));
    const char* large = FOLIO_PICTURE_ROOT "/03-large-original.png";
    const char* broken = FOLIO_PICTURE_ROOT "/04-broken-image.png";
    const char* candidate = FOLIO_PICTURE_ROOT "/05-small-original.png";
    fixture_wallpaper_png(large, 1280, 1920, false);
    fixture_wallpaper_png(candidate, 160, 240, true);
    FILE* file = fopen(broken, "wb"); assert(file);
    assert(fputs("not a PNG: intentional corrupt candidate B", file) >= 0 && fclose(file) == 0);
    struct stat info; assert(stat(large, &info) == 0);
    assert(info.st_size > 7 * 1024 * 1024 && info.st_size < 32 * 1024 * 1024);
    assert(1280u * 1920u > 2000000u); // Independently exceeds both old cover limits.
    assert(!folio_cover_validate_file(large)); // Book-cover limits remain unchanged.
    printf("Large wallpaper fixture: 1280x1920 RGB8, zlib level 0, %lld bytes\n", (long long)info.st_size);
    unsigned decodes = wallpaper_decodes, legacy = legacy_wallpaper_draws, hits = wallpaper_cache_hits;
    handle_tap(342,884);
    assert(s_view == VIEW_WALLPAPERS && s_wallpaper_count == 3 && s_wallpaper_total == 3);
    assert(!strcmp(s_file_entries[0].path, large) && s_file_entries[0].bytes == (uint64_t)info.st_size);
    assert(wallpaper_decodes == decodes); // Listing does not decode all candidates.
    save_pgm(directory, "180-wallpaper-large-list");
    tap_wallpaper_path(large);
    assert(s_view == VIEW_LOCK_PREVIEW && s_lock_preview_off && FOLIO_OFF_STYLE(&s_settings) == 2);
    assert(wallpaper_decodes == decodes+1 && wallpaper_cache_hits == hits+1 && legacy_wallpaper_draws == legacy);
    folio_settings_t selected_settings = s_settings;
    wallpaper_assert_selected(large, &selected_settings);
    assert(!memcmp(&persisted_settings, &selected_settings, sizeof(selected_settings)));
    uint8_t* cached = wallpaper_assert_cache(NULL);
    assert(memcmp(fallback, screen, sizeof(screen))); memcpy(rendered, screen, sizeof(screen));
    for (unsigned y = 0; y < 1040; ++y) for (unsigned x = 0; x < 620; ++x) {
        uint8_t packed = cached[(size_t)y*310+x/2];
        assert(screen[(size_t)(y+32)*SCREEN_W+x+32] == ((x&1) ? (packed&15) : (packed>>4))*17);
    }
    save_pgm(directory, "181-wallpaper-large-preview");
    decodes = wallpaper_decodes;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        draw_lock(true); assert(!memcmp(rendered, screen, sizeof(screen)));
        assert(wallpaper_decodes == decodes && legacy_wallpaper_draws == legacy);
    }
    // Forget RAM selection and re-open through production disk load, not a stub.
    s_wallpaper_path[0] = 0; load_wallpapers(); wallpaper_assert_selected(large, &selected_settings);
    free(wallpaper_assert_cache(cached));
    show_view(VIEW_LOCK_PREVIEW, true); assert(!memcmp(rendered, screen, sizeof(screen)));
    assert(wallpaper_decodes == decodes);
    wallpaper_disk_state_t original = wallpaper_disk_snapshot();

    handle_tap(342,900); unsigned writes = settings_writes;
    tap_wallpaper_path(broken);
    assert(s_view == VIEW_ERROR && s_error_return == VIEW_WALLPAPERS && *s_error);
    assert(wallpaper_decodes == decodes+1 && settings_writes == writes);
    wallpaper_assert_selected(large, &selected_settings); wallpaper_assert_disk_unchanged(&original);
    free(wallpaper_assert_cache(cached));
    save_pgm(directory, "182-wallpaper-bad-image-keeps-selection-error");
    tap_rect(done_rect()); assert(s_view == VIEW_WALLPAPERS);
    wallpaper_assert_selected(large, &selected_settings);
    save_pgm(directory, "183-wallpaper-failed-choice-keeps-large-selected");
    draw_lock(true); assert(!memcmp(rendered, screen, sizeof(screen)));

    unsigned active = wallpaper_active_slot(), inactive = 1-active;
    const char* blocked = wallpaper_state_paths[2+inactive];
    char backup[512]; snprintf(backup, sizeof(backup), "%s.ui-test-backup", blocked);
    assert(access(backup, F_OK) != 0 && errno == ENOENT);
    // These resolved targets are fixed children of the disposable fake card.
    assert(!strncmp(blocked, FOLIO_SD_ROOT "/.folio/", strlen(FOLIO_SD_ROOT "/.folio/")));
    assert(rename(blocked, backup) == 0 && mkdir(blocked, 0700) == 0);
    show_view(VIEW_WALLPAPERS, true); decodes = wallpaper_decodes;
    tap_wallpaper_path(candidate);
    assert(s_view == VIEW_ERROR && s_error_return == VIEW_WALLPAPERS && *s_error);
    assert(wallpaper_decodes == decodes+1 && settings_writes == writes);
    wallpaper_assert_selected(large, &selected_settings); free(wallpaper_assert_cache(cached));
    save_pgm(directory, "184-wallpaper-cache-write-blocked-keeps-selection");
    assert(rmdir(blocked) == 0 && rename(backup, blocked) == 0);
    wallpaper_assert_disk_unchanged(&original);
    draw_lock(true); assert(!memcmp(rendered, screen, sizeof(screen)));

    // Cache publication precedes NVS: SD and RAM path change, but the old style
    // (and every other settings byte) remain unchanged on the NVS failure.
    show_view(VIEW_HOME_SETTINGS, true); handle_tap(342,796);
    assert(FOLIO_OFF_STYLE(&s_settings) == 1);
    folio_settings_t old_settings = s_settings;
    assert(!memcmp(&persisted_settings, &old_settings, sizeof(old_settings)));
    handle_tap(342,884); writes = settings_writes;
    fail_settings_write = true; tap_wallpaper_path(candidate); fail_settings_write = false;
    assert(s_view == VIEW_ERROR && s_error_return == VIEW_WALLPAPERS);
    assert(strstr(s_error, "壁纸已选择，关机样式尚未保存") && settings_writes == writes+1);
    wallpaper_assert_selected(candidate, &old_settings);
    assert(!memcmp(&persisted_settings, &old_settings, sizeof(old_settings)));
    uint8_t* partial = wallpaper_assert_cache(NULL); assert(memcmp(partial, cached, 310u*1040u)); free(partial);
    save_pgm(directory, "185-wallpaper-nvs-failure-sd-selected-style-unchanged");
    assert(folio_settings_save(&s_settings));
    assert(!memcmp(&persisted_settings, &old_settings, sizeof(old_settings))); // Later saves cannot leak style 2.
    tap_rect(done_rect()); tap_wallpaper_path(candidate);
    assert(s_view == VIEW_LOCK_PREVIEW && FOLIO_OFF_STYLE(&s_settings) == 2);
    assert(!memcmp(&s_settings, &persisted_settings, sizeof(s_settings)));
    handle_tap(342,900); tap_wallpaper_path(large); wallpaper_assert_selected(large, &selected_settings);
    assert(!memcmp(rendered, screen, sizeof(screen))); free(wallpaper_assert_cache(cached));
    for (unsigned mode = 0; mode < 4; ++mode)
        test_wallpaper_pending_shutdown((mode & 1u) == 0, (mode & 2u) == 0);

    // Corrupt actual payload bytes with the large source still present. Neither
    // expanded decode nor legacy whole-file decode may be attempted at shutdown.
    active = wallpaper_active_slot(); const char* active_cache = wallpaper_state_paths[2+active];
    file = fopen(active_cache, "r+b"); assert(file && fseek(file, -1, SEEK_END) == 0);
    int byte = fgetc(file); assert(byte != EOF && fseek(file, -1, SEEK_END) == 0);
    assert(fputc(byte ^ 0x10, file) != EOF && fclose(file) == 0);
    decodes = wallpaper_decodes; legacy = legacy_wallpaper_draws;
    uint8_t* invalid = NULL; unsigned width = 1, height = 1;
    assert(!folio_storage_wallpaper_cache_load(&invalid, &width, &height) && !invalid && !width && !height);
    draw_lock(true); assert(!memcmp(fallback, screen, sizeof(screen)));
    assert(wallpaper_decodes == decodes && legacy_wallpaper_draws == legacy);
    wallpaper_assert_selected(large, &selected_settings);
    save_pgm(directory, "186-wallpaper-corrupt-cache-safe-default");
    // Restore by a real user selection, then remove only its original source.
    load_wallpapers(); tap_wallpaper_path(large); free(wallpaper_assert_cache(cached));
    assert(unlink(large) == 0); decodes = wallpaper_decodes; legacy = legacy_wallpaper_draws;
    assert(!folio_storage_wallpaper_cache_load(&invalid, &width, &height) && !invalid);
    draw_lock(true); assert(!memcmp(fallback, screen, sizeof(screen)));
    // The named fallback is the remaining corrupt small B, never missing A or
    // another large decode. It fails safely instead of reusing a stale snapshot.
    assert(wallpaper_decodes == decodes && legacy_wallpaper_draws == legacy+1);
    assert(!strcmp(last_legacy_wallpaper, broken));
    save_pgm(directory, "187-wallpaper-missing-source-safe-default");
    assert(unlink(broken) == 0 && unlink(candidate) == 0);
    draw_lock(true); assert(!memcmp(fallback, screen, sizeof(screen)));
    assert(wallpaper_decodes == decodes && legacy_wallpaper_draws == legacy+1);
    wallpaper_disk_snapshot_free(&original); free(cached); free(rendered); free(fallback);
    s_settings = saved; s_wallpaper_path[0] = 0; show_view(VIEW_HOME_SETTINGS, true);
    puts("Large wallpaper UI integration PASS: visible >7MiB/2MP PNG, real click/cache/reload/repeat draw, bad image + real cache write failure preserve A, NVS partial success, corrupt/missing fallback, four isolated PMU cases");
}

static void test_activity_lock_failure(const char* directory) {
    assert(s_view == VIEW_READING && s_reader && s_activity && s_font);
    folio_activity_t* retained_activity = s_activity;
    folio_reader_t* retained_reader = s_reader;
    folio_font_t* retained_font = s_font;
    unsigned syncs = card_sync_calls, suspends = power_suspend_calls;
    advance_clock(10); activity_track(true, true);
    assert(folio_activity_dirty(s_activity));
    uint64_t total = folio_activity_total_seconds(s_activity);
    // Block the two activity files with directories while leaving ordinary
    // reader-progress writes available. This is a real filesystem failure.
    const char* slots[] = {FOLIO_SD_ROOT "/.folio/activity.a", FOLIO_SD_ROOT "/.folio/activity.b"};
    const char* backups[] = {FOLIO_SD_ROOT "/.folio/activity.a-test-backup", FOLIO_SD_ROOT "/.folio/activity.b-test-backup"};
    for (unsigned i = 0; i < 2; ++i) {
        assert(rename(slots[i], backups[i]) == 0);
        assert(mkdir(slots[i], 0700) == 0);
    }
    suspend_device(false);
    assert(s_view == VIEW_ERROR && s_error_return == VIEW_READING);
    assert(s_activity == retained_activity && s_reader == retained_reader && s_font == retained_font);
    assert(s_activity_save_failed && folio_activity_dirty(s_activity));
    assert(folio_activity_total_seconds(s_activity) >= total);
    assert(card_sync_calls == syncs && power_suspend_calls == suspends);
    save_pgm(directory, "37-home-lock-save-failure");
    for (unsigned i = 0; i < 2; ++i) {
        assert(rmdir(slots[i]) == 0);
        assert(rename(backups[i], slots[i]) == 0);
    }
    assert(activity_flush() && !s_activity_save_failed && !folio_activity_dirty(s_activity));
    tap_rect(done_rect()); assert(s_view == VIEW_READING && s_reader == retained_reader);
}
static void test_home_status(const char* directory) {
    folio_settings_t saved=s_settings;
    uint32_t utc=mock_utc;int64_t us=mock_us;
    bool valid=mock_clock_valid;
    show_view(VIEW_HOME,true);
    int64_t idle=s_last_activity;
    unsigned before=frames;
    mock_battery="81%+";
    refresh_home_clock();assert(frames==before);
    mock_us+=30000000;refresh_home_clock();
    assert(frames==before+1 && !strcmp(s_home_battery,"81%+") && s_last_activity==idle);
    save_pgm(directory,"119-home-charging");
    before=frames;mock_us+=30000000;refresh_home_clock();assert(frames==before);
    mock_clock_valid=false;mock_battery="--%";mock_us+=30000000;refresh_home_clock();
    assert(!strcmp(s_home_battery,"--%") && s_last_activity==idle);
    save_pgm(directory,"120-home-power-unknown");
    mock_battery="0%";mock_us+=30000000;refresh_home_clock();
    assert(!strcmp(s_home_battery,"0%") && s_last_activity==idle);
    save_pgm(directory,"121-home-zero-battery");
    mock_battery="100%+";mock_clock_valid=true;mock_utc=utc;s_settings.reserved[0]|=0x10;
    mock_us+=30000000;refresh_home_clock();assert(!strcmp(s_home_battery,"100%+"));
    save_pgm(directory,"122-home-full-battery-12h");
    // USB observation must not postpone idle lock, nor bypass private screens.
    idle=s_last_activity;
    FILE* capture=tmpfile();assert(capture);fflush(stdout);int saved_stdout=dup(STDOUT_FILENO);assert(saved_stdout>=0);
    assert(dup2(fileno(capture),STDOUT_FILENO)>=0);
    uint32_t observed_utc=mock_utc;int64_t observed_us=mock_us;
    debug_execute(&(folio_debug_command_t){.kind=DBG_STATUS,.id=7});fflush(stdout);
    assert(dup2(saved_stdout,STDOUT_FILENO)>=0);assert(close(saved_stdout)==0);rewind(capture);
    char status[2048],field[80];size_t used=fread(status,1,sizeof(status)-1,capture);status[used]=0;assert(fclose(capture)==0);
    printf("%s",status);
    snprintf(field,sizeof(field),"\"utc\":%lu,",(unsigned long)observed_utc);assert(strstr(status,field));
    snprintf(field,sizeof(field),"\"rtc_utc\":%lu,",(unsigned long)observed_utc);assert(strstr(status,field));
    // The host timer advances by 1 ms on each read; STATUS samples it twice.
    snprintf(field,sizeof(field),"\"monotonic_us\":%lld,",(long long)(observed_us+2000));assert(strstr(status,field));
    snprintf(field,sizeof(field),"\"timezone_minutes\":%d,",s_settings.timezone_minutes);assert(strstr(status,field));
    assert(strstr(status,"\"rtc_read_ok\":1,") && strstr(status,"\"rtc_read_us\":1000,") && strstr(status,"\"system_utc\":") && strstr(status,"\"clock12h\":1,"));
    assert(s_last_activity==idle && mock_utc==observed_utc && mock_us==observed_us+2000);
    debug_execute(&(folio_debug_command_t){.kind=DBG_SCREEN,.id=8});
    assert(s_debug_frame && s_last_activity==idle);
    debug_execute(&(folio_debug_command_t){.kind=DBG_CANCEL,.id=9});
    assert(!s_debug_frame && s_last_activity==idle);
    s_view=VIEW_WIFI_EDIT;
    debug_execute(&(folio_debug_command_t){.kind=DBG_SCREEN,.id=10});
    assert(!s_debug_frame && s_last_activity==idle);
    s_settings=saved;mock_utc=utc;mock_us=us;mock_clock_valid=valid;mock_battery="82%";
    show_view(VIEW_HOME,true);
    puts("Home battery: charging/unknown/zero/full/12h, throttling, unchanged idle and private USB guards passed");
}

static void test_idle_and_card_recovery(const char* font_path) {
    snprintf(s_settings.font_path,sizeof(s_settings.font_path),"%s",font_path);
    uint16_t idle=s_settings.idle_minutes;s_settings.idle_minutes=5;
    s_last_activity=mock_us;assert(!idle_lock_due(false));
    mock_us+=301000000;mock_wr.busy=true;
    assert(!idle_lock_due(false) && s_last_activity>=mock_us-1000);
    mock_wr.busy=false;assert(!idle_lock_due(false));
    mock_us+=301000000;assert(idle_lock_due(false));assert(!idle_lock_due(true));
    s_settings.idle_minutes=0;assert(!idle_lock_due(false));s_settings.idle_minutes=idle;
    // Async probing completes before font/library loading, not after an empty home.
    unsigned remounts=card_remount_calls;card_probe_reads=3;
    load_card(true,false);
    assert(s_card_ready && !s_card_probe_pending && s_book_count==12 && card_remount_calls==remounts+1);
    // The UI can recover a probe that finishes beyond its bounded boot wait.
    card_probe_reads=20000;load_card(true,false);
    assert(!s_card_ready && s_card_probe_pending && !s_book_count);
    refresh_card_probe();assert(s_card_probe_pending && !s_card_ready);
    card_probe_reads=0;refresh_card_probe();
    assert(s_card_ready && !s_card_probe_pending && s_book_count==12);
    // The actual rescan action remounts an unavailable card, not merely an empty index.
    remounts=card_remount_calls;s_card_ready=false;mock_card_unmounted=true;
    show_view(VIEW_DEVICE,true);tap_row(4);
    assert(s_view==VIEW_LIBRARY && s_card_ready && s_book_count==12 && card_remount_calls==remounts+1);
    puts("Idle busy guard and async card wait/late recovery/rescan remount passed");
}

static void test_idle_settings(const char* directory) {
    folio_settings_t saved = s_settings;
    view_t previous_view = s_view;
    unsigned previous_tab = s_settings_tab;
    s_settings.idle_minutes = 5; s_settings_tab = 1; show_view(VIEW_SETTINGS, false);
    assert(screen[496 * SCREEN_W + 380] == (RULE | (RULE >> 4)));
    unsigned writes = settings_writes;
    handle_tap(380, 496);
    assert(s_settings.idle_minutes == 5 && settings_writes == writes);
    save_pgm(directory, "160-idle-minimum-5");
    for (unsigned expected = 10; expected <= 30; expected += 5) {
        handle_tap(588, 496);
        assert(s_settings.idle_minutes == expected && settings_writes == ++writes);
    }
    assert(screen[496 * SCREEN_W + 588] == (RULE | (RULE >> 4)));
    handle_tap(588, 496);
    assert(s_settings.idle_minutes == 30 && settings_writes == writes);
    save_pgm(directory, "161-idle-maximum-30");
    for (unsigned expected = 25; expected >= 5; expected -= 5) {
        handle_tap(380, 496);
        assert(s_settings.idle_minutes == expected && settings_writes == ++writes);
    }
    fail_settings_write = true; handle_tap(588, 496);
    assert(s_view == VIEW_ERROR && s_settings.idle_minutes == 5);
    fail_settings_write = false;
    s_settings = saved; s_settings_tab = previous_tab; show_view(previous_view, false);
}

static void test_auto_shutdown_settings(const char* directory) {
    assert(s_view==VIEW_SETTINGS && s_settings_tab==1);
    assert(!s_settings.auto_shutdown_enabled && s_settings.auto_shutdown_minutes==30);
    unsigned writes=settings_writes;
    tap_rect(auto_shutdown_choice(0));
    assert(settings_writes==writes && s_settings.auto_shutdown_minutes==30);
    tap_rect(auto_shutdown_toggle());assert(s_settings.auto_shutdown_enabled && settings_writes==writes+1);
    const uint16_t expected[]={30,180,360};
    assert(sizeof(auto_shutdown_minutes)/sizeof(auto_shutdown_minutes[0])==3);
    for(unsigned i=0;i<3;++i) {
        assert(auto_shutdown_minutes[i]==expected[i]);
        tap_rect(auto_shutdown_choice(i));assert(s_settings.auto_shutdown_minutes==expected[i]);
    }
    writes=settings_writes;
    change_setting(15,-1);change_setting(15,3);change_setting(15,4);
    handle_tap(140,1004);handle_tap(340,1004); // Removed second row has no actions.
    assert(settings_writes==writes && s_settings.auto_shutdown_minutes==360);
    save_pgm(directory,"123-auto-shutdown-enabled");
    tap_rect(auto_shutdown_toggle());assert(!s_settings.auto_shutdown_enabled && s_settings.auto_shutdown_minutes==360);
    save_pgm(directory,"124-auto-shutdown-disabled");
    writes=settings_writes;tap_rect(auto_shutdown_choice(1));assert(settings_writes==writes);
    fail_settings_write=true;tap_rect(auto_shutdown_toggle());
    assert(s_view==VIEW_ERROR && !s_settings.auto_shutdown_enabled && s_settings.auto_shutdown_minutes==360);
    fail_settings_write=false;tap_rect(done_rect());tap_rect(auto_shutdown_toggle());
    fail_settings_write=true;tap_rect(auto_shutdown_choice(1));
    assert(s_view==VIEW_ERROR && s_settings.auto_shutdown_enabled && s_settings.auto_shutdown_minutes==360);
    fail_settings_write=false;tap_rect(done_rect());tap_rect(auto_shutdown_choice(0));tap_rect(auto_shutdown_toggle());
    assert(!s_settings.auto_shutdown_enabled && s_settings.auto_shutdown_minutes==30);
    puts("Auto shutdown UI: every duration, disabled hit guards, preserved selection and save rollback passed");
}

static void test_home(const char* directory, const char* font_path) {
    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", font_path);
    load_card(false, true);
    assert(s_view == VIEW_HOME && !s_reader && s_activity && !s_settings.auto_resume);
    assert(folio_activity_recent_count(s_activity) == 0);
    save_pgm(directory, "27-home-empty");
    unsigned before = frames;
    tap_nav(0); assert(s_view == VIEW_HOME && frames == before);
    assert(root_nav_hit(238, 1136) == -1 && root_nav_hit(442, 1136) == -1);
    assert(root_nav_hit(342, 1080) == -1 && root_nav_hit(342, 1119) == -1);
    assert(root_nav_hit(342, 1120) == 1 && root_nav_hit(342, 1215) == 1);
    assert(root_nav_hit(342, 1216) == -1);
    handle_tap(342, 306); assert(s_view == VIEW_LIBRARY); // Empty continue action.
    tap_nav(2); assert(s_view == VIEW_SYSTEM);
    tap_nav(0); assert(s_view == VIEW_HOME);
    tap_nav(1); assert(s_view == VIEW_LIBRARY);
    tap_nav(0); assert(s_view == VIEW_HOME);

    assert(folio_session_save(s_books[0].path));
    load_card(false, true);
    assert(s_view == VIEW_HOME && !s_reader && folio_activity_recent_count(s_activity) == 1);
    const folio_recent_book_t* migrated = folio_activity_recent(s_activity, 0);
    assert(migrated && !strcmp(migrated->path, session_path));
    assert(migrated->page == 0 && migrated->percent == 0 && migrated->position == 0);
    assert(folio_activity_total_seconds(s_activity) == 0 && folio_activity_undated_seconds(s_activity) == 0);
    save_pgm(directory, "38-home-migrated-session");

    fixture_activity();
    test_heatmap_calendar(directory);
    assert(folio_cover_draw(s_books[0].path, screen, (EpdRect){48,176,196,280}));
    assert(folio_cover_draw(s_books[1].path, screen, (EpdRect){280,176,112,168}));
    assert(folio_cover_draw(s_books[2].path, screen, (EpdRect){488,176,112,168}));
    assert(!folio_cover_draw(s_books[3].path, screen, (EpdRect){48,176,196,280}));
    hide_embedded_cover = true; folio_cover_clear_cache();
    show_view(VIEW_HOME, true); save_pgm(directory, "39-home-without-cover");
    hide_embedded_cover = false; folio_cover_clear_cache();
    show_view(VIEW_HOME, true); save_pgm(directory, "28-home-heatmap");
    before = frames; int64_t idle_before = s_last_activity;
    refresh_home_clock(); assert(frames == before);
    advance_clock(61); refresh_home_clock();
    assert(frames == before + 1 && s_last_activity == idle_before);
    refresh_home_clock(); assert(frames == before + 1);

    tap_nav(2); handle_tap(200, 232 + 3 * 112 + 48);
    assert(s_view == VIEW_HOME_SETTINGS);
    save_pgm(directory, "29-home-chart-setting");
    before = settings_writes;
    handle_tap(492,444);assert(s_settings.home_chart==1&&settings_writes==before+1);
    tap_rect(back_rect());assert(s_view==VIEW_SYSTEM);tap_nav(0);
    save_pgm(directory,"30-home-bars");
    tap_nav(2);tap_rect(system_row(3));assert(s_view==VIEW_HOME_SETTINGS);
    fail_settings_write=true;handle_tap(188,444);assert(s_settings.home_chart==0&&s_view==VIEW_ERROR);
    fail_settings_write=false;tap_rect(done_rect());assert(s_view==VIEW_HOME_SETTINGS);
    handle_tap(342,620);assert(FOLIO_LOCK_STYLE(&s_settings)==1);
    handle_tap(542,796);assert(FOLIO_OFF_STYLE(&s_settings)==2);
    handle_tap(188,976);assert(s_view==VIEW_LOCK_PREVIEW);save_pgm(directory,"98-lock-preview");
    handle_tap(340,900);assert(s_view==VIEW_HOME_SETTINGS);
    handle_tap(492,976);assert(s_view==VIEW_LOCK_PREVIEW);save_pgm(directory,"99-off-preview");
    handle_tap(340,900);assert(s_view==VIEW_HOME_SETTINGS);
    handle_tap(542,620);handle_tap(188,976);assert(s_view==VIEW_LOCK_PREVIEW);save_pgm(directory,"103-clock-lock-preview");
    handle_tap(342,900);handle_tap(142,620);
    test_lock_personalization(directory);
    test_large_wallpaper_integration(directory);
    tap_rect(back_rect());tap_nav(0);

    uint64_t total = folio_activity_total_seconds(s_activity);
    mock_clock_valid = false; refresh_home_clock();
    assert(s_home_clock_minute == 0 && folio_activity_total_seconds(s_activity) == total);
    save_pgm(directory, "31-home-unknown-time");
    mock_clock_valid = true; refresh_home_clock();
    s_activity_save_failed = true; show_view(VIEW_HOME, true);
    save_pgm(directory, "32-home-save-warning"); s_activity_save_failed = false;
    s_activity_load_failed = true; show_view(VIEW_HOME, true);
    save_pgm(directory, "33-home-load-warning");
    folio_activity_t* saved_activity = s_activity; s_activity = NULL;
    show_view(VIEW_HOME, true); save_pgm(directory, "36-home-history-unavailable");
    s_activity = saved_activity; s_activity_load_failed = false;

    char first_path[256], second_path[256], first_title[128];
    snprintf(first_path, sizeof(first_path), "%s", folio_activity_recent(s_activity, 0)->path);
    snprintf(second_path, sizeof(second_path), "%s", folio_activity_recent(s_activity, 1)->path);
    snprintf(first_title, sizeof(first_title), "%s", folio_activity_recent(s_activity, 0)->title);
    handle_tap(342, 306); assert(s_view == VIEW_READING && s_reader && !strcmp(s_current_book, first_path));
    before = frames; advance_clock(61); refresh_home_clock();
    assert(frames == before); // Clock cannot refresh the reading page.
    open_tools(); total = folio_activity_total_seconds(s_activity);
    advance_clock(90); tap_rect(done_rect()); assert(s_view == VIEW_READING);
    assert(folio_activity_total_seconds(s_activity) == total); // Menus do not accrue reading time.
    advance_clock(10); show_root(VIEW_HOME);
    assert(folio_activity_total_seconds(s_activity) >= total + 10);
    assert(folio_activity_total_seconds(s_activity) <= total + 11);
    tap_rect(home_recent_rect(1)); assert(s_view == VIEW_READING && !strcmp(s_current_book, second_path));
    show_root(VIEW_HOME);
    assert(!strcmp(folio_activity_recent(s_activity, 0)->path, second_path));
    assert(!strcmp(folio_activity_recent(s_activity, 1)->path, first_path));
    assert(folio_activity_recent_count(s_activity) == 3);
    tap_rect(home_recent_rect(2)); assert(s_view == VIEW_READING && strstr(s_current_book, "山间日记.txt"));
    show_root(VIEW_HOME); assert(folio_activity_recent_count(s_activity) == 3);

    assert(folio_activity_touch_book(s_activity, first_path,
        "沿着河岸慢慢走直到晚风吹过城市的另一端再回到那间亮着灯的小屋", 37, 17, 0));
    show_view(VIEW_HOME, true); save_pgm(directory, "34-home-long-title");
    assert(folio_activity_touch_book(s_activity, first_path, first_title, 37, 17, 0));
    char missing_path[300]; snprintf(missing_path, sizeof(missing_path), "%s.missing", first_path);
    assert(rename(first_path, missing_path) == 0);
    show_root(VIEW_HOME); save_pgm(directory, "35-home-missing-book");
    handle_tap(342, 306); assert(s_view == VIEW_ERROR && s_error_return == VIEW_HOME);
    assert(folio_activity_recent_count(s_activity) == 3 && !strcmp(folio_activity_recent(s_activity, 0)->path, first_path));
    tap_rect(done_rect()); assert(s_view == VIEW_HOME);
    assert(rename(missing_path, first_path) == 0);
    show_root(VIEW_HOME);
    handle_key(0, false); assert(s_view == VIEW_LIBRARY);
    tap_nav(0); handle_key(2, false); assert(s_view == VIEW_SYSTEM);
    tap_nav(0); handle_key(1, false); assert(s_view == VIEW_READING && !strcmp(s_current_book, first_path));
    show_root(VIEW_HOME);
    activity_flush(); load_card(false, true);
    assert(s_view == VIEW_HOME && !s_reader && folio_activity_recent_count(s_activity) == 3);
    s_settings.auto_resume = 1; load_card(false, true);
    assert(s_view == VIEW_READING && s_reader && !strcmp(s_current_book, first_path));
    test_activity_lock_failure(directory);
    show_root(VIEW_HOME); s_settings.auto_resume = 0;
    s_settings.font_path[0] = '\0';
}

static void test_transfer_records(const char* directory) {
    folio_transfer_status_t before=mock_transfer;
    mock_transfer.completed=4;mock_transfer.recent_count=FOLIO_TRANSFER_RECENT_CAP;
    const char* names[]={"封面 100% + 特别版.jpg","这是一部名字很长的中文书籍 用于验证截断不会遮挡结果.epub","重复的书.txt","给未来的信.txt","风景原图.heic"};
    for(unsigned i=0;i<FOLIO_TRANSFER_RECENT_CAP;++i) {
        folio_transfer_record_t* r=&mock_transfer.recent[i];
        snprintf(r->name,sizeof(r->name),"%s",names[i]);r->size=(i+1)*1048576;
        r->result=i==2?3:0;r->saved_only=i==0 || i==4;
        snprintf(r->message,sizeof(r->message),"%s",r->result?"同名文件已存在，未覆盖":r->saved_only?"图片已保存，显示以设备支持为准":"文件已保存");
    }
    show_view(VIEW_TRANSFER,false);save_pgm(directory,"162-transfer-history");
    mock_transfer.busy=true;mock_transfer.total=100;mock_transfer.received=50;
    show_view(VIEW_TRANSFER,false);save_pgm(directory,"163-transfer-progress");
    mock_transfer.received=100;show_view(VIEW_TRANSFER,false);save_pgm(directory,"164-transfer-validating");
    mock_transfer.busy=false;mock_transfer.sta_connected=true;
    snprintf(mock_transfer.sta_url,sizeof(mock_transfer.sta_url),"http://192.168.1.112/");
    show_view(VIEW_TRANSFER,false);tap_rect(transfer_address_rect());
    assert(s_view==VIEW_TRANSFER && s_transfer_tab==1 && mock_transfer.running);
    save_pgm(directory,"165-transfer-lan-history");
    unsigned old_frames=frames;
    snprintf(mock_transfer.recent[0].name,sizeof(mock_transfer.recent[0].name),"新失败的文件.jpg");
    mock_transfer.recent[0].result=3;
    advance_clock(3);refresh_transfer_status();
    assert(frames==old_frames+1 && !strcmp(s_transfer_status.recent[0].name,"新失败的文件.jpg"));
    mock_transfer.recent_count=255;show_view(VIEW_TRANSFER,false); // Clamp untrusted count to fixed array capacity.
    fail_transfer_stop=true;tap_rect(transfer_done_rect());
    assert(s_view==VIEW_TRANSFER && s_transfer_tab==1 && mock_transfer.running);
    fail_transfer_stop=false;tap_rect(transfer_done_rect());
    assert(s_view==VIEW_DEVICE && !mock_transfer.running);
    enter_transfer();assert(s_view==VIEW_TRANSFER && mock_transfer.running);
    mock_transfer=before;s_transfer_tab=0;show_view(VIEW_TRANSFER,false);
}

static void test_management(const char* directory,const char* font_path) {
    snprintf(s_settings.font_path,sizeof(s_settings.font_path),"%s",font_path);
    show_root(VIEW_SYSTEM);tap_rect(system_row(5));assert(s_view==VIEW_DEVICE);
    save_pgm(directory,"43-device-tools");
    show_root(VIEW_LIBRARY);tap_rect(library_tool(0));assert(s_view==VIEW_CATALOG);save_pgm(directory,"44-catalog-search");
    snprintf(s_search,sizeof(s_search),"yiye");s_catalog_options.filter=FOLIO_CATALOG_ALL;
    tap_rect(done_rect());assert(s_view==VIEW_LIBRARY && s_shelf_count==1);
    size_t selected=s_shelf_indexes[0];tap_rect(library_item(0));
    assert(s_reader && !strcmp(s_current_book,s_books[selected].path));
    show_root(VIEW_LIBRARY);s_search[0]='\0';s_catalog_options.filter=FOLIO_CATALOG_UNREAD;catalog_select();
    for(size_t i=0;i<s_shelf_count;++i)assert(!folio_catalog_metadata(s_catalog,s_books[s_shelf_indexes[i]].path)->read);
    s_catalog_options.filter=FOLIO_CATALOG_ALL;catalog_select();
    show_root(VIEW_SYSTEM);tap_rect(system_row(5));handle_tap(200,288);
    assert(s_view==VIEW_TRANSFER && mock_transfer.running);save_pgm(directory,"45-transfer");
    test_transfer_records(directory);
    mock_transfer.busy=true;mock_transfer.total=100;mock_transfer.received=50;advance_clock(3);
    refresh_transfer_status();assert(s_transfer_status.received==50);
    fail_transfer_stop=true;unsigned sync_before=card_sync_calls;
    handle_key(1,false);assert(s_view==VIEW_TRANSFER && mock_transfer.running);
    assert(!prepare_card_change() && s_font && card_sync_calls==sync_before);
    fail_transfer_stop=false;handle_key(1,false);assert(s_view==VIEW_DEVICE && !mock_transfer.running);
    enter_storage();if(s_view!=VIEW_STORAGE)fprintf(stderr,"storage UI: %s\n",s_error);
    assert(s_view==VIEW_STORAGE);save_pgm(directory,"46-storage");
    tap_rect(done_rect());assert(s_view==VIEW_FORMAT_CONFIRM && !s_format_armed && format_calls==0);
    save_pgm(directory,"47-format-warning");tap_rect(done_rect());assert(s_view==VIEW_FORMAT_FINAL && s_format_armed && format_calls==0);
    save_pgm(directory,"48-format-confirm");handle_tap(342,1032);assert(s_view==VIEW_STORAGE && !s_format_armed && !format_calls);
    FILE* file=fopen(FOLIO_SD_ROOT "/delete-test.txt","wb");assert(file);assert(fputs("original",file)>=0 && fclose(file)==0);
    enter_file_directory(FOLIO_SD_ROOT);assert(s_view==VIEW_FILES);save_pgm(directory,"49-files");
    for(size_t i=0;i<s_file_count;++i)if(s_file_entries[i].directory) {
        const char* basename=strrchr(s_file_entries[i].path,'/');
        assert(basename && !strcmp(s_file_entries[i].name,basename+1));
    }
    size_t visible_total=s_file_total;
    handle_tap(540,140);assert(s_files_show_internal && s_file_total>visible_total);
    save_pgm(directory,"91-files-all");handle_tap(540,140);assert(!s_files_show_internal && s_file_total==visible_total);
    size_t found=SIZE_MAX;for(size_t i=0;i<s_file_count;++i)if(!strcmp(s_file_entries[i].name,"delete-test.txt"))found=i;
    assert(found!=SIZE_MAX);handle_tap(580,240+(int)found*108+48);assert(s_view==VIEW_FILE_ACTION);
    save_pgm(directory,"50-file-details");tap_rect(done_rect());assert(s_view==VIEW_DELETE_CONFIRM);
    save_pgm(directory,"51-delete-confirm");handle_tap(342,1032);assert(s_view==VIEW_FILE_ACTION && access(FOLIO_SD_ROOT "/delete-test.txt",F_OK)==0);
    tap_rect(done_rect());file=fopen(FOLIO_SD_ROOT "/delete-test.txt","ab");assert(file);assert(fputs("changed",file)>=0 && fclose(file)==0);
    tap_rect(done_rect());assert(s_view==VIEW_ERROR && access(FOLIO_SD_ROOT "/delete-test.txt",F_OK)==0);
    enter_file_directory(FOLIO_SD_ROOT);s_file_selected=s_file_entries[found];show_view(VIEW_FILE_ACTION,true);
    tap_rect(done_rect());
    unsigned delete_frames=frames;folio_font_t* delete_font=s_font;folio_activity_t* delete_activity=s_activity;
    tap_rect(done_rect());assert(s_view==VIEW_FILES && access(FOLIO_SD_ROOT "/delete-test.txt",F_OK)!=0);
    assert(frames==delete_frames+1 && s_font==delete_font && s_activity==delete_activity);
    enter_update();assert(s_view==VIEW_UPDATE && !s_update_local);
    handle_tap(490,268);assert(s_update_local && s_update_ready);save_pgm(directory,"52-update");
    go_back();assert(s_view==VIEW_DEVICE && update_calls==0 && restart_calls==0);
    show_root(VIEW_HOME);s_settings.font_path[0]='\0';
}
static void wr_state(folio_weread_state_t state,const char* message) {
    mock_wr.state=state;++mock_wr.revision;mock_wr.busy=false;
    snprintf(mock_wr.message,sizeof(mock_wr.message),"%s",message);
    show_view(VIEW_WEREAD,false);
}
static void wr_command_is(folio_weread_action_t action,unsigned previous,size_t index) {
    assert(wr_commands==previous+1 && wr_last_action==action && wr_last_index==index);
}
static void wr_refresh(void) {advance_clock(1);refresh_weread();}
static uint32_t fixture_weread_book(void) {
    const folio_weread_chapter_t chapters[]={
        {.uid="ch-01",.title="第一章 清晨的河岸"},
        {.uid="ch-02",.title="第二章 慢慢走回家"}};
    char error[192];
    folio_weread_cache_writer_t* writer=folio_weread_cache_begin("ui-preview","云上阅读笔记","林舟",chapters,2,error,sizeof(error));
    assert(writer);
    const char* sentence="清晨沿着河岸散步，风吹过树梢。读到喜欢的文字时，我们可以慢一点，再慢一点。\n";
    char original[16000]={0};
    for(unsigned i=0;i<60;++i)strcat(original,sentence);
    for(unsigned i=0;i<2;++i)assert(folio_weread_cache_write_chapter(writer,i,(const uint8_t*)original,strlen(original),FOLIO_WEREAD_TEXT,error,sizeof(error)));
    assert(folio_weread_cache_publish(writer,mock_wr_path,sizeof(mock_wr_path),error,sizeof(error)));
    folio_weread_cache_close(writer);
    char exported[256];assert(folio_weread_cache_export(mock_wr_path,exported,sizeof(exported),error,sizeof(error)));
    snprintf(mock_wr_path,sizeof(mock_wr_path),"%s",exported);
    folio_book_t book={0};snprintf(book.path,sizeof(book.path),"%s",mock_wr_path);
    folio_reader_options_t options=reader_options();
    folio_reader_t* reader=folio_reader_open_with_options(&book,s_font,&options);assert(reader);
    assert(folio_reader_next(reader));uint32_t position=folio_reader_position(reader);assert(position>0);
    assert(folio_reader_seek(reader,0) && folio_reader_save(reader));folio_reader_close(reader);
    folio_weread_position_t cloud;uint32_t restored;
    assert(folio_weread_cache_local_to_cloud(mock_wr_path,position,&cloud,error,sizeof(error)));
    assert(folio_weread_cache_cloud_to_local(mock_wr_path,&cloud,&restored,error,sizeof(error)) && restored==position);
    return position;
}
static void test_weread(const char* directory,const char* font_path) {
    snprintf(s_settings.font_path,sizeof(s_settings.font_path),"%s",font_path);
    show_root(VIEW_SYSTEM);
    unsigned start=wr_starts;tap_rect(system_row(0));
    assert(s_view==VIEW_WEREAD && wr_starts==start+1 && !mock_wr.running);
    save_pgm(directory,"56-weread-network-setup");
    unsigned initial_commands=wr_commands;
    handle_tap(188,1032);assert(s_view==VIEW_WEREAD && !s_wr_confirm && wr_commands==initial_commands);
    tap_rect(back_rect());assert(s_view==VIEW_SYSTEM && !mock_transfer.running);
    enter_wifi();assert(s_view==VIEW_WIFI && s_wifi_return==VIEW_SYSTEM);
    mock_network.has_credentials=mock_network.clock_valid=true;
    mock_network.running=mock_network.connected=true;unsigned stops_before_handoff=wr_stops;
    tap_rect(back_rect());assert(s_view==VIEW_SYSTEM);
    tap_rect(system_row(0));assert(s_view==VIEW_WEREAD && !mock_transfer.running && mock_wr.state==FOLIO_WR_LOGIN);
    assert(mock_network.connected && wr_stops==stops_before_handoff);
    save_pgm(directory,"57-weread-qr-login");
    unsigned commands=wr_commands;tap_rect(done_rect());wr_command_is(FOLIO_WR_NEW_QR,commands,0);
    int64_t qr_idle=s_last_activity;++mock_wr.revision;wr_refresh();assert(s_last_activity==qr_idle);

    wr_state(FOLIO_WR_OTP,"请在手机查看4位数字验证码，然后在下方输入。");
    assert(screen[1096*SCREEN_W+342]==(RULE|(RULE>>4)));save_pgm(directory,"125-weread-otp-empty");
    unsigned otp_full=full_frames;
    commands=wr_commands;tap_rect(done_rect());assert(wr_commands==commands);
    for(unsigned i=0;i<3;++i){tap_rect(wr_digit(i));tap_rect(done_rect());assert(wr_commands==commands && strlen(s_wr_otp)==i+1 && screen[1096*SCREEN_W+342]==(RULE|(RULE>>4)));}
    save_pgm(directory,"126-weread-otp-three");
    unsigned otp_regions=region_frames;tap_rect(wr_digit(3));
    assert(full_frames==otp_full && region_frames==otp_regions+2 && screen[1096*SCREEN_W+342]==0 && last_region.y==1096);
    tap_rect(wr_digit(4));tap_rect(wr_digit(10));assert(!strcmp(s_wr_otp,"1234"));
    save_pgm(directory,"58-weread-otp");
    tap_rect(wr_digit(11));assert(!strcmp(s_wr_otp,"123") && screen[1096*SCREEN_W+342]==(RULE|(RULE>>4)) && full_frames==otp_full);
    tap_rect(done_rect());assert(wr_commands==commands);tap_rect(wr_digit(3));
    tap_rect(done_rect());wr_command_is(FOLIO_WR_SUBMIT_OTP,commands,0);
    assert(!strcmp(wr_last_value,"1234") && !*s_wr_otp);
    for(unsigned i=0;i<6;++i)tap_rect(wr_digit(0));assert(strlen(s_wr_otp)==4);
    tap_rect(wr_digit(9));assert(!*s_wr_otp);
    tap_rect(wr_digit(10));assert(!strcmp(s_wr_otp,"0"));commands=wr_commands;
    tap_rect(back_rect());wr_command_is(FOLIO_WR_NEW_QR,commands,0);assert(!*s_wr_otp && s_view==VIEW_WEREAD && mock_wr.running);

    mock_wr.logged_in=true;mock_wr.count=13;mock_wr.item_count=6;mock_wr.page=0;
    const char* titles[]={"云上阅读笔记","沿着河岸慢慢走直到晚风吹过城市的另一端","一页微光","南方来信","山间日记","慢行笔记"};
    for(unsigned i=0;i<6;++i){snprintf(mock_wr.items[i].title,sizeof(mock_wr.items[i].title),"%s",titles[i]);
        snprintf(mock_wr.items[i].subtitle,sizeof(mock_wr.items[i].subtitle),"%s · %s",i%2?"林舟":"许安",i==0?"已有离线章节":"可下载阅读");mock_wr.items[i].cached=i==0;}
    wr_state(FOLIO_WR_SHELF,"选择书籍后查看章节，下载完成后可离线阅读。");assert(wr_can_relogin());save_pgm(directory,"59-weread-cloud-shelf");
    commands=wr_commands;tap_rect(wr_row(1));wr_command_is(FOLIO_WR_SELECT_BOOK,commands,1);assert(s_wr_book_index==1);
    commands=wr_commands;handle_tap(588,1140);wr_command_is(FOLIO_WR_PAGE,commands,1);
    mock_wr.page=1;show_view(VIEW_WEREAD,false);commands=wr_commands;tap_rect(wr_row(2));wr_command_is(FOLIO_WR_SELECT_BOOK,commands,8);
    commands=wr_commands;handle_tap(96,1140);wr_command_is(FOLIO_WR_PAGE,commands,0);
    tap_rect(wr_account(1));assert(s_wr_confirm==3);save_pgm(directory,"60-weread-logout-confirm");
    tap_rect(back_rect());assert(!s_wr_confirm && s_view==VIEW_WEREAD);
    tap_rect(wr_account(1));commands=wr_commands;tap_rect(done_rect());wr_command_is(FOLIO_WR_LOGOUT,commands,0);
    tap_rect(wr_account(0));assert(s_wr_confirm==5);save_pgm(directory,"103-weread-relogin-confirm");
    commands=wr_commands;tap_rect(done_rect());wr_command_is(FOLIO_WR_RELOGIN,commands,0);

    mock_wr_position=fixture_weread_book();
    snprintf(mock_wr.local_path,sizeof(mock_wr.local_path),"%s",mock_wr_path);
    snprintf(mock_wr.title,sizeof(mock_wr.title),"云上阅读笔记");mock_wr.page=0;mock_wr.count=8;mock_wr.item_count=6;mock_wr.can_read=true;
    for(unsigned i=0;i<6;++i){snprintf(mock_wr.items[i].title,sizeof(mock_wr.items[i].title),"第%u章 %s",i+1,i%2?"慢慢走回家":"清晨的河岸");
        snprintf(mock_wr.items[i].subtitle,sizeof(mock_wr.items[i].subtitle),"%s",i<2?"已缓存 · 可离线阅读":"尚未缓存");mock_wr.items[i].cached=i<2;}
    wr_state(FOLIO_WR_DOWNLOAD_ERROR,"第 1 章：无法连接微信读书服务器，请检查网络后重试。");
    save_pgm(directory,"90-weread-download-error");commands=wr_commands;
    tap_rect(wr_right(1096));wr_command_is(FOLIO_WR_RETRY_DOWNLOAD,commands,0);
    commands=wr_commands;tap_rect(wr_left(1096));wr_command_is(FOLIO_WR_RETURN_TOC,commands,0);
    commands=wr_commands;tap_rect(back_rect());wr_command_is(FOLIO_WR_RETURN_TOC,commands,0);
    wr_state(FOLIO_WR_BOOK,"已缓存 2 / 8 章。点选章节可下载或更新。");assert(!wr_can_relogin());save_pgm(directory,"61-weread-chapters");
    commands=wr_commands;tap_rect(wr_row(3));wr_command_is(FOLIO_WR_DOWNLOAD_CHAPTER,commands,3);
    commands=wr_commands;handle_tap(540,1032);wr_command_is(FOLIO_WR_DOWNLOAD_ALL,commands,0);
    commands=wr_commands;handle_tap(342,1032);wr_command_is(FOLIO_WR_CHECK_SYNC,commands,0);
    mock_wr.completed=3;mock_wr.total=8;mock_wr.busy=true;mock_wr.state=FOLIO_WR_WORK;++mock_wr.revision;
    snprintf(mock_wr.message,sizeof(mock_wr.message),"正在下载第 4 章。已完成的章节会保留，取消后可继续下载。");
    int64_t idle=s_last_activity;unsigned before=frames;wr_refresh();assert(frames==before+1 && s_last_activity>idle);
    idle=s_last_activity;++mock_wr.revision;wr_refresh();assert(s_last_activity>idle);
    before=frames;wr_refresh();assert(frames==before+1);
    before=frames;wr_refresh();assert(frames==before);save_pgm(directory,"62-weread-downloading");
    commands=wr_commands;tap_rect(wr_row(1));assert(wr_commands==commands);
    assert(!wr_can_relogin());
    tap_rect((EpdRect){48,984,588,96});assert(!s_wr_confirm && wr_commands==commands);
    // A state can remain WORK while busy clears; the hidden control stays inert.
    mock_wr.busy=false;show_view(VIEW_WEREAD,false);
    tap_rect((EpdRect){48,984,588,96});assert(!s_wr_confirm && wr_commands==commands);

    mock_wr.cloud_total_seconds=7200;mock_wr.cloud_time_available=true;
    mock_wr.sync_local=mock_wr.sync_cloud=true;mock_wr.pending_seconds=128;mock_wr.uncertain_seconds=60;
    snprintf(mock_wr.local_position,sizeof(mock_wr.local_position),"第 2 章 · 第 5 页 · 37%%");
    snprintf(mock_wr.cloud_position,sizeof(mock_wr.cloud_position),"第 3 章 · 手机端更新于刚才");
    wr_state(FOLIO_WR_SYNC,"请核对两端位置，再选择同步方向。没有确认时不会修改云端。");assert(!wr_can_relogin());save_pgm(directory,"63-weread-sync");
    uint64_t active_before=folio_activity_total_seconds(s_activity);advance_clock(20);activity_track(false,false);
    assert(folio_activity_total_seconds(s_activity)==active_before);
    commands=wr_commands;tap_rect(done_rect());wr_command_is(FOLIO_WR_CHECK_SYNC,commands,0);commands=wr_commands;
    handle_tap(342,911);handle_tap(342,1008);assert(!s_wr_confirm && wr_commands==commands);
    handle_tap(342,912);assert(s_wr_confirm==1);tap_rect(back_rect());
    handle_tap(342,1007);assert(s_wr_confirm==1);tap_rect(back_rect());
    handle_tap(342,960);assert(s_wr_confirm==1);save_pgm(directory,"64-weread-time-enable-confirm");
    tap_rect(done_rect());wr_command_is(FOLIO_WR_ENABLE_TIME,commands,0);
    mock_wr.time_enabled=true;show_view(VIEW_WEREAD,false);save_pgm(directory,"65-weread-time-enabled");
    tap_rect(wr_left(804));assert(s_wr_confirm==4);save_pgm(directory,"67-weread-position-upload-confirm");
    commands=wr_commands;tap_rect(done_rect());wr_command_is(FOLIO_WR_UPLOAD_SYNC,commands,0);
    commands=wr_commands;handle_tap(342,960);wr_command_is(FOLIO_WR_DISABLE_TIME,commands,0);
    commands=wr_commands;tap_rect(wr_right(804));wr_command_is(FOLIO_WR_USE_CLOUD,commands,0);

    // A cloud handoff must remain pending while the worker still owns the SD.
    fail_weread_stop=true;mock_wr_handoff=true;folio_font_t* font=s_font;
    before=frames;idle=s_last_activity;
    wr_refresh();assert(s_view==VIEW_WEREAD && s_wr_leaving && s_wr_open_pending && !s_reader);
    assert(frames==before+1 && s_last_activity==idle);
    assert(s_wr_open_position==mock_wr_position && !strcmp(s_wr_open_path,mock_wr_path));
    save_pgm(directory,"68-weread-closing");before=frames;wr_refresh();assert(frames==before);
    commands=wr_commands;tap_rect(wr_left(1096));assert(wr_commands==commands && !mock_transfer.running);
    assert(!prepare_card_change() && s_font==font && !s_reader);
    unsigned formats=format_calls;
    s_format_armed=true;format_card();assert(format_calls==formats && s_font==font && s_card_ready);
    assert(s_view==VIEW_WEREAD && s_wr_open_pending && s_wr_open_position==mock_wr_position);
    fail_weread_stop=false;wr_refresh();assert(s_view==VIEW_READING && s_reader && !s_wr_open_pending && !s_wr_leaving);
    assert(!strcmp(s_current_book,mock_wr_path) && folio_reader_position(s_reader)==mock_wr_position);
    save_pgm(directory,"69-weread-offline-reader");
    // Direct sync closes the reader only after recording its position, and
    // cancellation drains the worker before restoring that exact book/page.
    bool prior_cover=s_reader_cover;uint32_t prior_position=folio_reader_position(s_reader);
    open_tools();tap_rect(tool_rect(3));assert(s_view==VIEW_WEREAD && !s_reader);
    assert(!strcmp(mock_wr.local_path,mock_wr_path) && !strcmp(s_wr_reader_path,mock_wr_path));
    fail_weread_stop=true;tap_rect(back_rect());assert(s_wr_open_pending && s_wr_leaving);
    fail_weread_stop=false;wr_refresh();assert(s_reader && s_view==VIEW_READING);
    assert(folio_reader_position(s_reader)==prior_position && s_reader_cover==prior_cover && !s_wr_reader_path[0]);

    show_root(VIEW_DEVICE);enter_weread();mock_wr.can_read=true;
    snprintf(mock_wr.local_path,sizeof(mock_wr.local_path),"%s",mock_wr_path);
    wr_state(FOLIO_WR_BOOK,"已下载的章节可离线阅读。");
    fail_weread_stop=true;handle_tap(142,1032);assert(s_wr_open_pending && !s_reader && s_view==VIEW_WEREAD);
    assert(s_wr_open_position==UINT32_MAX);fail_weread_stop=false;wr_refresh();
    assert(s_view==VIEW_READING && folio_reader_position(s_reader)==mock_wr_position);

    show_root(VIEW_DEVICE);enter_weread();mock_wr.running=true;
    wr_state(FOLIO_WR_ERROR,"网络请求未完成。已下载章节和本地阅读进度仍保留，请检查 Wi-Fi 后重试。");
    save_pgm(directory,"70-weread-error");
    assert(!wr_can_relogin());commands=wr_commands;
    tap_rect((EpdRect){48,984,588,96});assert(!s_wr_confirm && wr_commands==commands);
    mock_network.connected=true;
    tap_rect(done_rect());wr_command_is(FOLIO_WR_RETRY,commands,0);
    mock_wr.running=false;unsigned prior_starts=wr_starts;commands=wr_commands;
    tap_rect(done_rect());assert(wr_starts==prior_starts+1 && mock_wr.running && wr_commands==commands);
    // Only an expired session presents relogin on the error page.
    mock_wr.login_required=true;
    wr_state(FOLIO_WR_ERROR,"登录已过期，请重新登录。");assert(wr_can_relogin());
    save_pgm(directory,"118-weread-session-expired");
    commands=wr_commands;tap_rect((EpdRect){48,984,588,96});assert(!s_wr_confirm && wr_commands==commands);
    tap_rect(done_rect());assert(s_wr_confirm==5);
    tap_rect(done_rect());wr_command_is(FOLIO_WR_RELOGIN,commands,0);
    mock_wr.running=false;prior_starts=wr_starts;commands=wr_commands;
    tap_rect(done_rect());assert(s_wr_confirm==5);tap_rect(done_rect());
    assert(wr_starts==prior_starts+1 && mock_wr.running);wr_command_is(FOLIO_WR_RELOGIN,commands,0);
    mock_wr.login_required=false;
    wr_state(FOLIO_WR_CONNECTING,"正在连接 Wi-Fi，请稍候。");mock_wr.busy=true;
    show_view(VIEW_WEREAD,false);save_pgm(directory,"71-weread-connecting");
    commands=wr_commands;assert(!wr_can_relogin());
    tap_rect((EpdRect){48,984,588,96});assert(!s_wr_confirm && wr_commands==commands);
    mock_wr.busy=false;show_view(VIEW_WEREAD,false);
    tap_rect((EpdRect){48,984,588,96});assert(!s_wr_confirm && wr_commands==commands);
    fail_weread_stop=true;tap_rect(done_rect());assert(s_wr_leaving && s_wr_target==VIEW_SYSTEM && s_view==VIEW_WEREAD);
    fail_weread_stop=false;wr_refresh();assert(s_view==VIEW_SYSTEM && !s_wr_leaving && !mock_wr.running);
    // A short power press while asynchronous network I/O drains must survive
    // until the UI loop retries it. The mock PMU then returns a recoverable error.
    enter_weread();fail_weread_stop=true;
    unsigned suspends=power_suspend_calls,syncs=card_sync_calls;
    suspend_device(false);assert(s_suspend_pending && !s_suspend_power_off && power_suspend_calls==suspends);
    suspend_device(s_suspend_power_off);assert(s_suspend_pending && card_sync_calls==syncs && s_font);
    fail_weread_stop=false;
    if(s_suspend_pending)suspend_device(s_suspend_power_off);
    assert(!s_suspend_pending && power_suspend_calls==suspends+1 && card_sync_calls==syncs && s_font);
    if(s_suspend_pending)suspend_device(s_suspend_power_off);
    assert(power_suspend_calls==suspends+1 && !mock_wr.running);
    assert(unlink(mock_wr_path)==0);show_root(VIEW_HOME);assert(s_book_count==12);s_settings.font_path[0]=0;
}
static void test_setup(const char* directory) {
    folio_settings_t saved=s_settings;uint32_t saved_utc=mock_utc;
    enter_wifi();assert(s_view==VIEW_WIFI && s_wifi_scanning);
    s_wifi_scanning=false;s_wifi_count=2;
    s_wifi_items[0]=(folio_network_ap_t){.ssid="Folio Home",.secured=true};
    s_wifi_items[1]=(folio_network_ap_t){.ssid="访客网络",.secured=false};
    snprintf(s_setup_message,sizeof(s_setup_message),"选择网络后输入密码");
    show_view(VIEW_WIFI,false);save_pgm(directory,"72-wifi-networks");
    tap_rect(wifi_row(0));assert(s_view==VIEW_WIFI_EDIT && !s_wifi_manual);
    unsigned input_full=full_frames,input_regions=region_frames;
    for(unsigned i=0;i<12;++i)tap_rect(catalog_key(0));
    assert(full_frames==input_full && region_frames==input_regions+12 && last_region.y==264 && last_region.height==144);
    tap_rect(catalog_key(28));tap_rect(catalog_key(0));assert(!strcmp(s_wifi_password,"q"));
    tap_rect(catalog_key(30));tap_rect(catalog_key(0));assert(!strcmp(s_wifi_password,"qQ"));
    tap_rect(catalog_key(27));assert(!strcmp(s_wifi_password,"q"));
    tap_rect(catalog_key(29));assert(s_wifi_keys==2);tap_rect(catalog_key(0));
    assert(!strcmp(s_wifi_password,"q1"));
    tap_rect(catalog_key(30));assert(s_wifi_keys==3);tap_rect(catalog_key(0));
    assert(!strcmp(s_wifi_password,"q1["));
    show_view(VIEW_WIFI_EDIT,false);save_pgm(directory,"80-wifi-symbols");
    tap_rect(catalog_key(29));assert(s_wifi_keys==0);
    snprintf(s_wifi_password,sizeof(s_wifi_password),"Folio home 123!");show_view(VIEW_WIFI_EDIT,false);
    save_pgm(directory,"73-wifi-password");
    snprintf(s_wifi_password,sizeof(s_wifi_password),"password8");
    tap_rect(done_rect());assert(s_view==VIEW_WIFI && s_wifi_connecting && !s_wifi_password[0]);
    refresh_setup();assert(!s_wifi_connecting);go_back();assert(s_view==VIEW_SYSTEM && mock_network.connected);
    enter_wifi();assert(mock_network.connected && !s_wifi_scanning && !strstr(s_setup_message,"校时"));go_back();assert(mock_network.connected);
    enter_wifi_from_weread();assert(s_wifi_return==VIEW_WEREAD && strstr(s_setup_message,"微信读书"));
    s_wifi_return=VIEW_SYSTEM;go_back();
    enter_time();save_pgm(directory,"74-time");
    handle_tap(480,830);assert(FOLIO_CLOCK_12H(&s_settings));
    char text[24];ui_clock(text,sizeof(text));assert(!strstr(text,"AM") && !strstr(text,"PM") && strchr(text,':'));
    save_pgm(directory,"75-time-12h");
    unsigned time_frames=frames,time_full=full_frames;int64_t time_idle=s_last_activity;
    uint32_t fixed_controls=screen_hash((EpdRect){48,472,588,720});
    refresh_setup();assert(frames==time_frames);
    advance_clock(60);refresh_setup();
    assert(frames==time_frames+2 && full_frames==time_full && s_last_activity==time_idle);
    assert(screen_hash((EpdRect){48,472,588,720})==fixed_controls);
    save_pgm(directory,"127-time-minute-refresh");
    time_frames=frames;refresh_setup();assert(frames==time_frames);
    uint32_t before_midnight=mock_utc;struct tm midnight={.tm_year=126,.tm_mon=9,.tm_mday=31,.tm_hour=23,.tm_min=59,.tm_sec=45};
    assert(folio_clock_to_utc(&midnight,s_settings.timezone_minutes,&mock_utc));mock_utc+=45;show_view(VIEW_TIME,true);
    uint32_t old_date=screen_hash((EpdRect){72,364,540,56});time_idle=s_last_activity;
    advance_clock(30);refresh_setup();
    assert(screen_hash((EpdRect){72,364,540,56})!=old_date && s_last_activity==time_idle);
    save_pgm(directory,"136-time-midnight-refresh");mock_utc=before_midnight;show_view(VIEW_TIME,true);
    uint8_t upper=s_settings.reserved[0]&0xf0;apply_catalog();assert((s_settings.reserved[0]&0xf0)==upper);
    enter_time();handle_tap(342,520);assert(s_view==VIEW_TIME_EDIT);
    s_clock_edit=(struct tm){.tm_year=124,.tm_mon=0,.tm_mday=31,.tm_hour=23,.tm_min=59};
    s_clock_field=1;change_clock(1);assert(s_clock_edit.tm_mon==1 && s_clock_edit.tm_mday==29);
    s_clock_field=0;change_clock(1);assert(s_clock_edit.tm_year==125 && s_clock_edit.tm_mday==28);
    show_view(VIEW_TIME_EDIT,false);save_pgm(directory,"76-time-manual");
    advance_clock(61);unsigned clock_full=full_frames,edit_frames=frames;struct tm draft=s_clock_edit;
    int64_t edit_idle=s_last_activity;refresh_setup();
    assert(frames==edit_frames && !memcmp(&draft,&s_clock_edit,sizeof(draft)) && s_last_activity==edit_idle);
    tap_rect(clock_field(4));handle_tap(490,800);assert(full_frames==clock_full && last_region.y==352);

    uint32_t expected;assert(folio_clock_to_utc(&s_clock_edit,s_settings.timezone_minutes,&expected));
    tap_rect(done_rect());assert(s_view==VIEW_TIME && mock_utc==expected);
    handle_tap(342,640);assert(s_clock_waiting);refresh_setup();assert(s_clock_ntp_started);
    setup_time_received(true);assert(!s_clock_waiting && !mock_network.running);refresh_setup();
    handle_tap(342,640);assert(s_clock_waiting);
    mock_network.connected=mock_network.connecting=false;
    snprintf(mock_network.message,sizeof(mock_network.message),"网络握手超时（204）");
    refresh_setup();assert(!s_clock_waiting && !mock_network.running && strstr(s_setup_message,"204"));
    save_pgm(directory,"81-time-network-error");
    handle_tap(342,640);refresh_setup();assert(s_clock_ntp_started);
    s_setup_deadline=esp_timer_get_time()-1;refresh_setup();
    assert(!s_clock_waiting && !mock_network.running && strstr(s_setup_message,"时间服务器"));
    save_pgm(directory,"82-time-server-timeout");
    mock_network.running=mock_network.connected=true;show_view(VIEW_SYSTEM,true);
    show_root(VIEW_HOME);assert(!mock_network.running);
    mock_wifi_saved=true;s_wifi_scanning=s_wifi_connecting=false;show_view(VIEW_WIFI,true);
    tap_rect(wifi_row(0));assert(s_wifi_saved && !strcmp(s_wifi_password,"saved-password"));
    save_pgm(directory,"83-wifi-saved-password");
    handle_tap(189,1144);assert(!mock_wifi_saved && !s_wifi_saved && !s_wifi_password[0]);
    save_pgm(directory,"84-wifi-forgotten");go_back();go_back();
    s_settings=saved;mock_utc=saved_utc;show_view(VIEW_SYSTEM,true);
    save_pgm(directory,"77-settings-system");
}
static void test_keyboard(void) {
    bool present[128]={false};present[' ']=true;
    for(unsigned mode=0;mode<4;++mode){const char* letters=keyboard_letters(mode);assert(strlen(letters)==26);
        for(unsigned i=0;i<26;++i)present[(unsigned char)letters[i]]=true;}
    for(unsigned c=32;c<127;++c)assert(present[c]);
    for(unsigned i=0;i<31;++i){EpdRect a=catalog_key(i);assert(a.x>=0 && a.y>=0 && a.x+a.width<=SCREEN_W && a.y+a.height<=1096);
        for(unsigned j=i+1;j<31;++j){EpdRect b=catalog_key(j);assert(a.x+a.width<=b.x || b.x+b.width<=a.x || a.y+a.height<=b.y || b.y+b.height<=a.y);}}
    view_t previous=s_view;show_view(VIEW_CATALOG,true);s_search[0]=0;s_search_keys=0;
    tap_rect(catalog_key(0));tap_rect(catalog_key(1));assert(!strcmp(s_search,"qw"));
    tap_rect(catalog_key(29));tap_rect(catalog_key(8));assert(!strcmp(s_search,"qw9"));
    tap_rect(catalog_key(28));assert(!s_search[0]);s_search_keys=0;show_view(previous,true);
}

static void test_cover_after_empty_frontmatter(const char* directory) {
    FILE* body = fopen(FOLIO_SD_ROOT "/blank-cover.txt", "wb"); assert(body);
    assert(fputc('\n', body) != EOF);
    assert(fputs(copyright_fixture, body) >= 0);
    assert(fputs("第一章\n从书页之间开始阅读。\n", body) >= 0);
    assert(fclose(body) == 0);
    mock_empty_cover = true;
    open_book_absolute(0, false);
    assert(s_view == VIEW_READING && s_reader_cover);
    assert(folio_reader_position(s_reader) == 1 && folio_reader_page_number(s_reader) == 1);
    assert(folio_reader_current_chapter(s_reader) == 1);
    uint8_t* cover = malloc(sizeof(screen)); assert(cover);
    memcpy(cover, screen, sizeof(screen));
    save_pgm(directory, "103-empty-frontmatter-cover");
    turn_page(true);
    assert(!s_reader_cover && folio_reader_position(s_reader) == 1);
    assert(memcmp(cover, screen, sizeof(screen)) != 0);
    save_pgm(directory, "104-empty-frontmatter-copyright");
    turn_page(false);
    assert(s_reader_cover && folio_reader_position(s_reader) == 1);
    assert(memcmp(cover, screen, sizeof(screen)) == 0);
    // A menu/TOC jump to the first text page must still open the text directly.
    open_tools(); tap_rect(tool_rect(0)); assert(s_view == VIEW_TOC);
    tap_row(1); assert(s_view == VIEW_READING && !s_reader_cover);
    assert(folio_reader_current_chapter(s_reader) == 1);
    turn_page(true);
    assert(!s_reader_cover && folio_reader_page_number(s_reader) == 2);
    assert(folio_reader_current_chapter(s_reader) == 2);
    turn_page(false); assert(!s_reader_cover && folio_reader_page_number(s_reader) == 1);
    turn_page(false); assert(s_reader_cover && memcmp(cover, screen, sizeof(screen)) == 0);
    close_reader();
    open_book_absolute(0, false);
    assert(s_reader_cover && folio_reader_position(s_reader) == 1);
    close_reader(); mock_empty_cover = false; free(cover);
    show_view(VIEW_LIBRARY, true);
}

static void test_numbered_section_labels(const char* directory) {
    mock_numbered_sections = true;
    open_book_absolute(0, false);
    assert(s_reader && s_view == VIEW_READING);
    open_tools(); tap_rect(tool_rect(0)); assert(s_view == VIEW_TOC);
    save_pgm(directory, "105-numbered-sections-toc");
    tap_row(1); assert(s_view == VIEW_READING && !s_reader_cover);
    assert(folio_reader_current_chapter(s_reader) == 1);
    assert(!strcmp(folio_reader_chapter_title(s_reader,1),"2"));
    char label[192];folio_reader_chapter_label(s_reader,1,label,sizeof(label));
    assert(!strcmp(label,"一 开车 · 2"));
    reader_chapter_label(1,22,100,label,sizeof(label));
    assert(text_width(22,label)<=100 && label[strlen(label)-1]=='2');
    save_pgm(directory, "106-numbered-section-footer");
    assert(folio_reader_seek(s_reader,0));
    close_reader();mock_numbered_sections = false;
    show_view(VIEW_LIBRARY, true);
}

static void assert_font_identity(const char* active, const char* desired) {
    assert(s_font && !strcmp(s_active_font, active) && !strcmp(s_settings.font_path, desired));
}
static void test_font_scanner(void) {
    assert(EXTERNAL_FONT_LIMIT == 31 && FONT_LIMIT == FOLIO_FONT_BUILTIN_COUNT + 31);
    char path[512];
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(path, sizeof(path), FOLIO_SD_ROOT "/fonts/font-scan-%02u.TtF", i);
        FILE* file = fopen(path, "wb"); assert(file && fclose(file) == 0);
    }
    char long_name[221]; memset(long_name, 'x', sizeof(long_name) - 1); long_name[220] = 0;
    char long_path[512]; snprintf(long_path, sizeof(long_path), FOLIO_SD_ROOT "/fonts/font-scan-%s.ttf", long_name);
    assert(strlen(long_path) >= sizeof(*s_fonts));
    FILE* file = fopen(long_path, "wb"); assert(file && fclose(file) == 0);
    unsigned opens = font_open_attempts;
    scan_fonts(); assert(s_font_count == FONT_LIMIT && font_open_attempts == opens);
    for (unsigned i = 0; i < FOLIO_FONT_BUILTIN_COUNT; ++i)
        assert(!strcmp(s_fonts[i], folio_font_builtin_path(i)));
    for (size_t i = FOLIO_FONT_BUILTIN_COUNT; i < s_font_count; ++i)
        assert(!strncmp(s_fonts[i], FOLIO_SD_ROOT "/fonts/", strlen(FOLIO_SD_ROOT "/fonts/")));
    s_list_page = 0; s_view = VIEW_FONTS;
    move_list(1); assert(s_list_page == 1);
    s_list_page = (s_font_count - 1) / list_rows();
    size_t last = s_list_page; move_list(1); assert(s_list_page == last);
    // These exact files were created above under the runner's private card root.
    for (unsigned i = 0; i < 32; ++i) {
        snprintf(path, sizeof(path), FOLIO_SD_ROOT "/fonts/font-scan-%02u.TtF", i);
        assert(unlink(path) == 0);
    }
    scan_fonts(); assert(s_font_count == FOLIO_FONT_BUILTIN_COUNT + 3); // Overlong path was not truncated into the list.
    assert(unlink(long_path) == 0);
    s_list_page = 0;
}
static void test_font_delete_preference(void) {
    const char* path = FOLIO_SD_ROOT "/fonts/font-delete-test.ttf";
    char error[192];
    copy_file(fixture_font_paths[0], path);
    close_reader(); folio_font_close(s_font); s_font = NULL;
    assert(open_active_font(path));
    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", ":builtin:future");
    assert(folio_settings_save(&s_settings));
    assert(folio_storage_preview_delete(path, &s_delete_preview, error, sizeof(error)));
    unsigned writes = settings_writes;
    assert_single_font = false; // Existing delete recovery opens Songti before closing the external face.
    delete_selected(); assert_single_font = true;
    assert(access(path, F_OK) != 0 && settings_writes == writes);
    assert_font_identity(FOLIO_FONT_BUILTIN_PATH, ":builtin:future");
    assert(!strcmp(persisted_settings.font_path, ":builtin:future"));

    copy_file(fixture_font_paths[0], path);
    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", path);
    assert(folio_settings_save(&s_settings));
    assert(folio_storage_preview_delete(path, &s_delete_preview, error, sizeof(error)));
    fail_settings_write = true; delete_selected(); fail_settings_write = false;
    assert(access(path, F_OK) != 0 && s_view == VIEW_ERROR);
    assert_font_identity(FOLIO_FONT_BUILTIN_PATH, path);
    assert(!strcmp(persisted_settings.font_path, path));

    copy_file(fixture_font_paths[0], path);
    assert(folio_storage_preview_delete(path, &s_delete_preview, error, sizeof(error)));
    FILE* file = fopen(path, "ab"); assert(file && fputc(0, file) != EOF && fclose(file) == 0);
    writes = settings_writes; delete_selected();
    assert(access(path, F_OK) == 0 && settings_writes == writes);
    assert_font_identity(FOLIO_FONT_BUILTIN_PATH, path);
    assert(folio_storage_preview_delete(path, &s_delete_preview, error, sizeof(error)));
    delete_selected(); assert(access(path, F_OK) != 0);
    assert_font_identity(FOLIO_FONT_BUILTIN_PATH, "");
    assert(!persisted_settings.font_path[0]);
}
static void test_native_font_selection(const char* directory) {
    const char* ids[] = {":builtin:", ":builtin:hei", ":builtin:kai", ":builtin:yuan"};
    const char* labels[] = {"内置宋体", "Folio 黑体", "Folio 楷体", "Folio 圆体"};
    assert(FOLIO_FONT_BUILTIN_COUNT == 4 && s_reader);
    folio_settings_t saved = s_settings;
    uint32_t saved_anchor = folio_reader_position(s_reader);
    bool saved_cover = s_reader_cover;
    char saved_book[256]; snprintf(saved_book, sizeof(saved_book), "%s", s_current_book);
    s_settings.auto_resume = 0; assert_single_font = true;
    s_fonts_return = VIEW_SETTINGS;
    mock_card_unmounted = true;
    for (unsigned i = 0; i < 4; ++i) {
        assert(!strcmp(folio_font_builtin_path(i), ids[i]));
        assert(!strcmp(folio_font_builtin_label(ids[i]), labels[i]));
        snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", ids[i]);
        unsigned writes = settings_writes, external = external_font_attempts;
        load_card(false, false);
        assert(!s_card_ready && !s_reader && !s_book_count && settings_writes == writes);
        assert(external_font_attempts == external);
        assert_font_identity(ids[i], ids[i]);
        scan_fonts(); assert(s_font_count == 4);
        s_list_page = 0; show_view(VIEW_FONTS, true);
        char name[80]; snprintf(name, sizeof(name), "170-native-%u-no-card-fonts", i); save_pgm(directory, name);
        show_view(VIEW_HOME, true);
        snprintf(name, sizeof(name), "171-native-%u-no-card-home", i); save_pgm(directory, name);
        s_lock_preview_off = false; s_settings.keep_page_on_lock = 2;
        show_view(VIEW_LOCK_PREVIEW, true);
        snprintf(name, sizeof(name), "172-native-%u-clock-144", i); save_pgm(directory, name);
        assert_centered_ink((EpdRect){48,400,588,96}, 32, labels[i]);
    }
    // A resident native object survives card loss even if reloading that asset fails.
    folio_font_t* retained = s_font;
    fail_font_paths[0] = ids[3]; s_card_ready = true;
    unsigned opens = font_open_attempts;
    assert(handle_card_lost() && s_font == retained && font_open_attempts == opens);
    assert_font_identity(ids[3], ids[3]);
    fail_font_paths[0] = NULL;

    // Missing or unknown resources preserve the desired ID; only actual loading
    // changes active identity, and ordinary saves retain that remembered choice.
    fail_font_paths[0] = ids[2];
    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", ids[2]);
    unsigned writes = settings_writes;
    load_card(false, false); assert_font_identity(ids[0], ids[2]);
    assert(settings_writes == writes);
    scan_fonts(); show_view(VIEW_FONTS, true); save_pgm(directory, "173-native-resource-fallback");
    assert(folio_settings_save(&s_settings) && !strcmp(persisted_settings.font_path, ids[2]));
    retained = s_font; opens = font_open_attempts;
    fail_settings_write = true; select_font(0); fail_settings_write = false;
    assert_font_identity(ids[0], ids[2]);
    assert(s_font == retained && font_open_attempts == opens && !strcmp(persisted_settings.font_path, ids[2]));
    select_font(0); assert_font_identity(ids[0], ids[0]);
    assert(s_font == retained && font_open_attempts == opens && !strcmp(persisted_settings.font_path, ids[0]));
    fail_font_paths[0] = NULL;
    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", ":builtin:future");
    unsigned external = external_font_attempts; writes = settings_writes;
    load_card(false, false); assert_font_identity(ids[0], ":builtin:future");
    assert(settings_writes == writes && external_font_attempts == external);
    scan_fonts(); select_font(2); assert_font_identity(ids[2], ids[2]); // Native selection works with no card.
    fail_settings_write = true; select_font(1); fail_settings_write = false;
    assert(s_view == VIEW_ERROR); assert_font_identity(ids[2], ids[2]);
    assert(!strcmp(persisted_settings.font_path, ids[2]));
    fail_font_paths[0] = ids[2]; fail_settings_write = true;
    select_font(1); fail_settings_write = false;
    assert_font_identity(ids[0], ids[2]); assert(strstr(s_error, "临时使用内置宋体"));
    assert(!strcmp(persisted_settings.font_path, ids[2]));
    fail_font_paths[0] = NULL;
    load_card(false, false); assert_font_identity(ids[2], ids[2]); // Recovery retries, but never saves automatically.
    fail_all_fonts = true; select_font(1);
    assert(!s_font && !s_active_font[0] && !strcmp(s_settings.font_path, ids[2]));
    assert(!strcmp(persisted_settings.font_path, ids[2]));
    fail_all_fonts = false; load_card(false, false); assert_font_identity(ids[2], ids[2]);

    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", fixture_font_paths[0]);
    load_card(false, false); assert_font_identity(ids[0], fixture_font_paths[0]);
    mock_card_unmounted = false; load_card(false, false);
    assert_font_identity(fixture_font_paths[0], fixture_font_paths[0]);
    assert(handle_card_lost()); assert_font_identity(ids[0], fixture_font_paths[0]);
    // Repair an old active-ID/render mismatch rather than retaining an external label.
    snprintf(s_active_font, sizeof(s_active_font), "%s", fixture_font_paths[0]);
    s_card_ready = true; assert(handle_card_lost()); assert_font_identity(ids[0], fixture_font_paths[0]);
    load_card(false, false); assert(s_card_ready && s_book_count == 12);
    test_font_scanner(); test_font_delete_preference();

    s_settings = saved; s_settings.auto_resume = 0;
    snprintf(s_settings.font_path, sizeof(s_settings.font_path), "%s", ids[0]);
    load_card(false, false); open_book_absolute(0, false); assert(s_reader);
    assert(folio_reader_seek_percent(s_reader, 37)); s_reader_cover = false;
    bool added_bookmark = folio_reader_bookmark_count(s_reader) == 0;
    if (added_bookmark) assert(folio_reader_toggle_bookmark(s_reader));
    size_t bookmarks = folio_reader_bookmark_count(s_reader); assert(bookmarks > 0);
    scan_fonts(); s_fonts_return = VIEW_SETTINGS;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t anchor = folio_reader_position(s_reader);
        select_font(i); assert(s_view == VIEW_SETTINGS && s_reader);
        assert_font_identity(ids[i], ids[i]);
        assert(folio_reader_position(s_reader) <= anchor && !s_reader_cover);
        assert(folio_reader_bookmark_count(s_reader) == bookmarks);
        const unsigned sizes[] = {24,38,56};
        for (unsigned j = 0; j < 3; ++j) {
            s_settings.font_size = sizes[j];
            assert(folio_font_set_size(s_font, sizes[j]));
            folio_reader_options_t options = reader_options(); assert(folio_reader_set_options(s_reader, &options));
            assert_body_bounds(); show_view(VIEW_READING, true);
            char name[80]; snprintf(name, sizeof(name), "174-native-%u-reading-%u", i, sizes[j]); save_pgm(directory, name);
        }
        s_settings.font_size = saved.font_size;
        assert(folio_font_set_size(s_font, s_settings.font_size));
        folio_reader_options_t options = reader_options(); assert(folio_reader_set_options(s_reader, &options));
        show_view(VIEW_FONTS, true);
        char name[80]; snprintf(name, sizeof(name), "175-native-%u-mixed-fonts", i); save_pgm(directory, name);
    }
    // Every failed transaction must leave the old settings intact, even on the
    // next unrelated save. Restored reader uses the original face and byte anchor.
    uint32_t anchor = folio_reader_position(s_reader);
    folio_settings_t previous = s_settings;
    assert(folio_settings_save(&previous));
    fail_settings_write = true; select_font(1); fail_settings_write = false;
    assert(s_view == VIEW_ERROR && s_reader && folio_reader_position(s_reader) == anchor);
    assert(!memcmp(&s_settings, &previous, sizeof(previous)) && !memcmp(&persisted_settings, &previous, sizeof(previous)));
    assert_font_identity(ids[3], ids[3]);
    assert(folio_settings_save(&s_settings) && !strcmp(persisted_settings.font_path, ids[3]));
    unsigned attempts = reader_open_attempts; writes = settings_writes;
    fail_reader_opens = 1; select_font(1);
    assert(reader_open_attempts == attempts + 2 && settings_writes == writes);
    assert(s_reader && folio_reader_position(s_reader) == anchor); assert_font_identity(ids[3], ids[3]);
    fail_reader_seeks = 1; select_font(1);
    assert(s_reader && folio_reader_position(s_reader) == anchor && settings_writes == writes);
    assert_font_identity(ids[3], ids[3]);
    fail_reader_opens = 2; select_font(1);
    assert(!s_reader && settings_writes == writes && strstr(s_error, "书籍未能重新打开"));
    assert_font_identity(ids[3], ids[3]);
    assert(reopen_font_reader(0, anchor, false));
    fail_font_paths[0] = ids[1]; select_font(1); fail_font_paths[0] = NULL;
    assert(s_view == VIEW_ERROR && s_reader && folio_reader_position(s_reader) == anchor);
    assert_font_identity(ids[3], ids[3]);
    retained = s_font; folio_reader_t* retained_reader = s_reader;
    writes = settings_writes; opens = font_open_attempts;
    fail_font_reader_save = true; select_font(1); fail_font_reader_save = false;
    assert(s_font == retained && s_reader == retained_reader && settings_writes == writes && font_open_attempts == opens);
    assert(!memcmp(&s_settings, &previous, sizeof(previous)));
    assert(folio_reader_seek(s_reader, 0)); s_reader_cover = false;
    select_font(2); assert(s_reader && !s_reader_cover); // Page 1 body must not turn into a cover.
    s_reader_cover = true; select_font(1); assert(s_reader && s_reader_cover);
    s_reader_cover = false;
    if (added_bookmark) assert(folio_reader_delete_bookmark(s_reader, bookmarks - 1));

    // Restore the surrounding regression's selected external face and reader.
    s_settings = saved; load_card(false, false);
    size_t book_index = SIZE_MAX;
    for (size_t i = 0; i < s_book_count; ++i) if (!strcmp(s_books[i].path, saved_book)) book_index = i;
    assert(book_index != SIZE_MAX);
    close_reader(); assert(reopen_font_reader(book_index, saved_anchor, saved_cover));
    snprintf(s_current_book, sizeof(s_current_book), "%s", saved_book);
    s_settings_tab = 0; s_fonts_return = VIEW_SETTINGS; s_list_page = 0; scan_fonts();
    show_view(VIEW_SETTINGS, true); assert_single_font = false;
    puts("Native fonts: four real resources, no-card boot/selection, card loss, 31 external slots, missing/unknown preservation, transactional load/reflow/NVS rollback, deletion preference and 24/38/56/144px rendering passed");
}

#include "test_online_ui.inc"

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "Usage: ui_preview font.ttf output_directory\n"); return 2; }
    assert(mkdir(argv[2], 0700) == 0 || errno == EEXIST);
    fixture_books();
    assert(read_pico_init(&s_hw) == ESP_OK);
    folio_settings_defaults(&s_settings);
    s_font = folio_font_open(argv[1], s_settings.font_size);
    if (s_font == NULL) { fprintf(stderr, "Could not load supported glyf/cmap4 font: %s\n", argv[1]); return 2; }
    test_ui_audit_regressions();
    test_centered_text();
    test_keyboard();
    test_weread_wrapping();
    s_card_ready = true;
    s_books = calloc(FOLIO_LIBRARY_LIMIT, sizeof(*s_books)); assert(s_books != NULL);
    refresh_library(); assert(s_book_count == 12 && list_rows() == 7 && LIBRARY_PAGE_SIZE == 6);
    assert(strstr(s_books[0].path, "一页微光.epub") != NULL);
    test_home(argv[2], argv[1]);
    test_home_status(argv[2]);
    test_idle_and_card_recovery(argv[1]);
    test_management(argv[2],argv[1]);
    test_online_update_ui(argv[2],argv[1]);
    test_weread(argv[2],argv[1]);
    test_setup(argv[2]);
    test_cover_after_empty_frontmatter(argv[2]);
    test_numbered_section_labels(argv[2]);
    show_view(VIEW_LIBRARY, true); save_pgm(argv[2], "01-library-grid");
    assert(library_hit(230, 300) == -1); // Visible whitespace is not a book tap.
    assert(list_hit(80, LIST_TOP + LIST_ROW - 1) == -1);
    assert(footer_hit(200, 1140) == -1);
    assert(key_at(80, 1500) == 0 && key_at(240, 1500) == 1 && key_at(400, 1500) == 2);
    assert(key_at(400, 1210) == -1);
    handle_tap(588, 160); assert(s_settings.library_list == 1 && settings_writes > 0);
    save_pgm(argv[2], "02-library-list");
    handle_tap(468, 160); assert(s_settings.library_list == 0);
    handle_tap(588, 1052); assert(s_shelf_page == 1);
    handle_tap(96, 1052); assert(s_shelf_page == 0);
    tap_rect(library_item(0)); assert(s_reader != NULL && s_view == VIEW_READING);
    assert(s_reader_cover);save_pgm(argv[2],"100-reading-cover");
    turn_page(false);assert(s_reader_notice&&!strcmp(s_reader_notice,"已经是第一页"));save_pgm(argv[2],"101-first-page-notice");
    advance_clock(3);refresh_home_clock();assert(!s_reader_notice);turn_page(true);assert(!s_reader_cover);
    assert_body_bounds(); show_view(VIEW_READING, false); save_pgm(argv[2], "03-reading");
    test_clock_layouts(argv[2]);
    s_settings.reserved[0]=(s_settings.reserved[0]&0x9f)|0x20;
    show_view(VIEW_READING,false);save_pgm(argv[2],"78-reading-chapter-page");
    s_settings.reserved[0]=(s_settings.reserved[0]&0x9f)|0x40;
    show_view(VIEW_READING,false);save_pgm(argv[2],"79-reading-percentage");
    s_settings.reserved[0]&=0x9f;
    open_tools(); handle_tap(200, 1000); assert(s_view == VIEW_LIBRARY && s_reader == NULL);
    save_pgm(argv[2], "22-library-current");
    // The frameless grid identifies this through its current-reading metadata.
    assert(strcmp(s_current_book, s_books[0].path) == 0);
    char original_title[sizeof(s_books[0].title)];
    snprintf(original_title, sizeof(original_title), "%s", s_books[0].title);
    uint32_t ordinary_title=screen_hash((EpdRect){48,240,588,760});
    snprintf(s_books[0].title, sizeof(s_books[0].title), "%s", "沿着河岸慢慢走直到晚风吹过城市的另一端");
    assert(folio_catalog_sync(s_catalog,s_books,s_book_count,mock_utc,true));
    const folio_catalog_metadata_t* long_metadata=folio_catalog_metadata(s_catalog,s_books[0].path);
    assert(long_metadata && !strcmp(long_metadata->title,s_books[0].title));
    show_view(VIEW_LIBRARY, false); save_pgm(argv[2], "23-library-long-title");
    assert(screen_hash((EpdRect){48,240,588,760})!=ordinary_title);
    snprintf(s_books[0].title, sizeof(s_books[0].title), "%s", original_title);
    assert(folio_catalog_sync(s_catalog,s_books,s_book_count,mock_utc,true));
    s_book_count = s_shelf_count = 0; show_view(VIEW_LIBRARY, true); save_pgm(argv[2], "24-library-empty");
    handle_tap(342, 752); assert(s_book_count == 12 && s_view == VIEW_LIBRARY);
    tap_rect(library_item(0)); assert(s_reader != NULL && s_view == VIEW_READING);
    draw_lock(false); save_pgm(argv[2], "18-lockscreen"); show_view(VIEW_READING, false);
    if(s_reader_cover)turn_page(true);
    uint32_t first = folio_reader_position(s_reader);
    handle_tap(600, 600); assert(folio_reader_position(s_reader) > first);
    handle_key(0, false); assert(folio_reader_position(s_reader) == first);
    open_tools();
    const folio_catalog_metadata_t* timed_book=folio_catalog_metadata(s_catalog,s_current_book);
    assert(timed_book);uint64_t timed_before=timed_book->reading_seconds;
    handle_tap(570,548);assert(s_view==VIEW_TOOLS);
    handle_tap(150,550);assert(s_view==VIEW_READING);
    advance_clock(12);activity_track(true,true);open_tools();
    assert(timed_book->reading_seconds>timed_before);
    save_pgm(argv[2], "04-reading-menu");
    tap_rect(tool_rect(2)); assert(s_view == VIEW_TOOLS && folio_reader_is_bookmarked(s_reader));
    tap_rect(tool_rect(1)); assert(s_view == VIEW_BOOKMARKS); save_pgm(argv[2], "05-bookmarks");
    handle_tap(532, LIST_TOP + 48); assert(s_view == VIEW_BOOKMARKS);
    handle_tap(588, LIST_TOP + 48); assert(s_view == VIEW_BOOKMARK_DELETE);
    save_pgm(argv[2], "06-bookmark-confirm");
    handle_tap(342, 1032); assert(s_view == VIEW_BOOKMARKS && folio_reader_bookmark_count(s_reader) == 1);
    handle_tap(588, LIST_TOP + 48); tap_rect(done_rect());
    assert(s_view == VIEW_BOOKMARKS && folio_reader_bookmark_count(s_reader) == 0);
    save_pgm(argv[2], "25-bookmarks-empty");
    tap_rect(back_rect()); assert(s_view == VIEW_READING);
    open_tools(); tap_rect(tool_rect(0)); assert(s_view == VIEW_TOC); save_pgm(argv[2], "07-contents");
    tap_row(1); assert(s_view == VIEW_READING && folio_reader_position(s_reader) > first);
    uint32_t sync_original_position=folio_reader_position(s_reader);
    open_tools();tap_rect(tool_rect(3));assert(s_view==VIEW_ERROR && s_reader);
    save_pgm(argv[2],"08-sync-unlinked-book");tap_rect(back_rect());assert(s_view==VIEW_TOOLS);
    tools_action(0);assert(folio_reader_position(s_reader)==sync_original_position);
    assert(folio_reader_seek_percent(s_reader,100));show_view(VIEW_READING,true);
    turn_page(true);assert(s_reader_notice&&!strcmp(s_reader_notice,"已经是最后一页"));save_pgm(argv[2],"102-last-page-notice");
    advance_clock(3);refresh_home_clock();assert(!s_reader_notice);
    assert(folio_reader_seek_percent(s_reader,37));show_view(VIEW_READING,true);
    open_tools(); tap_rect(tool_rect(4)); assert(s_view == VIEW_SETTINGS && s_settings_tab == 0);
    for(unsigned old_tab=1;old_tab<=2;++old_tab){
        handle_tap(142+(int)old_tab*200,260);assert(s_settings_tab==old_tab);
        tap_rect(done_rect());assert(s_view==VIEW_TOOLS);
        tap_rect(tool_rect(4));assert(s_view==VIEW_SETTINGS && s_settings_tab==0);
    }
    save_pgm(argv[2], "09-typography");
    unsigned before = s_settings.font_size;
    tap_rect(setting_step(0, 1)); assert(s_settings.font_size == before + 2 && settings_writes > 0);
    assert(folio_font_pixel_height(s_font) == s_settings.font_size);
    tap_rect(setting_step(0, -1)); assert(s_settings.font_size == before);
    handle_tap(300, 400); assert(s_view == VIEW_FONTS && s_fonts_return == VIEW_SETTINGS);
    assert(s_font_count == FOLIO_FONT_BUILTIN_COUNT && !strcmp(s_fonts[0],FOLIO_FONT_BUILTIN_PATH)); save_pgm(argv[2], "26-fonts-native");
    fixture_fonts(argv[1]); show_view(VIEW_FONTS, true); save_pgm(argv[2], "11-fonts");
    tap_row(FOLIO_FONT_BUILTIN_COUNT); assert(s_view == VIEW_SETTINGS && s_reader != NULL); // External choices retain source-view return.
    test_native_font_selection(argv[2]);
    handle_tap(340, 260); assert(s_settings_tab == 1); save_pgm(argv[2], "10-preferences");
    unsigned refresh = s_settings.refresh_every; tap_rect(setting_step(0, 1)); assert(s_settings.refresh_every == refresh + 1);
    bool resume = s_settings.auto_resume; handle_tap(540, 608); assert(s_settings.auto_resume != resume);
    handle_tap(48,608);assert(s_settings.auto_resume==resume);
    handle_tap(635,608);assert(s_settings.auto_resume!=resume);
    handle_tap(636,608);assert(s_settings.auto_resume!=resume);
    test_idle_settings(argv[2]);
    test_auto_shutdown_settings(argv[2]);
    handle_tap(540,260);assert(s_settings_tab==2);save_pgm(argv[2],"85-reader-display-settings");
    handle_tap(550,496);assert(FOLIO_READER_FOOTER(&s_settings)==2);
    handle_tap(550,608);assert(s_settings.progress_chapter);
    handle_tap(342,780);assert(s_settings.underline==1);
    handle_tap(550,940);assert(s_settings.underline_shade==2);
    tap_rect(done_rect());tap_rect(done_rect());assert(s_view==VIEW_READING);
    save_pgm(argv[2],"86-reading-chapter-percent-underlined");
    open_tools();tap_rect(tool_rect(4));handle_tap(540,260);
    handle_tap(550,780);assert(s_settings.underline==2);
    handle_tap(350,496);assert(FOLIO_READER_FOOTER(&s_settings)==0);
    tap_rect(done_rect());tap_rect(done_rect());
    uint32_t original_position=folio_reader_position(s_reader);
    while(!folio_reader_page_total(s_reader,false))refresh_reader_pagination();
    assert(folio_reader_position(s_reader)==original_position);
    show_view(VIEW_READING,true);save_pgm(argv[2],"87-reading-chapter-pages-dashed");
    open_tools();tap_rect(tool_rect(4));handle_tap(540,260);handle_tap(550,384);
    assert(s_settings.footer_hidden);save_pgm(argv[2],"88-reader-status-hidden-setting");
    tap_rect(done_rect());tap_rect(done_rect());save_pgm(argv[2],"89-reading-text-only");
    uint8_t* only_text=malloc(sizeof(screen));assert(only_text);memcpy(only_text,screen,sizeof(screen));
    epd_hl_set_all_white(&s_hw.hl);folio_reader_draw(s_reader,fb());assert(!memcmp(only_text,screen,sizeof(screen)));free(only_text);
    open_tools();tap_rect(tool_rect(4));handle_tap(540,260);handle_tap(550,384);handle_tap(140,780);handle_tap(350,608);
    assert(!s_settings.footer_hidden && !s_settings.underline && !s_settings.progress_chapter);
    handle_key(2,false);assert(s_settings_tab==0);
    handle_tap(380, 996); assert(!s_settings.justify);
    handle_tap(558, 996); assert(s_settings.justify);
    tap_rect(done_rect()); assert(s_view == VIEW_TOOLS);
    tap_rect(done_rect()); assert(s_view == VIEW_READING); save_pgm(argv[2], "12-reflowed-reading");
    handle_key(1, true); assert(s_view == VIEW_READING && s_turns_since_full == 0);
    folio_reader_t* locked_reader=s_reader;folio_font_t* locked_font=s_font;
    uint32_t locked_position=folio_reader_position(s_reader);mock_lock_success=true;
    uint64_t locked_seconds=folio_activity_total_seconds(s_activity);
    unsigned lock_syncs=card_sync_calls;uint8_t previous_lock_page=s_settings.keep_page_on_lock;s_settings.keep_page_on_lock=1;
    suspend_device(false);assert(s_view==VIEW_READING && s_reader==locked_reader && s_font==locked_font);
    assert(folio_reader_position(s_reader)==locked_position && folio_activity_total_seconds(s_activity)==locked_seconds);
    assert(card_sync_calls==lock_syncs && last_lock_minutes==0);
    s_settings.auto_shutdown_enabled=1;s_settings.auto_shutdown_minutes=360;
    s_settings.keep_page_on_lock=2;unsigned refreshes=lock_refresh_calls;
    suspend_device(false);assert(s_view==VIEW_READING && s_reader==locked_reader && s_font==locked_font);
    assert(folio_reader_position(s_reader)==locked_position && folio_activity_total_seconds(s_activity)==locked_seconds);
    assert(lock_refresh_calls==refreshes+1 && card_sync_calls==lock_syncs && last_lock_minutes==360);
    unsigned off_before_timeout=voluntary_off_calls;
    mock_lock_timed_out=true;s_settings.auto_resume=1;
    suspend_device(false);
    assert(last_lock_minutes==360 && voluntary_off_calls==off_before_timeout+1 && card_sync_calls==lock_syncs+1);
    assert(s_view==VIEW_ERROR && s_reader && s_font); // Simulated PMU cut failed, so recovery reopens safely.
    tap_rect(done_rect());assert(s_view==VIEW_READING);
    assert(folio_reader_position(s_reader)==locked_position);
    puts("Timed lock integration: chosen duration, safe close/unmount/off and failed-cut recovery passed");
    s_settings.auto_shutdown_enabled=0;s_settings.auto_shutdown_minutes=30;
    s_settings.keep_page_on_lock=previous_lock_page;mock_lock_success=mock_lock_timed_out=false;
    open_tools(); handle_tap(200, 1000); assert(s_view == VIEW_LIBRARY && s_reader == NULL);
    tap_nav(2); assert(s_view == VIEW_SYSTEM); save_pgm(argv[2], "13-system");
    handle_tap(495,984); assert(s_view == VIEW_OFF_CONFIRM);
    save_pgm(argv[2], "14-shutdown-confirm");
    handle_tap(342, 1032); assert(s_view == VIEW_SYSTEM);
    show_error("暂时无法读取这本书。请检查 TF 卡是否插好，再返回书架重试。原有阅读进度已保留。", VIEW_SYSTEM);
    save_pgm(argv[2], "15-error"); tap_rect(done_rect()); assert(s_view == VIEW_SYSTEM);
    open_book_absolute(0, false); assert(s_reader != NULL);
    s_settings.font_size = 56; s_settings.margin = 80; s_settings.line_gap = 24; s_settings.paragraph_gap = 32;
    assert(folio_font_set_size(s_font, s_settings.font_size));
    folio_reader_options_t options = reader_options(); assert(folio_reader_set_options(s_reader, &options));
    assert_body_bounds(); show_view(VIEW_READING, true); save_pgm(argv[2], "19-large-type");
    s_settings_tab = 0; show_view(VIEW_SETTINGS, true); save_pgm(argv[2], "21-typography-large");
    s_settings.font_size = 24; s_settings.margin = 24; s_settings.line_gap = 0; s_settings.paragraph_gap = 0;
    assert(folio_font_set_size(s_font, s_settings.font_size));
    options = reader_options(); assert(folio_reader_set_options(s_reader, &options));
    assert_body_bounds(); show_view(VIEW_READING, true); save_pgm(argv[2], "20-compact-type");
    // PMU emergencies must reply despite both progress and card-sync failures,
    // without a screen refresh or a user acknowledgement. Later retries must not
    // reopen/sync the card or re-save the book after readiness has been sent.
    assert(rename(FOLIO_SD_ROOT "/.folio/progress", FOLIO_SD_ROOT "/.folio/progress-backup") == 0);
    FILE* blocked_progress = fopen(FOLIO_SD_ROOT "/.folio/progress", "wb");
    assert(blocked_progress != NULL && fclose(blocked_progress) == 0);
    assert(folio_reader_seek_percent(s_reader, 40));
    assert(!folio_reader_save(s_reader) && *folio_reader_error(s_reader));
    unsigned frames_before_shutdown = frames;
    unsigned syncs_before_shutdown = card_sync_calls;
    fail_card_sync = true; emergency_shutdown();
    assert(s_reader == NULL && s_font == NULL && card_sync_calls == syncs_before_shutdown+1 && emergency_replies == 1);
    assert(frames == frames_before_shutdown);
    emergency_shutdown(); assert(card_sync_calls == syncs_before_shutdown+1 && emergency_replies == 2 && frames == frames_before_shutdown);
    assert(unlink(FOLIO_SD_ROOT "/.folio/progress") == 0);
    assert(rename(FOLIO_SD_ROOT "/.folio/progress-backup", FOLIO_SD_ROOT "/.folio/progress") == 0);
    for (unsigned i = 0; i < 3; ++i) assert(unlink(fixture_font_paths[i]) == 0);
    fail_all_fonts=true; // Emergency ASCII paths remain reachable with real host builtins bound.
    s_card_ready=false;mock_card_unformatted=true;show_view(VIEW_HOME,true);
    handle_tap(342,1032);assert(s_view==VIEW_FORMAT_CONFIRM && !s_format_armed && !format_calls);
    save_pgm(argv[2],"53-no-font-format-warning");tap_rect(done_rect());assert(s_view==VIEW_FORMAT_FINAL && s_format_armed && !format_calls);
    save_pgm(argv[2],"54-no-font-format-confirm");handle_tap(342,1032);assert(s_view==VIEW_HOME && !s_format_armed && !format_calls);
    mock_card_unformatted=false;s_card_ready=true;
    handle_tap(342,1032);assert(s_view==VIEW_TRANSFER && mock_transfer.running && !s_font);
    save_pgm(argv[2],"55-no-font-transfer");tap_rect(done_rect());assert(!mock_transfer.running && !s_font);
    show_view(VIEW_LIBRARY,true);
    draw_missing_font(); save_pgm(argv[2], "16-no-font");
    unsigned off_before_confirmation=voluntary_off_calls;
    show_view(VIEW_OFF_CONFIRM, true); assert(s_view == VIEW_OFF_CONFIRM && s_font == NULL);
    save_pgm(argv[2], "17-no-font-shutdown");
    handle_tap(342, 1032); assert(s_view == VIEW_LIBRARY && voluntary_off_calls == off_before_confirmation);
    // The simulated restart cannot reopen the temporary font after this point.
    s_settings.font_path[0] = '\0'; session_path[0] = '\0';
    show_view(VIEW_OFF_CONFIRM, true); fail_card_sync = false;
    tap_rect(done_rect()); assert(voluntary_off_calls == off_before_confirmation+1 && card_sync_calls == syncs_before_shutdown+2);
    assert(s_reader == NULL && s_font == NULL);
    activity_close(); catalog_close(); folio_cover_clear_cache(); free(s_books); free(s_fonts);
    assert(pixels_clipped == 0);
    printf("UI regression passed: %u presentations, no out-of-frame pixels; covers, home, library, reader, menus, WeRead login/OTP/shelf/download/sync/confirmations, real cache handoff, stop-failure guards and PMU failures verified.\n", frames);
    return 0;
}
