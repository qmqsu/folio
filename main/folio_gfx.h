#pragma once

#include <stdint.h>

#include "epdiy.h"

void folio_gfx_text(uint8_t* fb, int x, int y, int scale, const char* text, uint8_t color);
void folio_gfx_rule(uint8_t* fb, int x, int y, int width, uint8_t color);
void folio_gfx_outline(uint8_t* fb, EpdRect rect, uint8_t color);

// Integer rounded rectangles. Radius is clamped to [0, min(width,height)/2].
// Stroke thickness is inward; a stroke consuming the interior becomes a fill.
// Empty rectangles / nonpositive thickness do nothing; drawing stays in rect.
void folio_gfx_fill_round_rect(uint8_t* fb, EpdRect rect, int radius, uint8_t color);
void folio_gfx_round_rect(uint8_t* fb, EpdRect rect, int radius,
                          uint8_t color, int thickness);

// Original 24-unit line icons. The artwork is centered inside the rectangle;
// the background remains untouched. Use a larger independent touch target.
typedef enum {
    FOLIO_ICON_BACK,
    FOLIO_ICON_CHEVRON_LEFT,
    FOLIO_ICON_CHEVRON_RIGHT,
    FOLIO_ICON_GRID,
    FOLIO_ICON_LIST,
    FOLIO_ICON_BOOK,
    FOLIO_ICON_BOOKMARK,
    FOLIO_ICON_TYPE,
    FOLIO_ICON_SETTINGS,
    FOLIO_ICON_REFRESH,
    FOLIO_ICON_POWER,
    FOLIO_ICON_LOCK,
    FOLIO_ICON_CHECK,
    FOLIO_ICON_PLUS,
    FOLIO_ICON_MINUS,
    FOLIO_ICON_CLOSE,
    FOLIO_ICON_HOME,
    FOLIO_ICON_WIFI,
    FOLIO_ICON_COUNT,
} folio_icon_t;

void folio_gfx_line(uint8_t* fb, int x1, int y1, int x2, int y2,
                    uint8_t color, int thickness);
void folio_gfx_circle(uint8_t* fb, int x, int y, int radius,
                      uint8_t color, int thickness);
void folio_gfx_icon(uint8_t* fb, EpdRect rect, folio_icon_t icon, uint8_t color);
