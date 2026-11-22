#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "folio_reader.h"
#include "folio_epub.h"

struct folio_font { int height; };
static int screen_width = 240, screen_height = 480;
int epd_rotated_display_width(void) { return screen_width; }
int epd_rotated_display_height(void) { return screen_height; }
int folio_font_ascent(const folio_font_t* font) { return font->height * 4 / 5; }
int folio_font_line_height(const folio_font_t* font) { return font->height; }
int folio_font_pixel_height(const folio_font_t* font) { return font->height; }
void folio_font_flush_cache(folio_font_t* font) { (void)font; }
int folio_font_advance(folio_font_t* font, uint32_t cp, uint32_t previous) {
    (void)previous;
    return cp < 128 ? font->height / 2 : font->height;
}
bool folio_font_draw_glyph(folio_font_t* font, uint8_t* frame, int x, int y, uint32_t cp) {
    (void)font; (void)frame; (void)x; (void)y; (void)cp;
    return true;
}
static unsigned underline_pixels;
static int underline_min_x,underline_max_x;
void epd_draw_pixel(int x,int y,uint8_t color,uint8_t* frame) {
    (void)frame;assert(x>=0 && x<screen_width && y>=0 && y<screen_height);
    assert(color==0xB0 || color==0x80 || color==0x40);
    if(!underline_pixels || x<underline_min_x)underline_min_x=x;
    if(!underline_pixels || x>underline_max_x)underline_max_x=x;
    ++underline_pixels;
}
static char mock_epub_path[256];
bool folio_epub_prepare(const char* path, folio_epub_t* epub, char* error, size_t capacity) {
    (void)path; (void)error; (void)capacity;
    memset(epub, 0, sizeof(*epub));
    snprintf(epub->text_path, sizeof(epub->text_path), "%s", mock_epub_path);
    snprintf(epub->title, sizeof(epub->title), "Metadata title");
    epub->chapter_count = 3;
    epub->chapters = calloc(3, sizeof(*epub->chapters));
    for (size_t i = 0; i < 3; ++i) {
        epub->chapters[i].offset = (uint32_t)i * 400;
        snprintf(epub->chapters[i].title, sizeof(epub->chapters[i].title), "Chapter %zu", i + 1);
    }
    return true;
}
void folio_epub_free(folio_epub_t* epub) { free(epub->chapters); memset(epub, 0, sizeof(*epub)); }

// Access internal layout for geometry and transactional-failure assertions.
#include "../../main/book/folio_reader.c"

