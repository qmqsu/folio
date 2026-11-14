#include "folio_activity.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif

#define ACTIVITY_MAGIC UINT32_C(0x31414c46)
#define ACTIVITY_VERSION 1
#define USEC_PER_SECOND INT64_C(1000000)
#define USEC_PER_DAY (INT64_C(86400) * USEC_PER_SECOND)
#define IDLE_US ((int64_t)FOLIO_ACTIVITY_IDLE_SECONDS * USEC_PER_SECOND)
#define MAX_DAY_US ((uint64_t)UINT32_MAX * (uint64_t)USEC_PER_SECOND)
#define UNKNOWN_DAY INT32_MIN

typedef struct {
    int32_t day;
    uint32_t reserved;
    uint64_t microseconds;
} day_record_t;

typedef struct {
    uint32_t magic, version, sequence, byte_size;
    uint64_t total_us, undated_us;
    int32_t newest_day;
    uint32_t recent_count;
    day_record_t days[FOLIO_ACTIVITY_DAYS];
    folio_recent_book_t recent[FOLIO_RECENT_LIMIT];
    uint32_t checksum;
    uint32_t reserved[2];
} activity_record_t;

struct folio_activity {
    activity_record_t record;
    int active_slot;
    bool dirty, tracking, reading, wall_valid;
    int timezone_minutes;
    int64_t monotonic_us, active_until_us, wall_us;
    char error[160];
};

_Static_assert(sizeof(day_record_t) == 16, "Stable activity day record layout");
_Static_assert(sizeof(folio_recent_book_t) == 396, "Stable recent-book record layout");
_Static_assert(sizeof(activity_record_t) == 7096, "Stable activity file record layout");

static void set_error(folio_activity_t* activity, const char* text) {
    snprintf(activity->error, sizeof(activity->error), "%s", text);
}

static uint32_t checksum(const void* bytes, size_t size) {
    const uint8_t* p = bytes;
    uint32_t result = UINT32_C(2166136261);
    for (size_t i = 0; i < size; ++i) result = (result ^ p[i]) * UINT32_C(16777619);
    return result;
}

static bool valid_path(const char* path) {
    const char prefix[] = FOLIO_SD_ROOT "/books/";
    if (path == NULL || strncmp(path, prefix, sizeof(prefix) - 1) != 0 ||
        path[sizeof(prefix) - 1] == '\0') return false;
    if (strstr(path, "/../") || strstr(path, "/./")) return false;
    size_t size = strlen(path);
    if (path[size - 1] == '/' || (size >= 3 && strcmp(path + size - 3, "/..") == 0)) return false;
    for (size_t i = 0; i < size; ++i) if ((unsigned char)path[i] < 32) return false;
    return true;
}

static bool valid_recent(const folio_recent_book_t* book) {
    return memchr(book->path, '\0', sizeof(book->path)) != NULL &&
        memchr(book->title, '\0', sizeof(book->title)) != NULL &&
        book->title[0] != '\0' && valid_path(book->path) && book->percent <= 100;
}

static unsigned day_slot(int32_t day) {
    int remainder = day % FOLIO_ACTIVITY_DAYS;
    return (unsigned)(remainder < 0 ? remainder + FOLIO_ACTIVITY_DAYS : remainder);
}

static bool valid_record(const activity_record_t* record) {
    if (record->magic != ACTIVITY_MAGIC || record->version != ACTIVITY_VERSION ||
        record->byte_size != sizeof(*record) || record->reserved[0] != 0 || record->reserved[1] != 0 ||
        record->recent_count > FOLIO_RECENT_LIMIT || record->undated_us > record->total_us ||
        record->checksum != checksum(record, offsetof(activity_record_t, checksum))) return false;
    uint64_t dated_us = 0;
    for (unsigned i = 0; i < FOLIO_ACTIVITY_DAYS; ++i) {
        const day_record_t* day = &record->days[i];
        if (day->reserved != 0 || day->microseconds > MAX_DAY_US) return false;
        if (day->microseconds == 0) continue;
        if (record->newest_day == UNKNOWN_DAY || day->day > record->newest_day ||
            (int64_t)day->day < (int64_t)record->newest_day - FOLIO_ACTIVITY_DAYS + 1 ||
            day_slot(day->day) != i) return false;
        // 366 * MAX_DAY_US cannot overflow uint64_t.
        dated_us += day->microseconds;
    }
    if (dated_us > record->total_us - record->undated_us) return false;
    for (unsigned i = 0; i < record->recent_count; ++i) {
        if (!valid_recent(&record->recent[i])) return false;
        for (unsigned j = 0; j < i; ++j)
            if (strcmp(record->recent[i].path, record->recent[j].path) == 0) return false;
    }
    return true;
}

