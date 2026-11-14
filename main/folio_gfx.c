#include "folio_gfx.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>

// Compact, authored 5x7 fallback alphabet for recovery and font setup screens.
// Reading text uses the user-selected TrueType font.
static const uint8_t s_ascii[46][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}, {0,0,0,0,0,4,0}, {0,0,0,0,0,4,8},
    {0,4,4,31,4,4,0}, {0,0,0,31,0,0,0}, {0,0,0,0,0,0,0}, {31,17,2,4,4,0,4},
    {1,1,2,4,8,16,16}, {0,4,4,0,4,4,0},
    {10,10,31,10,31,10,10}, {0,0,0,0,0,0,31},
};
// Recovery credentials and URLs are case sensitive; never fold them to caps.
static const uint8_t s_lowercase[26][7] = {
    {0,0,14,1,15,17,15}, {16,16,30,17,17,17,30}, {0,0,14,17,16,17,14},
    {1,1,15,17,17,17,15}, {0,0,14,17,31,16,14}, {6,9,8,28,8,8,8},
    {0,15,17,17,15,1,14}, {16,16,30,17,17,17,17}, {4,0,12,4,4,4,14},
    {2,0,6,2,2,18,12}, {16,16,18,20,24,20,18}, {12,4,4,4,4,4,14},
    {0,0,26,21,21,21,21}, {0,0,30,17,17,17,17}, {0,0,14,17,17,17,14},
    {0,30,17,17,30,16,16}, {0,15,17,17,15,1,1}, {0,0,22,25,16,16,16},
    {0,0,15,16,14,1,30}, {8,8,28,8,8,9,6}, {0,0,17,17,17,19,13},
    {0,0,17,17,17,10,4}, {0,0,17,17,21,21,10}, {0,0,17,10,4,10,17},
    {0,17,17,17,15,1,14}, {0,0,31,2,4,8,31},
};

static const uint8_t* glyph_for(char ch) {
    if (ch >= 'a' && ch <= 'z') return s_lowercase[ch-'a'];
    if (ch >= 'A' && ch <= 'Z') return s_ascii[ch - 'A'];
    if (ch >= '0' && ch <= '9') return s_ascii[26 + ch - '0'];
    switch (ch) {
        case '.': return s_ascii[36];
        case ',': return s_ascii[37];
        case '+': return s_ascii[38];
        case '-': return s_ascii[39];
        case ' ': return s_ascii[40];
        case '?': return s_ascii[41];
        case '/': return s_ascii[42];
        case ':': return s_ascii[43];
        case '#': return s_ascii[44];
        case '_': return s_ascii[45];
        default: return s_ascii[41];
    }
}

void folio_gfx_text(uint8_t* fb, int x, int y, int scale, const char* text, uint8_t color) {
    if (fb == NULL || text == NULL || scale < 1 || scale > 8) return;
    int cursor = x;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            cursor = x;
            y += 9 * scale;
            continue;
        }
        const uint8_t* glyph = glyph_for(*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[row] & (1u << (4 - col))) != 0) {
                    epd_fill_rect((EpdRect){cursor + col * scale, y + row * scale, scale, scale},
                                  color, fb);
                }
            }
        }
        cursor += 6 * scale;
    }
}

void folio_gfx_rule(uint8_t* fb, int x, int y, int width, uint8_t color) {
    if (fb == NULL || width <= 0) return;
    epd_fill_rect((EpdRect){x, y, width, 2}, color, fb);
}

void folio_gfx_outline(uint8_t* fb, EpdRect rect, uint8_t color) {
    if (fb == NULL || rect.width <= 0 || rect.height <= 0) return;
    epd_draw_rect(rect, color, fb);
}

static int round_radius(EpdRect rect, int radius) {
    int limit = (rect.width < rect.height ? rect.width : rect.height) / 2;
    if (radius < 0) return 0;
    return radius > limit ? limit : radius;
}

