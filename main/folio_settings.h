#pragma once

#include <stdbool.h>
#include <stdint.h>

// Preserve the v2 record size: low four reserved bits retain catalog options.
#define FOLIO_LOCK_STYLE(s) ((s)->keep_page_on_lock & 3u)
#define FOLIO_OFF_STYLE(s) (((s)->keep_page_on_lock >> 2) & 3u)
#define FOLIO_CLOCK_12H(s) (((s)->reserved[0] & 0x10u) != 0)
#define FOLIO_READER_FOOTER(s) (((s)->reserved[0] >> 5) & 3u)
#define FOLIO_IDLE_MINUTES_MIN 5
#define FOLIO_IDLE_MINUTES_MAX 30
#define FOLIO_IDLE_MINUTES_STEP 5

typedef struct {
    uint16_t font_size;
    uint16_t margin;
    uint16_t line_gap;
    uint16_t paragraph_gap;
    uint16_t refresh_every;
    uint16_t idle_minutes;
    int16_t timezone_minutes;
    uint8_t justify;
    uint8_t auto_resume;
    uint8_t keep_page_on_lock; // Bits 0-1 lock / 2-3 shutdown: 0 cover, 1 body, 2 time / wallpaper.
    uint8_t library_list; // Reuses the first v2 reserved byte; 0=cover, 1=list.
    uint8_t home_chart; // Reuses the second v2 reserved byte; 0=heatmap, 1=bars.
    uint8_t reserved[1];
    char font_path[256];
    // v3 extension. Zero keeps the previous visible footer and whole-book scope.
    uint8_t footer_hidden;
    uint8_t progress_chapter;
    uint8_t underline; // 0=off, 1=solid, 2=dashed
    uint8_t underline_shade; // 0=light, 1=medium, 2=dark
    // v4 extension. Disabling preserves the selected duration.
    uint8_t auto_shutdown_enabled; // 0=off (default), 1=on; only while locked.
    uint16_t auto_shutdown_minutes; // 30 (default), 180, or 360. Known old choices migrate on load.

} folio_settings_t;

void folio_settings_defaults(folio_settings_t* settings);
bool folio_settings_load(folio_settings_t* settings);
bool folio_settings_save(const folio_settings_t* settings);
bool folio_session_load(char* path, unsigned capacity);
bool folio_session_save(const char* path);