static const char* slot_path(unsigned slot) {
    return slot == 0 ? FOLIO_SD_ROOT "/.folio/activity.a" : FOLIO_SD_ROOT "/.folio/activity.b";
}

typedef enum { SLOT_MISSING, SLOT_VALID, SLOT_DAMAGED, SLOT_IO_ERROR } slot_status_t;

static slot_status_t read_slot(unsigned slot, activity_record_t* record) {
    FILE* file = fopen(slot_path(slot), "rb");
    if (!file) return errno == ENOENT ? SLOT_MISSING : SLOT_IO_ERROR;
    bool complete = fread(record, 1, sizeof(*record), file) == sizeof(*record);
    bool length_ok = complete && fgetc(file) == EOF;
    bool io_error = ferror(file) != 0;
    if (fclose(file) != 0) io_error = true;
    if (io_error) return SLOT_IO_ERROR;
    return complete && length_ok && valid_record(record) ? SLOT_VALID : SLOT_DAMAGED;
}

folio_activity_t* folio_activity_open(void) {
    folio_activity_t* activity = calloc(1, sizeof(*activity));
    activity_record_t* candidate = malloc(sizeof(*candidate));
    if (!activity || !candidate) { free(activity); free(candidate); return NULL; }
    activity->active_slot = -1;
    activity->record.magic = ACTIVITY_MAGIC;
    activity->record.version = ACTIVITY_VERSION;
    activity->record.byte_size = sizeof(activity->record);
    activity->record.newest_day = UNKNOWN_DAY;
    slot_status_t statuses[2];
    for (unsigned slot = 0; slot < 2; ++slot) {
        statuses[slot] = read_slot(slot, candidate);
        if (statuses[slot] != SLOT_VALID) continue;
        if (activity->active_slot < 0 ||
            (int32_t)(candidate->sequence - activity->record.sequence) > 0) {
            activity->record = *candidate;
            activity->active_slot = (int)slot;
        }
    }
    free(candidate);
    if (statuses[0] == SLOT_IO_ERROR || statuses[1] == SLOT_IO_ERROR)
        set_error(activity, "阅读统计未能完整读取，请检查 TF 卡。可用记录已保留。");
    else if (statuses[0] == SLOT_DAMAGED || statuses[1] == SLOT_DAMAGED)
        set_error(activity, activity->active_slot >= 0 ?
            "部分阅读统计已损坏，已恢复另一份记录。" :
            "阅读统计记录已损坏，暂从空记录开始。");
    return activity;
}

bool folio_activity_save(folio_activity_t* activity) {
    if (!activity) return false;
    if (!activity->dirty) return true;
    struct stat info;
    if (mkdir(FOLIO_SD_ROOT "/.folio", 0777) != 0 &&
        (stat(FOLIO_SD_ROOT "/.folio", &info) != 0 || !S_ISDIR(info.st_mode))) {
        set_error(activity, "阅读统计未能保存，请检查 TF 卡空间或写入状态。");
        return false;
    }
    unsigned target = activity->active_slot == 0 ? 1 : 0;
    uint32_t previous_sequence = activity->record.sequence;
    uint32_t previous_checksum = activity->record.checksum;
    ++activity->record.sequence;
    activity->record.checksum = checksum(&activity->record, offsetof(activity_record_t, checksum));
    FILE* file = fopen(slot_path(target), "wb");
    bool okay = file != NULL;
    if (file) {
        okay = fwrite(&activity->record, 1, sizeof(activity->record), file) == sizeof(activity->record);
        if (okay) okay = fflush(file) == 0;
        if (okay) okay = fsync(fileno(file)) == 0;
        if (fclose(file) != 0) okay = false;
    }
    if (!okay) {
        activity->record.sequence = previous_sequence;
        activity->record.checksum = previous_checksum;
        set_error(activity, "阅读统计未能保存，请检查 TF 卡空间或写入状态。");
        return false;
    }
    activity->active_slot = (int)target;
    activity->dirty = false;
    activity->error[0] = '\0';
    return true;
}

