#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"

static unsigned char persisted[1024], pending[1024];
static size_t persisted_size, pending_size;
static int init_failure, commit_failure, open_failure, set_failure;
esp_err_t nvs_flash_init(void) { return init_failure; }
esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    assert(strcmp(name, "folio") == 0);
    if (open_failure) return open_failure;
    if (mode == NVS_READONLY && !persisted_size) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* value, size_t* size) {
    assert(handle == 1 && strcmp(key, "settings_v2") == 0);
    if (!persisted_size) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < persisted_size) return 2;
    memcpy(value, persisted, persisted_size); *size = persisted_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t size) {
    assert(handle == 1 && strcmp(key, "settings_v2") == 0 && size <= sizeof(pending));
    if (set_failure) return set_failure;
    memcpy(pending, value, size); pending_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1);
    if (commit_failure) return commit_failure;
    memcpy(persisted, pending, pending_size); persisted_size = pending_size;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

#include "../../main/folio_settings.c"

static void test_v2_compatibility(void) {
    // The 0.3 record had two reserved bytes after library_list. Construct that
    // exact layout instead of serializing today's struct through the new API.
    typedef struct {
        uint16_t font_size, margin, line_gap, paragraph_gap, refresh_every, idle_minutes;
        int16_t timezone_minutes;
        uint8_t justify, auto_resume, keep_page_on_lock, library_list, reserved[2];
        char font_path[256];
    } old_settings_t;
    typedef struct { uint32_t magic, version; old_settings_t settings; uint32_t checksum; } old_record_t;
    _Static_assert(sizeof(old_settings_t) == offsetof(folio_settings_t, footer_hidden), "preserve v2 prefix for migration");
    _Static_assert(offsetof(old_settings_t, font_path) == offsetof(folio_settings_t, font_path), "preserve paths");
    _Static_assert(sizeof(old_record_t) == 288 && offsetof(old_record_t, checksum) == 284, "frozen v2 wire layout");
    old_record_t old = {.magic = SETTINGS_MAGIC, .version = 2,
        .settings = {.font_size = 44, .margin = 48, .line_gap = 12, .paragraph_gap = 16,
            .refresh_every = 8, .idle_minutes = 5, .timezone_minutes = 480,
            .justify = 1, .auto_resume = 1, .library_list = 1}};
    strcpy(old.settings.font_path, "/sdcard/fonts/existing.ttf");
    old.settings.reserved[1]=0x30; // 12-hour + legacy chapter page mode
    old.checksum = 0x05efea34u; // Frozen little-endian v2 fixture, independently FNV-1a checked.
    assert(checksum(&old, offsetof(old_record_t, checksum)) == old.checksum);
    memcpy(persisted, &old, sizeof(old)); persisted_size = sizeof(old);
    folio_settings_t loaded;
    assert(folio_settings_load(&loaded));
    assert(loaded.font_size == 44 && loaded.auto_resume == 1 && loaded.library_list == 1 && loaded.home_chart == 0);
    assert(strcmp(loaded.font_path, old.settings.font_path) == 0);
    assert(FOLIO_CLOCK_12H(&loaded) && loaded.progress_chapter && !FOLIO_READER_FOOTER(&loaded));
    old_settings_t expected = old.settings;
    expected.reserved[1] &= (uint8_t)~0x60u;
    assert(memcmp(&loaded, &expected, sizeof(expected)) == 0);
    assert(!loaded.footer_hidden && !loaded.underline);
    assert(!loaded.auto_shutdown_enabled && loaded.auto_shutdown_minutes == 30);
    loaded.home_chart = 1; assert(folio_settings_save(&loaded));
    assert(folio_settings_load(&loaded) && loaded.home_chart == 1 && loaded.auto_resume == 1);
    loaded.home_chart = 2; assert(!folio_settings_save(&loaded));
    assert(folio_settings_load(&loaded) && loaded.home_chart == 1);
    persisted_size = pending_size = 0;
}

