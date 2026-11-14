#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "folio_font.h"

#define FOLIO_BOOK_PATH_CAP 256
#define FOLIO_BOOK_TITLE_CAP 128
#define FOLIO_LIBRARY_LIMIT 128

typedef struct {
    char path[FOLIO_BOOK_PATH_CAP];
    char title[FOLIO_BOOK_TITLE_CAP];
    uint32_t size;
} folio_book_t;

typedef struct folio_reader folio_reader_t;

typedef struct {
    int margin;
    int line_gap;
    int paragraph_gap;
    bool justify;
    bool hide_footer;
    uint8_t underline; // off / solid / dashed
    uint8_t underline_shade;

} folio_reader_options_t;

void folio_reader_options_default(folio_reader_options_t* options);
// Reflow preserves the byte position. Call after changing the font size as well.
bool folio_reader_set_options(folio_reader_t* reader, const folio_reader_options_t* options);

size_t folio_library_scan(folio_book_t* books, size_t capacity);
folio_reader_t* folio_reader_open(const folio_book_t* book, folio_font_t* font);
folio_reader_t* folio_reader_open_with_options(const folio_book_t* book, folio_font_t* font,
                                              const folio_reader_options_t* options);
void folio_reader_close(folio_reader_t* reader);
bool folio_reader_save(folio_reader_t* reader);
bool folio_reader_next(folio_reader_t* reader);
bool folio_reader_previous(folio_reader_t* reader);
void folio_reader_draw(folio_reader_t* reader, uint8_t* framebuffer);
const char* folio_reader_title(const folio_reader_t* reader);
const char* folio_reader_encoding(const folio_reader_t* reader);
uint32_t folio_reader_page_number(const folio_reader_t* reader);
// Exact chapter-relative number using the same layout as the visible page.
uint32_t folio_reader_chapter_page_number(folio_reader_t* reader);
unsigned folio_reader_percent(const folio_reader_t* reader);
unsigned folio_reader_chapter_percent(const folio_reader_t* reader);
// Incremental exact pagination. A zero total means it is still being counted.
// Does not move the visible page or change saved progress.
bool folio_reader_paginate_step(folio_reader_t* reader, unsigned budget);
uint32_t folio_reader_page_total(const folio_reader_t* reader, bool chapter);

uint32_t folio_reader_position(const folio_reader_t* reader);
bool folio_reader_seek(folio_reader_t* reader, uint32_t byte_offset);
bool folio_reader_seek_percent(folio_reader_t* reader, unsigned percent);

size_t folio_reader_chapter_count(const folio_reader_t* reader);
const char* folio_reader_chapter_title(const folio_reader_t* reader, size_t index);
// Display-only context for unambiguous numbered subsections; source titles stay intact.
void folio_reader_chapter_label(const folio_reader_t* reader, size_t index, char* out, size_t capacity);
size_t folio_reader_current_chapter(const folio_reader_t* reader);
bool folio_reader_jump_chapter(folio_reader_t* reader, size_t index);

size_t folio_reader_bookmark_count(const folio_reader_t* reader);
const char* folio_reader_bookmark_title(const folio_reader_t* reader, size_t index);
uint32_t folio_reader_bookmark_position(const folio_reader_t* reader, size_t index);
bool folio_reader_is_bookmarked(const folio_reader_t* reader);
bool folio_reader_toggle_bookmark(folio_reader_t* reader);
bool folio_reader_jump_bookmark(folio_reader_t* reader, size_t index);
bool folio_reader_delete_bookmark(folio_reader_t* reader, size_t index);

// Last open failure, or last operation error on a live reader. Empty means none.
const char* folio_reader_last_error(void);
const char* folio_reader_error(const folio_reader_t* reader);
