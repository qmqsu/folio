#include "folio_catalog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "folio_epub.h"
#include "folio_pdf.h"
#include "folio_weread_paths.h"
#include "folio_weread_cache.h"
#include "folio_text.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
#define CATALOG_MAGIC UINT32_C(0x31434c46)
#define CATALOG_VERSION 3u
#define CATALOG_HEADER_SIZE 24u
#define CATALOG_RECORD_SIZE 560u
#define CATALOG_KEY_CAP (FOLIO_BOOK_PATH_CAP * 2 + 1)
#define HASH_START UINT32_C(2166136261)

typedef struct {
    char path[FOLIO_BOOK_PATH_CAP];
    folio_catalog_metadata_t metadata;
    int64_t mtime;
    uint32_t source_size;
    bool active;
} catalog_entry_t;
struct folio_catalog {
    catalog_entry_t entries[FOLIO_CATALOG_LIMIT];
    size_t count;
    uint64_t order;
    uint32_t sequence;
    int active_slot;
    bool dirty;
    char error[160];
};

#include "folio_pinyin_table.inc"

static void* catalog_alloc(size_t size) {
    void* memory = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : calloc(1, size);
}
static void catalog_error(folio_catalog_t* catalog, const char* message) {
    if (catalog) snprintf(catalog->error, sizeof(catalog->error), "%s", message);
}
static uint32_t hash_bytes(uint32_t hash, const void* bytes, size_t size) {
    const uint8_t* p = bytes;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    return hash;
}
static uint32_t read32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const uint8_t* p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }
static void write32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static void write64(uint8_t* p, uint64_t value) {
    write32(p, (uint32_t)value); write32(p + 4, (uint32_t)(value >> 32));
}
static bool valid_path(const char* path) {
    const char prefix[] = FOLIO_SD_ROOT "/books/";
    if (!path || strnlen(path, FOLIO_BOOK_PATH_CAP) >= FOLIO_BOOK_PATH_CAP ||
        strncmp(path, prefix, sizeof(prefix) - 1)) return false;
    const char* name = path + sizeof(prefix) - 1;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
    return true;
}
static void copy_text(char* target, size_t capacity, const char* source) {
    size_t length = strlen(source), count = length < capacity - 1 ? length : capacity - 1;
    if (count < length) while (count && ((unsigned char)source[count] & 0xc0) == 0x80) --count;
    memset(target, 0, capacity); memcpy(target, source, count);
}
static catalog_entry_t* find_entry(folio_catalog_t* catalog, const char* path) {
    if (!catalog || !path) return NULL;
    for (size_t i = 0; i < catalog->count; ++i)
        if (!strcmp(catalog->entries[i].path, path)) return &catalog->entries[i];
    return NULL;
}
static const catalog_entry_t* const_entry(const folio_catalog_t* catalog, const char* path) {
    if (!catalog || !path) return NULL;
    for (size_t i = 0; i < catalog->count; ++i)
        if (!strcmp(catalog->entries[i].path, path)) return &catalog->entries[i];
    return NULL;
}
static const char* catalog_slot(unsigned slot) {
    return slot ? FOLIO_SD_ROOT "/.folio/catalog.b" : FOLIO_SD_ROOT "/.folio/catalog.a";
}
static void encode_entry(const catalog_entry_t* entry, uint8_t* bytes) {
    const folio_catalog_metadata_t* m = &entry->metadata;
    memset(bytes, 0, CATALOG_RECORD_SIZE);
    memcpy(bytes, entry->path, sizeof(entry->path));
    memcpy(bytes + 256, m->title, sizeof(m->title));
    memcpy(bytes + 384, m->author, sizeof(m->author));
    write32(bytes + 512, m->added_utc); write32(bytes + 516, m->lastread_utc);
    write64(bytes + 520, m->added_order); write64(bytes + 528, m->lastread_order);
    write64(bytes + 536, (uint64_t)entry->mtime); write32(bytes + 544, entry->source_size);
    write32(bytes + 548, m->read ? 1 : 0);
    write64(bytes + 552,m->reading_seconds);
}
static bool decode_entry(catalog_entry_t* entry, const uint8_t* bytes, uint64_t order) {
    if (!memchr(bytes, 0, 256) || !memchr(bytes + 256, 0, 128) || !memchr(bytes + 384, 0, 128)) return false;
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->path, bytes, 256);
    folio_catalog_metadata_t* m = &entry->metadata;
    memcpy(m->title, bytes + 256, 128); memcpy(m->author, bytes + 384, 128);
    m->added_utc = read32(bytes + 512); m->lastread_utc = read32(bytes + 516);
    m->added_order = read64(bytes + 520); m->lastread_order = read64(bytes + 528);
    entry->mtime = (int64_t)read64(bytes + 536); entry->source_size = read32(bytes + 544);
    m->reading_seconds=read64(bytes+552);
    uint32_t flags = read32(bytes + 548); m->read = flags == 1;
    return valid_path(entry->path) && *m->title && flags <= 1 && m->added_order &&
        m->added_order <= order && m->lastread_order <= order &&
        (m->read ? m->lastread_order != 0 : m->lastread_order == 0 && m->lastread_utc == 0);
}
typedef enum { CATALOG_MISSING, CATALOG_VALID, CATALOG_DAMAGED, CATALOG_IO } catalog_slot_status_t;
static catalog_slot_status_t read_catalog(unsigned slot, folio_catalog_t* catalog) {
    FILE* file = fopen(catalog_slot(slot), "rb");
    if (!file) return errno == ENOENT ? CATALOG_MISSING : CATALOG_IO;
    uint8_t bytes[CATALOG_RECORD_SIZE]; uint32_t hash = HASH_START;
    size_t record_size=CATALOG_RECORD_SIZE;
    bool okay = fread(bytes, 1, CATALOG_HEADER_SIZE, file) == CATALOG_HEADER_SIZE;
    uint32_t version = 0;
    if (okay) {
        hash = hash_bytes(hash, bytes, CATALOG_HEADER_SIZE);
        catalog->sequence = read32(bytes + 8); catalog->count = read32(bytes + 12);
        catalog->order = read64(bytes + 16);
        version=read32(bytes+4);
        if(version==1)record_size=552;
        okay = read32(bytes) == CATALOG_MAGIC && (version>=1 && version<=CATALOG_VERSION) &&
            catalog->count <= FOLIO_CATALOG_LIMIT;
    }
    for (size_t i = 0; okay && i < catalog->count; ++i) {
        memset(bytes,0,sizeof(bytes));
        okay = fread(bytes, 1, record_size, file) == record_size;
        if (!okay) break;
        hash = hash_bytes(hash, bytes, record_size);
        okay = decode_entry(&catalog->entries[i], bytes, catalog->order);
        if (version < 3 && !catalog->entries[i].metadata.author[0]) catalog->entries[i].mtime = -1;
        for (size_t j = 0; okay && j < i; ++j)
            if (!strcmp(catalog->entries[i].path, catalog->entries[j].path)) okay = false;
    }
    if (okay) okay = fread(bytes, 1, 4, file) == 4 && read32(bytes) == hash && fgetc(file) == EOF;
    bool io_error = ferror(file) != 0;
    if (fclose(file) != 0) io_error = true;
    return io_error ? CATALOG_IO : okay ? CATALOG_VALID : CATALOG_DAMAGED;
}
folio_catalog_t* folio_catalog_open(void) {
    folio_catalog_t* catalog = catalog_alloc(sizeof(*catalog));
    folio_catalog_t* candidate = catalog_alloc(sizeof(*candidate));
    if (!catalog || !candidate) { free(catalog); free(candidate); return NULL; }
    catalog->active_slot = -1;
    catalog_slot_status_t status[2];
    for (unsigned slot = 0; slot < 2; ++slot) {
        memset(candidate, 0, sizeof(*candidate));
        status[slot] = read_catalog(slot, candidate);
        if (status[slot] != CATALOG_VALID) continue;
        if (catalog->active_slot < 0 || (int32_t)(candidate->sequence - catalog->sequence) > 0) {
            *catalog = *candidate; catalog->active_slot = (int)slot;
        }
    }
    free(candidate);
    if (status[0] == CATALOG_IO || status[1] == CATALOG_IO)
        catalog_error(catalog, "书库索引未能完整读取，请检查 TF 卡。可用记录已保留。");
    else if (status[0] == CATALOG_DAMAGED || status[1] == CATALOG_DAMAGED)
        catalog_error(catalog, catalog->active_slot < 0 ? "书库索引损坏，将重新收录当前书籍。" : "书库索引已从另一份有效记录恢复。");
    return catalog;
}
void folio_catalog_free(folio_catalog_t* catalog) { free(catalog); }
bool folio_catalog_dirty(const folio_catalog_t* catalog) { return catalog && catalog->dirty; }
const char* folio_catalog_error(const folio_catalog_t* catalog) { return catalog ? catalog->error : "书库索引内存不足。"; }
static bool ensure_catalog_directory(void) {
    const char* path = FOLIO_SD_ROOT "/.folio";
    if (mkdir(path, 0777) == 0) return true;
    struct stat info; return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
bool folio_catalog_save(folio_catalog_t* catalog) {
    if (!catalog) return false;
    if (!catalog->dirty) return true;
    if (!ensure_catalog_directory()) { catalog_error(catalog, "书库索引目录不可写，请检查 TF 卡。"); return false; }
    unsigned target = catalog->active_slot == 0 ? 1 : 0;
    FILE* file = fopen(catalog_slot(target), "wb");
    if (!file) { catalog_error(catalog, "书库索引未保存，请检查 TF 卡后重试。"); return false; }
    uint8_t bytes[CATALOG_RECORD_SIZE]; uint32_t hash = HASH_START;
    write32(bytes, CATALOG_MAGIC); write32(bytes + 4, CATALOG_VERSION);
    write32(bytes + 8, catalog->sequence + 1); write32(bytes + 12, (uint32_t)catalog->count);
    write64(bytes + 16, catalog->order);
    hash = hash_bytes(hash, bytes, CATALOG_HEADER_SIZE);
    bool okay = fwrite(bytes, 1, CATALOG_HEADER_SIZE, file) == CATALOG_HEADER_SIZE;
    for (size_t i = 0; okay && i < catalog->count; ++i) {
        encode_entry(&catalog->entries[i], bytes);
        hash = hash_bytes(hash, bytes, CATALOG_RECORD_SIZE);
        okay = fwrite(bytes, 1, CATALOG_RECORD_SIZE, file) == CATALOG_RECORD_SIZE;
    }
    write32(bytes, hash);
    if (okay) okay = fwrite(bytes, 1, 4, file) == 4;
    if (okay) okay = fflush(file) == 0;
    if (okay) okay = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) okay = false;
    if (!okay) { catalog_error(catalog, "书库索引未保存，请检查 TF 卡后重试。"); return false; }
    catalog->active_slot = (int)target; ++catalog->sequence; catalog->dirty = false; catalog->error[0] = 0;
    return true;
}
static bool valid_books(const folio_book_t* books, size_t count) {
    if (count > FOLIO_LIBRARY_LIMIT || (count && !books)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!valid_path(books[i].path) || !memchr(books[i].title, 0, sizeof(books[i].title)) || !*books[i].title) return false;
        for (size_t j = 0; j < i; ++j) if (!strcmp(books[i].path, books[j].path)) return false;
    }
    return true;
}
// Only explicit bylines count as authors in plain text.
static void text_byline(const char* path, char* author, size_t cap) {
    FILE* file = fopen(path, "rb");
    if (!file) return;
    char line[512];
    size_t used = 0;
    while (used < 4096 && fgets(line, sizeof(line), file)) {
        used += strlen(line);
        char* p = line;
        if (!strncmp(p, "\xef\xbb\xbf", 3)) p += 3;
        p += strspn(p, " \t");
        const char* labels[] = {"作者：", "作者:", "Author:"};
        for (size_t i = 0; i < sizeof(labels)/sizeof(*labels); ++i) {
            if (strncasecmp(p, labels[i], strlen(labels[i]))) continue;
            p += strlen(labels[i]); p += strspn(p, " \t");
            p[strcspn(p, "\r\n")] = 0;
            copy_text(author, cap, p);
            break;
        }
        if (*author) break;
    }
    fclose(file);
}