static void test_v3_compatibility(void) {
    // Exact released v3 layout, deliberately independent of folio_settings_t.
    typedef struct {
        uint16_t font_size, margin, line_gap, paragraph_gap, refresh_every, idle_minutes;
        int16_t timezone_minutes;
        uint8_t justify, auto_resume, keep_page_on_lock, library_list, home_chart, reserved[1];
        char font_path[256];
        uint8_t footer_hidden, progress_chapter, underline, underline_shade;
    } legacy_settings_t;
    typedef struct { uint32_t magic, version; legacy_settings_t settings; uint32_t checksum; } legacy_record_t;
    _Static_assert(sizeof(legacy_settings_t) == 280, "frozen v3 settings size");
    _Static_assert(sizeof(legacy_record_t) == 292 && offsetof(legacy_record_t, checksum) == 288, "frozen v3 checksum alignment");
    legacy_record_t old = {.magic = 0x32534c46u, .version = 3,
        .settings = {.font_size = 42, .margin = 56, .line_gap = 14, .paragraph_gap = 18,
            .refresh_every = 12, .idle_minutes = 30, .timezone_minutes = -300, .justify = 1,
            .auto_resume = 1, .keep_page_on_lock = 9, .library_list = 1, .home_chart = 1,
            .reserved = {0x5b}, .font_path = "/sdcard/fonts/legacy-v3.ttf", .footer_hidden = 1,
            .progress_chapter = 1, .underline = 2, .underline_shade = 2}, .checksum = 0xdd0679e4u};
    assert(checksum(&old, offsetof(legacy_record_t, checksum)) == old.checksum);
    memcpy(persisted, &old, sizeof(old)); persisted_size = sizeof(old);
    folio_settings_t loaded;
    assert(folio_settings_load(&loaded));
    assert(memcmp(&loaded, &old.settings, sizeof(old.settings)) == 0);
    assert(!loaded.auto_shutdown_enabled && loaded.auto_shutdown_minutes == 30);
    assert(folio_settings_save(&loaded));
    assert(persisted_size == 296 && persisted[4] == 5);
    assert(folio_settings_load(&loaded) && memcmp(&loaded, &old.settings, sizeof(old.settings)) == 0);
    // Damaged or shortened legacy blobs must not silently import, or erase NVS.
    for (unsigned version = 2; version <= 3; ++version) {
        memcpy(persisted, &old, sizeof(old));
        persisted[4] = (unsigned char)version;
        persisted_size = version == 2 ? 288 : 292;
        persisted[28] ^= 0x40;
        assert(!folio_settings_load(&loaded));
        assert(loaded.font_size == 38 && !loaded.auto_shutdown_enabled && loaded.auto_shutdown_minutes == 30);
        assert(persisted_size == (version == 2 ? 288u : 292u));
        --persisted_size; assert(!folio_settings_load(&loaded));
    }
    persisted_size = pending_size = 0;
}

static void assert_default_settings(const folio_settings_t* loaded) {
    folio_settings_t defaults;
    folio_settings_defaults(&defaults);
    assert(memcmp(loaded, &defaults, sizeof(defaults)) == 0);
}

static settings_record_t v4_fixture(uint16_t minutes, uint8_t enabled) {
    // Nondefault values in every preference make accidental resets observable.
    settings_record_t record = {.magic = 0x32534c46u, .version = 4,
        .settings = {.font_size = 42, .margin = 56, .line_gap = 14, .paragraph_gap = 18,
            .refresh_every = 12, .idle_minutes = 30, .timezone_minutes = -300, .justify = 0,
            .auto_resume = 1, .keep_page_on_lock = 10, .library_list = 1, .home_chart = 1,
            .reserved = {0x5b}, .font_path = "/sdcard/fonts/existing-v4.ttf", .footer_hidden = 1,
            .progress_chapter = 1, .underline = 2, .underline_shade = 2,
            .auto_shutdown_enabled = enabled, .auto_shutdown_minutes = minutes}};
    record.checksum = checksum(&record, offsetof(settings_record_t, checksum));
    return record;
}

static void store_v4_fixture(const settings_record_t* record) {
    _Static_assert(sizeof(settings_record_t) == 296 && offsetof(settings_record_t, checksum) == 292,
                   "keep the released v4 wire layout");
    memcpy(persisted, record, sizeof(*record));
    persisted_size = sizeof(*record);
}