// Find the first included pixel center in this row's quarter-circle. Doubled
// coordinates keep the two sides symmetric for both odd and even dimensions.
static int round_inset(int height, int radius, int row) {
    int edge = row < height - 1 - row ? row : height - 1 - row;
    if (edge >= radius) return 0;
    int low = 0, high = radius;
    int64_t r = (int64_t)radius * 2, dy = r - 1 - (int64_t)edge * 2;
    while (low < high) {
        int middle = low + (high - low) / 2;
        int64_t dx = r - 1 - (int64_t)middle * 2;
        if (dx * dx + dy * dy <= r * r) high = middle;
        else low = middle + 1;
    }
    return low;
}

static void round_span(uint8_t* fb, int64_t left, int64_t right, int y, uint8_t color) {
    int screen_width = epd_rotated_display_width();
    if (left < 0) left = 0;
    if (right > screen_width) right = screen_width;
    for (int64_t x = left; x < right; ++x) epd_draw_pixel((int)x, y, color, fb);
}

static void round_rect_draw(uint8_t* fb, EpdRect rect, int radius, uint8_t color,
                           int thickness, int fill) {
    if (fb == NULL || rect.width <= 0 || rect.height <= 0 || (!fill && thickness <= 0)) return;
    int screen_height = epd_rotated_display_height();
    int64_t top = rect.y, bottom = top + rect.height;
    if (top < 0) top = 0;
    if (bottom > screen_height) bottom = screen_height;
    if (top >= bottom) return;
    radius = round_radius(rect, radius);
    int side = rect.width < rect.height ? rect.width : rect.height;
    // Compare before doubling so even INT_MAX thickness is harmless.
    int has_inner = !fill && thickness <= (side - 1) / 2;
    int inner_height = has_inner ? rect.height - 2 * thickness : 0;
    int inner_radius = has_inner && radius > thickness ? radius - thickness : 0;
    for (int64_t y = top; y < bottom; ++y) {
        int row = (int)(y - rect.y);
        int inset = round_inset(rect.height, radius, row);
        int64_t left = (int64_t)rect.x + inset, right = (int64_t)rect.x + rect.width - inset;
        if (has_inner && row >= thickness && row < rect.height - thickness) {
            int inner_inset = round_inset(inner_height, inner_radius, row - thickness);
            int64_t inner_left = (int64_t)rect.x + thickness + inner_inset;
            int64_t inner_right = (int64_t)rect.x + rect.width - thickness - inner_inset;
            round_span(fb, left, inner_left < right ? inner_left : right, (int)y, color);
            round_span(fb, inner_right > left ? inner_right : left, right, (int)y, color);
        } else round_span(fb, left, right, (int)y, color);
    }
}

void folio_gfx_fill_round_rect(uint8_t* fb, EpdRect rect, int radius, uint8_t color) {
    round_rect_draw(fb, rect, radius, color, 0, 1);
}

void folio_gfx_round_rect(uint8_t* fb, EpdRect rect, int radius,
                          uint8_t color, int thickness) {
    round_rect_draw(fb, rect, radius, color, thickness, 0);
}

static void stroke_pixel(uint8_t* fb, int x, int y, uint8_t color, int thickness) {
    int start = -(thickness / 2);
    int right = epd_rotated_display_width(), bottom = epd_rotated_display_height();
    for (int dy = start; dy < start + thickness; ++dy) {
        for (int dx = start; dx < start + thickness; ++dx) {
            int xx = x + dx, yy = y + dy;
            if (xx >= 0 && yy >= 0 && xx < right && yy < bottom)
                epd_draw_pixel(xx, yy, color, fb);
        }
    }
}

void folio_gfx_line(uint8_t* fb, int x1, int y1, int x2, int y2,
                    uint8_t color, int thickness) {
    if (fb == NULL || thickness < 1 || thickness > 16) return;
    // UI coordinates are tiny. Reject nonsensical inputs before abs/arithmetic.
    if (x1 < -32768 || x1 > 32767 || x2 < -32768 || x2 > 32767 ||
        y1 < -32768 || y1 > 32767 || y2 < -32768 || y2 > 32767) return;
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        stroke_pixel(fb, x1, y1, color, thickness);
        if (x1 == x2 && y1 == y2) break;
        int doubled = error * 2;
        if (doubled >= dy) { error += dy; x1 += sx; }
        if (doubled <= dx) { error += dx; y1 += sy; }
    }
}

