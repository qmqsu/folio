#include "folio_reader.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "epdiy.h"
#include "esp_heap_caps.h"
#include "folio_epub.h"
#include "folio_pdf.h"
#include "folio_text.h"
#include "folio_weread_cache.h"
#include "folio_weread_paths.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#define OPEN_MARK(name) ESP_LOGD("reader", "open %s +%lld ms", name, (long long)((esp_timer_get_time() - open_started) / 1000))
#define OPEN_START() int64_t open_started = esp_timer_get_time()
#else
#define OPEN_MARK(name) ((void)0)
#define OPEN_START() ((void)0)
#endif

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
#define BOOK_DIR FOLIO_SD_ROOT "/books"
#define STATE_DIR FOLIO_SD_ROOT "/.folio"
#define PROGRESS_DIR STATE_DIR "/progress"
#define PAGE_GLYPHS 1400
#define PAGE_LINES 64
#define PAGE_BYTES 65536
#define CHECKPOINT_INTERVAL 64
#define CHECKPOINT_COUNT 1024
#define BOOKMARK_LIMIT 32
#define BOOKMARK_TITLE_CAP 96
#define TOP_MARGIN 48
#define BOTTOM_MARGIN 64
#define PROGRESS_MAGIC 0x324F4C46u
#define INSPECT_MAGIC 0x31534E49u
#define LAYOUT_MAGIC 0x3159414Cu

typedef struct { uint32_t codepoint; int16_t x, baseline; } glyph_placement_t;
typedef struct {
    uint16_t first, count;
    int16_t start_x, natural_width;
    bool justify;
} line_layout_t;
typedef struct {
    uint32_t start, end, number;
    bool paragraph_start, next_paragraph;
    uint16_t placement_count, line_count, rule_count;
    int16_t rule_baselines[PAGE_LINES];
    glyph_placement_t placements[PAGE_GLYPHS];
    line_layout_t lines[PAGE_LINES];
} page_layout_t;
typedef struct { uint32_t offset; bool paragraph_start, valid; } checkpoint_t;
typedef struct { uint32_t number; checkpoint_t position; } recent_page_t;
typedef struct { uint32_t offset; char title[BOOKMARK_TITLE_CAP]; } bookmark_t;
typedef struct {
    uint32_t magic, version, generation, source_size;
    uint64_t path_hash, source_hash, text_hash;
    uint32_t text_size, offset, bookmark_count;
    bookmark_t bookmarks[BOOKMARK_LIMIT];
    uint32_t checksum;
} progress_record_t;

struct folio_reader {
    FILE* file;
    folio_font_t* font;
    folio_epub_t epub;
    folio_pdf_scan_t scan;
    bool is_pdf, is_epub, is_weread, ready, dirty;
    char title[FOLIO_BOOK_TITLE_CAP], path[FOLIO_BOOK_PATH_CAP];
    char progress_path[sizeof(PROGRESS_DIR) + 32], error[128];
    uint64_t path_hash, source_hash, text_hash;
    uint32_t source_size, file_size, content_start, generation, word_count;
    int active_slot;
    int layout_font_size;
    folio_encoding_t encoding;
    folio_reader_options_t options;
    page_layout_t *page, *scratch;
    uint32_t pagination_offset, pagination_pages;
    uint32_t* chapter_pages;
    bool pagination_paragraph, pagination_done, pagination_failed;
    checkpoint_t checkpoints[CHECKPOINT_COUNT];
    recent_page_t recent[CHECKPOINT_INTERVAL];
    size_t bookmark_count;
    size_t footer_chapter;
    uint32_t footer_base;
    bool footer_base_valid;
    uint32_t layout_saved_pages, layout_saved_page;
    char* io_buffer; // Owned here, never by stdio: picolibc has double-freed library buffers on fclose.
    bookmark_t bookmarks[BOOKMARK_LIMIT];
};
static char s_last_error[128];

// Yield by elapsed time rather than by fixed counts: a 1 ms tick sleep every
// few hundred glyphs dominated long reflows.
static void reader_yield(void) {
#ifdef ESP_PLATFORM
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last < 100000) return;
    last = now;