static void test_v4_compatibility(void) {
    const struct { uint16_t before, after; } durations[] = {
        {5, 30}, {15, 30}, {30, 30}, {60, 180}, {120, 180}, {180, 180}, {360, 360}
    };
    folio_settings_t loaded;
    for (unsigned i = 0; i < sizeof(durations)/sizeof(durations[0]); ++i) {
        for (uint8_t enabled = 0; enabled <= 1; ++enabled) {
            settings_record_t original = v4_fixture(durations[i].before, enabled);
            store_v4_fixture(&original);
            folio_settings_t expected = original.settings;
            expected.auto_shutdown_minutes = durations[i].after;
            assert(folio_settings_load(&loaded));
            // Every other byte/field, including the on/off state, must survive.
            assert(memcmp(&loaded, &expected, sizeof(expected)) == 0);
            assert(persisted_size == sizeof(original) && memcmp(persisted, &original, sizeof(original)) == 0);
            assert(folio_settings_save(&loaded));
            settings_record_t saved;
            memcpy(&saved, persisted, sizeof(saved));
            assert(persisted_size == 296 && saved.magic == original.magic && saved.version == 5);
            assert(saved.checksum == checksum(&saved, offsetof(settings_record_t, checksum)));
            assert(folio_settings_load(&loaded) && memcmp(&loaded, &expected, sizeof(expected)) == 0);

            settings_record_t damaged = original;
            damaged.checksum ^= 1u;
            store_v4_fixture(&damaged);
            assert(!folio_settings_load(&loaded));
            assert_default_settings(&loaded);
            assert(memcmp(persisted, &damaged, sizeof(damaged)) == 0);
            if (durations[i].before != durations[i].after) {
                // This checksum matches only the normalized bytes, never the raw blob.
                // Normalizing before verification must not rescue such corruption.
                damaged = v4_fixture(durations[i].after, enabled);
                damaged.settings.auto_shutdown_minutes = durations[i].before;
                assert(damaged.checksum != checksum(&damaged, offsetof(settings_record_t, checksum)));
                store_v4_fixture(&damaged);
                assert(!folio_settings_load(&loaded));
                assert_default_settings(&loaded);
                assert(memcmp(persisted, &damaged, sizeof(damaged)) == 0);
            }
        }
    }
    const uint16_t invalid[] = {0, 1, 10, 29, 31, 121, 179, 181, 359, 361, UINT16_MAX};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        for (uint8_t enabled = 0; enabled <= 1; ++enabled) {
            settings_record_t malformed = v4_fixture(invalid[i], enabled);
            store_v4_fixture(&malformed);
            assert(!folio_settings_load(&loaded));
            assert_default_settings(&loaded);
            assert(memcmp(persisted, &malformed, sizeof(malformed)) == 0);
        }
    }
    // A recognized old duration must not sanitize unrelated invalid fields.
    for (unsigned field = 0; field < 2; ++field) {
        settings_record_t malformed = v4_fixture(5, 1);
        if (field == 0) malformed.settings.font_size = 60;
        else malformed.settings.auto_shutdown_enabled = 2;
        malformed.checksum = checksum(&malformed, offsetof(settings_record_t, checksum));
        store_v4_fixture(&malformed);
        assert(!folio_settings_load(&loaded));
        assert_default_settings(&loaded);
    }
    persisted_size = pending_size = 0;
    puts("settings v4: all legacy/new durations, on/off and all other fields preserved; raw checksum first; unknown values rejected");
}

static void test_auto_shutdown(void) {
    const uint16_t durations[] = {30, 180, 360};
    const uint16_t invalid[] = {0, 1, 5, 10, 15, 29, 31, 60, 120, 121, 179, 181, 359, 361, UINT16_MAX};
    folio_settings_t config, loaded;
    assert(folio_settings_load(&config));
    assert(config.auto_shutdown_enabled == 0 && config.auto_shutdown_minutes == 30);
    for (unsigned i = 0; i < sizeof(durations)/sizeof(durations[0]); ++i) {
        config.auto_shutdown_enabled = 1; config.auto_shutdown_minutes = durations[i];
        assert(folio_settings_save(&config) && folio_settings_load(&loaded));
        assert(memcmp(&loaded, &config, sizeof(config)) == 0);
        config.auto_shutdown_enabled = 0;
        assert(folio_settings_save(&config) && folio_settings_load(&loaded));
        assert(memcmp(&loaded, &config, sizeof(config)) == 0);
    }
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        for (uint8_t enabled = 0; enabled <= 1; ++enabled) {
            config.auto_shutdown_minutes = invalid[i];
            config.auto_shutdown_enabled = enabled;
            assert(!folio_settings_save(&config));
            assert(folio_settings_load(&loaded) && loaded.auto_shutdown_minutes == 360 && !loaded.auto_shutdown_enabled);
        }
    }
    config.auto_shutdown_minutes = 30; config.auto_shutdown_enabled = 2;
    assert(!folio_settings_save(&config));
    persisted_size = pending_size = 0;
    puts("settings save: only 30/180/360 accepted, disabled remembers each new duration, default remains off/30");
}

