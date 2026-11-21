#include "folio_settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvs.h"
#include "nvs_flash.h"

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif

#define SETTINGS_MAGIC 0x32534c46u
#define SESSION_MAGIC 0x324c4c46u

typedef struct {
    uint32_t magic;
    uint32_t version;
    folio_settings_t settings;
    uint32_t checksum;
} settings_record_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    char path[256];
    uint32_t checksum;
} session_record_t;

// Frozen legacy wire sizes include the aligned u32 checksum, not today's struct.
#define SETTINGS_V2_BYTES 288u
#define SETTINGS_V3_BYTES 292u
_Static_assert(offsetof(settings_record_t, settings) == 8, "preserve record header");
_Static_assert(offsetof(folio_settings_t, footer_hidden) == 276, "preserve v2 settings");
_Static_assert(offsetof(folio_settings_t, auto_shutdown_enabled) == 280, "preserve v3 settings");
_Static_assert(offsetof(settings_record_t, checksum) == 292, "v4 checksum alignment");
_Static_assert(sizeof(settings_record_t) == 296, "v4 record size");

static bool s_nvs_ready;

static bool valid_auto_shutdown_minutes(uint16_t minutes) {
    return minutes == 30 || minutes == 180 || minutes == 360;
}

static uint16_t migrate_auto_shutdown_minutes(uint16_t minutes) {
    // Only recognized old choices migrate. Unknown values must still fail validation.
    if (minutes == 5 || minutes == 15) return 30;
    if (minutes == 60 || minutes == 120) return 180;
    return minutes;
}

static uint16_t migrate_idle_minutes(uint16_t minutes) {
    // Preserve only values accepted by older firmware. Round up within the new
    // range so a legacy non-multiple does not lock sooner than it used to.
    if (minutes > 60) return minutes;
    if (minutes < FOLIO_IDLE_MINUTES_MIN) return FOLIO_IDLE_MINUTES_MIN;
    if (minutes > FOLIO_IDLE_MINUTES_MAX) return FOLIO_IDLE_MINUTES_MAX;
    return (uint16_t)(((minutes + FOLIO_IDLE_MINUTES_STEP - 1) / FOLIO_IDLE_MINUTES_STEP) * FOLIO_IDLE_MINUTES_STEP);
}

static uint32_t checksum(const void* bytes, size_t count) {
    const unsigned char* p = bytes;
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < count; ++i) value = (value ^ p[i]) * 16777619u;
    return value;
}

static bool valid_settings(const folio_settings_t* value) {
    return value->font_size >= 24 && value->font_size <= 56 &&
           value->margin >= 24 && value->margin <= 80 &&
           value->line_gap <= 24 && value->paragraph_gap <= 32 &&
           value->refresh_every >= 1 && value->refresh_every <= 32 &&
           value->idle_minutes >= FOLIO_IDLE_MINUTES_MIN && value->idle_minutes <= FOLIO_IDLE_MINUTES_MAX &&
           value->idle_minutes % FOLIO_IDLE_MINUTES_STEP == 0 && value->timezone_minutes >= -720 &&
           value->timezone_minutes <= 840 && value->justify <= 1 &&
           value->auto_resume <= 1 && (value->keep_page_on_lock & 0xf0u) == 0 && FOLIO_LOCK_STYLE(value)<=2 && FOLIO_OFF_STYLE(value)<=2 && value->library_list <= 1 &&
           value->home_chart <= 1 && value->footer_hidden <= 1 &&
           value->progress_chapter <= 1 && value->underline <= 2 && value->underline_shade <= 2 &&
           value->weight_step >= -2 && value->weight_step <= 2 && value->auto_shutdown_enabled <= 1 && valid_auto_shutdown_minutes(value->auto_shutdown_minutes) &&
           memchr(value->font_path, '\0', sizeof(value->font_path)) != NULL;
}

void folio_settings_defaults(folio_settings_t* settings) {
    if (settings == NULL) return;
    memset(settings, 0, sizeof(*settings));
    settings->font_size = 38;
    settings->margin = 48;
    settings->line_gap = 10;
    settings->paragraph_gap = 12;
    settings->refresh_every = 8;
    settings->idle_minutes = 5;
    settings->timezone_minutes = 480;
    settings->justify = 1;
    settings->auto_resume = 0;
    settings->auto_shutdown_minutes = 30;
}

