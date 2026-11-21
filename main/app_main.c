#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "epdiy.h"
#include "folio_activity.h"
#include "folio_catalog.h"
#include "folio_cover.h"
#include "folio_wallpaper.h"
#include "folio_weread_paths.h"
#include "folio_font.h"
#include "folio_gfx.h"
#include "folio_power.h"
#include "folio_reader.h"
#include "folio_settings.h"
#include "folio_clock.h"
#include "folio_storage.h"
#include "book/folio_weread_cache.h"
#include "folio_update.h"
#include "folio_online_update.h"
#include "net/folio_transfer.h"
#include "net/folio_network.h"
#include "net/folio_weread.h"
#include "folio_text.h"
#include "read_pico_init.h"
#include "read_pico_sd.h"
#include "vendor/qrcodegen/qrcodegen.h"

#define EXTERNAL_FONT_LIMIT 31
#define FONT_LIMIT (FOLIO_FONT_BUILTIN_COUNT + EXTERNAL_FONT_LIMIT)
#define KEY_AREA_TOP 1300
#define KEY_AREA_WIDTH 480
#define KEY_PITCH 160

typedef enum {
    VIEW_LIBRARY, VIEW_READING, VIEW_TOOLS, VIEW_TOC, VIEW_BOOKMARKS,
    VIEW_JUMP, VIEW_SETTINGS, VIEW_FONTS, VIEW_SYSTEM, VIEW_ERROR, VIEW_OFF_CONFIRM,
    VIEW_BOOKMARK_DELETE, VIEW_HOME, VIEW_HOME_SETTINGS, VIEW_CATALOG,
    VIEW_DEVICE, VIEW_TRANSFER, VIEW_STORAGE, VIEW_FILES, VIEW_FILE_ACTION,
    VIEW_DELETE_CONFIRM, VIEW_FORMAT_CONFIRM, VIEW_FORMAT_FINAL, VIEW_UPDATE, VIEW_WEREAD, VIEW_WIFI, VIEW_WIFI_EDIT, VIEW_TIME, VIEW_TIME_EDIT, VIEW_LOCK_PREVIEW, VIEW_WALLPAPERS
} view_t;
static const char* TAG = "folio";
static read_pico_handle_t s_hw;
static folio_font_t* s_font;
static folio_reader_t* s_reader;
static folio_activity_t* s_activity;
static folio_catalog_t* s_catalog;
static size_t s_shelf_indexes[FOLIO_LIBRARY_LIMIT], s_shelf_count;
static char s_search[FOLIO_CATALOG_QUERY_CAP];
static folio_catalog_options_t s_catalog_options;
static folio_book_t* s_books;
static folio_settings_t s_settings;
static view_t s_view = VIEW_HOME, s_error_return = VIEW_HOME, s_settings_return = VIEW_SYSTEM;
static view_t s_fonts_return = VIEW_TOOLS, s_power_return = VIEW_SYSTEM;
static char s_error[192], s_active_font[256], s_current_book[256];
static char (*s_fonts)[256];
static size_t s_book_count, s_font_count, s_shelf_page, s_list_page, s_pending_bookmark;
static unsigned s_jump_percent, s_turns_since_full, s_settings_tab;
static int64_t s_last_activity;
static bool s_first_present = true, s_card_ready, s_card_probe_pending;
static bool s_activity_save_failed, s_activity_load_failed;
static uint32_t s_home_clock_minute;
static char s_home_battery[16];
static int64_t s_home_status_checked;
static uint32_t s_lock_preview_minute;
static bool s_reader_cover, s_lock_preview_off;
static view_t s_lock_preview_return=VIEW_HOME_SETTINGS;
static size_t s_wallpaper_page,s_wallpaper_count,s_wallpaper_total;
static char s_wallpaper_path[256];
static const char* s_reader_notice;
static int64_t s_reader_notice_until;
static folio_transfer_status_t s_transfer_status;
static folio_storage_summary_t s_storage_summary;
static folio_storage_entry_t s_file_entries[7], s_file_selected;
static folio_storage_preview_t s_delete_preview;
static char s_file_directory[FOLIO_STORAGE_PATH_CAP]=FOLIO_SD_ROOT;
static size_t s_file_page, s_file_count, s_file_total;
static bool s_files_show_internal;
static unsigned s_search_keys;
static unsigned s_transfer_tab;
static bool s_format_armed;
static folio_update_info_t s_update_info;
static bool s_update_ready;
static char s_update_message[192];
// UI owns lifecycle; worker callbacks see only these atomic safety bits.
enum { ONLINE_CARD = 1, ONLINE_POWER = 2, ONLINE_AUTH = 4, ONLINE_CANCEL = 8 };
static atomic_uint s_online_safety;
static bool s_update_local, s_online_confirm, s_online_initialized;
static bool s_online_operation, s_online_install_owned, s_online_network;
static bool s_online_leaving, s_online_card_lost, s_online_wifi_setup;
static bool s_online_stopping;
static view_t s_online_target = VIEW_DEVICE;
static unsigned s_online_wait;
static int64_t s_online_deadline, s_online_draw_at;
static folio_online_snapshot_t s_online_snapshot;
static char s_online_message[192];
static bool online_active(void);
static void online_publish_safety(void);
static void online_cancel(void);
static bool online_leave(view_t target);
static bool online_quiesce(void);
static void refresh_online(void);
static void draw_online_update(void);
static void online_tap(int x, int y);
static bool online_setup_back(void);
static folio_weread_status_t s_wr_status;
static bool s_wr_leaving, s_wr_network_setup, s_wr_open_pending;
static view_t s_wr_target=VIEW_DEVICE;
static char s_wr_open_path[256],s_wr_otp[9],s_wr_reader_path[256];
static uint32_t s_wr_reader_position;
static bool s_wr_reader_cover;
static uint32_t s_wr_open_position;
static size_t s_wr_book_index;
static unsigned s_wr_confirm;
static bool s_power_initialized;
static bool s_suspend_pending, s_suspend_power_off;
static void* s_ui_task;
static folio_power_event_t s_deferred_power;
bool folio_main_io_cancelled(void) {
    static int64_t next_poll;
    if(xTaskGetCurrentTaskHandle()!=s_ui_task) {
        // Never inherit the previous WeRead worker's stopping flag.
        if(folio_online_update_is_worker())return folio_online_update_cancel_requested();
        return folio_weread_io_cancelled();
    }
    if(!s_power_initialized)return false;
    int64_t now=esp_timer_get_time();
    if(now>=next_poll) {
        next_poll=now+250000;
        folio_power_event_t event=folio_power_poll();
        if(event>s_deferred_power)s_deferred_power=event;
        online_publish_safety();
    }
    return folio_power_shutdown_pending();
}
bool folio_storage_cancelled(void) {return folio_main_io_cancelled();}
static void show_view(view_t view, bool full);
static void emergency_shutdown(void);
static bool stop_transfer(void) {
    if(!online_quiesce())return false;
    if(!folio_weread_stop())return false;
    if(folio_transfer_stop())return true;
    if(!folio_power_shutdown_pending())show_view(VIEW_TRANSFER,true);
    return false;
}
static void open_book_absolute(size_t index, bool feedback);
static void enter_update(void);
static void install_update(void);
static void draw_update(void);
static void enter_weread(void);
static void enter_reader_sync(void);
static void draw_weread(void);
static void enter_wifi(void);
static void enter_wifi_from_weread(void);
static void enter_time(void);
static void draw_wifi(void);
static void draw_wifi_edit(void);
static void draw_time(void);
static void draw_time_edit(void);
static bool setup_back(void);
static bool setup_tap(int x,int y);
static void refresh_setup(void);
static void setup_time_received(bool saved);
static bool card_needs_format(void) {
    read_pico_sd_info_t info={0};
    return read_pico_sd_get_info(&info) == ESP_FAIL && info.present && info.needs_format;
}
static void catalog_select(void) {
    s_catalog_options.search=s_search;
    if(s_catalog) s_shelf_count=folio_catalog_select(s_catalog,s_books,s_book_count,
        &s_catalog_options,s_shelf_indexes,FOLIO_LIBRARY_LIMIT);
    else { s_shelf_count=s_book_count; for(size_t i=0;i<s_book_count;++i)s_shelf_indexes[i]=i; }
}
static bool catalog_save(void) {
    return !s_catalog || !s_card_ready || !folio_catalog_dirty(s_catalog) || folio_catalog_save(s_catalog);
}
static void catalog_close(void) { folio_catalog_free(s_catalog); s_catalog=NULL; }
static void activity_track(bool reading, bool interaction) {
    uint64_t before=folio_activity_total_seconds(s_activity);
    folio_activity_update(s_activity, esp_timer_get_time(), folio_power_unix_time(),
        s_settings.timezone_minutes, reading && s_reader != NULL && s_card_ready, interaction);
    uint64_t after=folio_activity_total_seconds(s_activity);
    if(s_reader && s_current_book[0] && after>before)
        (void)folio_catalog_add_reading(s_catalog,s_current_book,after-before);
    if(s_reader && s_current_book[0] && after>before)
        folio_weread_record(s_current_book,folio_reader_position(s_reader),folio_reader_percent(s_reader),
            (uint32_t)(after-before>UINT32_MAX?UINT32_MAX:after-before));
}
static void activity_record_book(void) {
    if (s_reader && s_current_book[0]) {
        (void)folio_activity_touch_book(s_activity, s_current_book, folio_reader_title(s_reader),
            folio_reader_percent(s_reader), folio_reader_page_number(s_reader), folio_reader_position(s_reader));
        folio_weread_record(s_current_book,folio_reader_position(s_reader),folio_reader_percent(s_reader),0);
    }
}
static bool activity_flush(void) {
    if (!s_activity) return true;
    s_activity_save_failed = !folio_activity_save(s_activity);
    if(!folio_weread_flush())ESP_LOGW(TAG,"WeRead local synchronization record was not saved");
    if (s_activity_save_failed) ESP_LOGW(TAG, "activity save: %s", folio_activity_error(s_activity));
    return !s_activity_save_failed;
}
static void activity_close(void) {
    // All callers settle and attempt saving in close_reader first. A failed
    // emergency write must not be retried repeatedly before the PMU reply.
    folio_activity_free(s_activity); s_activity = NULL;
}
#include "folio_ui.inc"

