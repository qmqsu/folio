#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct folio_font folio_font_t;

#define FOLIO_FONT_BUILTIN_PATH ":builtin:"
#define FOLIO_FONT_BUILTIN_COUNT 4u
// Unknown native IDs remain native: never interpret them as filesystem paths.
bool folio_font_is_builtin_path(const char* path);
const char* folio_font_builtin_path(unsigned index);
const char* folio_font_builtin_label(const char* path);
folio_font_t* folio_font_open_builtin(int pixel_height);

// Card files are paged; this limit is independent of the resident built-in TTF limit.
#define FOLIO_FONT_MAX_FILE_BYTES (32u * 1024u * 1024u)
#define FOLIO_FONT_MAX_BYTES (6u * 1024u * 1024u)

folio_font_t* folio_font_open(const char* path, int pixel_height);
// Validate every outline of a staged upload before installing the file.
bool folio_font_validate_file(const char* path);
folio_font_t* folio_font_open_first_on_card(int pixel_height, char* path_out, uint32_t path_cap);
void folio_font_close(folio_font_t* font);
// Changes size without reloading the TTF. The bounded bitmap cache keeps sizes separate.
bool folio_font_set_size(folio_font_t* font, int pixel_height);

int folio_font_ascent(const folio_font_t* font);
int folio_font_line_height(const folio_font_t* font);
int folio_font_pixel_height(const folio_font_t* font);
int folio_font_advance(folio_font_t* font, uint32_t codepoint, uint32_t previous);
// Current-size visible ink bounds for black text on white, relative to pen x
// and baseline y; right/bottom are exclusive. Uses the same cached raster and
// '?' fallback as drawing. Blank glyphs/failure return false and zero outputs.
// Optional output pointers may be NULL. A cache miss creates only the normal
// glyph bitmap; measuring does not allocate another image or alter advance.
bool folio_font_glyph_bounds(folio_font_t* font, uint32_t codepoint,
                             int* left, int* top, int* right, int* bottom);
bool folio_font_draw_glyph(folio_font_t* font, uint8_t* framebuffer, int x, int baseline_y,
                           uint32_t codepoint);
// Colors use the high nibble (0x00 black, 0xF0 white). Caller paints the background;
// antialiased coverage is blended against that background without reading pixels.
bool folio_font_draw_glyph_color(folio_font_t* font, uint8_t* framebuffer, int x, int baseline_y,
                                 uint32_t codepoint, uint8_t color, uint8_t background);
int folio_font_draw_utf8(folio_font_t* font, uint8_t* framebuffer, int x, int baseline_y,
                         const char* text, uint8_t color);