bool folio_settings_load(folio_settings_t* settings) {
    if (settings == NULL) return false;
    folio_settings_defaults(settings);
    // A failed NVS init must never erase another firmware's settings or credentials.
    s_nvs_ready = nvs_flash_init() == ESP_OK;
    if (!s_nvs_ready) return false;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("folio", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (error != ESP_OK) return false;
    settings_record_t record = {0};
    size_t size = sizeof(record);
    error = nvs_get_blob(handle, "settings_v2", &record, &size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (error != ESP_OK || record.magic != SETTINGS_MAGIC) return false;
    folio_settings_t loaded;
    folio_settings_defaults(&loaded);
    if (record.version == 2 || record.version == 3) {
        const size_t legacy_size = record.version == 2 ? SETTINGS_V2_BYTES : SETTINGS_V3_BYTES;
        const size_t end = legacy_size - sizeof(uint32_t);
        uint32_t old_checksum;
        if (size != legacy_size) return false;
        memcpy(&old_checksum, (const uint8_t*)&record + end, sizeof(old_checksum));
        if (old_checksum != checksum(&record, end)) return false;
        // Never copy the legacy checksum into the new fields (or their padding).
        memcpy(&loaded, &record.settings, end - offsetof(settings_record_t, settings));
        if (record.version == 2) {
            loaded.progress_chapter = FOLIO_READER_FOOTER(&loaded) == 1;
            if (loaded.progress_chapter) loaded.reserved[0] &= (uint8_t)~0x60u;
        }
    } else {
        if ((record.version != 4 && record.version != 5) || size != sizeof(record) ||
            record.checksum != checksum(&record, offsetof(settings_record_t, checksum))) return false;
        loaded = record.settings;
    }
    // Verify the original bytes above before migrating, without resetting other preferences.
    if (record.version < 5) loaded.weight_step = 0;
    loaded.auto_shutdown_minutes = migrate_auto_shutdown_minutes(loaded.auto_shutdown_minutes);
    loaded.idle_minutes = migrate_idle_minutes(loaded.idle_minutes);
    if (!valid_settings(&loaded)) return false;
    *settings = loaded;
    return true;
}

bool folio_settings_save(const folio_settings_t* settings) {
    if (!s_nvs_ready || settings == NULL || !valid_settings(settings)) return false;
    settings_record_t record = {0};
    record.magic = SETTINGS_MAGIC;
    record.version = 5;
    record.settings = *settings;
    record.checksum = checksum(&record, offsetof(settings_record_t, checksum));
    nvs_handle_t handle;
    if (nvs_open("folio", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_blob(handle, "settings_v2", &record, sizeof(record));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

static const char* slot_path(unsigned slot) {
    return slot == 0 ? FOLIO_SD_ROOT "/.folio/session.a" : FOLIO_SD_ROOT "/.folio/session.b";
}

static bool read_slot(unsigned slot, session_record_t* record) {
    FILE* file = fopen(slot_path(slot), "rb");
    if (file == NULL) return false;
    bool okay = fread(record, 1, sizeof(*record), file) == sizeof(*record) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return okay && record->magic == SESSION_MAGIC && record->path[0] != '\0' &&
           memchr(record->path, '\0', sizeof(record->path)) != NULL &&
           strncmp(record->path, FOLIO_SD_ROOT "/books/", strlen(FOLIO_SD_ROOT "/books/")) == 0 &&
           record->checksum == checksum(record, offsetof(session_record_t, checksum));
}

static int newest_slot(session_record_t records[2]) {
    bool a = read_slot(0, &records[0]);
    bool b = read_slot(1, &records[1]);
    if (!a) return b ? 1 : -1;
    if (!b) return 0;
    return (int32_t)(records[1].sequence - records[0].sequence) > 0 ? 1 : 0;
}

bool folio_session_load(char* path, unsigned capacity) {
    if (path == NULL || capacity == 0) return false;
    path[0] = '\0';
    session_record_t records[2];
    int slot = newest_slot(records);
    if (slot < 0 || strlen(records[slot].path) >= capacity) return false;
    memcpy(path, records[slot].path, strlen(records[slot].path) + 1);
    return true;
}

bool folio_session_save(const char* path) {
    if (path == NULL || strlen(path) >= sizeof(((session_record_t*)0)->path) ||
        strncmp(path, FOLIO_SD_ROOT "/books/", strlen(FOLIO_SD_ROOT "/books/")) != 0) return false;
    struct stat info;
    if (mkdir(FOLIO_SD_ROOT "/.folio", 0777) != 0 &&
        (stat(FOLIO_SD_ROOT "/.folio", &info) != 0 || !S_ISDIR(info.st_mode))) return false;
    session_record_t records[2];
    int newest = newest_slot(records);
    if (newest >= 0 && strcmp(records[newest].path, path) == 0) return true;
    unsigned target = newest == 0 ? 1 : 0;
    session_record_t record = {0};
    record.magic = SESSION_MAGIC;
    record.sequence = newest >= 0 ? records[newest].sequence + 1 : 1;
    memcpy(record.path, path, strlen(path) + 1);
    record.checksum = checksum(&record, offsetof(session_record_t, checksum));
    FILE* file = fopen(slot_path(target), "wb");
    if (file == NULL) return false;
    bool okay = fwrite(&record, 1, sizeof(record), file) == sizeof(record) && fflush(file) == 0;
    if (okay) okay = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) okay = false;
    return okay;
}
