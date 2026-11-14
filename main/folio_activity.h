#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOLIO_ACTIVITY_DAYS 366
#define FOLIO_RECENT_LIMIT 3
#define FOLIO_ACTIVITY_IDLE_SECONDS 180

typedef struct folio_activity folio_activity_t;
typedef struct {
    char path[256];
    char title[128];
    uint32_t percent;
    uint32_t page; // Zero means no known page metadata, e.g. migrated last book.
    uint32_t position;
} folio_recent_book_t;

// Reads TF-card history. Missing records are an empty history; corrupt or
// unreadable records leave a user-facing error. NULL means allocation failed.
folio_activity_t* folio_activity_open(void);
// Attempts save before freeing. Use save explicitly when failure must be shown.
void folio_activity_close(folio_activity_t* activity);
// Frees without file I/O. Use after an explicit save attempt when retrying is
// undesirable (e.g. a shutdown deadline). Any remaining dirty changes are lost.
void folio_activity_free(folio_activity_t* activity);
bool folio_activity_save(folio_activity_t* activity);
bool folio_activity_dirty(const folio_activity_t* activity);
const char* folio_activity_error(const folio_activity_t* activity);

// Settle the PREVIOUS state up to monotonic_us, then enter reading_visible.
// Entering reading starts a 180-second activity window; interaction renews it.
// A caller must pause (reading_visible=false) before loading, sleep or menus.
// utc_seconds=0 means an uncalibrated clock. Unknown/discontinuous wall-clock
// intervals count toward total/undated time, never a fabricated calendar day.
// This function does not perform file I/O. Call save periodically and on exits.
void folio_activity_update(folio_activity_t* activity, int64_t monotonic_us,
                           int64_t utc_seconds, int timezone_minutes,
                           bool reading_visible, bool interaction);

// Move a successfully opened/read book to the front, removing duplicate paths.
// Values are copied. page=0 is valid when progress metadata is unavailable.
bool folio_activity_touch_book(folio_activity_t* activity, const char* path,
                               const char* title, uint32_t percent,
                               uint32_t page, uint32_t position);
size_t folio_activity_recent_count(const folio_activity_t* activity);
const folio_recent_book_t* folio_activity_recent(const folio_activity_t* activity,
                                               size_t index);
uint64_t folio_activity_total_seconds(const folio_activity_t* activity);
uint64_t folio_activity_undated_seconds(const folio_activity_t* activity);
uint32_t folio_activity_day_seconds(const folio_activity_t* activity,
                                    int32_t local_epoch_day);

void folio_activity_relocate(folio_activity_t* activity,const char* old_path,const char* new_path);
