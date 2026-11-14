#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "folio_reader.h"
#include "folio_epub.h"
#include "folio_text.h"

/* Use the production reader implementation, exposing page placements only to
 * check that cache bytes are rendered once and in order. Font/XML/ZIP are all
 * real modules. The screen and ESP heap are the only stubs. */
#include "../../main/book/folio_reader.c"

static unsigned pixels;
int epd_rotated_display_width(void) { return 684; }
int epd_rotated_display_height(void) { return 1216; }
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* frame) {
    (void)color; (void)frame;
    assert(x >= 0 && x < 684 && y >= 0 && y < 1216);
    ++pixels;
}

static folio_reader_t* open_reader(const folio_book_t* book, folio_font_t* font) {
    folio_reader_t* reader = folio_reader_open(book, font);
    if (reader == NULL) fprintf(stderr, "real EPUB reader failed: %s\n", folio_reader_last_error());
    assert(reader != NULL);
    return reader;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    folio_font_t* font = folio_font_open(argv[2], 34);
    assert(font != NULL);
    assert(folio_font_advance(font, 'A', 0) == 17);
    assert(folio_font_advance(font, 0x4E2D, 0) == 34);
    folio_book_t book = {0};
    snprintf(book.path, sizeof(book.path), "%s", argv[1]);
    snprintf(book.title, sizeof(book.title), "source filename");
    struct stat info;
    assert(stat(book.path, &info) == 0);
    book.size = (uint32_t)info.st_size;
    folio_reader_t* reader = open_reader(&book, font);
    assert(strcmp(folio_reader_title(reader), "真实集成") == 0);
    assert(folio_reader_chapter_count(reader) == 3);
    assert(folio_reader_bookmark_count(reader) == 0);

    FILE* cache = fopen(reader->epub.text_path, "rb");
    assert(cache != NULL);
    uint32_t pages = 0, rendered = 0;
    uint8_t framebuffer[1] = {0};
    do {
        assert(reader->page->end > reader->page->start);
        for (size_t i = 0; i < reader->page->placement_count; ++i) {
            uint32_t expected;
            do { assert(folio_text_next(cache, FOLIO_ENCODING_UTF8, &expected)); }
            while (expected == '\n' || expected == '\r');
            assert(expected == reader->page->placements[i].codepoint);
            ++rendered;
        }
        if (pages < 2) folio_reader_draw(reader, framebuffer);
        ++pages;
    } while (folio_reader_next(reader));
    uint32_t trailing;
    while (folio_text_next(cache, FOLIO_ENCODING_UTF8, &trailing)) assert(trailing == '\n');
    assert(!ferror(cache)); fclose(cache);
    assert(pages > 20 && rendered > 10000 && pixels > 0);
    assert(folio_reader_percent(reader) == 100);
    assert(folio_reader_error(reader)[0] == '\0');

    assert(folio_reader_jump_chapter(reader, 2));
    assert(folio_reader_current_chapter(reader) == 2);
    assert(folio_reader_position(reader) == reader->epub.chapters[2].offset);
    folio_reader_draw(reader, framebuffer);
    assert(folio_reader_next(reader));
    uint32_t saved = folio_reader_position(reader);
    uint32_t page_number = folio_reader_page_number(reader);
    assert(folio_reader_toggle_bookmark(reader));
    assert(folio_reader_bookmark_count(reader) == 1);
    assert(folio_reader_save(reader));
    folio_reader_close(reader);

    reader = open_reader(&book, font);
    assert(folio_reader_position(reader) == saved);
    assert(folio_reader_page_number(reader) == page_number);
    assert(folio_reader_bookmark_count(reader) == 1);
    assert(folio_reader_previous(reader));
    assert(folio_reader_position(reader) < saved);
    assert(folio_reader_jump_bookmark(reader, 0));
    assert(folio_reader_position(reader) == saved);
    assert(folio_reader_jump_chapter(reader, 1));
    assert(folio_reader_current_chapter(reader) == 1);
    uint32_t anchor = folio_reader_position(reader);
    folio_reader_options_t options;
    folio_reader_options_default(&options);
    options.line_gap = 8; options.margin = 56;
    assert(folio_font_set_size(font, 44));
    assert(folio_reader_set_options(reader, &options));
    assert(reader->page->start <= anchor && reader->page->end > anchor);
    folio_reader_draw(reader, framebuffer);
    assert(folio_reader_delete_bookmark(reader, 0));
    assert(folio_reader_save(reader));
    folio_reader_close(reader);
    reader = open_reader(&book, font);
    assert(folio_reader_bookmark_count(reader) == 0);
    folio_reader_close(reader);
    folio_font_close(font);
    printf("real EPUB + TTF + reader integration: %u pages, %u codepoints; TOC, draw, bookmark, restart and reflow passed\n", pages, rendered);
    return 0;
}