bool folio_catalog_sync(folio_catalog_t* catalog, const folio_book_t* books,
                        size_t count, uint32_t utc, bool refresh_metadata) {
    if (!catalog || !valid_books(books, count) || catalog->order > UINT64_MAX - count) {
        catalog_error(catalog, "书库索引参数无效。"); return false;
    }
    for (size_t i = 0; i < catalog->count; ++i) catalog->entries[i].active = false;
    for (size_t i = 0; i < count; ++i) {
        catalog_entry_t* entry = find_entry(catalog, books[i].path);
        if (entry) entry->active = true;
    }
    bool okay = true;
    for (size_t i = 0; i < count; ++i) {
        const folio_book_t* book = &books[i]; struct stat info;
        if (stat(book->path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > UINT32_MAX) {
            catalog_error(catalog, "部分书籍未能读取，书库索引保留可用记录。"); okay = false; continue;
        }
        catalog_entry_t* entry = find_entry(catalog, book->path);
        bool fresh = !entry;
        if (!entry) {
            if (catalog->count < FOLIO_CATALOG_LIMIT) entry = &catalog->entries[catalog->count++];
            else {
                for (size_t j = 0; j < catalog->count; ++j) {
                    catalog_entry_t* candidate = &catalog->entries[j];
                    if (!candidate->active && (!entry || candidate->metadata.added_order < entry->metadata.added_order)) entry = candidate;
                }
            }
            if (!entry) { catalog_error(catalog, "书库索引已满。"); okay = false; continue; }
            memset(entry, 0, sizeof(*entry)); copy_text(entry->path, sizeof(entry->path), book->path);
            entry->metadata.added_utc = utc; entry->metadata.added_order = ++catalog->order;
            catalog->dirty = true;
        }
        entry->active = true;
        bool changed = fresh || entry->source_size != (uint32_t)info.st_size || entry->mtime != (int64_t)info.st_mtime;
        if (changed || refresh_metadata) {
            char title[FOLIO_BOOK_TITLE_CAP] = {0}, author[FOLIO_CATALOG_AUTHOR_CAP] = {0}, error[160] = {0};
            const char* extension = strrchr(book->path, '.');
            char id[64];
            bool native = folio_wr_path_id(book->path, id, sizeof(id)) &&
                folio_weread_cache_metadata(book->path,title,sizeof(title),author,sizeof(author),error,sizeof(error));

            if (!native && extension && !strcasecmp(extension, ".epub") &&
                !folio_epub_metadata(book->path, title, sizeof(title), author, sizeof(author), error, sizeof(error))) {
                catalog_error(catalog, "部分 EPUB 元数据未能读取，暂时使用文件名。"); okay = false;
            } else if(!native && extension && !strcasecmp(extension,".wrb") &&
                !folio_weread_cache_metadata(book->path,title,sizeof(title),author,sizeof(author),error,sizeof(error))) {
                catalog_error(catalog,"部分微信读书缓存信息未能读取，暂时使用文件名。");okay=false;
            }
            if (!native && extension && !strcasecmp(extension,".pdf"))
                folio_pdf_metadata(book->path,title,sizeof(title),author,sizeof(author));
            if (extension && !strcasecmp(extension,".txt")) text_byline(book->path,author,sizeof(author));
            if (!*title) copy_text(title, sizeof(title), book->title);
            if (changed || strcmp(entry->metadata.title, title) || strcmp(entry->metadata.author, author)) catalog->dirty = true;
            copy_text(entry->metadata.title, sizeof(entry->metadata.title), title);
            copy_text(entry->metadata.author, sizeof(entry->metadata.author), author);
            entry->source_size = (uint32_t)info.st_size; entry->mtime = (int64_t)info.st_mtime;
        }
        if ((i & 7u) == 7u) vTaskDelay(1);
    }
    return okay;
}
bool folio_catalog_touch(folio_catalog_t* catalog, const char* path, uint32_t utc) {
    if (!catalog || !valid_path(path) || catalog->order == UINT64_MAX) return false;
    catalog_entry_t* entry = find_entry(catalog, path);
    if (!entry) { catalog_error(catalog, "当前书籍尚未收录到书库索引。"); return false; }
    entry->metadata.read = true; entry->metadata.lastread_utc = utc;
    entry->metadata.lastread_order = ++catalog->order; catalog->dirty = true;
    return true;
}
const folio_catalog_metadata_t* folio_catalog_metadata(const folio_catalog_t* catalog, const char* path) {
    const catalog_entry_t* entry = const_entry(catalog, path);
    return entry ? &entry->metadata : NULL;
}

static const char* pinyin(uint32_t cp) {
    size_t left = 0, right = sizeof(catalog_pinyin_map) / sizeof(catalog_pinyin_map[0]);
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        uint32_t word = catalog_pinyin_map[middle], candidate = word >> 9;
        if (candidate < cp) left = middle + 1;
        else if (candidate > cp) right = middle;
        else return catalog_pinyin_syllables[word & 511u];
    }
    return NULL;
}
static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c; }
static bool raw_contains(const char* text, const char* query) {
    for (const char* start = text; *start; ++start) {
        const char* a = start; const char* b = query;
        while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return !*query;
}
static void make_key(const char* text, char* key, size_t capacity, bool initials) {
    size_t used = 0; const char* cursor = text;
    while (*cursor && used + 1 < capacity) {
        const char* start = cursor; uint32_t cp = folio_utf8_next(&cursor);
        const char* syllable = pinyin(cp);
        if (syllable) {
            size_t length = initials ? 1 : strlen(syllable);
            if (length >= capacity - used) break;
            memcpy(key + used, syllable, length); used += length;
        } else if (cp == 0xfc || cp == 0xdc) key[used++] = 'v';
        else if (cp < 128) {
            char c = ascii_lower((char)cp);
            if (c == 'u' && *cursor == ':') { c = 'v'; ++cursor; }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '-' && c != '\'') key[used++] = c;
        } else {
            size_t length = (size_t)(cursor - start);
            if (length >= capacity - used) break;
            memcpy(key + used, start, length); used += length;
        }
    }
    key[used] = 0;
}
static bool text_matches(const char* text, const char* query, const char* query_key) {
    if (raw_contains(text, query)) return true;
    if (!*query_key) return false;
    char key[CATALOG_KEY_CAP]; make_key(text, key, sizeof(key), false);
    if (strstr(key, query_key)) return true;
    make_key(text, key, sizeof(key), true);
    return strstr(key, query_key) != NULL;
}
static int compare_text(const char* a, const char* b) {
    char left[CATALOG_KEY_CAP], right[CATALOG_KEY_CAP];
    make_key(a, left, sizeof(left), false); make_key(b, right, sizeof(right), false);
    int value = strcmp(left, right);
    return value ? value : strcmp(a, b);
}
static const char* entry_title(const catalog_entry_t* entry, const folio_book_t* book) {
    return entry && *entry->metadata.title ? entry->metadata.title : book->title;
}
static int compare_books(const folio_book_t* books, const catalog_entry_t* const* entries,
                          size_t a, size_t b, folio_catalog_sort_t sort) {
    const catalog_entry_t* left = entries[a]; const catalog_entry_t* right = entries[b];
    if (sort == FOLIO_CATALOG_SORT_RECENT || sort == FOLIO_CATALOG_SORT_ADDED) {
        uint64_t x = left ? (sort == FOLIO_CATALOG_SORT_RECENT ? left->metadata.lastread_order : left->metadata.added_order) : 0;
        uint64_t y = right ? (sort == FOLIO_CATALOG_SORT_RECENT ? right->metadata.lastread_order : right->metadata.added_order) : 0;
        if (x != y) return x > y ? -1 : 1;
    } else if (sort == FOLIO_CATALOG_SORT_AUTHOR) {
        const char* x = left ? left->metadata.author : "";
        const char* y = right ? right->metadata.author : "";
        if (!*x != !*y) return *x ? -1 : 1;
        int compared = compare_text(x, y); if (compared) return compared;
    }
    int compared = compare_text(entry_title(left, &books[a]), entry_title(right, &books[b]));
    return compared ? compared : strcmp(books[a].path, books[b].path);
}
size_t folio_catalog_select(const folio_catalog_t* catalog, const folio_book_t* books,
                            size_t count, const folio_catalog_options_t* options,
                            size_t* indexes, size_t capacity) {
    if (!indexes || !capacity || !valid_books(books, count)) return 0;
    folio_catalog_options_t defaults = {FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_TITLE, NULL};
    if (!options) options = &defaults;
    if (options->filter > FOLIO_CATALOG_UNREAD || options->filter < FOLIO_CATALOG_ALL ||
        options->sort > FOLIO_CATALOG_SORT_ADDED || options->sort < FOLIO_CATALOG_SORT_RECENT) return 0;
    const char* query = options->search ? options->search : "";
    if (strnlen(query, FOLIO_CATALOG_QUERY_CAP) >= FOLIO_CATALOG_QUERY_CAP) return 0;
    char query_key[FOLIO_CATALOG_QUERY_CAP * 2 + 1]; make_key(query, query_key, sizeof(query_key), false);
    const catalog_entry_t* entries[FOLIO_LIBRARY_LIMIT];
    uint16_t selected[FOLIO_LIBRARY_LIMIT]; size_t matches = 0;
    for (size_t i = 0; i < count; ++i) {
        entries[i] = const_entry(catalog, books[i].path);
        bool read = entries[i] && entries[i]->metadata.read;
        if ((options->filter == FOLIO_CATALOG_RECENT && !read) || (options->filter == FOLIO_CATALOG_UNREAD && read)) continue;
        if (*query) {
            const char* author = entries[i] ? entries[i]->metadata.author : "";
            const char* filename = strrchr(books[i].path, '/'); filename = filename ? filename + 1 : books[i].path;
            if (!text_matches(entry_title(entries[i], &books[i]), query, query_key) &&
                !text_matches(author, query, query_key) && !text_matches(filename, query, query_key)) continue;
        }
        size_t position = matches;
        while (position && compare_books(books, entries, i, selected[position - 1], options->sort) < 0) {
            selected[position] = selected[position - 1]; --position;
        }
        selected[position] = (uint16_t)i; ++matches;
        if ((i & 15u) == 15u) vTaskDelay(1);
    }
    size_t written = matches < capacity ? matches : capacity;
    for (size_t i = 0; i < written; ++i) indexes[i] = selected[i];
    return written;
}

bool folio_catalog_add_reading(folio_catalog_t* catalog,const char* path,uint64_t seconds) {
    catalog_entry_t* entry=find_entry(catalog,path);if(!entry)return false;
    if(seconds){uint64_t old=entry->metadata.reading_seconds;
        entry->metadata.reading_seconds=seconds>UINT64_MAX-old?UINT64_MAX:old+seconds;catalog->dirty=true;}
    return true;
}

bool folio_catalog_relocate(folio_catalog_t* c,const char* old_path,const char* new_path){
    if(!c||!valid_path(old_path)||!valid_path(new_path))return false;
    for(size_t i=0;i<c->count;++i)if(!strcmp(c->entries[i].path,new_path))return true;
    for(size_t i=0;i<c->count;++i)if(!strcmp(c->entries[i].path,old_path)){
        copy_text(c->entries[i].path,sizeof(c->entries[i].path),new_path);c->dirty=true;return true;}
    return true;
}