void folio_activity_close(folio_activity_t* activity) {
    if (activity) { (void)folio_activity_save(activity); folio_activity_free(activity); }
}
void folio_activity_free(folio_activity_t* activity) { free(activity); }

bool folio_activity_dirty(const folio_activity_t* activity) { return activity && activity->dirty; }
const char* folio_activity_error(const folio_activity_t* activity) {
    return activity ? activity->error : "阅读统计未能加载。";
}

static uint64_t add_saturated(uint64_t previous, uint64_t delta) {
    return delta > UINT64_MAX - previous ? UINT64_MAX : previous + delta;
}

static void add_day(folio_activity_t* activity, int32_t day, uint64_t microseconds) {
    activity_record_t* record = &activity->record;
    if (record->newest_day == UNKNOWN_DAY || day > record->newest_day) {
        record->newest_day = day;
        for (unsigned i = 0; i < FOLIO_ACTIVITY_DAYS; ++i)
            if ((int64_t)record->days[i].day < (int64_t)day - FOLIO_ACTIVITY_DAYS + 1)
                memset(&record->days[i], 0, sizeof(record->days[i]));
    }
    if ((int64_t)day < (int64_t)record->newest_day - FOLIO_ACTIVITY_DAYS + 1) return;
    day_record_t* target = &record->days[day_slot(day)];
    if (target->day != day) { memset(target, 0, sizeof(*target)); target->day = day; }
    uint64_t next = add_saturated(target->microseconds, microseconds);
    target->microseconds = next > MAX_DAY_US ? MAX_DAY_US : next;
}

static int64_t floor_day(int64_t microseconds) {
    int64_t result = microseconds / USEC_PER_DAY;
    if (microseconds < 0 && microseconds % USEC_PER_DAY) --result;
    return result;
}

static void credit(folio_activity_t* activity, int64_t duration, bool dated) {
    if (duration <= 0) return;
    uint64_t amount = (uint64_t)duration;
    // When the lifetime counter saturates, do not create a day sum larger than it.
    if (amount > UINT64_MAX - activity->record.total_us) amount = UINT64_MAX - activity->record.total_us;
    if (!amount) return;
    activity->record.total_us += amount;
    activity->dirty = true;
    if (!dated) {
        activity->record.undated_us = add_saturated(activity->record.undated_us, amount);
        return;
    }
    int64_t local_us = activity->wall_us + (int64_t)activity->timezone_minutes * 60 * USEC_PER_SECOND;
    while (amount) {
        int64_t day = floor_day(local_us);
        int64_t boundary = (day + 1) * USEC_PER_DAY;
        uint64_t part = (uint64_t)(boundary - local_us);
        if (part > amount) part = amount;
        add_day(activity, (int32_t)day, part);
        local_us += (int64_t)part;
        amount -= part;
    }
}

static bool valid_wall(int64_t seconds, int timezone_minutes) {
    // Headroom keeps UTC microseconds + timezone + one day representable.
    return seconds > 0 && seconds < INT64_MAX / USEC_PER_SECOND - 172800 &&
        timezone_minutes >= -720 && timezone_minutes <= 840;
}