static folio_book_t write_book(const char* filename, const void* data, size_t length) {
    folio_book_t book = {0};
    snprintf(book.path, sizeof(book.path), BOOK_DIR "/%s", filename);
    snprintf(book.title, sizeof(book.title), "%s", filename);
    book.size = (uint32_t)length;
    FILE* file = fopen(book.path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, length, file) == length);
    assert(fclose(file) == 0);
    return book;
}
static folio_font_t font = {.height = 20};
static folio_reader_t* open_book(const folio_book_t* book) {
    folio_reader_t* reader = folio_reader_open(book, &font);
    if (reader == NULL) fprintf(stderr, "open failed: %s\n", folio_reader_last_error());
    assert(reader != NULL);
    return reader;
}
static char* novel(size_t length) {
    char* bytes = malloc(length);
    assert(bytes != NULL);
    for (size_t i = 0; i < length; ++i) bytes[i] = (char)('a' + i % 26);
    return bytes;
}
static void test_continuation_and_eof(void) {
    char* content = novel(1200);
    folio_book_t book = write_book("continuation.txt", content, 1200);
    folio_reader_t* reader = open_book(&book);
    assert(reader->page->placements[0].x == reader->options.margin + 2 * font.height);
    folio_reader_options_t options = reader->options;
    options.disable_indent = true;
    assert(folio_reader_set_options(reader, &options));
    assert(reader->page->placements[0].x == options.margin);
    options.disable_indent = false;
    assert(folio_reader_set_options(reader, &options));
    assert(reader->page->placements[0].x == options.margin + 2 * font.height);
    size_t rendered = 0;
    do {
        for (unsigned i = 0; i < reader->page->placement_count; ++i) {
            assert(reader->page->placements[i].codepoint == (uint32_t)(unsigned char)content[rendered++]);
            assert(reader->page->placements[i].x + font.height / 2 <= screen_width - reader->options.margin);
        }
        if (folio_reader_page_number(reader) > 1) assert(reader->page->placements[0].x == reader->options.margin);
    } while (folio_reader_next(reader));
    assert(rendered == 1200);
    assert(folio_reader_percent(reader) == 100);
    folio_reader_close(reader);
    free(content);
}
static void test_restart_and_previous(void) {
    // The immersive layout fits more text per page. Keep enough source for at
    // least 130 pages so this still crosses two 64-page checkpoint boundaries.
    const size_t content_length = 32000;
    char* content = novel(content_length);
    folio_book_t book = write_book("restart.txt", content, content_length);
    folio_reader_t* reader = open_book(&book);
    uint32_t offsets[130];
    for (unsigned i = 0; i < 130; ++i) {
        offsets[i] = folio_reader_position(reader);
        if (i != 129) assert(folio_reader_next(reader));
    }
    assert(folio_reader_toggle_bookmark(reader));
    folio_reader_close(reader);
    reader = open_book(&book);
    assert(folio_reader_page_number(reader) == 130);
    assert(folio_reader_position(reader) == offsets[129]);
    assert(folio_reader_bookmark_count(reader) == 1);
    for (int i = 128; i >= 0; --i) {
        assert(folio_reader_previous(reader));
        assert(folio_reader_page_number(reader) == (uint32_t)i + 1);
        assert(folio_reader_position(reader) == offsets[i]);
    }
    assert(!folio_reader_previous(reader));
    assert(folio_reader_jump_bookmark(reader, 0));
    assert(folio_reader_position(reader) == offsets[129]);
    assert(folio_reader_delete_bookmark(reader, 0));
    folio_reader_close(reader);
    reader = open_book(&book);
    assert(folio_reader_bookmark_count(reader) == 0);
    folio_reader_close(reader);
    // Replacing a book by equally many different bytes must discard old progress.
    memset(content, 'Z', content_length);
    book = write_book("restart.txt", content, content_length);
    reader = open_book(&book);
    assert(folio_reader_page_number(reader) == 1);
    assert(folio_reader_bookmark_count(reader) == 0);
    folio_reader_close(reader);
    free(content);
}
static void test_torn_slot(void) {
    char* content = novel(2000);
    folio_book_t book = write_book("torn.txt", content, 2000);
    folio_reader_t* reader = open_book(&book);
    assert(folio_reader_next(reader));
    uint32_t previous = folio_reader_position(reader);
    assert(folio_reader_next(reader));
    char path[sizeof(reader->progress_path) + 4];
    snprintf(path, sizeof(path), "%s.%d", reader->progress_path, reader->active_slot);
    FILE* corrupt = fopen(path, "wb");
    assert(corrupt != NULL && fwrite("bad", 1, 3, corrupt) == 3);
    fclose(corrupt);
    folio_reader_close(reader);
    reader = open_book(&book);
    assert(folio_reader_position(reader) == previous);
    folio_reader_close(reader);
    free(content);
}
static void test_failed_read_preserves_page(void) {
    char* content = novel(2000);
    folio_book_t book = write_book("io.txt", content, 2000);
    folio_reader_t* reader = open_book(&book);
    uint32_t position = folio_reader_position(reader);
    size_t layout_size = sizeof(*reader->page);
    page_layout_t* before = malloc(layout_size);
    memcpy(before, reader->page, layout_size);
    // A real file error: replacing the open stream with a directory produces EISDIR.
    fclose(reader->file);
    reader->file = fopen(BOOK_DIR, "rb");
    assert(reader->file != NULL);
    assert(!folio_reader_next(reader));
    assert(folio_reader_position(reader) == position);
    assert(memcmp(before, reader->page, layout_size) == 0);
    assert(folio_reader_error(reader)[0] != 0);
    folio_reader_close(reader);
    free(before); free(content);
}
static void test_save_failure_and_bookmark_limit(void) {
    char* content = novel(12000);
    folio_book_t book = write_book("save-failure.txt", content, 12000);
    folio_reader_t* reader = open_book(&book);
    char inactive[sizeof(reader->progress_path) + 4];
    snprintf(inactive, sizeof(inactive), "%s.%d", reader->progress_path, reader->active_slot == 0 ? 1 : 0);
    assert(mkdir(inactive, 0700) == 0);
    // The inactive target cannot be overwritten; current bookmark state rolls back.
    assert(!folio_reader_toggle_bookmark(reader));
    assert(folio_reader_bookmark_count(reader) == 0);
    assert(folio_reader_error(reader)[0] != 0);
    assert(folio_reader_next(reader));
    assert(!folio_reader_save(reader));
    uint32_t pending_position = folio_reader_position(reader);
    assert(rmdir(inactive) == 0);
    assert(folio_reader_save(reader));
    for (unsigned i = 0; i < BOOKMARK_LIMIT; ++i) {
        assert(folio_reader_toggle_bookmark(reader));
        assert(folio_reader_next(reader));
    }
    assert(folio_reader_bookmark_count(reader) == BOOKMARK_LIMIT);
    assert(!folio_reader_toggle_bookmark(reader));
    assert(folio_reader_bookmark_count(reader) == BOOKMARK_LIMIT);
    assert(folio_reader_jump_bookmark(reader, 0));
    assert(folio_reader_position(reader) == pending_position);
    assert(folio_reader_toggle_bookmark(reader));
    assert(folio_reader_bookmark_count(reader) == BOOKMARK_LIMIT - 1);
    folio_reader_close(reader);
    free(content);
}
static void test_bom_gbk_and_punctuation(void) {
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF, 'A', 0xFF, 'B'};
    folio_book_t book = write_book("bom.txt", bom, sizeof(bom));
    folio_reader_t* reader = open_book(&book);
    assert(folio_reader_position(reader) == 3);
    assert(strcmp(folio_reader_encoding(reader), "UTF-8") == 0);
    assert(reader->page->placements[1].codepoint == 0xFFFD);
    folio_reader_close(reader);
    const unsigned char gbk[] = {0xD6, 0xD0, 0xCE, 0xC4};
    book = write_book("gbk.txt", gbk, sizeof(gbk));
    reader = open_book(&book);
    uint32_t words;
    assert(folio_reader_word_count(reader,&words) && words==2);
    assert(strcmp(folio_reader_encoding(reader), "GBK") == 0);
    assert(reader->page->placements[0].codepoint == 0x4E2D);
    folio_reader_close(reader);
    const char mixed[]="中文， hello world! 123";
    book=write_book("words.txt",mixed,strlen(mixed));reader=open_book(&book);
    assert(folio_reader_word_count(reader,&words) && words==5);folio_reader_close(reader);
    const char* punctuation = "甲乙丙（丁戊），己庚。甲乙丙（丁戊），己庚。甲乙丙（丁戊），己庚。";
    book = write_book("punctuation.txt", punctuation, strlen(punctuation));
    reader = open_book(&book);
    do {
        for (unsigned i = 0; i < reader->page->line_count; ++i) {
            line_layout_t* line = &reader->page->lines[i];
            assert(!folio_is_closing_punctuation(reader->page->placements[line->first].codepoint));
            assert(!folio_is_opening_punctuation(reader->page->placements[line->first + line->count - 1].codepoint));
        }
    } while (folio_reader_next(reader));
    folio_reader_close(reader);
}
static void test_reflow_seek_epub(void) {
    char* content = novel(8000);
    folio_book_t book = write_book("reflow.txt", content, 8000);
    folio_reader_t* reader = open_book(&book);
    assert(folio_reader_seek_percent(reader, 60));
    uint32_t position = folio_reader_position(reader);
    folio_reader_options_t options = reader->options;
    options.margin = 24; options.line_gap = 4; options.justify = false;
    font.height = 24;
    assert(folio_reader_set_options(reader, &options));
    assert(reader->page->start <= position && reader->page->end > position);
    assert(folio_reader_seek_percent(reader, 100));
    assert(folio_reader_percent(reader) == 100);
    folio_reader_close(reader);
    font.height = 20;
    folio_book_t cache = write_book("mock-cache.txt", content, 8000);
    snprintf(mock_epub_path, sizeof(mock_epub_path), "%s", cache.path);
    book = write_book("mock.epub", "mock zip identity", 17);
    reader = open_book(&book);
    assert(strcmp(folio_reader_title(reader), "Metadata title") == 0);
    assert(folio_reader_chapter_count(reader) == 3);
    assert(folio_reader_jump_chapter(reader, 2));
    assert(reader->page->start == 800 && reader->page->end > 800);
    assert(folio_reader_current_chapter(reader) == 2);
    assert(folio_reader_chapter_page_number(reader) == 1);
    uint32_t chapter_start_page=folio_reader_page_number(reader);
    assert(folio_reader_next(reader));
    assert(folio_reader_chapter_page_number(reader) == 2);
    assert(folio_reader_page_number(reader) == chapter_start_page+1);
    assert(folio_reader_previous(reader));
    assert(folio_reader_chapter_page_number(reader) == 1);
    assert(folio_reader_seek_percent(reader,90));
    assert(folio_reader_chapter_page_number(reader) == folio_reader_page_number(reader)-chapter_start_page+1);
    assert(folio_reader_set_options(reader,&options));
    assert(folio_reader_chapter_page_number(reader) > 0);
    assert(folio_reader_jump_chapter(reader,2));
    assert(folio_reader_chapter_page_number(reader) == 1);
    options.line_gap=8;assert(folio_reader_set_options(reader,&options));
    assert(folio_reader_jump_chapter(reader,2));
    position=folio_reader_position(reader);uint32_t page=folio_reader_page_number(reader);
    while(folio_reader_paginate_step(reader,3)){}
    assert(folio_reader_position(reader)==position && folio_reader_page_number(reader)==page);
    uint32_t total=folio_reader_page_total(reader,false),chapter_total=folio_reader_page_total(reader,true);
    assert(total>chapter_total && chapter_total>1 && folio_reader_chapter_percent(reader)==0);
    assert(folio_reader_seek_percent(reader,100));
    assert(folio_reader_page_number(reader)==total && folio_reader_chapter_page_number(reader)==chapter_total);
    assert(folio_reader_chapter_percent(reader)==100);
    options=reader->options;options.underline=1;options.underline_shade=2;
    assert(folio_reader_set_options(reader,&options));
    assert(folio_reader_page_total(reader,false)==total); // Decoration does not repaginate.
    uint8_t frame=0;underline_pixels=0;folio_reader_draw(reader,&frame);unsigned solid=underline_pixels;assert(solid>0);
    assert(underline_min_x==reader->options.margin && underline_max_x==screen_width-reader->options.margin-1);
    options.underline=2;assert(folio_reader_set_options(reader,&options));
    underline_pixels=0;folio_reader_draw(reader,&frame);assert(underline_pixels>0 && underline_pixels<solid);
    options.hide_footer=true;assert(folio_reader_set_options(reader,&options));assert(!folio_reader_page_total(reader,false));
    while(folio_reader_paginate_step(reader,4)){}
    assert(folio_reader_page_total(reader,false)<=total);
    assert(folio_reader_toggle_bookmark(reader));
    assert(folio_reader_bookmark_count(reader) == 1);
    folio_reader_close(reader);
    free(content);
}
static void test_weread_reader(void) {
    const folio_weread_chapter_t chapters[] = {{"chapter1", "云端第一章"}, {"chapter2", "云端第二章"}};
    char error[256]; folio_book_t book = {0};
    folio_weread_cache_writer_t* writer = folio_weread_cache_begin("reader", "云端书名", "作者", chapters, 2, error, sizeof(error));
    assert(writer);
    char raw[8500];
    strcpy(raw, "<body><p>来自微信读书的中文正文&amp;Emoji😀</p><p>");
    size_t prefix = strlen(raw); memset(raw + prefix, 'a', 8000);
    strcpy(raw + prefix + 8000, "</p></body>");
    assert(folio_weread_cache_write_chapter(writer, 0, (const uint8_t*)raw, strlen(raw), FOLIO_WEREAD_XHTML, error, sizeof(error)));
    assert(folio_weread_cache_publish(writer, book.path, sizeof(book.path), error, sizeof(error)));
    folio_weread_cache_close(writer);
    snprintf(book.title, sizeof(book.title), "scan fallback");
    folio_reader_t* reader = open_book(&book);
    assert(!strcmp(folio_reader_title(reader), "云端书名"));
    assert(folio_reader_chapter_count(reader) == 1);
    assert(reader->page->placements[0].codepoint == 0x6765);
    assert(folio_reader_seek(reader, 5000)); assert(folio_reader_toggle_bookmark(reader));
    uint32_t saved_position = folio_reader_position(reader); assert(saved_position > 0);
    folio_reader_close(reader);
    char exported[256];assert(folio_weread_cache_export(book.path,exported,sizeof(exported),error,sizeof(error)));
    snprintf(book.path,sizeof(book.path),"%s",exported);
    reader = open_book(&book); assert(folio_reader_position(reader) == saved_position); assert(folio_reader_bookmark_count(reader) == 1);
    folio_reader_close(reader);
    writer = folio_weread_cache_begin("reader", "云端书名", "作者", chapters, 2, error, sizeof(error)); assert(writer);
    assert(folio_weread_cache_write_chapter(writer, 1, (const uint8_t*)"第二章正文", strlen("第二章正文"), FOLIO_WEREAD_TEXT, error, sizeof(error)));
    assert(folio_weread_cache_publish(writer, book.path, sizeof(book.path), error, sizeof(error)));
    folio_weread_cache_close(writer);
    assert(folio_weread_cache_export(book.path,exported,sizeof(exported),error,sizeof(error)));
    snprintf(book.path,sizeof(book.path),"%s",exported);
    reader = open_book(&book); assert(folio_reader_position(reader) == 0); assert(folio_reader_bookmark_count(reader) == 0);
    assert(folio_reader_chapter_count(reader) == 2); assert(folio_reader_jump_chapter(reader, 1)); folio_reader_close(reader);
    folio_book_t found[128]; size_t count = folio_library_scan(found, 128); bool matched = false;
    for (size_t i = 0; i < count; ++i) if (!strcmp(found[i].path, book.path)) { matched = true; assert(!strcmp(found[i].title, "云端书名")); }
    assert(matched);
}
static void test_empty_frontmatter_boundary(void) {
    const folio_weread_chapter_t chapters[] = {
        {"cover", "封面"}, {"blank", "扉页"}, {"copyright", "版权信息"}, {"body", "第一章"}
    };
    const char* contents[] = {"\n\n", " \n", "版权信息\n作者：测试作者\n版权所有。", "第一章\n正文内容。"};
    char error[256]; folio_book_t book = {0};
    folio_weread_cache_writer_t* writer = folio_weread_cache_begin("frontmatter", "版权页分页测试", "作者",
        chapters, 4, error, sizeof(error));
    assert(writer);
    for (size_t i = 0; i < 4; ++i) {
        assert(folio_weread_cache_write_chapter(writer, i, (const uint8_t*)contents[i], strlen(contents[i]),
            FOLIO_WEREAD_TEXT, error, sizeof(error)));
    }
    assert(folio_weread_cache_publish(writer, book.path, sizeof(book.path), error, sizeof(error)));
    folio_weread_cache_close(writer);
    folio_reader_t* reader = open_book(&book);
    uint32_t copyright_start = reader->epub.chapters[2].offset;
    assert(reader->page->placement_count == 16); // The whole copyright block, never just “版”.
    assert(reader->page->placements[0].codepoint == 0x7248);
    assert(folio_reader_position(reader) == copyright_start);
    assert(folio_reader_current_chapter(reader) == 2);
    assert(folio_reader_chapter_page_number(reader) == 1);
    while (folio_reader_paginate_step(reader, 3)) {}
    assert(folio_reader_page_total(reader, false) == 2);
    assert(folio_reader_page_total(reader, true) == 1);
    assert(folio_reader_next(reader));
    assert(folio_reader_current_chapter(reader) == 3);
    assert(folio_reader_page_number(reader) == 2);
    assert(folio_reader_chapter_page_number(reader) == 1);
    assert(!folio_reader_next(reader));
    assert(folio_reader_previous(reader));
    assert(folio_reader_position(reader) == copyright_start);
    assert(folio_reader_jump_chapter(reader, 0)); // Empty cover resolves to first readable chapter.
    assert(folio_reader_current_chapter(reader) == 2);
    assert(folio_reader_toggle_bookmark(reader));
    // A pre-fix saved position/bookmark on the empty cover must still load.
    char progress_path[sizeof(reader->progress_path) + 4];
    snprintf(progress_path, sizeof(progress_path), "%s.%d", reader->progress_path, reader->active_slot);
    FILE* saved = fopen(progress_path, "r+b"); assert(saved);
    progress_record_t record;
    assert(fread(&record, 1, sizeof(record), saved) == sizeof(record));
    record.offset = record.bookmarks[0].offset = 0;
    record.checksum = checksum_bytes(&record, offsetof(progress_record_t, checksum));
    rewind(saved); assert(fwrite(&record, 1, sizeof(record), saved) == sizeof(record));
    assert(fclose(saved) == 0);
    folio_reader_close(reader);
    reader = open_book(&book);
    assert(folio_reader_bookmark_count(reader) == 1);
    assert(folio_reader_jump_bookmark(reader, 0));
    assert(folio_reader_current_chapter(reader) == 2);
    assert(reader->page->placement_count == 16);
    folio_reader_close(reader);
}
static void check_chapter_labels(const char* const* titles, const char* const* expected, size_t count) {
    folio_reader_t* reader = reader_alloc(sizeof(*reader)); assert(reader);
    reader->is_epub = true;
    reader->epub.chapter_count = count;
    reader->epub.chapters = calloc(count, sizeof(*reader->epub.chapters)); assert(reader->epub.chapters);
    for (size_t i = 0; i < count; ++i) {
        snprintf(reader->epub.chapters[i].title, FOLIO_EPUB_TITLE_CAP, "%s", titles[i]);
        reader->epub.chapters[i].offset = (uint32_t)(i * 100);
    }
    char label[160];
    for (size_t i = 0; i < count; ++i) {
        folio_reader_chapter_label(reader, i, label, sizeof(label));
        assert(!strcmp(label, expected ? expected[i] : titles[i]));
        assert(!strcmp(folio_reader_chapter_title(reader, i), titles[i]));
        assert(reader->epub.chapters[i].offset == i * 100);
    }
    folio_reader_chapter_label(reader, count, label, sizeof(label)); assert(!*label);
    folio_reader_chapter_label(NULL, 0, label, sizeof(label)); assert(!*label);
    folio_reader_chapter_label(reader, 0, NULL, 0);
    folio_epub_free(&reader->epub); free(reader);
}
static void test_chapter_display_context(void) {
    const char* titles[] = {"封面", "版权信息", "一 开车", "2", "3", "二 疯子", "2", "3", "4",
                           "三 弃婴", "2", "3", "4", "5"};
    const char* labels[] = {"封面", "版权信息", "一 开车", "一 开车 · 2", "一 开车 · 3", "二 疯子",
                           "二 疯子 · 2", "二 疯子 · 3", "二 疯子 · 4", "三 弃婴", "三 弃婴 · 2",
                           "三 弃婴 · 3", "三 弃婴 · 4", "三 弃婴 · 5"};
    check_chapter_labels(titles, labels, sizeof(titles) / sizeof(titles[0]));
    const char* from_one[] = {"十一　三个人", "1", "2", "十二 又一天", "1", "2"};
    const char* from_one_labels[] = {"十一　三个人", "十一　三个人 · 1", "十一　三个人 · 2",
                                    "十二 又一天", "十二 又一天 · 1", "十二 又一天 · 2"};
    check_chapter_labels(from_one, from_one_labels, 6);
    const char* wencheng[] = {"封面", "版权信息", "一", "二", "三", "四"};
    check_chapter_labels(wencheng, NULL, 6);
    const char* bare[] = {"封面", "序言", "1", "2", "3"};
    check_chapter_labels(bare, NULL, 5);
    const char* broken[] = {"一 开车", "2", "3", "5", "二 疯子", "2"};
    check_chapter_labels(broken, NULL, 6); // Validate the entire run, not merely its prefix.
    const char* restart[] = {"一 开车", "2", "3", "1", "2"};
    check_chapter_labels(restart, NULL, 5);
    const char* unfamiliar[] = {"Chapter One", "1", "2", "一二 标题", "1", "2", "一 2", "1", "2",
                                "一 二", "1", "2", "一 开车", "01", "02", "二 疯子", "3", "4"};
    check_chapter_labels(unfamiliar, NULL, sizeof(unfamiliar) / sizeof(unfamiliar[0]));
    const char* interrupted[] = {"一 开车", "1", "版权信息", "2", "3"};
    check_chapter_labels(interrupted, NULL, 5);

    folio_reader_t* reader = reader_alloc(sizeof(*reader)); assert(reader);
    reader->is_epub = true; reader->epub.chapter_count = 3;
    reader->epub.chapters = calloc(3, sizeof(*reader->epub.chapters)); assert(reader->epub.chapters);
    strcpy(reader->epub.chapters[0].title, "一 ");
    for (size_t i = 0; i < 30; ++i) strcat(reader->epub.chapters[0].title, "长");
    strcpy(reader->epub.chapters[1].title, "2"); strcpy(reader->epub.chapters[2].title, "3");
    char full[160]; folio_reader_chapter_label(reader, 1, full, sizeof(full));
    assert(strstr(full, " · 2"));
    for (size_t cap = 0; cap <= sizeof(full); ++cap) {
        char small[161]; memset(small, 0x5A, sizeof(small));
        folio_reader_chapter_label(reader, 1, small, cap);
        assert((unsigned char)small[cap] == 0x5A);
        if (!cap) continue;
        assert(strlen(small) < cap);
        const char* cursor = small;
        while (*cursor) assert(folio_utf8_next(&cursor) != 0xFFFD);
        if (cap >= 16) assert(!strcmp(small + strlen(small) - strlen(" · 2"), " · 2"));
    }
    folio_epub_free(&reader->epub); free(reader);
}
int main(void) {
    assert(ensure_directory(FOLIO_SD_ROOT));
    assert(ensure_directory(BOOK_DIR));
    folio_book_t book = {.title="Reflow", .path=BOOK_DIR "/Reflow.PDF"};
    folio_reader_t *pdf = open_book(&book);
    assert(!strcmp(folio_reader_encoding(pdf),"PDF"));
    assert(fseek(pdf->file,0,SEEK_SET)==0);
    char line[64];
    assert(fgets(line,sizeof(line),pdf->file) && !strcmp(line,"Folio PDF reader\n"));
    assert(fgets(line,sizeof(line),pdf->file) && !strcmp(line,"中文\n"));
    assert(folio_reader_next(pdf));
    uint32_t position = folio_reader_position(pdf);
    folio_reader_close(pdf);
    pdf=open_book(&book);
    assert(folio_reader_position(pdf)==position);
    folio_reader_close(pdf);
    snprintf(book.path,sizeof(book.path),BOOK_DIR "/NoText.pdf");
    assert(!folio_reader_open(&book,&font));
    snprintf(book.path,sizeof(book.path),BOOK_DIR "/Scan.pdf");
    pdf = open_book(&book);
    uint32_t unknown; assert(!folio_reader_word_count(pdf,&unknown));
    assert(pdf->scan.count == 2 && pdf->scan.pixels && !pdf->file);
    assert(folio_reader_next(pdf) && !folio_reader_next(pdf));
    folio_reader_close(pdf);
    pdf = open_book(&book);
    assert(pdf->page->number == 1 && folio_reader_previous(pdf) && pdf->page->number == 0);
    folio_reader_close(pdf);
    test_continuation_and_eof();
    test_restart_and_previous();
    test_torn_slot();
    test_failed_read_preserves_page();
    test_save_failure_and_bookmark_limit();
    test_bom_gbk_and_punctuation();
    test_reflow_seek_epub();
    test_weread_reader();
    test_empty_frontmatter_boundary();
    test_chapter_display_context();
    puts("reader: pagination, restart/backtracking, torn slots, source replacement, I/O rollback, encoding, punctuation, reflow, EPUB integration passed");
    return 0;
}