void folio_gfx_circle(uint8_t* fb, int x, int y, int radius,
                      uint8_t color, int thickness) {
    if (fb == NULL || radius < 0 || radius > 1024 || thickness < 1 || thickness > 16 ||
        x < -32768 || x > 32767 || y < -32768 || y > 32767) return;
    int dx = radius, dy = 0, error = 1 - radius;
    do {
        stroke_pixel(fb, x + dx, y + dy, color, thickness);
        stroke_pixel(fb, x + dy, y + dx, color, thickness);
        stroke_pixel(fb, x - dy, y + dx, color, thickness);
        stroke_pixel(fb, x - dx, y + dy, color, thickness);
        stroke_pixel(fb, x - dx, y - dy, color, thickness);
        stroke_pixel(fb, x - dy, y - dx, color, thickness);
        stroke_pixel(fb, x + dy, y - dx, color, thickness);
        stroke_pixel(fb, x + dx, y - dy, color, thickness);
        ++dy;
        if (error < 0) error += 2 * dy + 1;
        else { --dx; error += 2 * (dy - dx) + 1; }
    } while (dy <= dx);
}

typedef struct { int8_t x, y; } icon_point_t;
typedef struct { uint8_t* fb; EpdRect box; uint8_t color; int stroke; } icon_context_t;

static int icon_x(const icon_context_t* c, int x) { return c->box.x + (x * (c->box.width - 1) + 12) / 24; }
static int icon_y(const icon_context_t* c, int y) { return c->box.y + (y * (c->box.height - 1) + 12) / 24; }
static void icon_line(const icon_context_t* c, int x1, int y1, int x2, int y2) {
    folio_gfx_line(c->fb, icon_x(c, x1), icon_y(c, y1), icon_x(c, x2), icon_y(c, y2), c->color, c->stroke);
}
static void icon_path(const icon_context_t* c, const icon_point_t* points, size_t count) {
    for (size_t i = 1; i < count; ++i)
        icon_line(c, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y);
}
static void icon_rect(const icon_context_t* c, int x1, int y1, int x2, int y2) {
    icon_line(c, x1, y1, x2, y1); icon_line(c, x2, y1, x2, y2);
    icon_line(c, x2, y2, x1, y2); icon_line(c, x1, y2, x1, y1);
}