#endif
    vTaskDelay(1);
}
static void* reader_alloc(size_t size) {
    void* memory = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : calloc(1, size);
}
// Must run before the stream's first read; stdio otherwise uses 128-byte reads.
static void attach_io_buffer(folio_reader_t* reader, FILE* file) {
    if (!reader->io_buffer) reader->io_buffer = reader_alloc(16384);
    if (reader->io_buffer) (void)setvbuf(file, reader->io_buffer, _IOFBF, 16384);
}
static void copy_utf8(char* destination, size_t capacity, const char* source, size_t length) {
    if (capacity == 0) return;
    size_t count = length < capacity - 1 ? length : capacity - 1;
    if (count < length) {
        while (count > 0 && ((unsigned char)source[count] & 0xC0) == 0x80) --count;
    }
    memcpy(destination, source, count);
    destination[count] = '\0';
}
static void set_error(folio_reader_t* reader, const char* message) {
    snprintf(reader->error, sizeof(reader->error), "%s", message);
}
static uint64_t hash_update(uint64_t hash, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) { hash ^= data[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static uint32_t checksum_bytes(const void* data, size_t length) {
    uint64_t hash = hash_update(UINT64_C(14695981039346656037), data, length);
    return (uint32_t)hash ^ (uint32_t)(hash >> 32);
}
static bool ensure_directory(const char* path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
static int book_compare(const void* left, const void* right) {
    const folio_book_t* a = left;
    const folio_book_t* b = right;
    return strcasecmp(a->title, b->title);
}
size_t folio_library_scan(folio_book_t* books, size_t capacity) {
    if (books == NULL || capacity == 0 || !ensure_directory(BOOK_DIR)) return 0;
    DIR* dir = opendir(BOOK_DIR);
    if (dir == NULL) return 0;
    size_t count = 0;
    struct dirent* item;
    while ((item = readdir(dir)) != NULL && count < capacity) {
        if (item->d_name[0] == '.') continue;
        const char* extension = strrchr(item->d_name, '.');
        if (extension == NULL || (strcasecmp(extension, ".txt") != 0 && strcasecmp(extension, ".epub") != 0 &&
                                  strcasecmp(extension, ".pdf") != 0 && strcasecmp(extension, ".wrb") != 0)) continue;
        folio_book_t* book = &books[count];
        memset(book, 0, sizeof(*book));
        int path_len = snprintf(book->path, sizeof(book->path), "%s/%s", BOOK_DIR, item->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(book->path)) continue;
        struct stat info;
        if (stat(book->path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > INT32_MAX) continue;
        book->size = (uint32_t)info.st_size;
        copy_utf8(book->title, sizeof(book->title), item->d_name, (size_t)(extension - item->d_name));
        if ((strcasecmp(extension, ".wrb") == 0 || folio_wr_path_id(book->path, (char[64]){0},64)) &&
            !folio_weread_cache_metadata(book->path, book->title, sizeof(book->title), NULL, 0, NULL, 0) && strcasecmp(extension,".wrb")==0) continue;
        ++count;
    }
    closedir(dir);
    qsort(books, count, sizeof(*books), book_compare);
    return count;
}

// Full content identity prevents reuse after same-size replacements. Streaming
// UTF-8 validation and periodic yields keep memory and watchdog time bounded.
typedef struct { uint32_t total; bool in_word; } word_counter_t;

static void count_scalar(word_counter_t* counter, uint32_t cp) {
    bool latin = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                 (cp >= '0' && cp <= '9') || (cp >= 0xc0 && cp <= 0x2af && cp != 0xd7 && cp != 0xf7);
    bool ideograph = (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
                     (cp >= 0x20000 && cp <= 0x323af) || (cp >= 0x3041 && cp <= 0x30fa) ||
                     (cp >= 0xac00 && cp <= 0xd7a3);
    if (ideograph || (latin && !counter->in_word)) ++counter->total;
    counter->in_word = latin;
}

static bool inspect_file(FILE* file, uint64_t* hash_out, uint32_t* size_out,
                         folio_encoding_t* encoding_out, uint32_t* start_out, uint32_t* words) {
    if (fseek(file, 0, SEEK_SET) != 0) return false;
    uint8_t buffer[2048], first[4] = {0};
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t total = 0, scalar = 0, minimum = 0;
    unsigned continuation = 0, batches = 0;
    bool valid_utf8 = true;
    word_counter_t counter = {0};
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        if (count > INT32_MAX - total) return false;
        for (size_t i = 0; total + i < sizeof(first) && i < count; ++i) first[total + i] = buffer[i];
        hash = hash_update(hash, buffer, count);
        if (valid_utf8 && encoding_out != NULL) {
            for (size_t i = 0; i < count; ++i) {
                unsigned byte = buffer[i];
                if (continuation) {
                    if ((byte & 0xC0) != 0x80) { valid_utf8 = false; break; }
                    scalar = (scalar << 6) | (byte & 0x3F);
                    if (--continuation == 0 && (scalar < minimum || scalar > 0x10FFFF ||
                                                (scalar >= 0xD800 && scalar <= 0xDFFF))) {
                        valid_utf8 = false;
                        break;
                    }
                    if (!continuation && words) count_scalar(&counter, scalar);
                } else if (byte < 0x80) {
                    if (words) count_scalar(&counter, byte);
                    continue;
                } else if (byte >= 0xC2 && byte <= 0xDF) {
                    continuation = 1; scalar = byte & 0x1F; minimum = 0x80;
                } else if (byte >= 0xE0 && byte <= 0xEF) {
                    continuation = 2; scalar = byte & 0x0F; minimum = 0x800;
                } else if (byte >= 0xF0 && byte <= 0xF4) {
                    continuation = 3; scalar = byte & 7; minimum = 0x10000;
                } else { valid_utf8 = false; break; }
            }
        }
        total += (uint32_t)count;
        if (++batches % 32 == 0) vTaskDelay(1);
    }
    if (ferror(file)) return false;
    if (continuation) valid_utf8 = false;
    bool bom = total >= 3 && first[0] == 0xEF && first[1] == 0xBB && first[2] == 0xBF;
    if (encoding_out != NULL) {
        if (total >= 2 && ((first[0] == 0xFF && first[1] == 0xFE) ||
                           (first[0] == 0xFE && first[1] == 0xFF))) return false;
        if (total >= 4 && first[0] == 0 && first[1] == 0 && first[2] == 0xFE && first[3] == 0xFF) return false;
        *encoding_out = (bom || valid_utf8) ? FOLIO_ENCODING_UTF8 : FOLIO_ENCODING_GBK;
    }
    if (words && !valid_utf8) {
        if (fseek(file, bom ? 3 : 0, SEEK_SET)) return false;
        counter = (word_counter_t){0};
        uint32_t cp;
        while (folio_text_next(file, *encoding_out, &cp)) count_scalar(&counter, cp);
        if (ferror(file)) return false;
    }
    if (words) *words = counter.total;
    *hash_out = hash;
    *size_out = total;
    if (start_out != NULL) *start_out = bom ? 3 : 0;
    return fseek(file, bom ? 3 : 0, SEEK_SET) == 0;
}
// Full-file passes over multi-megabyte books dominate opening time. Their
// results are cached and reused while size, mtime and both file ends match.
typedef struct {
    uint32_t magic, size, mtime, head, tail, start, words, encoding, full;
    uint64_t hash;
    uint32_t checksum;
} inspect_record_t;
static void inspect_record_path(const char* path, char* out, size_t cap) {
    uint64_t key = hash_update(UINT64_C(14695981039346656037), (const uint8_t*)path, strlen(path));
    snprintf(out, cap, PROGRESS_DIR "/%08lx%08lx.i", (unsigned long)(uint32_t)(key >> 32), (unsigned long)(uint32_t)key);
}
static bool inspect_identity(FILE* file, const char* path, inspect_record_t* record) {
    struct stat info;
    uint8_t edge[1024];
    if (stat(path, &info) != 0 || info.st_size <= 0 || info.st_size > INT32_MAX) return false;
    record->size = (uint32_t)info.st_size; record->mtime = (uint32_t)info.st_mtime;
    size_t n = record->size < sizeof(edge) ? record->size : sizeof(edge);
    if (fseek(file, 0, SEEK_SET) != 0 || fread(edge, 1, n, file) != n) return false;
    record->head = checksum_bytes(edge, n);
    if (fseek(file, (long)(record->size - n), SEEK_SET) != 0 || fread(edge, 1, n, file) != n) return false;
    record->tail = checksum_bytes(edge, n);
    return true;
}
static bool inspect_cached(FILE* file, const char* path, uint64_t* hash_out, uint32_t* size_out,
                           folio_encoding_t* encoding_out, uint32_t* start_out, uint32_t* words) {
    inspect_record_t now = {0}, saved = {0};
    char record_path[sizeof(PROGRESS_DIR) + 24];
    inspect_record_path(path, record_path, sizeof(record_path));
    bool identified = inspect_identity(file, path, &now);
    bool want_full = encoding_out != NULL;
    if (identified) {
        FILE* cache = fopen(record_path, "rb");
        if (cache != NULL) {
            bool read = fread(&saved, sizeof(saved), 1, cache) == 1;
            fclose(cache);
            if (read && saved.magic == INSPECT_MAGIC && saved.checksum == checksum_bytes(&saved, offsetof(inspect_record_t, checksum)) &&
                saved.size == now.size && saved.mtime == now.mtime && saved.head == now.head && saved.tail == now.tail &&
                (saved.full || !want_full) && saved.start < saved.size && saved.encoding <= FOLIO_ENCODING_GBK) {
                *hash_out = saved.hash; *size_out = saved.size;
                if (encoding_out) *encoding_out = (folio_encoding_t)saved.encoding;
                if (start_out) *start_out = saved.start;
                if (words) *words = saved.words;
                return fseek(file, (long)saved.start, SEEK_SET) == 0;
            }
        }
    }
    folio_encoding_t encoding = FOLIO_ENCODING_UTF8;
    uint32_t start = 0, count = 0;
    if (!inspect_file(file, hash_out, size_out, want_full ? &encoding : NULL, want_full ? &start : NULL, want_full ? &count : NULL)) return false;
    if (encoding_out) *encoding_out = encoding;
    if (start_out) *start_out = start;
    if (words) *words = count;
    if (identified && now.size == *size_out && ensure_directory(STATE_DIR) && ensure_directory(PROGRESS_DIR)) {
        now.magic = INSPECT_MAGIC; now.hash = *hash_out; now.full = want_full;
        now.encoding = (uint32_t)encoding; now.start = start; now.words = count;
        now.checksum = checksum_bytes(&now, offsetof(inspect_record_t, checksum));
        FILE* cache = fopen(record_path, "wb");
        if (cache != NULL) { (void)fwrite(&now, sizeof(now), 1, cache); fclose(cache); }
    }
    return fseek(file, (long)(want_full ? start : 0), SEEK_SET) == 0;
}
void folio_reader_options_default(folio_reader_options_t* options) {
    if (options != NULL) *options = (folio_reader_options_t){.margin = 48, .line_gap = 8, .paragraph_gap = 10, .justify = true};
}
static bool options_valid(const folio_reader_options_t* options) {
    return options != NULL && options->margin >= 12 && options->margin <= 120 &&
           options->line_gap >= 0 && options->line_gap <= 48 && options->paragraph_gap >= 0 && options->paragraph_gap <= 72 && options->underline <= 2 && options->underline_shade <= 2;
}
static unsigned next_codepoint(folio_reader_t* reader, uint32_t* cp) {
    unsigned bytes = folio_text_next_bytes(reader->file, reader->encoding, cp);
    if (!bytes) return 0;
    if (*cp == '\r') {
        int following = fgetc(reader->file);
        if (following != EOF && following != '\n' && ungetc(following, reader->file) == EOF) return false;
        if (following == '\n') ++bytes;
        *cp = '\n';
    }
    if (*cp == '\t') *cp = ' ';
    return bytes;
}
static bool close_line(page_layout_t* page, uint16_t first, int start_x, int end_x, bool justify) {
    if (page->placement_count == first) return true;
    if (page->line_count >= PAGE_LINES) return false;
    line_layout_t* line = &page->lines[page->line_count++];
    *line = (line_layout_t){.first = first, .count = page->placement_count - first,
                          .start_x = (int16_t)start_x, .natural_width = (int16_t)(end_x - start_x),
                          .justify = justify && page->placement_count - first > 1};
    return true;
}
typedef struct { uint32_t cp, start, end; } text_token_t;
typedef struct {
    folio_reader_t* reader;
    text_token_t lookahead[9];
    unsigned count;
    uint32_t offset;
    bool eof, failed;
} text_cursor_t;

// A small decoded queue permits punctuation lookahead without seeking the SD
// card for every character. The logical cursor is independent of read-ahead.
static const text_token_t* cursor_peek(text_cursor_t* cursor, unsigned index) {
    if (index >= sizeof(cursor->lookahead) / sizeof(cursor->lookahead[0])) return NULL;
    while (cursor->count <= index && !cursor->eof && !cursor->failed) {
        uint32_t start = cursor->count ? cursor->lookahead[cursor->count-1].end : cursor->offset;
        uint32_t cp;
        unsigned bytes = next_codepoint(cursor->reader, &cp);
        if (!bytes) {
            cursor->failed = ferror(cursor->reader->file) || !feof(cursor->reader->file) ||
                             (uint32_t)start < cursor->reader->file_size;
            cursor->eof = true;
            break;
        }
        uint32_t end = start + bytes;
        if (end <= start || end > cursor->reader->file_size) { cursor->failed = true; break; }
        cursor->lookahead[cursor->count++] = (text_token_t){.cp = cp, .start = (uint32_t)start, .end = (uint32_t)end};
    }
    return index < cursor->count ? &cursor->lookahead[index] : NULL;
}
static void cursor_consume(text_cursor_t* cursor) {
    cursor->offset = cursor->lookahead[0].end;
    --cursor->count;
    memmove(cursor->lookahead, cursor->lookahead + 1, cursor->count * sizeof(cursor->lookahead[0]));
}
// Keep opening punctuation with the following character and short closing runs
// with the preceding character. Never let punctuation hang beyond the margin.
static int break_group_width(text_cursor_t* cursor, uint32_t cp, int width) {
    uint32_t previous = cp;
    for (unsigned i = 1; i < 9; ++i) {
        const text_token_t* next = cursor_peek(cursor, i);
        if (next == NULL || next->cp == '\n' || next->cp < 32 ||
            (!folio_is_opening_punctuation(previous) && !folio_is_closing_punctuation(next->cp))) break;
        width += folio_font_advance(cursor->reader->font, next->cp, previous);
        previous = next->cp;
    }
    return width;
}
static uint32_t next_chapter_boundary(const folio_reader_t* reader, uint32_t offset) {
    uint32_t end = reader->file_size;
    for (size_t i = 0; i < reader->epub.chapter_count; ++i) {
        uint32_t boundary = reader->epub.chapters[i].offset;
        if (boundary > offset && boundary < end) end = boundary;
    }
    return end;
}
static bool build_page(folio_reader_t* reader, uint32_t offset, bool paragraph_start,
                       uint32_t number, page_layout_t* page) {
    memset(page, 0, sizeof(*page));
    page->start = offset;
    page->number = number;
    page->paragraph_start = paragraph_start;
    if (offset >= reader->file_size || fseek(reader->file, (long)offset, SEEK_SET) != 0) return false;
    clearerr(reader->file);
    int width = epd_rotated_display_width(), height = epd_rotated_display_height();
    int left = reader->options.margin, right = width - reader->options.margin;
    int bottom = height - (reader->options.hide_footer ? 24 : BOTTOM_MARGIN);
    int font_height = folio_font_line_height(reader->font);
    int line_height = font_height + (reader->options.underline && reader->options.line_gap<6 ? 6 : reader->options.line_gap);
    int ascent = folio_font_ascent(reader->font), indent = reader->options.disable_indent ? 0 : folio_font_pixel_height(reader->font) * 2;
    int baseline = TOP_MARGIN + ascent;
    if (line_height <= 0 || font_height <= 0 || right - left < indent + folio_font_pixel_height(reader->font) * 2 ||
        baseline + font_height - ascent > bottom) return false;
    page->rule_baselines[page->rule_count++]=(int16_t)baseline;
    int line_x = left + (paragraph_start ? indent : 0), x = line_x;
    uint16_t line_first = 0;
    uint32_t previous = 0;
    uint32_t chapter_end = next_chapter_boundary(reader, offset);
    unsigned work = 0;
    text_cursor_t cursor = {.reader = reader, .offset = offset};
    while (page->placement_count < PAGE_GLYPHS && page->line_count < PAGE_LINES) {
        uint32_t before = cursor.offset;
        // Start directory sections on a fresh page. This also ensures a jump
        // and its chapter header describe the same visible section.
        if (before >= chapter_end && before < reader->file_size) {
            if (page->placement_count != 0) break;
            // Empty cover/frontmatter and chapter separators must not lend
            // their boundary/title to the following chapter's first glyph.
            chapter_end = next_chapter_boundary(reader, before);
            page->start = before;
            page->paragraph_start = paragraph_start = true;
            baseline = TOP_MARGIN + ascent;
            page->rule_count = 1;
            page->rule_baselines[0] = (int16_t)baseline;
            line_x = x = left + indent;
            previous = 0;
        }
        if ((uint32_t)before - offset >= PAGE_BYTES) break;
        const text_token_t* token = cursor_peek(&cursor, 0);
        if (token == NULL) break;
        uint32_t cp = token->cp;
        if (++work % 64 == 0) reader_yield();
        if(reader->is_weread && (cp==' '||cp==0xa0||cp==0x3000) && page->placement_count==line_first){cursor_consume(&cursor);continue;}
        if (cp == '\n') {
            cursor_consume(&cursor);
            if(reader->is_weread && page->placement_count==line_first){paragraph_start=true;continue;}
            if (!close_line(page, line_first, line_x, x, false)) return false;
            line_first = page->placement_count;
            paragraph_start = true;
            baseline += line_height + reader->options.paragraph_gap;
            line_x = left + indent; x = line_x; previous = 0;
            if (baseline + font_height - ascent > bottom || page->rule_count >= PAGE_LINES) break;
            page->rule_baselines[page->rule_count++]=(int16_t)baseline;
            continue;
        }
        if (cp < 32 || cp == 0x7F || cp == 0xFEFF) { cursor_consume(&cursor); continue; }
        int advance = folio_font_advance(reader->font, cp, previous);
        if (advance <= 0) advance = folio_font_pixel_height(reader->font);
        int group_width = break_group_width(&cursor, cp, advance);
        if (cursor.failed) return false;
        if (page->placement_count > line_first && (x + advance > right || x + group_width > right)) {
            if (!close_line(page, line_first, line_x, x, reader->options.justify)) return false;
            line_first = page->placement_count;
            baseline += line_height;
            line_x = left; x = line_x; previous = 0;
            if (baseline + font_height - ascent > bottom || page->rule_count >= PAGE_LINES) {
                break;
            }
            page->rule_baselines[page->rule_count++]=(int16_t)baseline;
            advance = folio_font_advance(reader->font, cp, 0);
        }
        if (x + advance > right) return false;
        glyph_placement_t* placement = &page->placements[page->placement_count++];
        *placement = (glyph_placement_t){.codepoint = cp, .x = (int16_t)x, .baseline = (int16_t)baseline};
        cursor_consume(&cursor);
        x += advance; previous = cp; paragraph_start = false;
    }
    if (cursor.failed || ferror(reader->file) || !close_line(page, line_first, line_x, x, false)) return false;
    if (cursor.offset <= offset || cursor.offset > reader->file_size) return false;
    page->end = cursor.offset;
    page->next_paragraph = paragraph_start;
    return true;
}
static void clear_checkpoints(folio_reader_t* reader) {
    reader->layout_font_size=folio_font_pixel_height(reader->font);
    reader->footer_base_valid = false;
    reader->pagination_offset = reader->content_start; reader->pagination_pages = 0;
    reader->pagination_paragraph = true; reader->pagination_done = reader->pagination_failed = false;
    if(reader->chapter_pages)memset(reader->chapter_pages,0,reader->epub.chapter_count*sizeof(uint32_t));
    memset(reader->checkpoints, 0, sizeof(reader->checkpoints));
    memset(reader->recent, 0, sizeof(reader->recent));
    reader->checkpoints[0] = (checkpoint_t){.offset = reader->content_start, .paragraph_start = true, .valid = true};
    reader->layout_saved_pages = reader->layout_saved_page = UINT32_MAX;
}
static void remember_checkpoint(folio_reader_t* reader, const page_layout_t* page) {
    if (page->number % CHECKPOINT_INTERVAL != 0) return;
    size_t index = page->number / CHECKPOINT_INTERVAL;
    if (index < CHECKPOINT_COUNT) reader->checkpoints[index] = (checkpoint_t){
        .offset = page->start, .paragraph_start = page->paragraph_start, .valid = true};
}
static void remember_page(folio_reader_t* reader, const page_layout_t* page) {
    reader->recent[page->number % CHECKPOINT_INTERVAL] = (recent_page_t){.number = page->number,
        .position = {.offset = page->start, .paragraph_start = page->paragraph_start, .valid = true}};
    remember_checkpoint(reader,page);
}
static void commit_page(folio_reader_t* reader) {
    page_layout_t* previous = reader->page;
    reader->page = reader->scratch; reader->scratch = previous;
    remember_page(reader, reader->page);
    reader->dirty = true;
}
static bool seek_internal(folio_reader_t* reader, uint32_t target, bool by_page) {
    if (reader->scan.count) {
        if (target>=reader->scan.count) target=reader->scan.count-1;
        if (target!=reader->page->number) {
            folio_font_flush_cache(reader->font);
            folio_pdf_scan_t next={.count=reader->scan.count};
            // Two full-page images plus the JPEG and decoder exhaust PSRAM; drop
            // the current image first and redraw it below if decoding fails.
            uint32_t current=reader->page->number;
            free(reader->scan.pixels); reader->scan.pixels=NULL;
            if (!folio_pdf_scan_page(reader->path,target,epd_rotated_display_width(),epd_rotated_display_height()-BOTTOM_MARGIN,&next)) {
                folio_pdf_scan_t again={.count=reader->scan.count};
                if (folio_pdf_scan_page(reader->path,current,epd_rotated_display_width(),epd_rotated_display_height()-BOTTOM_MARGIN,&again))
                    reader->scan=again;
                set_error(reader,"无法读取这页扫描图像，请检查文件及 TF 卡"); return false;
            }
            reader->scan=next;
        }
        reader->page->number=reader->page->start=target;
        reader->page->end=target+1;
        reader->dirty=true;
        reader->pagination_done=true;
        reader->pagination_pages=reader->scan.count;
        return true;
    }
    if (!by_page) {
        if (target < reader->content_start) target = reader->content_start;
        if (target >= reader->file_size) target = reader->file_size - 1;
    }
    size_t best = 0;
    for (size_t i = 1; i < CHECKPOINT_COUNT; ++i) {
        if (!reader->checkpoints[i].valid) continue;
        if (by_page ? i * CHECKPOINT_INTERVAL <= target : reader->checkpoints[i].offset <= target) best = i;
    }
    uint32_t offset = reader->checkpoints[best].offset, number = (uint32_t)best * CHECKPOINT_INTERVAL;
    bool paragraph = reader->checkpoints[best].paragraph_start;
    for (size_t i = 0; i < CHECKPOINT_INTERVAL; ++i) {
        const recent_page_t* recent = &reader->recent[i];
        if (!recent->position.valid || recent->number < number) continue;
        if (by_page ? recent->number <= target : recent->position.offset <= target) {
            offset = recent->position.offset; number = recent->number; paragraph = recent->position.paragraph_start;
        }
    }
    for (;;) {
        if (!build_page(reader, offset, paragraph, number, reader->scratch)) {
            set_error(reader, "无法读取页面，请检查 TF 卡和排版设置"); return false;
        }
        remember_page(reader, reader->scratch);
        if ((by_page && number == target) || (!by_page && target < reader->scratch->end)) { commit_page(reader); return true; }
        if (reader->scratch->end >= reader->file_size || number == UINT32_MAX) {
            set_error(reader, "跳转位置超出书籍范围"); return false;
        }
        offset = reader->scratch->end; paragraph = reader->scratch->next_paragraph;
        ++number; reader_yield();
    }
}
// Pagination checkpoints are persisted so reopening a book deep into its text
// does not lay out every page from the beginning again.
typedef struct {
    uint32_t magic, count, chapters, page_number, page_offset, pagination_offset, pagination_pages, footer_base;
    uint64_t key;
    uint8_t page_paragraph, pagination_paragraph, pagination_done, footer_valid;
    uint32_t footer_chapter;
} layout_header_t;
typedef struct { uint32_t index, offset, paragraph; } layout_checkpoint_t;
static uint64_t layout_key(folio_reader_t* reader) {
    static const uint32_t samples[] = {0x4E00, 0x7684, 0x3002, 0xFF0C, 0x201C, 'a', 'm', 'W', '1', ' '};
    int32_t values[24 + sizeof(samples) / sizeof(samples[0])];
    size_t n = 0;
    values[n++] = (int32_t)reader->file_size; values[n++] = (int32_t)reader->content_start;
    values[n++] = (int32_t)reader->text_hash; values[n++] = (int32_t)(reader->text_hash >> 32);
    values[n++] = reader->options.margin; values[n++] = reader->options.line_gap;
    values[n++] = reader->options.paragraph_gap; values[n++] = reader->options.justify;
    values[n++] = reader->options.disable_indent; values[n++] = reader->options.hide_footer;
    values[n++] = reader->options.underline && reader->options.line_gap < 6;
    values[n++] = epd_rotated_display_width(); values[n++] = epd_rotated_display_height();
    values[n++] = folio_font_pixel_height(reader->font); values[n++] = folio_font_line_height(reader->font);
    values[n++] = folio_font_ascent(reader->font); values[n++] = reader->is_weread;
    values[n++] = (int32_t)reader->epub.chapter_count;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) values[n++] = folio_font_advance(reader->font, samples[i], 0);
    uint64_t key = hash_update(UINT64_C(14695981039346656037), (const uint8_t*)values, n * sizeof(values[0]));
    for (size_t i = 0; i < reader->epub.chapter_count; ++i)
        key = hash_update(key, (const uint8_t*)&reader->epub.chapters[i].offset, sizeof(uint32_t));
    return key;
}
static void layout_path(const folio_reader_t* reader, char* out, size_t cap) {
    snprintf(out, cap, "%s.l", reader->progress_path);
}
static void save_layout(folio_reader_t* reader) {
    if (reader->scan.count || (reader->layout_saved_pages == reader->pagination_pages &&
                               reader->layout_saved_page == reader->page->number)) return;
    layout_header_t header = {.magic = LAYOUT_MAGIC, .chapters = reader->chapter_pages ? (uint32_t)reader->epub.chapter_count : 0,
        .page_number = reader->page->number, .page_offset = reader->page->start, .page_paragraph = reader->page->paragraph_start,
        .pagination_offset = reader->pagination_offset, .pagination_pages = reader->pagination_pages,
        .pagination_paragraph = reader->pagination_paragraph, .pagination_done = reader->pagination_done,
        .footer_valid = reader->footer_base_valid, .footer_base = reader->footer_base,
        .footer_chapter = (uint32_t)reader->footer_chapter, .key = layout_key(reader)};
    for (size_t i = 1; i < CHECKPOINT_COUNT; ++i) header.count += reader->checkpoints[i].valid;
    char path[sizeof(reader->progress_path) + 4];
    layout_path(reader, path, sizeof(path));
    FILE* file = fopen(path, "wb");
    if (file == NULL) return;
    uint64_t sum = hash_update(UINT64_C(14695981039346656037), (const uint8_t*)&header, sizeof(header));
    bool okay = fwrite(&header, sizeof(header), 1, file) == 1;
    for (size_t i = 1; okay && i < CHECKPOINT_COUNT; ++i) {
        if (!reader->checkpoints[i].valid) continue;
        layout_checkpoint_t entry = {(uint32_t)i, reader->checkpoints[i].offset, reader->checkpoints[i].paragraph_start};
        sum = hash_update(sum, (const uint8_t*)&entry, sizeof(entry));
        okay = fwrite(&entry, sizeof(entry), 1, file) == 1;
    }
    if (okay && header.chapters) {
        sum = hash_update(sum, (const uint8_t*)reader->chapter_pages, header.chapters * sizeof(uint32_t));
        okay = fwrite(reader->chapter_pages, sizeof(uint32_t), header.chapters, file) == header.chapters;
    }
    okay = okay && fwrite(&sum, sizeof(sum), 1, file) == 1;
    if (fclose(file) != 0) okay = false;
    if (okay) { reader->layout_saved_pages = reader->pagination_pages; reader->layout_saved_page = reader->page->number; }
    else (void)remove(path);
}
static void load_layout(folio_reader_t* reader, uint32_t saved_offset) {
    char path[sizeof(reader->progress_path) + 4];
    layout_path(reader, path, sizeof(path));
    FILE* file = fopen(path, "rb");
    if (file == NULL) return;
    layout_header_t header;
    layout_checkpoint_t* entries = NULL;
    uint32_t* chapters = NULL;
    uint64_t sum = 0, stored = 0;
    bool okay = fread(&header, sizeof(header), 1, file) == 1 && header.magic == LAYOUT_MAGIC &&
        header.count < CHECKPOINT_COUNT && header.key == layout_key(reader) &&
        (header.chapters == 0 || header.chapters == reader->epub.chapter_count) &&
        header.page_offset < reader->file_size && header.pagination_offset <= reader->file_size;
    if (okay) {
        sum = hash_update(UINT64_C(14695981039346656037), (const uint8_t*)&header, sizeof(header));
        entries = reader_alloc((header.count + 1) * sizeof(*entries));
        chapters = header.chapters ? reader_alloc(header.chapters * sizeof(uint32_t)) : NULL;
        okay = entries && (!header.chapters || chapters) &&
            fread(entries, sizeof(*entries), header.count, file) == header.count &&
            (!header.chapters || fread(chapters, sizeof(uint32_t), header.chapters, file) == header.chapters) &&
            fread(&stored, sizeof(stored), 1, file) == 1;
    }
    fclose(file);
    if (okay) {
        sum = hash_update(sum, (const uint8_t*)entries, header.count * sizeof(*entries));
        if (header.chapters) sum = hash_update(sum, (const uint8_t*)chapters, header.chapters * sizeof(uint32_t));
        okay = sum == stored;
    }
    for (uint32_t i = 0; okay && i < header.count; ++i)
        okay = entries[i].index > 0 && entries[i].index < CHECKPOINT_COUNT && entries[i].offset < reader->file_size;
    if (okay) {
        for (uint32_t i = 0; i < header.count; ++i)
            reader->checkpoints[entries[i].index] = (checkpoint_t){.offset = entries[i].offset,
                .paragraph_start = entries[i].paragraph != 0, .valid = true};
        reader->pagination_offset = header.pagination_offset; reader->pagination_pages = header.pagination_pages;
        reader->pagination_paragraph = header.pagination_paragraph; reader->pagination_done = header.pagination_done;
        if (chapters) { free(reader->chapter_pages); reader->chapter_pages = chapters; chapters = NULL; }
        else if (reader->epub.chapter_count) { reader->pagination_offset = reader->content_start; reader->pagination_pages = 0;
            reader->pagination_paragraph = true; reader->pagination_done = false; }
        if (header.footer_valid && header.footer_chapter < reader->epub.chapter_count) {
            reader->footer_base_valid = true; reader->footer_base = header.footer_base; reader->footer_chapter = header.footer_chapter;
        }
        if (header.page_offset == saved_offset) reader->recent[header.page_number % CHECKPOINT_INTERVAL] =
            (recent_page_t){.number = header.page_number, .position = {.offset = header.page_offset,
                .paragraph_start = header.page_paragraph != 0, .valid = true}};
        reader->layout_saved_pages = header.pagination_pages; reader->layout_saved_page = header.page_number;
    }
    free(entries); free(chapters);
}
static bool record_valid(const folio_reader_t* reader, const progress_record_t* record) {
    if (record->magic != PROGRESS_MAGIC || record->version != 2 ||
        record->path_hash != reader->path_hash || record->source_size != reader->source_size ||
        record->source_hash != reader->source_hash || record->text_hash != reader->text_hash ||
        record->text_size != reader->file_size || record->offset < reader->content_start ||
        record->offset >= reader->file_size || record->bookmark_count > BOOKMARK_LIMIT ||
        record->checksum != checksum_bytes(record, offsetof(progress_record_t, checksum))) return false;
    for (uint32_t i = 0; i < record->bookmark_count; ++i) {
        if (record->bookmarks[i].offset < reader->content_start || record->bookmarks[i].offset >= reader->file_size ||
            memchr(record->bookmarks[i].title, 0, sizeof(record->bookmarks[i].title)) == NULL) return false;
    }
    return true;
}
static uint32_t load_progress(folio_reader_t* reader) {
    progress_record_t* record = reader_alloc(sizeof(*record));
    if (record == NULL) return reader->content_start;
    uint32_t offset = reader->content_start;
    for (int slot = 0; slot < 2; ++slot) {
        char path[sizeof(reader->progress_path) + 4];
        snprintf(path, sizeof(path), "%s.%d", reader->progress_path, slot);
        FILE* file = fopen(path, "rb");
        if (file == NULL) continue;
        bool okay = fread(record, 1, sizeof(*record), file) == sizeof(*record) && fgetc(file) == EOF && !ferror(file);
        fclose(file);
        if (!okay || !record_valid(reader, record)) continue;
        if (reader->active_slot >= 0 && (int32_t)(record->generation - reader->generation) <= 0) continue;
        reader->active_slot = slot; reader->generation = record->generation; offset = record->offset;
        reader->bookmark_count = record->bookmark_count;
        memcpy(reader->bookmarks, record->bookmarks, sizeof(reader->bookmarks));
    }
    free(record);
    return offset;
}
bool folio_reader_save(folio_reader_t* reader) {
    if (reader == NULL || !reader->ready) return false;
    save_layout(reader);
    if (!reader->dirty) return true;
    if (!ensure_directory(STATE_DIR) || !ensure_directory(PROGRESS_DIR)) {
        set_error(reader, "无法创建进度目录，请检查 TF 卡"); return false;
    }
    progress_record_t* record = reader_alloc(sizeof(*record));
    if (record == NULL) { set_error(reader, "内存不足，进度尚未保存"); return false; }
    record->magic = PROGRESS_MAGIC; record->version = 2; record->generation = reader->generation + 1;
    record->path_hash = reader->path_hash; record->source_hash = reader->source_hash; record->text_hash = reader->text_hash;
    record->source_size = reader->source_size; record->text_size = reader->file_size;
    record->offset = reader->page->start; record->bookmark_count = (uint32_t)reader->bookmark_count;
    memcpy(record->bookmarks, reader->bookmarks, sizeof(reader->bookmarks));
    record->checksum = checksum_bytes(record, offsetof(progress_record_t, checksum));
    int next_slot = reader->active_slot == 0 ? 1 : 0;
    char path[sizeof(reader->progress_path) + 4];
    snprintf(path, sizeof(path), "%s.%d", reader->progress_path, next_slot);
    // Write only the inactive slot: FAT rename-over-existing is not portable.
    // A torn write leaves the preceding valid slot available on restart.
    FILE* file = fopen(path, "wb");
    bool okay = file != NULL;
    if (file != NULL) {
        okay = fwrite(record, 1, sizeof(*record), file) == sizeof(*record) && fflush(file) == 0;
        if (okay && fsync(fileno(file)) != 0) okay = false;
        if (fclose(file) != 0) okay = false;
    }
    if (okay) {
        reader->active_slot = next_slot; reader->generation = record->generation; reader->dirty = false; reader->error[0] = '\0';
    } else set_error(reader, "进度尚未保存，请检查 TF 卡后重试");
    free(record);
    return okay;
}
folio_reader_t* folio_reader_open_with_options(const folio_book_t* book, folio_font_t* font,
                                              const folio_reader_options_t* options) {
    s_last_error[0] = '\0';
    OPEN_START();
    if (book == NULL || font == NULL || (options != NULL && !options_valid(options)) ||
        (book != NULL && (memchr(book->path, 0, sizeof(book->path)) == NULL ||
                          memchr(book->title, 0, sizeof(book->title)) == NULL))) {
        snprintf(s_last_error, sizeof(s_last_error), "书籍、字体或排版设置无效"); return NULL;
    }
    folio_reader_t* reader = reader_alloc(sizeof(*reader));
    if (reader == NULL) { snprintf(s_last_error, sizeof(s_last_error), "内存不足，无法打开书籍"); return NULL; }
    reader->font = font; reader->active_slot = -1;
    folio_reader_options_default(&reader->options);
    if (options != NULL) reader->options = *options;
    copy_utf8(reader->path, sizeof(reader->path), book->path, strnlen(book->path, sizeof(book->path)));
    copy_utf8(reader->title, sizeof(reader->title), book->title, strnlen(book->title, sizeof(book->title)));
    char identity[256],pointer_path[256],wr_id[64];
    bool native_cache=folio_wr_path_id(reader->path,wr_id,sizeof(wr_id)) && folio_weread_cache_revision(reader->path)!=0;
    const char* identity_path=reader->path;
    if(native_cache && folio_wr_identity(reader->path,identity,sizeof(identity)))identity_path=identity;
    reader->path_hash = hash_update(UINT64_C(14695981039346656037), (const uint8_t*)identity_path, strlen(identity_path));
    snprintf(reader->progress_path, sizeof(reader->progress_path), PROGRESS_DIR "/%08lx%08lx",
             (unsigned long)(uint32_t)(reader->path_hash >> 32), (unsigned long)(uint32_t)reader->path_hash);
    const char* source_path=reader->path;
    if(native_cache && folio_wr_aux(reader->path,"entry.wrb",pointer_path,sizeof(pointer_path)) && access(pointer_path,F_OK)==0)source_path=pointer_path;
    FILE* source = fopen(source_path, "rb");
    if (source == NULL) { set_error(reader, "无法打开书籍，请检查 TF 卡"); goto fail; }
    const char* kind = strrchr(reader->path, '.');
    if (!native_cache && kind && strcasecmp(kind, ".txt") == 0) attach_io_buffer(reader, source);
    const char* extension = strrchr(reader->path, '.');
    bool is_weread = native_cache || (extension != NULL && strcasecmp(extension, ".wrb") == 0);
    reader->is_weread = is_weread;
    reader->is_epub = is_weread || (extension != NULL && strcasecmp(extension, ".epub") == 0);
    reader->is_pdf = extension && !strcasecmp(extension, ".pdf");
    if (reader->is_pdf) {
        folio_font_flush_cache(font);
        bool inspected = inspect_cached(source, source_path, &reader->source_hash, &reader->source_size, NULL, NULL, NULL);
        fclose(source);
        if (!inspected || !ensure_directory(STATE_DIR)) {
            set_error(reader, "无法读取 PDF 或创建文本缓存"); goto fail;
        }
        char cache[sizeof(STATE_DIR)+48];
        snprintf(cache, sizeof(cache), STATE_DIR "/pdf1-%08lx%08lx.txt",
                 (unsigned long)(uint32_t)(reader->source_hash>>32), (unsigned long)(uint32_t)reader->source_hash);
        reader->file = fopen(cache, "rb");
        if (reader->file) attach_io_buffer(reader, reader->file);
        if (!reader->file && folio_pdf_scan_page(source_path,0,epd_rotated_display_width(),
                epd_rotated_display_height()-BOTTOM_MARGIN,&reader->scan)) {
            reader->file_size=reader->scan.count;
            reader->text_hash=reader->source_hash;
        } else if (!reader->file) {
            char temp[sizeof(cache)+8];
            snprintf(temp,sizeof(temp),"%s.XXXXXX",cache);
            int fd = mkstemp(temp);
            FILE *output = fd >= 0 ? fdopen(fd,"wb") : NULL;
            if (!output) {
                if (fd >= 0) { close(fd); unlink(temp); }
                set_error(reader,"无法创建 PDF 文本缓存，请检查 TF 卡"); goto fail;
            }
            bool ok = folio_pdf_extract(source_path,output);
            if (fclose(output) || !ok || rename(temp,cache)) {
                unlink(temp);
                set_error(reader,"无法读取 PDF，请检查文档加密、页面格式和 TF 卡空间"); goto fail;
            }
            reader->file = fopen(cache,"rb");
            if (reader->file) attach_io_buffer(reader, reader->file);
        }
        if (!reader->scan.count && (!reader->file || !inspect_cached(reader->file,cache,&reader->text_hash,&reader->file_size,
                                            &reader->encoding,&reader->content_start, &reader->word_count))) {
            set_error(reader,"无法读取 PDF 文本缓存"); goto fail;
        }
    } else if (reader->is_epub) {
        bool inspected = !is_weread || inspect_cached(source, source_path, &reader->source_hash, &reader->source_size, NULL, NULL, NULL);
        fclose(source);
        if (!inspected) { set_error(reader, "无法读取 EPUB 文件"); goto fail; }
        if (is_weread ? !folio_weread_cache_prepare(reader->path, &reader->epub, reader->error, sizeof(reader->error)) :
                       !folio_epub_prepare(reader->path, &reader->epub, reader->error, sizeof(reader->error))) goto fail;
        OPEN_MARK("epub_prepare");
        if (!is_weread) {
            reader->source_hash = reader->epub.source_hash;
            reader->source_size = reader->epub.source_size;
        }
        reader->file = fopen(reader->epub.text_path, "rb");
        if (reader->file) attach_io_buffer(reader, reader->file);
        if (reader->file == NULL || !inspect_cached(reader->file, reader->epub.text_path, &reader->text_hash, &reader->file_size,
                                                &reader->encoding, &reader->content_start, &reader->word_count)) {
            set_error(reader, "无法读取 EPUB 文本缓存"); goto fail;
        }
        if (reader->encoding != FOLIO_ENCODING_UTF8) { set_error(reader, "EPUB 文本编码无效"); goto fail; }
        if (reader->epub.chapter_count > 0 && reader->epub.chapters == NULL) {
            set_error(reader, "EPUB 目录缓存无效"); goto fail;
        }
        for (size_t i = 0; i < reader->epub.chapter_count; ++i) {
            if (reader->epub.chapters[i].offset >= reader->file_size ||
                memchr(reader->epub.chapters[i].title, 0, sizeof(reader->epub.chapters[i].title)) == NULL) {
                set_error(reader, "EPUB 目录位置无效"); goto fail;
            }
        }
        if (reader->epub.title[0]) copy_utf8(reader->title, sizeof(reader->title), reader->epub.title, strlen(reader->epub.title));
    } else {
        reader->file = source;
        if (!inspect_cached(source, source_path, &reader->source_hash, &reader->source_size, &reader->encoding, &reader->content_start, &reader->word_count)) {
            set_error(reader, "TXT 读取失败，仅支持 UTF-8 或 GBK 编码"); goto fail;
        }
        reader->text_hash = reader->source_hash; reader->file_size = reader->source_size;
    }
    OPEN_MARK("inspect");
    if (reader->content_start >= reader->file_size) { set_error(reader, "书籍没有可阅读的文本"); goto fail; }
    reader->page = reader_alloc(sizeof(*reader->page)); reader->scratch = reader_alloc(sizeof(*reader->scratch));
    if (reader->page == NULL || reader->scratch == NULL) { set_error(reader, "内存不足，无法排版书籍"); goto fail; }
    clear_checkpoints(reader);
    uint32_t saved_offset = load_progress(reader);
    if (!reader->scan.count) load_layout(reader, saved_offset);
    OPEN_MARK("progress");
    if (!seek_internal(reader, saved_offset, false)) goto fail;
    if (reader->active_slot >= 0 && reader->page->start == saved_offset) reader->dirty = false;
    OPEN_MARK("seek");
#if defined(ESP_PLATFORM) && defined(CONFIG_FOLIO_USB_DEBUG) && CONFIG_FOLIO_USB_DEBUG
    ESP_LOGI("reader", "open page=%lu offset=%lu size=%lu", (unsigned long)reader->page->number, (unsigned long)saved_offset, (unsigned long)reader->file_size);
#endif
    reader->ready = true;
    (void)folio_reader_save(reader);
    OPEN_MARK("save");
    return reader;
fail:
    snprintf(s_last_error, sizeof(s_last_error), "%s", reader->error[0] ? reader->error : "无法打开书籍");
    folio_reader_close(reader);
    return NULL;
}
folio_reader_t* folio_reader_open(const folio_book_t* book, folio_font_t* font) {
    return folio_reader_open_with_options(book, font, NULL);
}
void folio_reader_close(folio_reader_t* reader) {
    if (reader == NULL) return;
    if (reader->ready) (void)folio_reader_save(reader);
    if (reader->file != NULL) fclose(reader->file);
    free(reader->io_buffer);
    folio_epub_free(&reader->epub);
    free(reader->scan.pixels);
    free(reader->chapter_pages);
    free(reader->page); free(reader->scratch); free(reader);
}
bool folio_reader_turn(folio_reader_t* reader, bool next) {
    if (reader == NULL || !reader->ready) return false;
    reader->error[0] = '\0';
    if (!next) {
        if (reader->page->number == 0) return false;
        return seek_internal(reader, reader->page->number - 1, true);
    }
    if (reader->page->end >= reader->file_size) return false;
    if (reader->scan.count) return seek_internal(reader, reader->page->end, false);
    if (!build_page(reader, reader->page->end, reader->page->next_paragraph, reader->page->number + 1, reader->scratch)) {
        set_error(reader, "翻页失败，请检查 TF 卡"); return false;
    }
    commit_page(reader);
    return true;
}
bool folio_reader_next(folio_reader_t* reader) {
    if (!folio_reader_turn(reader, true)) return false;
    (void)folio_reader_save(reader);
    return true;
}
bool folio_reader_previous(folio_reader_t* reader) {
    if (!folio_reader_turn(reader, false)) return false;
    (void)folio_reader_save(reader);
    return true;
}
bool folio_reader_seek(folio_reader_t* reader, uint32_t byte_offset) {
    if (reader == NULL || !reader->ready) return false;
    reader->error[0] = '\0';
    if (!seek_internal(reader, byte_offset, false)) return false;
    (void)folio_reader_save(reader);
    return true;
}
bool folio_reader_seek_percent(folio_reader_t* reader, unsigned percent) {
    if (reader == NULL || percent > 100) return false;
    uint32_t length = reader->file_size - reader->content_start;
    uint32_t offset = reader->content_start + (uint32_t)((uint64_t)length * percent / 100);
    return folio_reader_seek(reader, offset);
}
bool folio_reader_set_options(folio_reader_t* reader, const folio_reader_options_t* options) {
    if (reader == NULL || !reader->ready || !options_valid(options)) return false;
    reader->error[0] = '\0';
    if (reader->scan.count) { reader->options=*options; return true; }
    folio_reader_options_t previous = reader->options;
    reader->options = *options;
    if(previous.margin==options->margin && previous.line_gap==options->line_gap &&
        (previous.underline!=0 || previous.line_gap>=6)==(options->underline!=0 || options->line_gap>=6) &&
        previous.paragraph_gap==options->paragraph_gap && previous.justify==options->justify &&
        previous.disable_indent==options->disable_indent &&
        previous.hide_footer==options->hide_footer && reader->layout_font_size==folio_font_pixel_height(reader->font))return true;
    reader->layout_font_size=folio_font_pixel_height(reader->font);
    clear_checkpoints(reader);
    if (!seek_internal(reader, reader->page->start, false)) {
        reader->options = previous;
        clear_checkpoints(reader);
        return false;
    }
    (void)folio_reader_save(reader);
    return true;
}
void folio_reader_draw(folio_reader_t* reader, uint8_t* framebuffer) {
    if (reader == NULL || !reader->ready || framebuffer == NULL) return;
    if (reader->scan.pixels) {
        const folio_pdf_scan_t *scan=&reader->scan;
        int left=(epd_rotated_display_width()-(int)scan->width)/2;
        int top=(epd_rotated_display_height()-BOTTOM_MARGIN-(int)scan->height)/2;
        for (unsigned y=0;y<scan->height;++y)
            for (unsigned x=0;x<scan->width;++x)
                epd_draw_pixel(left+(int)x,top+(int)y,scan->pixels[(size_t)y*scan->width+x],framebuffer);
        return;
    }
    const page_layout_t* page = reader->page;
    if(reader->options.underline && page->rule_count) {
        int start=reader->options.margin,end=epd_rotated_display_width()-reader->options.margin;
        int descent=folio_font_line_height(reader->font)-folio_font_ascent(reader->font);
        int step=folio_font_line_height(reader->font)+(reader->options.line_gap<6?6:reader->options.line_gap);
        int bottom=epd_rotated_display_height()-(reader->options.hide_footer?24:BOTTOM_MARGIN);
        const uint8_t shades[]={0xB0,0x80,0x40};
        unsigned row=0;int baseline=page->rule_baselines[0];
        while(baseline+descent+2<bottom) {
            int y=baseline+descent+2;
            for(int x=start;x<end;++x)
                if(reader->options.underline==1 || (x-start)%12<7)
                    epd_draw_pixel(x,y,shades[reader->options.underline_shade],framebuffer);
            // Preserve paragraph spacing, but also rule empty/trailing rows.
            baseline=++row<page->rule_count?page->rule_baselines[row]:baseline+step;
        }
    }
    for (uint16_t line_index = 0; line_index < page->line_count; ++line_index) {
        const line_layout_t* line = &page->lines[line_index];
        int available = epd_rotated_display_width() - reader->options.margin - line->start_x;
        int extra = line->justify ? available - line->natural_width : 0;
        if (extra < 0) extra = 0;
        int gaps = line->count > 1 ? line->count - 1 : 1;
        int base_gap = extra / gaps, remainder = extra % gaps, added = 0;
        for (uint16_t i = 0; i < line->count; ++i) {
            const glyph_placement_t* glyph = &page->placements[line->first + i];
            (void)folio_font_draw_glyph(reader->font, framebuffer, glyph->x + added, glyph->baseline, glyph->codepoint);
            if (line->justify && i + 1 < line->count) added += base_gap + ((int)i < remainder ? 1 : 0);
        }
    }
}
bool folio_reader_word_count(const folio_reader_t* reader, uint32_t* count) {
    if (!reader || !count || reader->scan.count) return false;
    *count = reader->word_count;
    return true;
}
size_t folio_reader_chapter_count(const folio_reader_t* reader) { return reader != NULL && reader->is_epub ? reader->epub.chapter_count : 0; }
const char* folio_reader_chapter_title(const folio_reader_t* reader, size_t index) {
    return index < folio_reader_chapter_count(reader) ? reader->epub.chapters[index].title : "";
}
static bool bare_chapter_number(const char* title, uint32_t* number) {
    if (*title < '1' || *title > '9') return false;
    uint32_t value = 0;
    for (const char* p = title; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (UINT32_MAX - (unsigned)(*p - '0')) / 10) return false;
        value = value * 10 + (unsigned)(*p - '0');
    }
    *number = value;
    return true;
}
static bool chinese_chapter_digit(const char* text) {
    static const char* digits[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九"};
    for (size_t i = 0; i < sizeof(digits) / sizeof(digits[0]); ++i)
        if (strncmp(text, digits[i], 3) == 0) return true;
    return false;
}
static size_t chapter_label_space(const char* text) {
    if (*text == ' ' || *text == '\t') return 1;
    return strncmp(text, "　", 3) == 0 ? 3 : 0;
}
static const char* chinese_chapter_ordinal_end(const char* title) {
    const char* p = title;
    bool digit = chinese_chapter_digit(p);
    if (digit) p += 3;
    if (strncmp(p, "十", 3) == 0) {
        p += 3;
        if (chinese_chapter_digit(p)) p += 3;
    } else if (!digit) return NULL;
    return p;
}
static bool named_chapter_heading(const char* title) {
    // Deliberately narrow: Chinese ordinals 1..99, whitespace, then a name.
    // A bare ordinal (e.g. Wen Cheng's 一/二/三) is never a parent here.
    const char* p = chinese_chapter_ordinal_end(title);
    if (!p) return false;
    size_t space = chapter_label_space(p);
    if (!space) return false;
    do { p += space; space = chapter_label_space(p); } while (space);
    uint32_t ignored;
    const char* ordinal_end = chinese_chapter_ordinal_end(p);
    return *p && !bare_chapter_number(p, &ignored) && (!ordinal_end || *ordinal_end);
}
void folio_reader_chapter_label(const folio_reader_t* reader, size_t index, char* out, size_t capacity) {
    if (!out || !capacity) return;
    const char* title = folio_reader_chapter_title(reader, index);
    copy_utf8(out, capacity, title, strlen(title));
    uint32_t number;
    if (!bare_chapter_number(title, &number)) return;
    size_t first = index, count = folio_reader_chapter_count(reader);
    while (first && bare_chapter_number(reader->epub.chapters[first - 1].title, &number)) --first;
    if (!first || !named_chapter_heading(reader->epub.chapters[first - 1].title)) return;
    uint32_t expected;
    if (!bare_chapter_number(reader->epub.chapters[first].title, &expected) || expected > 2) return;
    size_t end = first;
    while (end < count && bare_chapter_number(reader->epub.chapters[end].title, &number)) {
        if (number != expected || expected == UINT32_MAX) return;
        ++expected; ++end;
    }
    // One isolated number is insufficient evidence of a subsection sequence.
    if (end - first < 2) return;
    const char* parent = reader->epub.chapters[first - 1].title;
    const char separator[] = " · ";
    size_t suffix = strlen(separator) + strlen(title);
    if (capacity <= suffix + 1) return;
    copy_utf8(out, capacity - suffix, parent, strlen(parent));
    size_t prefix = strlen(out);
    if (!prefix) { copy_utf8(out, capacity, title, strlen(title)); return; }
    memcpy(out + prefix, separator, strlen(separator));
    memcpy(out + prefix + strlen(separator), title, strlen(title) + 1);
}
size_t folio_reader_current_chapter(const folio_reader_t* reader) {
    size_t selected = SIZE_MAX;
    uint32_t best = 0;
    for (size_t i = 0; i < folio_reader_chapter_count(reader); ++i) {
        uint32_t offset = reader->epub.chapters[i].offset;
        if (offset <= reader->page->start && (selected == SIZE_MAX || offset >= best)) { selected = i; best = offset; }
    }
    return selected;
}
bool folio_reader_jump_chapter(folio_reader_t* reader, size_t index) {
    return index < folio_reader_chapter_count(reader) && folio_reader_seek(reader, reader->epub.chapters[index].offset);
}
uint32_t folio_reader_chapter_page_number(folio_reader_t* reader) {
    if (!reader || !reader->ready) return 0;
    size_t chapter=folio_reader_current_chapter(reader);
    if(chapter==SIZE_MAX)return folio_reader_page_number(reader);
    if(!reader->footer_base_valid || reader->footer_chapter!=chapter) {
        uint32_t target=reader->epub.chapters[chapter].offset;
        size_t best=0;
        for(size_t i=1;i<CHECKPOINT_COUNT;++i)
            if(reader->checkpoints[i].valid && reader->checkpoints[i].offset<=target)best=i;
        uint32_t offset=reader->checkpoints[best].offset,number=(uint32_t)best*CHECKPOINT_INTERVAL;
        bool paragraph=reader->checkpoints[best].paragraph_start;
        for(size_t i=0;i<CHECKPOINT_INTERVAL;++i) {
            const recent_page_t* recent=&reader->recent[i];
            if(recent->position.valid && recent->position.offset<=target && recent->number>=number) {
                offset=recent->position.offset;number=recent->number;paragraph=recent->position.paragraph_start;
            }
        }
        for(;;) {
            if(!build_page(reader,offset,paragraph,number,reader->scratch) || reader->scratch->end<=offset)return 0;
            if(target<reader->scratch->end)break;
            offset=reader->scratch->end;paragraph=reader->scratch->next_paragraph;++number;
            reader_yield();
        }
        if(number>reader->page->number)return 0;
        reader->footer_base=number;
        reader->footer_chapter=chapter;reader->footer_base_valid=true;
    }
    return reader->page->number+1-reader->footer_base;
}
// Count with the exact same layout used by turns and seeks, in small idle slices.
bool folio_reader_paginate_step(folio_reader_t* reader, unsigned budget) {
    if(!reader || !reader->ready || reader->pagination_done || reader->pagination_failed)return false;
    if(reader->epub.chapter_count && !reader->chapter_pages) {
        reader->chapter_pages=reader_alloc(reader->epub.chapter_count*sizeof(uint32_t));
        if(!reader->chapter_pages){reader->pagination_failed=true;return false;}
    }
    while(budget-- && !reader->pagination_done) {
        if(!build_page(reader,reader->pagination_offset,reader->pagination_paragraph,reader->pagination_pages,reader->scratch)) {
            reader->pagination_failed=true;return false;
        }
        page_layout_t* page=reader->scratch;
        remember_checkpoint(reader,page);
        size_t section=SIZE_MAX;uint32_t best=0;
        for(size_t i=0;i<reader->epub.chapter_count;++i) {
            uint32_t start=reader->epub.chapters[i].offset;
            if(start<=page->start && (section==SIZE_MAX || start>=best)){section=i;best=start;}
        }
        if(section!=SIZE_MAX)++reader->chapter_pages[section];
        ++reader->pagination_pages;reader->pagination_offset=page->end;
        reader->pagination_paragraph=page->next_paragraph;
        reader->pagination_done=page->end>=reader->file_size;
    }
    return true;
}
static void chapter_range(const folio_reader_t* reader,uint32_t* start,uint32_t* end) {
    *start=reader->content_start;*end=reader->file_size;
    size_t chapter=folio_reader_current_chapter(reader);
    if(chapter!=SIZE_MAX)*start=reader->epub.chapters[chapter].offset;
    for(size_t i=0;i<reader->epub.chapter_count;++i) {
        uint32_t next=reader->epub.chapters[i].offset;
        if(next>*start && next<*end)*end=next;
    }
}
uint32_t folio_reader_page_total(const folio_reader_t* reader,bool chapter) {
    if(!reader || !reader->ready || reader->pagination_failed)return 0;
    size_t section=folio_reader_current_chapter(reader);
    if(!chapter || section==SIZE_MAX)return reader->pagination_done?reader->pagination_pages:0;
    uint32_t start,end;chapter_range(reader,&start,&end);
    return reader->chapter_pages && reader->pagination_offset>=end?reader->chapter_pages[section]:0;
}
unsigned folio_reader_chapter_percent(const folio_reader_t* reader) {
    if(!reader || !reader->ready)return 0;
    if(folio_reader_current_chapter(reader)==SIZE_MAX)return folio_reader_percent(reader);
    uint32_t start,end;chapter_range(reader,&start,&end);
    if(end<=start)return 0;
    if(reader->page->end>=end)return 100;
    return (unsigned)((uint64_t)(reader->page->start-start)*100/(end-start));
}
static size_t bookmark_at_page(const folio_reader_t* reader) {
    if (reader == NULL || !reader->ready) return SIZE_MAX;
    for (size_t i = 0; i < reader->bookmark_count; ++i) {
        if (reader->bookmarks[i].offset >= reader->page->start && reader->bookmarks[i].offset < reader->page->end) return i;
    }
    return SIZE_MAX;
}
size_t folio_reader_bookmark_count(const folio_reader_t* reader) { return reader != NULL ? reader->bookmark_count : 0; }
const char* folio_reader_bookmark_title(const folio_reader_t* reader, size_t index) {
    return reader != NULL && index < reader->bookmark_count ? reader->bookmarks[index].title : "";
}
uint32_t folio_reader_bookmark_position(const folio_reader_t* reader, size_t index) {
    return reader != NULL && index < reader->bookmark_count ? reader->bookmarks[index].offset : 0;
}
bool folio_reader_is_bookmarked(const folio_reader_t* reader) { return bookmark_at_page(reader) != SIZE_MAX; }
bool folio_reader_delete_bookmark(folio_reader_t* reader, size_t index) {
    if (reader == NULL || !reader->ready || index >= reader->bookmark_count) return false;
    bookmark_t removed = reader->bookmarks[index];
    bool was_dirty = reader->dirty;
    memmove(&reader->bookmarks[index], &reader->bookmarks[index + 1], (reader->bookmark_count - index - 1) * sizeof(bookmark_t));
    --reader->bookmark_count;
    memset(&reader->bookmarks[reader->bookmark_count], 0, sizeof(bookmark_t));
    reader->dirty = true;
    if (folio_reader_save(reader)) return true;
    memmove(&reader->bookmarks[index + 1], &reader->bookmarks[index], (reader->bookmark_count - index) * sizeof(bookmark_t));
    reader->bookmarks[index] = removed; ++reader->bookmark_count; reader->dirty = was_dirty;
    return false;
}
bool folio_reader_toggle_bookmark(folio_reader_t* reader) {
    if (reader == NULL || !reader->ready) return false;
    reader->error[0] = '\0';
    size_t existing = bookmark_at_page(reader);
    if (existing != SIZE_MAX) return folio_reader_delete_bookmark(reader, existing);
    if (reader->bookmark_count >= BOOKMARK_LIMIT) { set_error(reader, "每本书最多保存 32 个书签"); return false; }
    bool was_dirty = reader->dirty;
    bookmark_t* bookmark = &reader->bookmarks[reader->bookmark_count++];
    memset(bookmark, 0, sizeof(*bookmark));
    bookmark->offset = reader->page->start;
    size_t chapter = folio_reader_current_chapter(reader);
    char title[192];
    unsigned percent = (unsigned)((uint64_t)(bookmark->offset - reader->content_start) * 100 /
                                   (reader->file_size - reader->content_start));
    if (chapter != SIZE_MAX) snprintf(title, sizeof(title), "%u%% · %s", percent, folio_reader_chapter_title(reader, chapter));
    else {
        size_t length = (size_t)snprintf(title, sizeof(title), "%u%% · ", percent);
        for (unsigned i = 0; i < reader->page->placement_count && i < 20 && length + 4 < sizeof(title); ++i) {
            uint32_t cp = reader->page->placements[i].codepoint;
            if (cp < 0x80) title[length++] = (char)cp;
            else if (cp < 0x800) {
                title[length++] = (char)(0xC0 | (cp >> 6));
                title[length++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                title[length++] = (char)(0xE0 | (cp >> 12));
                title[length++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                title[length++] = (char)(0x80 | (cp & 0x3F));
            } else {
                title[length++] = (char)(0xF0 | (cp >> 18));
                title[length++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                title[length++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                title[length++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        title[length] = '\0';
    }
    copy_utf8(bookmark->title, sizeof(bookmark->title), title, strlen(title));
    reader->dirty = true;
    if (folio_reader_save(reader)) return true;
    memset(bookmark, 0, sizeof(*bookmark)); --reader->bookmark_count; reader->dirty = was_dirty;
    return false;
}
bool folio_reader_jump_bookmark(folio_reader_t* reader, size_t index) {
    return reader != NULL && index < reader->bookmark_count && folio_reader_seek(reader, reader->bookmarks[index].offset);
}
const char* folio_reader_title(const folio_reader_t* reader) { return reader != NULL ? reader->title : ""; }
const char* folio_reader_encoding(const folio_reader_t* reader) {
    if (reader == NULL) return "";
    if (reader->is_weread) return "微信读书";
    if (reader->is_pdf) return "PDF";
    if (reader->is_epub) return "EPUB";
    return reader->encoding == FOLIO_ENCODING_GBK ? "GBK" : "UTF-8";
}
uint32_t folio_reader_page_number(const folio_reader_t* reader) { return reader != NULL && reader->ready ? reader->page->number + 1 : 0; }
unsigned folio_reader_percent(const folio_reader_t* reader) {
    if (reader == NULL || !reader->ready || reader->file_size <= reader->content_start) return 0;
    if (reader->page->end == reader->file_size) return 100;
    return (unsigned)((uint64_t)(reader->page->start - reader->content_start) * 100 / (reader->file_size - reader->content_start));
}
uint32_t folio_reader_position(const folio_reader_t* reader) { return reader != NULL && reader->ready ? reader->page->start : 0; }
const char* folio_reader_last_error(void) { return s_last_error; }
const char* folio_reader_error(const folio_reader_t* reader) { return reader != NULL ? reader->error : s_last_error; }