void folio_activity_update(folio_activity_t* activity, int64_t monotonic_us,
                           int64_t utc_seconds, int timezone_minutes,
                           bool reading_visible, bool interaction) {
    if (!activity) return;
    if (monotonic_us < 0) { activity->tracking = false; return; }
    bool current_wall_valid = valid_wall(utc_seconds, timezone_minutes);
    int64_t current_wall = current_wall_valid ? utc_seconds * USEC_PER_SECOND : 0;
    bool continuous = false;
    int64_t predicted_wall = current_wall;
    if (activity->tracking && monotonic_us >= activity->monotonic_us) {
        int64_t elapsed = monotonic_us - activity->monotonic_us;
        if (activity->wall_valid && current_wall_valid &&
            timezone_minutes == activity->timezone_minutes &&
            elapsed <= INT64_MAX - activity->wall_us) {
            predicted_wall = activity->wall_us + elapsed;
            // RTC input is whole seconds; allow quantization, not clock jumps.
            int64_t difference = current_wall - predicted_wall;
            continuous = difference >= -2 * USEC_PER_SECOND && difference <= 2 * USEC_PER_SECOND;
        }
        if (activity->reading && activity->active_until_us > activity->monotonic_us) {
            int64_t until = monotonic_us < activity->active_until_us ? monotonic_us : activity->active_until_us;
            if (until > activity->monotonic_us) credit(activity, until - activity->monotonic_us, continuous);
        }
    } else {
        // A reset/backward monotonic sample establishes a new baseline only.
        activity->reading = false;
    }
    if (reading_visible && (!activity->reading || interaction)) {
        activity->active_until_us = monotonic_us > INT64_MAX - IDLE_US ? INT64_MAX : monotonic_us + IDLE_US;
    }
    activity->tracking = true;
    activity->reading = reading_visible;
    activity->monotonic_us = monotonic_us;
    activity->wall_valid = current_wall_valid;
    activity->wall_us = continuous ? predicted_wall : current_wall;
    activity->timezone_minutes = timezone_minutes;
}

bool folio_activity_touch_book(folio_activity_t* activity, const char* path,
                               const char* title, uint32_t percent,
                               uint32_t page, uint32_t position) {
    if (!activity) return false;
    if (!path || !title || strlen(path) >= sizeof(((folio_recent_book_t*)0)->path) ||
        strlen(title) >= sizeof(((folio_recent_book_t*)0)->title) || !*title ||
        !valid_path(path) || percent > 100) {
        set_error(activity, "这本书的最近阅读记录未能更新。");
        return false;
    }
    folio_recent_book_t next = {0};
    memcpy(next.path, path, strlen(path) + 1);
    memcpy(next.title, title, strlen(title) + 1);
    next.percent = percent;
    next.page = page;
    next.position = position;
    activity_record_t* record = &activity->record;
    if (record->recent_count && memcmp(&record->recent[0], &next, sizeof(next)) == 0) return true;
    unsigned found = record->recent_count;
    for (unsigned i = 0; i < record->recent_count; ++i)
        if (strcmp(record->recent[i].path, path) == 0) { found = i; break; }
    unsigned shift = found < FOLIO_RECENT_LIMIT ? found : FOLIO_RECENT_LIMIT - 1;
    memmove(&record->recent[1], &record->recent[0], shift * sizeof(record->recent[0]));
    record->recent[0] = next;
    if (found == record->recent_count && record->recent_count < FOLIO_RECENT_LIMIT) ++record->recent_count;
    activity->dirty = true;
    return true;
}

size_t folio_activity_recent_count(const folio_activity_t* activity) {
    return activity ? activity->record.recent_count : 0;
}
const folio_recent_book_t* folio_activity_recent(const folio_activity_t* activity, size_t index) {
    return activity && index < activity->record.recent_count ? &activity->record.recent[index] : NULL;
}
uint64_t folio_activity_total_seconds(const folio_activity_t* activity) {
    return activity ? activity->record.total_us / USEC_PER_SECOND : 0;
}
uint64_t folio_activity_undated_seconds(const folio_activity_t* activity) {
    return activity ? activity->record.undated_us / USEC_PER_SECOND : 0;
}
uint32_t folio_activity_day_seconds(const folio_activity_t* activity, int32_t local_epoch_day) {
    if (!activity) return 0;
    const day_record_t* day = &activity->record.days[day_slot(local_epoch_day)];
    return day->day == local_epoch_day ? (uint32_t)(day->microseconds / USEC_PER_SECOND) : 0;
}

void folio_activity_relocate(folio_activity_t* a,const char* old_path,const char* new_path){
    if(!a||!valid_path(old_path)||!valid_path(new_path)||strlen(new_path)>=256)return;
    for(unsigned i=0;i<a->record.recent_count;++i)if(!strcmp(a->record.recent[i].path,old_path)){
        snprintf(a->record.recent[i].path,sizeof(a->record.recent[i].path),"%s",new_path);a->dirty=true;}
}