void folio_gfx_icon(uint8_t* fb, EpdRect rect, folio_icon_t icon, uint8_t color) {
    if (fb == NULL || rect.width < 8 || rect.height < 8 || rect.width > 256 || rect.height > 256 ||
        rect.x < -1024 || rect.y < -1024 || rect.x > 32700 || rect.y > 32700 ||
        icon < 0 || icon >= FOLIO_ICON_COUNT) return;
    int side = rect.width < rect.height ? rect.width : rect.height;
    rect.x += (rect.width - side) / 2; rect.y += (rect.height - side) / 2;
    rect.width = rect.height = side;
    icon_context_t c = {fb, rect, color, (side + 8) / 18};
    if (c.stroke < 1) c.stroke = 1;
    switch (icon) {
        case FOLIO_ICON_BACK:
            icon_line(&c, 20, 12, 4, 12);
            icon_path(&c, (icon_point_t[]){{11,5},{4,12},{11,19}}, 3); break;
        case FOLIO_ICON_CHEVRON_LEFT:
            icon_path(&c, (icon_point_t[]){{15,5},{8,12},{15,19}}, 3); break;
        case FOLIO_ICON_CHEVRON_RIGHT:
            icon_path(&c, (icon_point_t[]){{9,5},{16,12},{9,19}}, 3); break;
        case FOLIO_ICON_GRID:
            icon_rect(&c, 4, 4, 9, 9); icon_rect(&c, 15, 4, 20, 9);
            icon_rect(&c, 4, 15, 9, 20); icon_rect(&c, 15, 15, 20, 20); break;
        case FOLIO_ICON_LIST:
            for (int row = 5; row <= 19; row += 7) {
                icon_line(&c, 4, row, 6, row); icon_line(&c, 11, row, 20, row);
            }
            break;
        case FOLIO_ICON_BOOK:
            icon_path(&c, (icon_point_t[]){{12,6},{8,4},{3,4},{3,19},{8,19},{12,21},{16,19},{21,19},{21,4},{16,4},{12,6},{12,21}}, 12);
            break;
        case FOLIO_ICON_BOOKMARK:
            icon_path(&c, (icon_point_t[]){{6,4},{18,4},{18,21},{12,17},{6,21},{6,4}}, 6); break;
        case FOLIO_ICON_TYPE:
            icon_path(&c, (icon_point_t[]){{4,20},{10,4},{16,20}}, 3);
            icon_line(&c, 6, 14, 14, 14); icon_line(&c, 18, 11, 22, 11);
            icon_line(&c, 20, 11, 20, 20); break;
        case FOLIO_ICON_SETTINGS:
            // Three sliders remain identifiable at small sizes without a busy gear.
            icon_line(&c, 4, 5, 10, 5); icon_line(&c, 14, 5, 20, 5);
            icon_line(&c, 12, 3, 12, 7);
            icon_line(&c, 4, 12, 5, 12); icon_line(&c, 9, 12, 20, 12);
            icon_line(&c, 7, 10, 7, 14);
            icon_line(&c, 4, 19, 14, 19); icon_line(&c, 18, 19, 20, 19);
            icon_line(&c, 16, 17, 16, 21); break;
        case FOLIO_ICON_REFRESH:
            icon_path(&c, (icon_point_t[]){{20,9},{18,6},{15,4},{11,4},{7,6},{4,10},{4,14},{6,18},{10,20},{14,20},{18,18}}, 11);
            icon_path(&c, (icon_point_t[]){{15,9},{20,9},{20,4}}, 3); break;
        case FOLIO_ICON_POWER:
            icon_line(&c, 12, 3, 12, 11);
            icon_path(&c, (icon_point_t[]){{7,6},{4,10},{4,15},{7,19},{12,21},{17,19},{20,15},{20,10},{17,6}}, 9); break;
        case FOLIO_ICON_LOCK:
            icon_rect(&c, 5, 10, 19, 21);
            icon_path(&c, (icon_point_t[]){{8,10},{8,6},{10,3},{14,3},{16,6},{16,10}}, 6);
            icon_line(&c, 12, 14, 12, 17); break;
        case FOLIO_ICON_CHECK:
            icon_path(&c, (icon_point_t[]){{4,12},{10,18},{21,6}}, 3); break;
        case FOLIO_ICON_PLUS:
            icon_line(&c, 5, 12, 19, 12); icon_line(&c, 12, 5, 12, 19); break;
        case FOLIO_ICON_MINUS:
            icon_line(&c, 5, 12, 19, 12); break;
        case FOLIO_ICON_CLOSE:
            icon_line(&c, 6, 6, 18, 18); icon_line(&c, 18, 6, 6, 18); break;
        case FOLIO_ICON_HOME:
            icon_path(&c, (icon_point_t[]){{2,11},{12,3},{22,11}}, 3);
            icon_path(&c, (icon_point_t[]){{5,9},{5,21},{10,21},{10,14},{14,14},{14,21},{19,21},{19,9}}, 8); break;
        case FOLIO_ICON_WIFI:
            icon_path(&c, (icon_point_t[]){{2,8},{5,6},{8,5},{12,4},{16,5},{19,6},{22,8}},7);
            icon_path(&c, (icon_point_t[]){{5,12},{8,10},{12,9},{16,10},{19,12}},5);
            icon_path(&c, (icon_point_t[]){{8,16},{10,14},{14,14},{16,16}},4);
            icon_line(&c,12,19,12,20); break;
        case FOLIO_ICON_COUNT: break;
    }
}