static void test_idle_minutes(void) {
    folio_settings_t loaded;
    for (unsigned version = 2; version <= 4; ++version) {
        for (unsigned before = 0; before <= 60; ++before) {
            settings_record_t record = v4_fixture(180, 1);
            record.version = version;
            record.settings.idle_minutes = (uint16_t)before;
            size_t bytes = version == 2 ? SETTINGS_V2_BYTES : version == 3 ? SETTINGS_V3_BYTES : sizeof(record);
            size_t end = bytes - sizeof(uint32_t);
            folio_settings_t expected;
            folio_settings_defaults(&expected);
            memcpy(&expected, &record.settings, end - offsetof(settings_record_t, settings));
            if (version == 2) {
                expected.progress_chapter = FOLIO_READER_FOOTER(&expected) == 1;
                if (expected.progress_chapter) expected.reserved[0] &= (uint8_t)~0x60u;
            }
            expected.idle_minutes = before < 5 ? 5 : before > 30 ? 30 : (uint16_t)(((before + 4) / 5) * 5);
            uint32_t sum = checksum(&record, end);
            memcpy((unsigned char*)&record + end, &sum, sizeof(sum));
            memcpy(persisted, &record, bytes); persisted_size = bytes;
            assert(folio_settings_load(&loaded));
            assert(memcmp(&loaded, &expected, sizeof(expected)) == 0);
            assert(!memcmp(persisted, &record, bytes)); // Loading never writes migrated settings.
            assert(folio_settings_save(&loaded));
            assert(folio_settings_load(&loaded) && !memcmp(&loaded, &expected, sizeof(expected)));
        }
    }
    for (unsigned value = 0; value <= 65; ++value) {
        folio_settings_defaults(&loaded); loaded.idle_minutes = (uint16_t)value;
        bool allowed = value >= 5 && value <= 30 && value % 5 == 0;
        assert(folio_settings_save(&loaded) == allowed);
    }
    settings_record_t record = v4_fixture(180, 1);
    record.settings.idle_minutes = 61;
    record.checksum = checksum(&record, offsetof(settings_record_t, checksum));
    store_v4_fixture(&record);
    assert(!folio_settings_load(&loaded)); assert_default_settings(&loaded);
    record.settings.idle_minutes = 5;
    record.checksum = checksum(&record, offsetof(settings_record_t, checksum));
    record.settings.idle_minutes = 0; // A checksum of migrated bytes must not validate the original blob.
    store_v4_fixture(&record);
    assert(!folio_settings_load(&loaded)); assert_default_settings(&loaded);
    persisted_size = pending_size = 0;
    puts("idle: v2/v3/v4 recognized 0..60 migrated to 5-minute choices 5..30; all other bytes preserved; invalid saves/raw checksums rejected");
}

static void test_font_preferences(void) {
    const char* paths[] = {"", ":builtin:", ":builtin:hei", ":builtin:kai", ":builtin:yuan",
        ":builtin:future", "/sdcard/fonts/existing.ttf", "/sdcard/fonts/暂未插入的字体.TTF"};
    folio_settings_t loaded;
    assert(folio_settings_load(&loaded));
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        settings_record_t record = v4_fixture(180, 1);
        snprintf(record.settings.font_path, sizeof(record.settings.font_path), "%s", paths[i]);
        assert(folio_settings_save(&record.settings));
        unsigned char before[sizeof(persisted)]; memcpy(before, persisted, sizeof(before));
        assert(folio_settings_load(&loaded));
        assert(!memcmp(&loaded, &record.settings, sizeof(loaded)));
        assert(!memcmp(before, persisted, sizeof(before))); // No resource/card probe or migration write.
        folio_settings_t candidate = loaded;
        snprintf(candidate.font_path, sizeof(candidate.font_path), "%s", ":builtin:failed-choice");
        for (unsigned failure = 0; failure < 3; ++failure) {
            open_failure = failure == 0 ? 2 : 0;
            set_failure = failure == 1 ? 2 : 0;
            commit_failure = failure == 2 ? 2 : 0;
            assert(!folio_settings_save(&candidate));
            open_failure = set_failure = commit_failure = 0;
            assert(folio_settings_load(&loaded));
            assert(!memcmp(&loaded, &record.settings, sizeof(loaded)));
            assert(!memcmp(before, persisted, sizeof(before)));
        }
    }
    persisted_size = pending_size = 0;
    puts("font preferences: stable native/legacy/unknown IDs and external paths roundtrip without resource checks; open/set/commit failures retain bytes");
}

