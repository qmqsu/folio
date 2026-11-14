#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../main/folio_activity.c"

#define SECOND INT64_C(1000000)
#define DAY INT64_C(20752)
#define UTC (DAY * 86400 + 12 * 3600)

static void clear_files(void) {
    for (unsigned i = 0; i < 2; ++i) {
        if (remove(slot_path(i)) != 0) assert(errno == ENOENT);
    }
}

static folio_activity_t* fresh(void) {
    clear_files();
    folio_activity_t* activity = folio_activity_open();
    assert(activity && !*folio_activity_error(activity));
    assert(!folio_activity_dirty(activity));
    assert(folio_activity_total_seconds(activity) == 0);
    return activity;
}

static void overwrite(const char* path, const void* bytes, size_t length) {
    FILE* file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void tick(folio_activity_t* activity, int64_t seconds, bool reading, bool interaction) {
    folio_activity_update(activity, seconds * SECOND, UTC + seconds, 0, reading, interaction);
}

static void touch(folio_activity_t* activity, const char* name, unsigned percent) {
    char path[256];
    snprintf(path, sizeof(path), FOLIO_SD_ROOT "/books/%s.txt", name);
    assert(folio_activity_touch_book(activity, path, name, percent, percent + 1, percent * 123));
}

static void test_mru(void) {
    folio_activity_t* activity = fresh();
    assert(folio_activity_recent_count(activity) == 0 && folio_activity_recent(activity, 0) == NULL);
    assert(folio_activity_touch_book(activity, FOLIO_SD_ROOT "/books/old.epub", "旧版上次阅读", 0, 0, 0));
    assert(folio_activity_recent(activity, 0)->page == 0);
    touch(activity, "第一本", 10); touch(activity, "第二本", 20); touch(activity, "第三本", 30);
    assert(folio_activity_recent_count(activity) == 3);
    assert(strcmp(folio_activity_recent(activity, 0)->title, "第三本") == 0);
    assert(strcmp(folio_activity_recent(activity, 2)->title, "第一本") == 0);
    touch(activity, "第二本", 42);
    assert(strcmp(folio_activity_recent(activity, 0)->title, "第二本") == 0);
    assert(strcmp(folio_activity_recent(activity, 1)->title, "第三本") == 0);
    assert(folio_activity_recent(activity, 0)->percent == 42 && folio_activity_recent(activity, 0)->position == 5166);
    assert(folio_activity_save(activity) && !folio_activity_dirty(activity));
    touch(activity, "第二本", 42);
    assert(!folio_activity_dirty(activity));
    assert(!folio_activity_touch_book(activity, "/outside/book.txt", "x", 0, 0, 0));
    assert(!folio_activity_touch_book(activity, FOLIO_SD_ROOT "/books/../x", "x", 0, 0, 0));
    assert(!folio_activity_touch_book(activity, FOLIO_SD_ROOT "/books/", "x", 0, 0, 0));
    assert(!folio_activity_touch_book(activity, FOLIO_SD_ROOT "/books/x", "x", 101, 0, 0));
    assert(folio_activity_recent_count(activity) == 3);
    folio_activity_close(activity);
    activity = folio_activity_open();
    assert(activity && !*folio_activity_error(activity));
    assert(folio_activity_recent_count(activity) == 3);
    assert(strcmp(folio_activity_recent(activity, 0)->title, "第二本") == 0);
    assert(folio_activity_recent(activity, 0)->page == 43);
    folio_activity_close(activity);
}

static void test_fractional_pause_and_idle(void) {
    folio_activity_t* activity = fresh();
    tick(activity, 0, true, false);
    for (int i = 1; i <= 100; ++i) {
        int64_t us = i * INT64_C(30000);
        folio_activity_update(activity, us, UTC + us / SECOND, 0, true, false);
    }
    assert(folio_activity_total_seconds(activity) == 3 && folio_activity_day_seconds(activity, DAY) == 3);
    tick(activity, 3, false, false);
    tick(activity, 1000, false, false);
    assert(folio_activity_total_seconds(activity) == 3);
    tick(activity, 1000, true, false);
    tick(activity, 1240, true, false);
    assert(folio_activity_total_seconds(activity) == 183); // Only 180 of 240 idle seconds.
    tick(activity, 1241, true, true); // Interaction renews prospectively, no backfill.
    tick(activity, 1242, true, false);
    assert(folio_activity_total_seconds(activity) == 184);
    tick(activity, 1242, false, false);
    tick(activity, 9000, false, false);
    assert(folio_activity_total_seconds(activity) == 184);
    folio_activity_close(activity);
    activity = folio_activity_open();
    folio_activity_update(activity, 0, UTC + 10000, 0, true, false);
    assert(folio_activity_total_seconds(activity) == 184); // A reboot does not count its gap.
    folio_activity_update(activity, SECOND, UTC + 10001, 0, false, false);
    assert(folio_activity_total_seconds(activity) == 185);
    folio_activity_close(activity);
}

static void test_midnight_and_timezone(void) {
    folio_activity_t* activity = fresh();
    int64_t before_midnight = DAY * 86400 - 8 * 3600 - 2;
    folio_activity_update(activity, 0, before_midnight, 480, true, false);
    folio_activity_update(activity, 4 * SECOND, before_midnight + 4, 480, false, false);
    assert(folio_activity_total_seconds(activity) == 4);
    assert(folio_activity_day_seconds(activity, DAY - 1) == 2);
    assert(folio_activity_day_seconds(activity, DAY) == 2);
    folio_activity_close(activity);
    activity = fresh();
    folio_activity_update(activity, 0, 1, -720, true, false);
    folio_activity_update(activity, 10 * SECOND, 11, -720, false, false);
    assert(folio_activity_day_seconds(activity, -1) == 10);
    folio_activity_close(activity);
}

static void test_unknown_and_clock_changes(void) {
    folio_activity_t* activity = fresh();
    folio_activity_update(activity, 0, 0, 0, true, false);
    folio_activity_update(activity, 10 * SECOND, 0, 0, true, false);
    folio_activity_update(activity, 20 * SECOND, UTC + 20, 0, true, false);
    assert(folio_activity_total_seconds(activity) == 20 && folio_activity_undated_seconds(activity) == 20);
    folio_activity_update(activity, 30 * SECOND, UTC + 30, 0, true, false);
    assert(folio_activity_day_seconds(activity, DAY) == 10);
    folio_activity_update(activity, 40 * SECOND, UTC + 3600, 0, true, false);
    assert(folio_activity_undated_seconds(activity) == 30 && folio_activity_total_seconds(activity) == 40);
    folio_activity_update(activity, 50 * SECOND, UTC + 3610, 0, true, false);
    assert(folio_activity_day_seconds(activity, DAY) == 20);
    folio_activity_update(activity, 60 * SECOND, UTC - 3600, 0, true, false);
    folio_activity_update(activity, 70 * SECOND, UTC - 3590, 480, true, false);
    assert(folio_activity_undated_seconds(activity) == 50 && folio_activity_total_seconds(activity) == 70);
    folio_activity_update(activity, 80 * SECOND, 0, 480, false, false);
    assert(folio_activity_undated_seconds(activity) == 60 && folio_activity_total_seconds(activity) == 80);
    assert(folio_activity_day_seconds(activity, 0) == 0);
    folio_activity_close(activity);
}

static void test_monotonic_edges(void) {
    folio_activity_t* activity = fresh();
    tick(activity, 100, true, false); tick(activity, 110, true, false);
    assert(folio_activity_total_seconds(activity) == 10);
    folio_activity_update(activity, SECOND, UTC + 111, 0, true, false);
    assert(folio_activity_total_seconds(activity) == 10);
    folio_activity_update(activity, 2 * SECOND, UTC + 112, 0, true, false);
    assert(folio_activity_total_seconds(activity) == 11);
    folio_activity_update(activity, -1, UTC + 113, 0, true, false);
    folio_activity_update(activity, 999 * SECOND, UTC + 999, 0, true, false);
    assert(folio_activity_total_seconds(activity) == 11);
    folio_activity_close(activity);
    activity = fresh();
    folio_activity_update(activity, INT64_MAX - 10 * SECOND, UTC, 0, true, false);
    folio_activity_update(activity, INT64_MAX, UTC + 10, 0, false, false);
    assert(folio_activity_total_seconds(activity) == 10);
    folio_activity_close(activity);
}

static void test_subsecond_restart(void) {
    folio_activity_t* activity = fresh();
    tick(activity, 0, true, false);
    folio_activity_update(activity, SECOND / 2, UTC, 0, false, false);
    assert(folio_activity_total_seconds(activity) == 0 && folio_activity_dirty(activity));
    folio_activity_close(activity);
    activity = folio_activity_open();
    tick(activity, 0, true, false);
    folio_activity_update(activity, SECOND / 2, UTC, 0, false, false);
    assert(folio_activity_total_seconds(activity) == 1 && folio_activity_day_seconds(activity, DAY) == 1);
    folio_activity_close(activity);
}

static void test_retention(void) {
    folio_activity_t* activity = fresh();
    for (int i = 0; i < 367; ++i) {
        int64_t mono = (int64_t)i * 86400 * SECOND;
        int64_t utc = UTC + (int64_t)i * 86400;
        folio_activity_update(activity, mono, utc, 0, true, false);
        folio_activity_update(activity, mono + 60 * SECOND, utc + 60, 0, false, false);
    }
    assert(folio_activity_total_seconds(activity) == 367 * 60);
    assert(folio_activity_day_seconds(activity, DAY) == 0);
    for (int i = 1; i < 367; ++i) assert(folio_activity_day_seconds(activity, (int32_t)DAY + i) == 60);
    folio_activity_close(activity);
    activity = folio_activity_open();
    assert(activity && !*folio_activity_error(activity));
    assert(folio_activity_total_seconds(activity) == 367 * 60);
    assert(folio_activity_day_seconds(activity, DAY + 1) == 60);
    folio_activity_close(activity);
}

static void test_torn_slot_and_failed_save(void) {
    folio_activity_t* activity = fresh();
    tick(activity, 0, true, false); tick(activity, 10, true, false);
    touch(activity, "已保存", 10);
    assert(folio_activity_save(activity)); // a
    tick(activity, 20, true, false);
    touch(activity, "新进度", 20);
    assert(mkdir(slot_path(1), 0700) == 0); // Opening the next destination really fails.
    assert(!folio_activity_save(activity) && folio_activity_dirty(activity));
    assert(*folio_activity_error(activity));
    folio_activity_t* saved = folio_activity_open();
    assert(saved && folio_activity_total_seconds(saved) == 10);
    assert(strcmp(folio_activity_recent(saved, 0)->title, "已保存") == 0);
    assert(*folio_activity_error(saved));
    folio_activity_close(saved);
    assert(rmdir(slot_path(1)) == 0);
    assert(folio_activity_save(activity) && !folio_activity_dirty(activity)); // retry b
    assert(!*folio_activity_error(activity));
    folio_activity_close(activity);
    activity = folio_activity_open();
    assert(activity && folio_activity_total_seconds(activity) == 20);
    assert(strcmp(folio_activity_recent(activity, 0)->title, "新进度") == 0);
    folio_activity_close(activity);
    overwrite(slot_path(1), "power-cut", 9);
    activity = folio_activity_open();
    assert(activity && folio_activity_total_seconds(activity) == 10);
    assert(strcmp(folio_activity_recent(activity, 0)->title, "已保存") == 0 && *folio_activity_error(activity));
    folio_activity_close(activity);
    overwrite(slot_path(0), "damaged", 7);
    activity = folio_activity_open();
    assert(activity && folio_activity_total_seconds(activity) == 0 && folio_activity_recent_count(activity) == 0);
    assert(*folio_activity_error(activity) && !folio_activity_dirty(activity));
    folio_activity_close(activity);
}

static void test_checksum_and_generation(void) {
    folio_activity_t* activity = fresh();
    touch(activity, "校验记录", 50); assert(folio_activity_save(activity));
    folio_activity_close(activity);
    activity_record_t record;
    FILE* file = fopen(slot_path(0), "rb"); assert(file);
    assert(fread(&record, 1, sizeof(record), file) == sizeof(record) && fclose(file) == 0);
    record.sequence = UINT32_MAX;
    record.checksum = checksum(&record, offsetof(activity_record_t, checksum));
    overwrite(slot_path(0), &record, sizeof(record));
    activity = folio_activity_open();
    assert(activity && !*folio_activity_error(activity));
    touch(activity, "序号回绕", 70); assert(folio_activity_save(activity)); // seq0 is newer than UINT32_MAX
    folio_activity_close(activity);
    activity = folio_activity_open();
    assert(activity && strcmp(folio_activity_recent(activity, 0)->title, "序号回绕") == 0);
    folio_activity_close(activity);
    file = fopen(slot_path(1), "rb"); assert(file);
    assert(fread(&record, 1, sizeof(record), file) == sizeof(record) && fclose(file) == 0);
    record.recent[0].percent = 88; // Same-size corruption, checksum unchanged.
    overwrite(slot_path(1), &record, sizeof(record));
    activity = folio_activity_open();
    assert(activity && strcmp(folio_activity_recent(activity, 0)->title, "校验记录") == 0);
    assert(*folio_activity_error(activity)); folio_activity_close(activity);
}

static void test_semantic_record_validation(void) {
    folio_activity_t* activity = fresh();
    touch(activity, "完整记录", 25); assert(folio_activity_save(activity));
    folio_activity_close(activity);
    activity_record_t original;
    FILE* file = fopen(slot_path(0), "rb"); assert(file);
    assert(fread(&original, 1, sizeof(original), file) == sizeof(original) && fclose(file) == 0);
    // A matching checksum is not sufficient: reject malformed fields before
    // exposing them to callers, including a path with no terminating byte.
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        activity_record_t invalid = original;
        invalid.sequence = original.sequence + 1;
        switch (scenario) {
            case 0: memset(invalid.recent[0].path, 'x', sizeof(invalid.recent[0].path)); break;
            case 1: invalid.recent[0].percent = 101; break;
            case 2: invalid.recent_count = FOLIO_RECENT_LIMIT + 1; break;
            case 3: invalid.undated_us = invalid.total_us + SECOND; break;
            default:
                invalid.newest_day = DAY;
                invalid.days[day_slot(DAY)].day = DAY;
                invalid.days[day_slot(DAY)].microseconds = SECOND;
                break; // Daily sum cannot exceed the lifetime sum.
        }
        invalid.checksum = checksum(&invalid, offsetof(activity_record_t, checksum));
        overwrite(slot_path(1), &invalid, sizeof(invalid));
        activity = folio_activity_open();
        assert(activity && *folio_activity_error(activity));
        assert(folio_activity_recent_count(activity) == 1);
        assert(strcmp(folio_activity_recent(activity, 0)->title, "完整记录") == 0);
        assert(folio_activity_total_seconds(activity) == 0);
        folio_activity_close(activity);
    }
}