static void scan_fonts(void) {
    s_font_count = 0;
    if (s_fonts == NULL) s_fonts = calloc(FONT_LIMIT, sizeof(*s_fonts));
    if (s_fonts == NULL) return;
    for (unsigned i = 0; i < FOLIO_FONT_BUILTIN_COUNT; ++i)
        snprintf(s_fonts[s_font_count++], sizeof(*s_fonts), "%s", folio_font_builtin_path(i));
    if (!s_card_ready) return;
    DIR* dir = opendir(FOLIO_SD_ROOT "/fonts");
    if (dir == NULL) return;
    struct dirent* entry;
    while (s_font_count < FONT_LIMIT && (entry = readdir(dir)) != NULL) {
        const char* ext = strrchr(entry->d_name, '.');
        if (ext == NULL || strcasecmp(ext, ".ttf") != 0) continue;
        int n = snprintf(s_fonts[s_font_count], sizeof(*s_fonts), FOLIO_SD_ROOT "/fonts/%s", entry->d_name);
        if (n > 0 && (size_t)n < sizeof(*s_fonts)) ++s_font_count;
    }
    closedir(dir);
}

static bool save_reader(void) {
    activity_track(false, false);
    if (s_reader == NULL || folio_reader_save(s_reader)) return true;
    show_error(folio_reader_error(s_reader), s_view);
    return false;
}
static void close_reader(void) {
    activity_track(false, false); activity_record_book(); activity_flush();
    if (s_reader != NULL) folio_reader_close(s_reader);
    s_reader = NULL;
}
static void show_root(view_t view) {
    if(online_active() && !online_leave(view))return;
    s_online_wifi_setup=false;
    if (!save_reader()) return;
    if(view==VIEW_HOME || view==VIEW_LIBRARY) {
        folio_network_status_t network;folio_network_get_status(&network);
        if(network.running && !stop_transfer())return;
    }
    close_reader();
    if (view == VIEW_LIBRARY || view == VIEW_HOME) refresh_library();
    show_view(view, true);
}
static void show_library(void) { show_root(VIEW_LIBRARY); }
static void open_recent(size_t index) {
    const folio_recent_book_t* recent = folio_activity_recent(s_activity, index);
    if (!recent) { show_library(); return; }
    refresh_library();
    for (size_t i = 0; i < s_book_count; ++i) if (strcmp(recent->path, s_books[i].path) == 0) {
        open_book_absolute(i, true); return;
    }
    show_error("书架中没有找到这本书。请将原文件放回 books 目录，阅读记录会保留。", VIEW_HOME);
}
static void open_book_absolute(size_t index, bool feedback) {
    if (index >= s_book_count || s_font == NULL || !save_reader()) return;
    close_reader();
    if (feedback) show_busy("正在打开书籍");
    (void)folio_font_set_size(s_font, s_settings.font_size);
    folio_reader_options_t options = reader_options();
    s_reader = folio_reader_open_with_options(&s_books[index], s_font, &options);
    if (s_reader == NULL) { show_error(folio_reader_last_error(), VIEW_LIBRARY); return; }
    uint32_t restored_position;
    if(folio_weread_local_position(s_books[index].path,&restored_position))
        (void)folio_reader_seek(s_reader,restored_position);
    if(s_catalog) (void)folio_catalog_touch(s_catalog,s_books[index].path,folio_power_unix_time());
    snprintf(s_current_book, sizeof(s_current_book), "%s", s_books[index].path);
    s_reader_cover=folio_reader_page_number(s_reader)==1;s_reader_notice=NULL;
    activity_record_book(); activity_flush();
    s_turns_since_full = 0;
    if (!folio_session_save(s_current_book)) {
        show_error("上次阅读的书籍未能保存，当前仍可阅读。请检查 TF 卡空间。", VIEW_READING); return;
    }
    const char* save_error = folio_reader_error(s_reader);
    if (save_error != NULL && *save_error) { show_error(save_error, VIEW_READING); return; }
    show_view(VIEW_READING, true);
    if(!catalog_save())show_error("这次阅读的书架记录尚未保存，请检查 TF 卡空间。",VIEW_READING);
}
// Call only after releasing the previous font. Preference and active identity
// differ while a missing card/resource is temporarily using a recovery font.
static bool open_active_font(const char* path) {
    s_active_font[0] = '\0';
    if (!path || !*path || (!folio_font_is_builtin_path(path) && !s_card_ready)) return false;
    s_font = folio_font_open(path, s_settings.font_size);
    folio_font_set_weight(s_font,s_settings.weight_step);
    if (s_font) snprintf(s_active_font, sizeof(s_active_font), "%s", path);
    return s_font != NULL;
}
static void load_system_font(void) {
    const char* desired = s_settings.font_path[0] ? s_settings.font_path : FOLIO_FONT_BUILTIN_PATH;
    if (!open_active_font(desired) && strcmp(desired, FOLIO_FONT_BUILTIN_PATH))
        (void)open_active_font(FOLIO_FONT_BUILTIN_PATH);
    // A missing/corrupt primary resource must not hide the other verified
    // built-in faces, including the TF-card fallback in /folio-fonts.
    if (!s_font) {
        for (unsigned i = 1; i < FOLIO_FONT_BUILTIN_COUNT; ++i)
            if (open_active_font(folio_font_builtin_path(i))) break;
    }
    if (!s_font && s_card_ready)
        s_font = folio_font_open_first_on_card(s_settings.font_size, s_active_font, sizeof(s_active_font));
    ESP_LOGI(TAG, "System font: %s", !s_font ? "unavailable" :
        folio_font_is_builtin_path(s_active_font) ? "built-in" : "TF card");
}
static bool wait_card_ready(read_pico_sd_info_t* info) {
    int64_t started=esp_timer_get_time();
    for(;;) {
        folio_power_event_t event=folio_power_poll();
        if(event>s_deferred_power)s_deferred_power=event;
        if(event==FOLIO_POWER_SHUTDOWN_NOW || folio_power_shutdown_pending())return false;
        esp_err_t error=read_pico_sd_get_info(info);
        s_card_probe_pending=error==ESP_ERR_NOT_FINISHED || info->error==ESP_ERR_NOT_FINISHED;
        if(!s_card_probe_pending)return error==ESP_OK && info->mounted;
        if(esp_timer_get_time()-started>=15000000)return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void migrate_wallpaper_size(void) {
    if (FOLIO_OFF_STYLE(&s_settings) != 2 || !folio_storage_lock(500)) return;
    uint8_t *cached = NULL;
    unsigned w = 0, h = 0;
    bool old_size = folio_storage_wallpaper_cache_load(&cached, &w, &h) &&
                    (w != 684 || h != 1216);
    free(cached);
    char path[256], error[128];
    folio_wallpaper_image_t image = {0};
    if (old_size && folio_storage_wallpaper_load(path, sizeof(path)) &&
        folio_storage_validate_parent(path, error, sizeof(error)) &&
        folio_wallpaper_decode_cropped(path, 684, 1216, &image, error, sizeof(error))) {
        (void)folio_storage_wallpaper_save_prepared(path, image.pixels, image.width, image.height,
                                                    error, sizeof(error));
    }
    folio_wallpaper_image_free(&image);
    folio_storage_unlock();
}
static void load_card(bool remount, bool resume) {
    if(!stop_transfer())return;
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    folio_storage_cancel_delete();s_format_armed=false;s_update_ready=false;
    activity_track(false, false); activity_record_book();
    if (s_card_ready && !activity_flush()) {
        show_error("阅读统计尚未保存，请检查 TF 卡后重试。当前记录仍保留在内存中。", s_view); return;
    }
    if(!catalog_save()) {show_error("书架记录尚未保存，请检查 TF 卡后重试。",s_view);return;}
    close_reader(); catalog_close(); folio_font_close(s_font); s_font = NULL; s_active_font[0] = '\0';
    activity_close(); folio_cover_clear_cache();
    if (remount) (void)read_pico_sd_remount();
    read_pico_sd_info_t info = {0};
    s_card_ready=wait_card_ready(&info);
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    s_book_count=s_shelf_count=0;
    s_activity_save_failed = s_activity_load_failed = false;
    if (s_card_ready) {
        migrate_wallpaper_size();
        char wallpaper_error[128];
        if(!folio_storage_prepare_wallpapers(wallpaper_error,sizeof(wallpaper_error)))
            ESP_LOGW(TAG,"picture directory preparation: %s",wallpaper_error);
        s_catalog=folio_catalog_open();
        s_activity = folio_activity_open();
        s_activity_load_failed = !s_activity || folio_activity_error(s_activity)[0] != '\0';
    }
    // Internal resources and NVS preferences do not depend on a mounted TF card.
    // Recovery must never rewrite a remembered choice merely because loading fails.
    load_system_font();

    s_view = VIEW_HOME;
    if (s_font != NULL && s_card_ready) {
        refresh_library();
        ESP_LOGI(TAG,"Card state: mounted=%u books=%u reading_seconds=%llu",s_card_ready,(unsigned)s_book_count,
            (unsigned long long)folio_activity_total_seconds(s_activity));
        char last_book[256] = {0};
        bool has_last = folio_session_load(last_book, sizeof(last_book));
        char migrated[256];if(has_last&&folio_weread_cache_public_path(last_book,migrated,sizeof(migrated))){
            snprintf(last_book,sizeof(last_book),"%s",migrated);(void)folio_session_save(last_book);}

        if (has_last && !s_activity_load_failed && !folio_activity_recent_count(s_activity)) {
            for (size_t i = 0; i < s_book_count; ++i) if (strcmp(last_book, s_books[i].path) == 0) {
                (void)folio_activity_touch_book(s_activity, last_book, s_books[i].title, 0, 0, 0);
                activity_flush(); break;
            }
        }
        const folio_recent_book_t* recent = folio_activity_recent(s_activity, 0);
        for(size_t i=folio_activity_recent_count(s_activity);i>0;--i) {
            const folio_recent_book_t* item=folio_activity_recent(s_activity,i-1);
            const folio_catalog_metadata_t* meta=folio_catalog_metadata(s_catalog,item->path);
            if(meta && !meta->read)(void)folio_catalog_touch(s_catalog,item->path,0);
        }
        (void)catalog_save();catalog_select();
        snprintf(s_current_book, sizeof(s_current_book), "%s", recent ? recent->path : "");
        if (resume && s_settings.auto_resume && has_last) {
            for (size_t i = 0; i < s_book_count; ++i) if (strcmp(last_book, s_books[i].path) == 0) {
                s_shelf_page = i / LIBRARY_PAGE_SIZE; open_book_absolute(i, true); return;
            }
        }
    }
    show_view(VIEW_HOME, true);
}
static void refresh_card_probe(void) {
    if(!s_card_probe_pending)return;
    read_pico_sd_info_t info={0};esp_err_t error=read_pico_sd_get_info(&info);
    if(error==ESP_ERR_NOT_FINISHED || info.error==ESP_ERR_NOT_FINISHED)return;
    if(error==ESP_OK && info.mounted)load_card(false,false);
    else s_card_probe_pending=false;
}
static bool handle_card_lost(void) {
    if(online_active()) {
        s_online_card_lost=true;
        atomic_fetch_and(&s_online_safety,~(unsigned)ONLINE_CARD);
        online_cancel();
        return false;
    }
    s_online_card_lost=false;s_online_wifi_setup=false;
    if (!stop_transfer() || !folio_weread_stop()) return false;
    ESP_LOGW(TAG, "Card lost: closing reader and returning to HOME");
    folio_storage_cancel_delete(); s_format_armed = false; s_update_ready = false;
    s_card_ready = false;
    close_reader(); catalog_close(); activity_close(); folio_cover_clear_cache();
    // A resident native font remains usable even when its resource later becomes
    // unavailable. Do not replace it with Songti merely because the card vanished.
    if (!s_font || !folio_font_is_builtin_path(s_active_font) || !folio_font_builtin_label(s_active_font)) {
        folio_font_close(s_font); s_font = NULL;
        load_system_font();
    }
    s_book_count = s_shelf_count = 0;
    show_view(VIEW_HOME, true);
    return true;
}
static bool rescan_library(void) {
    if(!s_card_ready)load_card(true,false);
    else {folio_cover_clear_cache();refresh_library();}
    return s_card_ready && !folio_power_shutdown_pending();
}
static void turn_page(bool next) {
    activity_track(false, false);
    (void)folio_font_set_size(s_font, s_settings.font_size);
    s_reader_notice=NULL;
    if(s_reader_cover && next){s_reader_cover=false;show_view(VIEW_READING,true);return;}
    if(!s_reader_cover && !next && folio_reader_page_number(s_reader)==1 &&
        folio_cover_draw(s_current_book,fb(),(EpdRect){48,48,588,1120})){
        s_reader_cover=true;show_view(VIEW_READING,true);return;
    }
    bool changed = !s_reader_cover && (next ? folio_reader_next(s_reader) : folio_reader_previous(s_reader));
    if (!changed) {
        const char* error = folio_reader_error(s_reader);
        if (error != NULL && *error) show_error(error, VIEW_READING);
        else {s_reader_notice=next?"已经是最后一页":"已经是第一页";s_reader_notice_until=esp_timer_get_time()+2000000;
            show_view(VIEW_READING,false);activity_track(true,false);}
        return;
    }
    ++s_turns_since_full;
    bool full = s_turns_since_full >= s_settings.refresh_every;
    show_view(VIEW_READING, full);
    if (full) s_turns_since_full = 0;
    const char* save_error = folio_reader_error(s_reader);
    if (save_error != NULL && *save_error) show_error(save_error, VIEW_READING);
}
static void move_list(int direction) {
    size_t* page = s_view == VIEW_LIBRARY ? &s_shelf_page : &s_list_page;
    size_t count = s_view == VIEW_LIBRARY ? s_shelf_count : s_view == VIEW_TOC ? folio_reader_chapter_count(s_reader) :
        s_view == VIEW_BOOKMARKS ? folio_reader_bookmark_count(s_reader) : s_font_count;
    if (direction < 0 && *page > 0) --*page;
    else if (direction > 0 && (*page + 1) * (s_view == VIEW_LIBRARY ? LIBRARY_PAGE_SIZE : list_rows()) < count) ++*page;
    else return;
    show_view(s_view, false);
}
static void change_setting(int row, int direction) {
    folio_settings_t previous = s_settings;
    int value;
    switch (row) {
        case 0: value = s_settings.font_size + direction * 2; if (value >= 24 && value <= 56) s_settings.font_size = value; break;
        case 1: value = s_settings.line_gap + direction * 2; if (value >= 0 && value <= 24) s_settings.line_gap = value; break;
        case 2: value = s_settings.paragraph_gap + direction * 4; if (value >= 0 && value <= 32) s_settings.paragraph_gap = value; break;
        case 3: value = s_settings.margin + direction * 8; if (value >= 24 && value <= 80) s_settings.margin = value; break;
        case 4: s_settings.reserved[0] ^= 0x80u; break;
        case 5: value = s_settings.refresh_every + direction; if (value >= 1 && value <= 32) s_settings.refresh_every = value; break;
        case 6:
            value = s_settings.idle_minutes + direction * FOLIO_IDLE_MINUTES_STEP;
            if (value >= FOLIO_IDLE_MINUTES_MIN && value <= FOLIO_IDLE_MINUTES_MAX) s_settings.idle_minutes = value;
            break;
        case 7: s_settings.auto_resume = !s_settings.auto_resume; break;
        case 8: break; /* Lock appearance lives in personalization. */
        case 9: s_settings.footer_hidden = !s_settings.footer_hidden; break;
        case 10: s_settings.reserved[0]=(uint8_t)((s_settings.reserved[0]&~0x60u)|(direction?0x40u:0)); break;
        case 11: s_settings.progress_chapter=direction!=0; break;
        case 12: if(direction>=0 && direction<=2)s_settings.underline=(uint8_t)direction; break;
        case 13: if(direction>=0 && direction<=2)s_settings.underline_shade=(uint8_t)direction; break;
        case 14: s_settings.auto_shutdown_enabled=!s_settings.auto_shutdown_enabled; break;
        case 15:
            if(!s_settings.auto_shutdown_enabled || direction<0 || (unsigned)direction>=AUTO_SHUTDOWN_CHOICES)return;
            s_settings.auto_shutdown_minutes=auto_shutdown_minutes[direction];break;
        case 16:
            value = s_settings.weight_step + direction;
            if (!folio_font_has_weight(s_font) || value < -2 || value > 2) return;
            s_settings.weight_step = (int8_t)value;
            folio_font_set_weight(s_font,value);
            break;
        default: return;
    }
    if (memcmp(&previous, &s_settings, sizeof(previous)) == 0) return;
    if(row==6 || row==14 || row==15) {
        if(!folio_settings_save(&s_settings)) {
            s_settings=previous;
            show_error(row==6?"空闲锁屏设置未能保存，已恢复之前的选择。":"自动关机设置未能保存，已恢复之前的选择。",VIEW_SETTINGS);
        } else show_view(VIEW_SETTINGS,false);
        return;
    }
    bool okay = folio_font_set_size(s_font, s_settings.font_size);
    if (okay && s_reader != NULL && (row <= 4 || row == 9 || row == 12 || row == 13)) {
        folio_reader_options_t options = reader_options(); okay = folio_reader_set_options(s_reader, &options);
    }
    if (!okay) {
        s_settings = previous; (void)folio_font_set_size(s_font, previous.font_size);
        if (s_reader != NULL) { folio_reader_options_t options = reader_options(); (void)folio_reader_set_options(s_reader, &options); }
        show_error("这次排版调整未能完成，已恢复之前的设置。", VIEW_SETTINGS); return;
    }
    if (!folio_settings_save(&s_settings)) {
        show_error("设置已应用，但未能保存到设备。重新启动后可能恢复之前的设置。", VIEW_SETTINGS); return;
    }
    show_view(VIEW_SETTINGS, false);
}
// Reopen instead of swapping the reader's font pointer: same-size faces have
// different metrics, and all pagination/checkpoint caches must be rebuilt.
static bool reopen_font_reader(size_t book_index, uint32_t anchor, bool cover) {
    if (!s_font || book_index >= s_book_count || !s_card_ready) return false;
    (void)folio_font_set_size(s_font, s_settings.font_size);
    folio_reader_options_t options = reader_options();
    s_reader = folio_reader_open_with_options(&s_books[book_index], s_font, &options);
    if (!s_reader) return false;
    // Opening itself may save a reflowed page start. Seek the captured byte
    // anchor explicitly, including on rollback, rather than trusting that save.
    if (*folio_reader_error(s_reader) || !folio_reader_seek(s_reader, anchor) || *folio_reader_error(s_reader)) {
        folio_reader_close(s_reader); s_reader = NULL;
        return false;
    }
    s_reader_cover = cover; s_reader_notice = NULL;
    return true;
}
static void select_font(size_t index) {
    if (index >= s_font_count || !save_reader()) return;
    folio_settings_t candidate = s_settings;
    snprintf(candidate.font_path, sizeof(candidate.font_path), "%s", s_fonts[index]);
    const char* desired = s_settings.font_path[0] ? s_settings.font_path : FOLIO_FONT_BUILTIN_PATH;
    if (s_font && !strcmp(s_active_font, s_fonts[index])) {
        // Explicitly choosing the temporary fallback replaces a remembered,
        // unavailable choice even though no font object needs to be reloaded.
        if (strcmp(desired, candidate.font_path)) {
            if (!folio_settings_save(&candidate)) {
                show_error("字体选择未能保存，已保留之前的选择。", s_fonts_return); return;
            }
            s_settings = candidate;
        }
        show_view(s_fonts_return, true); return;
    }
    bool had_reader = s_reader != NULL, previous_cover = s_reader_cover;
    uint32_t anchor = had_reader ? folio_reader_position(s_reader) : 0;
    size_t book_index = SIZE_MAX;
    if (had_reader) {
        for (size_t i = 0; i < s_book_count; ++i)
            if (!strcmp(s_books[i].path, s_current_book)) { book_index = i; break; }
        if (book_index == SIZE_MAX) {
            show_error("当前书籍不在书架中，字体选择未改变。请返回书架后重试。", s_fonts_return); return;
        }
    }
    char previous_font[256];
    snprintf(previous_font, sizeof(previous_font), "%s", s_active_font);
    show_busy("正在加载字体");
    close_reader(); folio_cover_clear_cache();
    // Two full TTFs may exceed PSRAM. Release first, then perform an explicit,
    // fallible rollback; never publish a candidate preference before it works.
    folio_font_close(s_font); s_font = NULL;
    enum { FONT_LOAD_FAILED, FONT_READER_FAILED, FONT_SAVE_FAILED } failure = FONT_LOAD_FAILED;
    bool ready = open_active_font(candidate.font_path);
    if (ready && had_reader) {
        failure = FONT_READER_FAILED;
        ready = reopen_font_reader(book_index, anchor, previous_cover);
    }
    if (ready) {
        failure = FONT_SAVE_FAILED;
        if (folio_settings_save(&candidate)) {
            s_settings = candidate; s_turns_since_full = 0;
            show_view(s_fonts_return, true); return;
        }
    }
    // Keep s_settings unchanged, so an unrelated later save cannot accidentally
    // persist this failed selection. Roll back the actual face independently.
    folio_reader_close(s_reader); s_reader = NULL;
    folio_font_close(s_font); s_font = NULL;
    if (folio_power_shutdown_pending()) { emergency_shutdown(); return; }
    bool restored_font = open_active_font(previous_font);
    if (!restored_font && strcmp(previous_font, FOLIO_FONT_BUILTIN_PATH))
        (void)open_active_font(FOLIO_FONT_BUILTIN_PATH);
    bool restored_reader = !had_reader || reopen_font_reader(book_index, anchor, previous_cover);
    if (!s_font) {
        show_error("字体切换未完成，原选择已保留。当前字体无法加载，请重试或重新启动。", s_fonts_return); return;
    }
    if (!restored_font) {
        show_error(restored_reader ? "原字体暂时无法加载，现临时使用内置宋体。记住的字体选择未改变。" :
            "原字体暂时无法加载，现临时使用内置宋体，原选择未改变。书籍未能打开，请返回书架重试。", s_fonts_return); return;
    }
    if (!restored_reader) {
        show_error("原字体选择已保留，但书籍未能重新打开。请返回书架重试。", s_fonts_return); return;
    }
    const char* message = failure == FONT_SAVE_FAILED ? "字体选择未能保存，已恢复原字体和阅读位置。" :
        failure == FONT_READER_FAILED ? "新字体未能完成阅读排版，已恢复原字体和阅读位置。" :
        !strcmp(candidate.font_path, FOLIO_FONT_BUILTIN_PATH) ? "内置宋体未能加载，已恢复原字体。" :
        folio_font_is_builtin_path(candidate.font_path) ? "已恢复原字体。可将配套字库放入 TF 卡根目录 folio-fonts/ 后重试；内存不足时也无法加载。" :
        "无法加载这款字体，已恢复原字体。请检查 TF 卡和 TrueType glyf 字体文件。";
    show_error(message, s_fonts_return);
}
static void emergency_shutdown(void) {
    static bool files_closed;
    // Nonblocking: PMU poll continues, but no font/card/network teardown while
    // the online worker can still hold a descriptor or the final install lock.
    atomic_fetch_or(&s_online_safety,ONLINE_CANCEL);
    if(!online_quiesce())return;
    if (!files_closed) {
        if(!folio_weread_stop() || !folio_transfer_stop())return;
        files_closed = true;
        // PMU shutdown has a deadline. A failed write must not wait for a modal
        // acknowledgement, redraw the display, or prevent the readiness reply.
        activity_track(false, false);
        if (s_reader != NULL && !folio_reader_save(s_reader))
            ESP_LOGE(TAG, "emergency progress save failed: %s", folio_reader_error(s_reader));
        close_reader(); (void)catalog_save(); catalog_close(); activity_close(); folio_cover_clear_cache(); folio_font_close(s_font); s_font = NULL;
        esp_err_t error = read_pico_sd_sync();
        if (error != ESP_OK) ESP_LOGE(TAG, "emergency card sync failed: %s", esp_err_to_name(error));
    }
    (void)folio_power_finish_shutdown(&s_hw);
}
static void refresh_locked_time(void* context) {
    (void)context;
    if(!s_font || folio_power_shutdown_pending())return;
    int64_t idle=s_last_activity;
    draw_lock(false);present(false);s_last_activity=idle;
}
static void suspend_device(bool shutdown) {
    if(!stop_transfer()){s_suspend_pending=true;s_suspend_power_off=shutdown;return;}
    s_suspend_pending=false;s_online_wifi_setup=false;
    view_t wake_view=s_view;
    if(wake_view==VIEW_TRANSFER || wake_view==VIEW_WEREAD || wake_view==VIEW_WIFI || wake_view==VIEW_WIFI_EDIT || wake_view==VIEW_TIME)wake_view=VIEW_SYSTEM;
    if (!s_hw.pmu_ready) { show_error("电源管理器未就绪，无法进入低功耗状态。", s_view); return; }
    if (folio_power_poll() == FOLIO_POWER_SHUTDOWN_NOW) { emergency_shutdown(); return; }
    if (!save_reader()) return;
    activity_record_book();
    if (!activity_flush()) {
        show_error("阅读统计尚未保存，请检查 TF 卡后再锁屏或关机。当前记录仍保留在内存中。", s_view); return;
    }
    if(!catalog_save()) {show_error("书架记录尚未保存，请检查 TF 卡后再锁屏。",s_view);return;}
    activity_track(false,false);
    if (s_font == NULL) {
        epd_hl_set_all_white(&s_hw.hl);
        folio_gfx_text(fb(), 48, 156, 3, shutdown ? "POWER OFF" : "LOCKED", 0x00);
        folio_gfx_text(fb(), 48, 256, 2, "PRESS POWER TO WAKE", 0x00);
        present(true);
    } else {
        draw_lock(shutdown);
        if (folio_power_shutdown_pending()) { emergency_shutdown(); return; }
        present(true);
    }
    if(!shutdown) {
        // Reader/activity/catalog saves above already fflush+fsync their records.
        // read_pico_sd_sync() UNMOUNTS the volume: never use it with live handles.
        activity_track(false,false); // Keeping the page visible must not time the sleep.
        ESP_LOGI(TAG,"Light lock: retaining mounted card, reader and font");
        bool timed_out=false;
        uint16_t minutes=s_settings.auto_shutdown_enabled?s_settings.auto_shutdown_minutes:0;
        esp_err_t error=folio_power_lock_timed(&s_hw,
            FOLIO_LOCK_STYLE(&s_settings)==2?refresh_locked_time:NULL,NULL,minutes,&timed_out);
        if(folio_power_shutdown_pending()){emergency_shutdown();return;}
        s_last_activity=esp_timer_get_time();
        if(error==ESP_OK && timed_out) {
            ESP_LOGI(TAG,"Light lock: auto shutdown after %u minutes",(unsigned)minutes);
            // Reuse the saved, rendered, unmounted shutdown path, never cut power here.
            suspend_device(true);return;
        }
        if(error==ESP_OK){ESP_LOGI(TAG,"Light lock: resumed without reboot");show_view(wake_view,true);}
        else show_error("锁屏待机未能完成，已恢复界面。",wake_view);
        return;
    }
    close_reader(); catalog_close(); activity_close(); folio_cover_clear_cache(); folio_font_close(s_font); s_font = NULL;
    esp_err_t error = read_pico_sd_sync();
    if (error == ESP_OK) error = folio_power_suspend(&s_hw, shutdown);
    if (folio_power_shutdown_pending()) { emergency_shutdown(); return; }
    ESP_LOGW(TAG, "power-down did not complete: %s", esp_err_to_name(error));
    load_card(true, true);
    if (s_font != NULL) show_error("设备未能进入低功耗状态，已恢复阅读。", s_reader ? VIEW_READING : VIEW_LIBRARY);
}
static void enter_fonts(void) { s_fonts_return = s_view; scan_fonts(); s_list_page = 0; show_view(VIEW_FONTS, true); }
static void tools_action(int row) {
    switch (row) {
        case 0: show_view(VIEW_READING, false); break;
        case 1: {
            size_t chapter = folio_reader_current_chapter(s_reader);
            s_list_page = chapter == SIZE_MAX ? 0 : chapter / list_rows(); show_view(VIEW_TOC, true); break;
        }
        case 2:
            if (!folio_reader_toggle_bookmark(s_reader)) show_error(folio_reader_error(s_reader), VIEW_TOOLS);
            else show_view(VIEW_TOOLS, false);
            break;
        case 3: s_list_page = 0; show_view(VIEW_BOOKMARKS, true); break;
        case 4: enter_reader_sync(); break;
        case 5: s_settings_tab = 0; s_settings_return = VIEW_TOOLS; show_view(VIEW_SETTINGS, true); break;
        case 6: enter_fonts(); break;
        case 7: show_library(); break;
        case 8: s_turns_since_full = 0; show_view(VIEW_READING, true); break;
    }
}
#include "folio_manage.inc"
#include "folio_weread_ui.inc"
#include "folio_setup.inc"
#include "folio_online_ui.inc"
static void load_wallpapers(void) {
    char error[128];
    if(!folio_storage_wallpapers(s_wallpaper_page*7,s_file_entries,7,
        &s_wallpaper_count,&s_wallpaper_total,error,sizeof(error))){
        show_error(error,VIEW_HOME_SETTINGS);return;
    }
    if(!folio_storage_wallpaper_load(s_wallpaper_path,sizeof(s_wallpaper_path)))s_wallpaper_path[0]=0;
    show_view(VIEW_WALLPAPERS,true);
}
static void select_wallpaper(const char* path) {
    char error[192] = {0};
    folio_wallpaper_image_t image = {0};
    if (folio_main_io_cancelled()) { emergency_shutdown(); return; }
    show_busy("正在适配壁纸，请稍候");
    if (!folio_storage_lock(2000)) {
        show_error("TF 卡正忙，壁纸尚未更改，请稍后重试。", VIEW_WALLPAPERS);
        return;
    }
    bool ready = folio_storage_validate_parent(path, error, sizeof(error)) &&
        folio_wallpaper_decode_cropped(path, 684, 1216, &image, error, sizeof(error));
    bool saved = ready && folio_storage_wallpaper_save_prepared(path, image.pixels,
        image.width, image.height, error, sizeof(error));
    folio_wallpaper_image_free(&image);
    folio_storage_unlock();
    if (folio_power_shutdown_pending()) { emergency_shutdown(); return; }
    if (!saved) {
        show_error(error[0] ? error : "图片未能完成处理，请检查 TF 卡后重试。", VIEW_WALLPAPERS);
        return;
    }
    snprintf(s_wallpaper_path, sizeof(s_wallpaper_path), "%s", path);
    folio_settings_t candidate = s_settings;
    candidate.keep_page_on_lock = (candidate.keep_page_on_lock & 3u) | (2u << 2);
    if (!folio_settings_save(&candidate)) {
        show_error("壁纸已选择，关机样式尚未保存，请重试。", VIEW_WALLPAPERS);
        return;
    }
    s_settings = candidate;
    s_lock_preview_return = VIEW_WALLPAPERS;
    s_lock_preview_off = true;
    show_view(VIEW_LOCK_PREVIEW, true);
}
static void go_back(void) {
    if(online_setup_back())return;
    if(online_active() && s_view!=VIEW_OFF_CONFIRM) {
        (void)online_leave(VIEW_DEVICE);return;
    }
    if(setup_back())return;
    if(s_view==VIEW_WEREAD){weread_back();return;}
    if(manage_back())return;
    switch(s_view) {
        case VIEW_SETTINGS: show_view(s_settings_return,true); break;
        case VIEW_FONTS: show_view(s_fonts_return,true); break;
        case VIEW_SYSTEM: show_root(VIEW_HOME); break;
        case VIEW_HOME_SETTINGS: show_view(VIEW_SYSTEM,true); break;
        case VIEW_WALLPAPERS:show_view(VIEW_HOME_SETTINGS,true);break;
        case VIEW_ERROR: show_view(s_error_return,true); break;
        case VIEW_OFF_CONFIRM:s_suspend_pending=false;show_view(s_power_return,true);break;
        case VIEW_BOOKMARK_DELETE: show_view(VIEW_BOOKMARKS,true); break;
        default: show_view(s_reader?VIEW_READING:VIEW_LIBRARY,true); break;
    }
}
static void handle_tap(int x,int y) {
    if(x<0 || x>=width() || y<0 || y>=height())return;
    activity_track(s_view==VIEW_READING,true);
    if(online_active() && s_view!=VIEW_OFF_CONFIRM) {
        if(hit(back_rect(),x,y)){(void)online_leave(VIEW_DEVICE);return;}
        if(s_view==VIEW_UPDATE)online_tap(x,y);
        else if(s_view==VIEW_ERROR && hit(done_rect(),x,y))(void)online_leave(VIEW_DEVICE);
        return;
    }
    if(s_view==VIEW_OFF_CONFIRM) {
        if(hit(back_rect(),x,y) || hit((EpdRect){48,984,588,96},x,y))go_back();
        else if(hit(done_rect(),x,y))suspend_device(true);
        return;
    }
    if(!s_font) {
        if(s_view==VIEW_TRANSFER) {if(hit(back_rect(),x,y) || hit(done_rect(),x,y))manage_back();}
        else if(s_view==VIEW_FORMAT_CONFIRM || s_view==VIEW_FORMAT_FINAL) {
            if(hit(back_rect(),x,y) || hit((EpdRect){48,984,588,96},x,y)){s_format_armed=false;show_view(VIEW_HOME,true);}
            else if(hit(done_rect(),x,y)) {
                if(s_view==VIEW_FORMAT_CONFIRM){s_format_armed=true;show_view(VIEW_FORMAT_FINAL,true);}
                else format_card();
            }
        }
        else if(hit((EpdRect){48,984,588,96},x,y) && s_card_ready)enter_transfer();
        else if(hit((EpdRect){48,984,588,96},x,y) && card_needs_format()){s_format_armed=false;show_view(VIEW_FORMAT_CONFIRM,true);}
        else if(hit(done_rect(),x,y))load_card(true,true);
        return;
    }
    if(s_view==VIEW_LOCK_PREVIEW){show_view(s_lock_preview_return,true);return;}
    if(s_view==VIEW_HOME || s_view==VIEW_LIBRARY || s_view==VIEW_SYSTEM) {
        int nav=root_nav_hit(x,y);
        const view_t roots[]={VIEW_HOME,VIEW_LIBRARY,VIEW_SYSTEM};
        if(nav>=0) {if(s_view!=roots[nav])show_root(roots[nav]);return;}
    }
    if(s_view!=VIEW_HOME && s_view!=VIEW_SYSTEM && s_view!=VIEW_LIBRARY && s_view!=VIEW_READING && s_view!=VIEW_TOOLS && hit(back_rect(),x,y)) { go_back(); return; }
    if(s_view==VIEW_ERROR) { if(hit(done_rect(),x,y))go_back(); return; }
    if(setup_tap(x,y))return;
    if(manage_tap(x,y))return;
    if(s_view==VIEW_WEREAD){weread_tap(x,y);return;}
    if(s_view==VIEW_BOOKMARK_DELETE) {
        if(hit((EpdRect){48,984,588,96},x,y))go_back();
        else if(hit(done_rect(),x,y)) {
            if(!folio_reader_delete_bookmark(s_reader,s_pending_bookmark))show_error(folio_reader_error(s_reader),VIEW_BOOKMARKS);
            else {
                size_t count=folio_reader_bookmark_count(s_reader);
                if(s_list_page && s_list_page*list_rows()>=count)--s_list_page;
                show_view(VIEW_BOOKMARKS,true);
            }
        }
        return;
    }
    if(s_view==VIEW_HOME) {
        if(hit(home_continue_rect(),x,y)) {open_recent(0);return;}
        for(unsigned i=0;i<folio_activity_recent_count(s_activity);++i)
            if(hit(home_recent_rect(i),x,y)){open_recent(i);return;}
    } else if(s_view==VIEW_HOME_SETTINGS) {
        if(hit((EpdRect){48,232,588,96},x,y)){s_fonts_return=VIEW_HOME_SETTINGS;scan_fonts();s_list_page=0;show_view(VIEW_FONTS,true);return;}
        uint8_t old_chart=s_settings.home_chart,old_style=s_settings.keep_page_on_lock;
        for(unsigned i=0;i<2;++i)if(hit((EpdRect){48+(int)i*300,400,288,88},x,y))s_settings.home_chart=i;
        for(unsigned i=0;i<3;++i){
            if(hit((EpdRect){48+(int)i*200,576,188,88},x,y))s_settings.keep_page_on_lock=(s_settings.keep_page_on_lock&12u)|i;
            if(hit((EpdRect){48+(int)i*200,752,188,88},x,y))s_settings.keep_page_on_lock=(s_settings.keep_page_on_lock&3u)|(i<<2);
        }
        if(hit((EpdRect){48,848,588,72},x,y)){s_wallpaper_page=0;load_wallpapers();return;}
        if(hit((EpdRect){48,944,282,96},x,y)||hit((EpdRect){354,944,282,96},x,y)){
            s_lock_preview_return=VIEW_HOME_SETTINGS;s_lock_preview_off=x>=342;show_view(VIEW_LOCK_PREVIEW,true);return;}
        if(old_chart==s_settings.home_chart && old_style==s_settings.keep_page_on_lock)return;
        if(!folio_settings_save(&s_settings))show_error("设置尚未保存，请重试。",VIEW_HOME_SETTINGS);
        else show_view(VIEW_HOME_SETTINGS,false);
    } else if(s_view==VIEW_WALLPAPERS) {
        int row=list_hit(x,y);
        if(row>=0 && (size_t)row<s_wallpaper_count){
            select_wallpaper(s_file_entries[row].path);return;
        }
        int direction=footer_hit(x,y);
        if(direction==0 && s_wallpaper_page){--s_wallpaper_page;load_wallpapers();}
        else if(direction==2 && (s_wallpaper_page+1)*7<s_wallpaper_total){++s_wallpaper_page;load_wallpapers();}
    } else if(s_view==VIEW_LIBRARY) {
        if(hit(library_tool(0),x,y)){show_view(VIEW_CATALOG,true);return;}
        int mode=-1;
        if(hit(library_tool(1),x,y))mode=0;
        else if(hit(library_tool(2),x,y))mode=1;
        if(mode>=0) {
            if(s_settings.library_list!=(uint8_t)mode) {
                s_settings.library_list=(uint8_t)mode;
                if(!folio_settings_save(&s_settings))show_error("书架视图已切换，但未能保存。",VIEW_LIBRARY);
                else show_view(VIEW_LIBRARY,true);
            }
            return;
        }
        if(hit((EpdRect){32,1004,128,96},x,y))move_list(-1);
        else if(hit((EpdRect){524,1004,128,96},x,y))move_list(1);
        else if(!s_shelf_count && hit((EpdRect){174,704,336,96},x,y)) {
            if(s_book_count)show_view(VIEW_CATALOG,true);
            else if(rescan_library())show_view(VIEW_LIBRARY,true);
        }
        else { int row=library_hit(x,y);size_t i=s_shelf_page*LIBRARY_PAGE_SIZE+(size_t)(row<0?0:row);
            if(row>=0 && i<s_shelf_count)open_book_absolute(s_shelf_indexes[i],true); }
    } else if(s_view==VIEW_READING) {
        if(x>=width()/3 && x<2*width()/3)show_view(VIEW_TOOLS,true);
        else turn_page(x>=2*width()/3);
    } else if(s_view==VIEW_TOOLS) {
        if(y<480 || hit((EpdRect){56,504,368,116},x,y) || hit((EpdRect){56,1064,572,112},x,y)) {tools_action(0);return;}
        const int actions[]={1,3,2,4,5,6};
        for(unsigned i=0;i<6;++i)if(hit(tool_rect(i),x,y)){tools_action(actions[i]);return;}
        if(hit((EpdRect){56,944,278,96},x,y))tools_action(7);
        else if(hit((EpdRect){350,944,278,96},x,y))tools_action(8);
    } else if(s_view==VIEW_TOC || s_view==VIEW_BOOKMARKS || s_view==VIEW_FONTS) {
        int bottom=footer_hit(x,y), row=list_hit(x,y);
        if(bottom==0 || bottom==2) {move_list(bottom==0?-1:1);return;}
        if(row<0)return;
        size_t index=s_list_page*list_rows()+(size_t)row;
        if(s_view==VIEW_FONTS) {select_font(index);return;}
        size_t count=s_view==VIEW_TOC?folio_reader_chapter_count(s_reader):folio_reader_bookmark_count(s_reader);
        if(index>=count)return;
        if(s_view==VIEW_BOOKMARKS && x>=540) {s_pending_bookmark=index;show_view(VIEW_BOOKMARK_DELETE,true);return;}
        if(s_view==VIEW_BOOKMARKS && x>=524)return;
        show_busy("正在定位阅读位置"); (void)folio_font_set_size(s_font,s_settings.font_size);
        bool okay=s_view==VIEW_TOC?folio_reader_jump_chapter(s_reader,index):folio_reader_jump_bookmark(s_reader,index);
        if(okay)show_view(VIEW_READING,true); else show_error(folio_reader_error(s_reader),VIEW_READING);
    } else if(s_view==VIEW_JUMP) {
        if(hit(done_rect(),x,y)) {
            show_busy("正在定位阅读位置"); (void)folio_font_set_size(s_font,s_settings.font_size);
            if(folio_reader_seek_percent(s_reader,s_jump_percent))show_view(VIEW_READING,true);
            else show_error(folio_reader_error(s_reader),VIEW_READING);
            return;
        }
        if(hit((EpdRect){40,432,604,96},x,y)) {
            int position=(x-48)*100/588; unsigned percent=position<0?0:position>100?100:(unsigned)position;
            if(percent!=s_jump_percent){s_jump_percent=percent;show_view(VIEW_JUMP,false);}return;
        }
        const int steps[]={-10,-1,1,10};
        for(unsigned i=0;i<4;++i)if(hit(jump_step(i),x,y)) {
            int value=(int)s_jump_percent+steps[i]; unsigned percent=value<0?0:value>100?100:(unsigned)value;
            if(percent!=s_jump_percent){s_jump_percent=percent;show_view(VIEW_JUMP,false);}return;
        }
    } else if(s_view==VIEW_SETTINGS) {
        if(hit(done_rect(),x,y)){go_back();return;}
        if(hit((EpdRect){48,216,588,96},x,y)) {
            unsigned tab=(unsigned)(x-48)/200; if(tab>2)tab=2; if(tab!=s_settings_tab){s_settings_tab=tab;show_view(VIEW_SETTINGS,true);}return;
        }
        if(!s_settings_tab) {
            if(hit((EpdRect){48,328,588,160},x,y)){enter_fonts();return;}
            if (folio_font_has_weight(s_font)) {
                if(hit(setting_step(4,-1),x,y)){change_setting(16,-1);return;}
                if(hit(setting_step(4,1),x,y)){change_setting(16,1);return;}
            }
            for(int i=0;i<4;++i) {
                if(hit(setting_step(i,-1),x,y)){change_setting(i,-1);return;}
                if(hit(setting_step(i,1),x,y)){change_setting(i,1);return;}
            }
            if(hit((EpdRect){308,968,156,96},x,y) && FOLIO_FIRST_LINE_INDENT(&s_settings))change_setting(4,-1);
            else if(hit((EpdRect){480,968,156,96},x,y) && !FOLIO_FIRST_LINE_INDENT(&s_settings))change_setting(4,1);
        } else if(s_settings_tab==1) {
            for(int i=0;i<2;++i) {
                if(hit(setting_step(i,-1),x,y)){change_setting(5+i,-1);return;}
                if(hit(setting_step(i,1),x,y)){change_setting(5+i,1);return;}
            }
            if(hit((EpdRect){48,560,588,96},x,y)){change_setting(7,1);return;}
            if(hit(auto_shutdown_toggle(),x,y)){change_setting(14,1);return;}
            for(unsigned i=0;i<AUTO_SHUTDOWN_CHOICES;++i)if(hit(auto_shutdown_choice(i),x,y)){
                change_setting(15,(int)i);return;
            }
        } else {
            if(hit((EpdRect){48,336,588,96},x,y)){change_setting(9,1);return;}
            for(unsigned i=0;i<2;++i) {
                if(hit((EpdRect){280+(int)i*184,448,172,96},x,y)){change_setting(10,(int)i);return;}
                if(hit((EpdRect){280+(int)i*184,560,172,96},x,y)){change_setting(11,(int)i);return;}
            }
            for(unsigned i=0;i<3;++i) {
                if(hit((EpdRect){48+(int)i*200,736,188,88},x,y)){change_setting(12,(int)i);return;}
                if(hit((EpdRect){48+(int)i*200,896,188,88},x,y)){change_setting(13,(int)i);return;}
            }
        }
    } else if(s_view==VIEW_SYSTEM) {
        if(hit((EpdRect){48,936,282,96},x,y)){suspend_device(false);return;}
        if(hit((EpdRect){354,936,282,96},x,y)){show_view(VIEW_OFF_CONFIRM,true);return;}
        for(unsigned row=0;row<6;++row)if(hit(system_row(row),x,y)) {
            if(row==0){s_wr_reader_path[0]=0;enter_weread();}
            else if(row==1)enter_wifi();
            else if(row==2){s_settings_return=VIEW_SYSTEM;show_view(VIEW_SETTINGS,true);}
            else if(row==3)show_view(VIEW_HOME_SETTINGS,true);
            else if(row==4)enter_time();
            else show_view(VIEW_DEVICE,true);
            return;
        }
    }
}
static int key_at(int x,int y) {return y>=KEY_AREA_TOP && x>=0 && x<KEY_AREA_WIDTH?x/KEY_PITCH:-1;}
static void handle_key(int key,bool held) {
    activity_track(s_view==VIEW_READING,true);
    if(online_active() && s_view!=VIEW_OFF_CONFIRM) {
        if(key==1)(void)online_leave(VIEW_DEVICE);
        return;
    }
    if(held && key==1){s_turns_since_full=0;show_view(s_view,true);return;}
    if(s_view==VIEW_LOCK_PREVIEW){show_view(s_lock_preview_return,true);return;}
    if(s_view==VIEW_WALLPAPERS){
        if(key==1)go_back();
        else if(key==0 && s_wallpaper_page){--s_wallpaper_page;load_wallpapers();}
        else if(key==2 && (s_wallpaper_page+1)*7<s_wallpaper_total){++s_wallpaper_page;load_wallpapers();}
        return;
    }
    if(key==1 && (s_view==VIEW_OFF_CONFIRM || s_view==VIEW_BOOKMARK_DELETE)){go_back();return;}
    if(!s_font){if(s_view==VIEW_TRANSFER && key==1)manage_back();else load_card(true,true);return;}
    if(s_view==VIEW_HOME) {if(key==1)open_recent(0);else show_root(key==0?VIEW_LIBRARY:VIEW_SYSTEM);}
    else if(s_view==VIEW_READING) {if(key==1)show_view(VIEW_TOOLS,true);else turn_page(key==2);}
    else if(s_view==VIEW_WEREAD){if(key==1)weread_back();else weread_page(key==0?-1:1);}
    else if(s_view==VIEW_LIBRARY || s_view==VIEW_TOC || s_view==VIEW_BOOKMARKS || s_view==VIEW_FONTS) {
        if(key==1){if(s_view==VIEW_LIBRARY)show_view(VIEW_SYSTEM,true);else go_back();}
        else move_list(key==0?-1:1);
    } else if(s_view==VIEW_FILES && key!=1)files_move(key==0?-1:1);
    else if(key==1)go_back();
    else if(s_view==VIEW_SETTINGS){unsigned tab=key==0?(s_settings_tab+2)%3:(s_settings_tab+1)%3;if(s_settings_tab!=tab){s_settings_tab=tab;show_view(VIEW_SETTINGS,true);}}
}
// Clock refresh is automatic, so it must not postpone the user's idle lock.
static void refresh_home_clock(void) {
    if(s_reader_notice && s_view==VIEW_READING && esp_timer_get_time()>=s_reader_notice_until){s_reader_notice=NULL;show_view(VIEW_READING,false);}
    uint32_t minute=folio_power_unix_time()/60;
    if(s_view==VIEW_HOME && s_font) {
        bool changed=minute!=s_home_clock_minute;
        int64_t now=esp_timer_get_time();
        // Battery also updates with an unset RTC; avoid redraws on every PMU poll.
        if(now<s_home_status_checked || now-s_home_status_checked>=30000000) {
            char power[48];folio_power_status(power,sizeof(power),s_settings.timezone_minutes);
            const char* battery=strrchr(power,' ');battery=battery?battery+1:"--%";
            changed=changed || strcmp(battery,s_home_battery)!=0;
            s_home_status_checked=now;
        }
        if(changed) {
            int64_t last=s_last_activity;
            show_view(VIEW_HOME,false);
            s_last_activity=last;
        }
    }
}
#include "folio_debug.inc"

static bool idle_lock_due(bool pressed) {
    int64_t now=esp_timer_get_time();
    // A slow request need not emit progress; it is still active work.
    if(folio_weread_busy() || online_active()){s_last_activity=now;return false;}
    return !pressed && s_settings.idle_minutes && s_hw.pmu_ready &&
        now-s_last_activity>=(int64_t)s_settings.idle_minutes*60*1000000;
}

static void handle_power_event(folio_power_event_t event) {
    online_publish_safety();
    if(event==FOLIO_POWER_SHUTDOWN_NOW){emergency_shutdown();return;}
    if(event==FOLIO_POWER_LOCK){suspend_device(false);return;}
    if(event==FOLIO_POWER_OFF_REQUEST) {
        s_suspend_pending=false;
        if(online_active())online_cancel();
        // A long press still needs the existing voluntary-off confirmation.
        show_view(VIEW_OFF_CONFIRM,true);
    }
}
void app_main(void) {
    s_ui_task=xTaskGetCurrentTaskHandle();
    /* First line of every boot: why the device restarted and how much room is
     * left. A panic reset is what "生成 EPUB 后退回首页" usually looks like. */
    esp_reset_reason_t reset=esp_reset_reason();
    const char* reset_name=reset==ESP_RST_POWERON?"power-on":reset==ESP_RST_PANIC?"panic":
        reset==ESP_RST_INT_WDT?"int-wdt":reset==ESP_RST_TASK_WDT?"task-wdt":
        reset==ESP_RST_WDT?"wdt":reset==ESP_RST_BROWNOUT?"brownout":
        reset==ESP_RST_SW?"software":reset==ESP_RST_DEEPSLEEP?"deepsleep":
        reset==ESP_RST_EXT?"external":reset==ESP_RST_SDIO?"sdio":reset==ESP_RST_USB?"usb":"other";
    ESP_LOGW(TAG,"Boot: reset=%d(%s) internal=%u largest=%u psram=%u",(int)reset,reset_name,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bool settings_loaded = folio_settings_load(&s_settings);
    s_catalog_options.filter=(folio_catalog_filter_t)(s_settings.reserved[0]&3);
    if(s_catalog_options.filter>FOLIO_CATALOG_UNREAD)s_catalog_options.filter=FOLIO_CATALOG_ALL;
    s_catalog_options.sort=(folio_catalog_sort_t)((s_settings.reserved[0]>>2)&3);
    esp_err_t error = read_pico_init(&s_hw);
    if (error != ESP_OK) { ESP_LOGE(TAG, "board init: %s", esp_err_to_name(error)); return; }
    folio_power_init();
    s_power_initialized=true;
    while (folio_power_shutdown_pending()) {
        emergency_shutdown();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    epd_poweron(); epd_clear(); epd_poweroff();
    s_books = heap_caps_calloc(FOLIO_LIBRARY_LIMIT, sizeof(*s_books), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_books == NULL) s_books = calloc(FOLIO_LIBRARY_LIMIT, sizeof(*s_books));
    if (s_books == NULL) { ESP_LOGE(TAG, "book list allocation failed"); return; }
    // load_card owns bounded, PMU-aware waiting and late probe completion.
    load_card(false, true);
    folio_debug_init();
    if(!folio_update_confirm_boot(s_hw.touch_ready && s_hw.pmu_ready && !s_first_present))
        ESP_LOGW(TAG,"OTA boot verification is still pending");
    if (!settings_loaded && s_font != NULL) show_error("设备设置未能加载，暂时使用默认值。原有设备数据已保留。", s_reader ? VIEW_READING : VIEW_HOME);
    bool pressed = false, cancelled = false;
    int start_x = 0, start_y = 0, last_x = 0, last_y = 0;
    int64_t press_time = 0, next_power_poll = 0, next_card_poll = 0, next_activity_save = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (folio_power_shutdown_pending()) {
            emergency_shutdown();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (now >= next_power_poll) {
            next_power_poll = now + 250000;
            folio_power_event_t event=s_deferred_power;s_deferred_power=FOLIO_POWER_NONE;
            if(event==FOLIO_POWER_NONE)event=folio_power_poll();
            if(event!=FOLIO_POWER_NONE)pressed=false;
            handle_power_event(event);
            if(event==FOLIO_POWER_SHUTDOWN_NOW || folio_power_shutdown_pending())continue;
        }
        refresh_online();
        if(s_suspend_pending)suspend_device(s_suspend_power_off);
        folio_debug_poll();
        activity_track(s_view==VIEW_READING,false);
        if (s_hw.touch_ready) {
            cst836u_touch_t touch = {0};
            if (cst836u_read(s_hw.touch, &touch) == ESP_OK) {
                if (touch.touched) {
                    s_last_activity = now;
                    if (!pressed) { activity_track(s_view==VIEW_READING,true); start_x = touch.x; start_y = touch.y; press_time = now; cancelled = touch.count != 1; }
                    pressed = true;
                    if (touch.count != 1) cancelled = true;
                    last_x = touch.x; last_y = touch.y;
                } else if (pressed) {
                    pressed = false;
                    if (!cancelled) {
                        int first_key = key_at(start_x, start_y), last_key = key_at(last_x, last_y);
                        int dx = last_x - start_x, dy = last_y - start_y;
                        if (first_key >= 0 && last_key == first_key) handle_key(first_key, now - press_time >= 600000);
                        else if (first_key < 0 && last_key < 0 && abs(dx) < 36 && abs(dy) < 36) handle_tap(last_x, last_y);
                        else if (first_key < 0 && last_key < 0 && abs(dx) >= 100 && abs(dy) < 80) {
                            if (s_view == VIEW_READING) turn_page(dx < 0);
                            else if (s_view == VIEW_LIBRARY || s_view == VIEW_TOC || s_view == VIEW_BOOKMARKS || s_view == VIEW_FONTS) move_list(dx < 0 ? 1 : -1);
                            else if(s_view==VIEW_FILES)files_move(dx<0?1:-1);
                        }
                    }
                    s_last_activity = esp_timer_get_time();
                }
            }
        }
        now = esp_timer_get_time();
        if (folio_power_shutdown_pending()) continue;
        if (now >= next_card_poll) {
            next_card_poll = now + 2000000;
            if(s_card_probe_pending && !online_active())refresh_card_probe();
            read_pico_sd_info_t info;
            if (s_card_ready && (read_pico_sd_get_info(&info) != ESP_OK || !info.mounted)) {
                // Stop volume-owning workers before releasing reader/card state.
                if (!handle_card_lost()) continue;
            }
        }
        if(now>=next_activity_save && !online_active()) {
            next_activity_save=now+30000000;
            activity_record_book(); activity_flush(); (void)catalog_save();
        }
        if(!pressed){refresh_reader_pagination();refresh_home_clock();refresh_lock_preview();refresh_transfer_status();refresh_weread();refresh_setup();}
        uint32_t browser_utc;
        if(folio_network_take_utc(&browser_utc)) {bool saved=folio_power_set_time(browser_utc);setup_time_received(saved);if(!saved)ESP_LOGW(TAG,"PMU time could not be saved");}
        if (idle_lock_due(pressed)) suspend_device(false);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