int main(void) {
    test_font_preferences();
    test_idle_minutes();
    test_v2_compatibility();
    test_v3_compatibility();
    test_v4_compatibility();
    test_auto_shutdown();
    folio_settings_t config, loaded;
    assert(folio_settings_load(&config));
    assert(config.font_size == 38 && !config.auto_resume && config.home_chart == 0 && config.idle_minutes == 5);
    for(unsigned lock=0;lock<3;++lock)for(unsigned off=0;off<3;++off){
        config.keep_page_on_lock=lock|(off<<2);assert(folio_settings_save(&config));
        assert(folio_settings_load(&loaded)&&FOLIO_LOCK_STYLE(&loaded)==lock&&FOLIO_OFF_STYLE(&loaded)==off);
    }
    config.keep_page_on_lock=3;assert(!folio_settings_save(&config));
    config.keep_page_on_lock=12;assert(!folio_settings_save(&config));config.keep_page_on_lock=0;
    config.font_size = 42;
    assert(folio_settings_save(&config));
    assert(folio_settings_load(&loaded) && loaded.font_size == 42);
    config.reserved[0]=0x50|9;
    assert(folio_settings_save(&config));
    assert(folio_settings_load(&loaded) && FOLIO_CLOCK_12H(&loaded) && FOLIO_READER_FOOTER(&loaded)==2 && (loaded.reserved[0]&15)==9);
    config.footer_hidden=1;config.progress_chapter=1;config.underline=2;config.underline_shade=2;
    assert(folio_settings_save(&config) && folio_settings_load(&loaded));
    assert(loaded.footer_hidden && loaded.progress_chapter && loaded.underline==2 && loaded.underline_shade==2);
    config.underline=3;assert(!folio_settings_save(&config));config.underline=2;
    config.font_size = 60;
    assert(!folio_settings_save(&config));
    assert(folio_settings_load(&loaded) && loaded.font_size == 42);
    commit_failure = 2; config.font_size = 50;
    assert(!folio_settings_save(&config));
    assert(folio_settings_load(&loaded) && loaded.font_size == 42);
    commit_failure = 0;
    persisted[20] ^= 0x80;
    assert(!folio_settings_load(&loaded) && loaded.font_size == 38);
    assert(persisted_size != 0); // Bad configuration was not erased.
    init_failure = 3;
    assert(!folio_settings_load(&loaded));
    assert(!folio_settings_save(&loaded));
    assert(persisted_size != 0);

    assert(mkdir(FOLIO_SD_ROOT, 0700) == 0);
    char path[256];
    assert(!folio_session_load(path, sizeof(path)) && path[0] == '\0');
    assert(!folio_session_save("/arbitrary/file.txt"));
    assert(folio_session_save(FOLIO_SD_ROOT "/books/first.txt"));
    assert(folio_session_load(path, sizeof(path)) && strcmp(path, FOLIO_SD_ROOT "/books/first.txt") == 0);
    assert(folio_session_save(FOLIO_SD_ROOT "/books/second.txt"));
    assert(folio_session_load(path, sizeof(path)) && strcmp(path, FOLIO_SD_ROOT "/books/second.txt") == 0);
    FILE* torn = fopen(FOLIO_SD_ROOT "/.folio/session.b", "wb");
    assert(torn != NULL && fwrite("torn", 1, 4, torn) == 4 && fclose(torn) == 0);
    assert(folio_session_load(path, sizeof(path)) && strcmp(path, FOLIO_SD_ROOT "/books/first.txt") == 0);
    assert(folio_session_save(FOLIO_SD_ROOT "/books/third.txt"));
    assert(folio_session_load(path, sizeof(path)) && strcmp(path, FOLIO_SD_ROOT "/books/third.txt") == 0);
    assert(!folio_session_load(path, 2));
    puts("settings: v2/v3/v4 compatibility and 30/180/360 persistence, chart preferences, missing/corrupt NVS, commit failure and torn-session recovery passed");
    return 0;
}