static void test_free_does_not_retry_save(void) {
    folio_activity_t* activity = fresh();
    touch(activity, "已保存", 25); assert(folio_activity_save(activity));
    touch(activity, "仅在内存", 50);
    assert(mkdir(slot_path(1), 0700) == 0);
    assert(!folio_activity_save(activity) && folio_activity_dirty(activity));
    assert(rmdir(slot_path(1)) == 0); // A retry would now succeed.
    folio_activity_free(activity);
    assert(access(slot_path(1), F_OK) != 0 && errno == ENOENT);
    activity = folio_activity_open();
    assert(activity && folio_activity_recent_count(activity) == 1);
    assert(strcmp(folio_activity_recent(activity, 0)->title, "已保存") == 0);
    folio_activity_close(activity);
    folio_activity_free(NULL);
}

static void test_relocate(void) {
    folio_activity_t* a=fresh();touch(a,"迁移书",42);tick(a,0,true,true);tick(a,33,true,true);
    uint64_t seconds=folio_activity_total_seconds(a);assert(seconds==33);
    folio_activity_relocate(a,FOLIO_SD_ROOT "/books/迁移书.txt",FOLIO_SD_ROOT "/books/迁移书 [wr-61].epub");
    assert(folio_activity_save(a));folio_activity_close(a);a=folio_activity_open();
    const folio_recent_book_t* r=folio_activity_recent(a,0);
    assert(r&&!strcmp(r->path,FOLIO_SD_ROOT "/books/迁移书 [wr-61].epub")&&r->percent==42&&r->position==42*123);
    assert(folio_activity_total_seconds(a)==seconds);folio_activity_close(a);
}
int main(void) {
    assert(mkdir(FOLIO_SD_ROOT, 0700) == 0 || errno == EEXIST);
    assert(mkdir(FOLIO_SD_ROOT "/.folio", 0700) == 0 || errno == EEXIST);
    test_mru(); test_fractional_pause_and_idle(); test_midnight_and_timezone();
    test_unknown_and_clock_changes(); test_monotonic_edges(); test_subsecond_restart();
    test_retention(); test_torn_slot_and_failed_save(); test_checksum_and_generation();
    test_semantic_record_validation();
    test_free_does_not_retry_save();test_relocate();
    puts("Activity: MRU/restart, fractional time, pause/idle, midnight, clock changes, 366-day retention, torn slots and save retry passed");
    return 0;
}
