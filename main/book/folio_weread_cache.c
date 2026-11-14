#include "folio_weread_cache.h"
#include "folio_weread_paths.h"
#include "../folio_storage.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// ESP FAT VFS has no symlinks or lstat; parent validation still walks each
// directory. Host tests retain lstat to reject symlinks explicitly.
#define cache_lstat stat
#else
#define cache_lstat lstat
#endif

#define CACHE_ROOT FOLIO_SD_ROOT "/.folio/weread"
#define BOOK_ROOT FOLIO_SD_ROOT "/books"
#define FORMAT 1u
#define MANIFEST_MAGIC 0x314D5257u
#define CHAPTER_MAGIC 0x31435257u
#define POINTER_MAGIC 0x31505257u
#define RESUME_MAGIC 0x31525257u
#define HASH_INIT UINT64_C(14695981039346656037)
#define PATH_CAP 256
#define WAIT_MS 500u
#define LOCK() do { if (!folio_storage_lock(WAIT_MS)) return fail(error, cap, "TF 卡正忙，请稍后重试。"); } while (0)

typedef struct {
    folio_weread_chapter_t chapter;
    uint64_t key, source_hash, text_hash;
    uint32_t source_size, text_size, kind, reserved;
} chapter_record_t;
typedef struct {
    uint32_t magic, version, sequence, count;
    uint64_t body_hash;
    uint32_t body_size, reserved;
    char book_id[64], title[128], author[128];
    chapter_record_t records[FOLIO_WEREAD_CHAPTER_LIMIT];
    uint64_t checksum;
} manifest_t;
typedef struct {
    uint32_t magic, version, sequence, reserved;
    char book_id[64];
    uint64_t manifest_hash, checksum;
} pointer_t;
/* Resume checkpoints were full manifests: 119 KB rewritten and fsynced for
 * every chapter, plus two of them re-read. The compact form carries the same
 * fields for only the chapters that exist, so a 42-chapter book writes 10 KB
 * per checkpoint instead of 119 KB. Slot files in the old size are still read. */
typedef struct {
    uint32_t magic, version, sequence, count;
    uint64_t body_hash;
    uint32_t body_size, reserved;
    char book_id[64], title[128], author[128];
    uint64_t checksum;
} resume_header_t;
typedef struct {
    uint32_t magic, version, source_size, text_size;
    uint64_t source_hash, text_hash;
    uint32_t kind, reserved;
    uint64_t checksum;
} chapter_header_t;
_Static_assert(sizeof(chapter_record_t) == 232, "WeRead chapter record layout");
_Static_assert(sizeof(manifest_t) == 119144, "WeRead manifest layout");
_Static_assert(sizeof(pointer_t) == 96, "WeRead publication pointer layout");
_Static_assert(sizeof(chapter_header_t) == 48, "WeRead chapter header layout");
/* used_bytes tracks the book directory size so a chapter does not have to
 * opendir()/stat() every sibling file (twice) before every write. */
struct folio_weread_cache_writer { manifest_t manifest; int resume_slot; uint64_t used_bytes; };
static bool writer_active;
extern bool folio_io_cancelled(void) __attribute__((weak));
extern bool folio_storage_cancelled(void) __attribute__((weak));

static bool cancelled(void) {
    return (folio_io_cancelled && folio_io_cancelled()) ||
           (folio_storage_cancelled && folio_storage_cancelled());
}
#ifdef ESP_PLATFORM
/* One tick is 10 ms, so yielding per block would dominate the write time.
 * Yield only when the last yield is old enough to matter for the UI. */
static void cooperate(void) {
    static int64_t next;
    int64_t now = esp_timer_get_time();
    if (now < next) return;
    next = now + 4000;
    vTaskDelay(1);
}
#else
static void cooperate(void) {}
#endif
static void* allocate(size_t n) {
#ifdef ESP_PLATFORM
    void* p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return calloc(1, n);
}
static bool fail(char* error, size_t cap, const char* message) {
    if (error && cap) snprintf(error, cap, "%s", message);
    return false;
}
static void clear_error(char* error, size_t cap) { if (error && cap) *error = 0; }
static uint64_t hash_bytes(uint64_t hash, const void* bytes, size_t n) {
    const uint8_t* p = bytes;
    for (size_t i = 0; i < n; ++i) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static uint64_t hash(const void* bytes, size_t n) { return hash_bytes(HASH_INIT, bytes, n); }
static bool id_valid(const char* id) {
    if (!id || !*id || strnlen(id, 64) >= 64) return false;
    for (; *id; ++id) if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') ||
        (*id >= '0' && *id <= '9') || *id == '_' || *id == '-')) return false;
    return true;
}
/* Strict Unicode scalars. No replacement characters are invented for invalid input. */
static bool scalar(const uint8_t* p, size_t n, uint32_t* cp, size_t* bytes) {
    if (!n) return false;
    uint32_t value = p[0], minimum = 0; size_t size = 1;
    if (value >= 0xC2 && value <= 0xDF) { value &= 31; minimum = 0x80; size = 2; }
    else if (value >= 0xE0 && value <= 0xEF) { value &= 15; minimum = 0x800; size = 3; }
    else if (value >= 0xF0 && value <= 0xF4) { value &= 7; minimum = 0x10000; size = 4; }
    else if (value >= 0x80) return false;
    if (n < size) return false;
    for (size_t i = 1; i < size; ++i) {
        if ((p[i] & 0xC0) != 0x80) return false;
        value = (value << 6) | (p[i] & 63);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) || value == 0) return false;
    *cp = value; *bytes = size; return true;
}
static bool text_valid(const char* text, size_t cap, bool empty) {
    if (!text) return false;
    size_t n = strnlen(text, cap);
    if (n == cap || (!empty && !n)) return false;
    for (size_t i = 0; i < n;) {
        uint32_t cp; size_t size;
        if (!scalar((const uint8_t*)text + i, n - i, &cp, &size) || cp < 32 || cp == 127) return false;
        i += size;
    }
    return true;
}
static void copy_text(char* to, size_t cap, const char* from) {
    if (!to || !cap) return;
    size_t n = strlen(from); if (n >= cap) n = cap - 1;
    while (n && ((uint8_t)from[n] & 0xC0) == 0x80) --n;
    memcpy(to, from, n); to[n] = 0;
}
static bool directory(const char* path) {
    struct stat st;
    if (cache_lstat(path, &st) == 0) return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    return errno == ENOENT && mkdir(path, 0777) == 0;
}
static bool root_ready(void) {
    return directory(FOLIO_SD_ROOT) && directory(FOLIO_SD_ROOT "/.folio") &&
           directory(CACHE_ROOT) && directory(BOOK_ROOT);
}
static bool id_hex(const char* id, char hex[127]) {
    static const char digits[] = "0123456789abcdef";
    if (!id_valid(id)) return false;
    size_t i = 0;
    for (; id[i]; ++i) { hex[2 * i] = digits[(uint8_t)id[i] >> 4]; hex[2 * i + 1] = digits[(uint8_t)id[i] & 15]; }
    hex[2 * i] = 0;
    return true;
}
static bool book_dir(char out[PATH_CAP], const char* id) {
    char hex[127]; if (!id_hex(id, hex)) return false;
    int n = snprintf(out, PATH_CAP, CACHE_ROOT "/%s", hex);
    return n > 0 && n < PATH_CAP;
}
static bool book_path(char out[PATH_CAP], const char* id) {
    char hex[127]; if (!id_hex(id, hex)) return false;
    int n = snprintf(out, PATH_CAP, BOOK_ROOT "/wr-%s.wrb", hex);
    return n > 0 && n < PATH_CAP;
}
static bool named_path(char out[PATH_CAP], const char* id, const char* name) {
    char dir[PATH_CAP]; if (!book_dir(dir, id)) return false;
    int n = snprintf(out, PATH_CAP, "%s/%s", dir, name);
    return n > 0 && n < PATH_CAP;
}
static bool object_path(char out[PATH_CAP], const char* id, char type, uint64_t key) {
    const char* ext = type == 'c' ? "wrc" : type == 'm' ? "idx" : "txt";
    char name[32]; int n = snprintf(name, sizeof(name), "%c-%016llx.%s", type, (unsigned long long)key, ext);
    return key && n > 0 && n < (int)sizeof(name) && named_path(out, id, name);
}
static bool ordinary_file(const struct stat* st) {
    if (!S_ISREG(st->st_mode)) return false;
#ifndef ESP_PLATFORM
    if (st->st_nlink != 1) return false;
#endif
    return true;
}
static bool writable_path(const char* path) {
    if (!folio_storage_validate_parent(path, NULL, 0)) return false;
    struct stat st;
    if (cache_lstat(path, &st) == 0) return ordinary_file(&st);
    return errno == ENOENT;
}
static bool safe_file(const char* path, uint64_t expected) {
    struct stat st;
    return folio_storage_validate_parent(path, NULL, 0) && cache_lstat(path, &st) == 0 &&
           ordinary_file(&st) && st.st_size >= 0 && (uint64_t)st.st_size == expected;
}
static bool close_write(FILE* file, bool okay) {
    if (!file) return false;
    if (okay) okay = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file)) okay = false;
    return okay;
}
static bool read_exact(const char* path, void* data, size_t n) {
    if (!safe_file(path, n)) return false;
    FILE* f = fopen(path, "rb"); if (!f) return false;
    bool okay = fread(data, 1, n, f) == n && !ferror(f);
    if (fclose(f)) okay = false;
    return okay;
}
static bool stream_hash(FILE* f, uint32_t length, uint64_t* out, FILE* copy) {
    /* Keep full-file verification, but amortize SD/stdio locking on large
     * books. A bounded PSRAM buffer does not grow the worker's 16 KiB stack. */
    uint8_t fallback[512];size_t capacity=length<16384u?length:16384u;
    uint8_t* allocated=capacity?allocate(capacity):NULL;
    uint8_t* bytes=allocated?allocated:fallback;
    if(!allocated)capacity=sizeof(fallback);
    uint64_t value=HASH_INIT;bool okay=true;
    while (length && okay) {
        if (cancelled()) {okay=false;break;}
        size_t n = length < capacity ? length : capacity;
        if (fread(bytes, 1, n, f) != n || (copy && fwrite(bytes, 1, n, copy) != n)) {okay=false;break;}
        value = hash_bytes(value, bytes, n); length -= (uint32_t)n; cooperate();
    }
    okay=okay&&!ferror(f);free(allocated);
    if(okay)*out=value;
    return okay;
}
static bool memory_transfer(FILE* f, uint8_t* bytes, size_t length, bool writing) {
    while (length) {
        if (cancelled()) return false;
        size_t n = length < 4096 ? length : 4096;
        if ((writing ? fwrite(bytes, 1, n, f) : fread(bytes, 1, n, f)) != n) return false;
        bytes += n; length -= n; cooperate();
    }
    return !ferror(f);
}
static bool file_hash(const char* path, uint32_t length, uint64_t expected) {
    if (!safe_file(path, length)) return false;
    FILE* f = fopen(path, "rb"); if (!f) return false;
    uint64_t h; bool okay = stream_hash(f, length, &h, NULL) && h == expected;
    if (fclose(f)) okay = false;
    return okay;
}
static bool manifest_valid(const manifest_t* m, const char* id) {
    if (m->magic != MANIFEST_MAGIC || m->version != FORMAT || m->reserved || !m->sequence ||
        m->count == 0 || m->count > FOLIO_WEREAD_CHAPTER_LIMIT ||
        !memchr(m->book_id, 0, sizeof(m->book_id)) || !id_valid(m->book_id) || strcmp(m->book_id, id) ||
        !text_valid(m->title, sizeof(m->title), false) || !text_valid(m->author, sizeof(m->author), true) ||
        m->body_size > FOLIO_WEREAD_BOOK_BYTES || hash(m, offsetof(manifest_t, checksum)) != m->checksum) return false;
    uint64_t total = 0;
    for (size_t i = 0; i < m->count; ++i) {
        const chapter_record_t* r = &m->records[i];
        if (!memchr(r->chapter.uid, 0, 64) || !id_valid(r->chapter.uid) ||
            !text_valid(r->chapter.title, sizeof(r->chapter.title), false) || r->reserved || r->kind > 1 ||
            r->source_size > FOLIO_WEREAD_CHAPTER_BYTES || r->text_size > FOLIO_WEREAD_CHAPTER_BYTES) return false;
        for (size_t j = 0; j < i; ++j) if (!strcmp(r->chapter.uid, m->records[j].chapter.uid)) return false;
        if (!r->key && (r->source_hash || r->text_hash || r->source_size || r->text_size || r->kind)) return false;
        if (r->key && (!r->source_size || !r->source_hash || !r->text_hash)) return false;
        total += r->source_size + (uint64_t)r->text_size * 2;
    }
    return total <= FOLIO_WEREAD_BOOK_BYTES;
}
static bool pointer_valid(const pointer_t* p, const char* id) {
    return p->magic == POINTER_MAGIC && p->version == FORMAT && p->sequence && !p->reserved &&
           memchr(p->book_id, 0, 64) && !strcmp(p->book_id, id) && p->manifest_hash &&
           hash(p, offsetof(pointer_t, checksum)) == p->checksum;
}
static void pointers(const char* id, pointer_t p[2]) {
    memset(p, 0, sizeof(pointer_t) * 2); char path[PATH_CAP]; struct stat st;
    if (!book_path(path, id)) return;
    char hidden[PATH_CAP];
    if (named_path(hidden,id,"entry.wrb") && cache_lstat(hidden,&st)==0) copy_text(path,sizeof(path),hidden);
    if (!folio_storage_validate_parent(path, NULL, 0) ||
        cache_lstat(path, &st) || !ordinary_file(&st) || st.st_size > (off_t)(sizeof(pointer_t) * 2)) return;
    FILE* f = fopen(path, "rb"); if (!f) return;
    for (size_t i = 0; i < 2; ++i) {
        if (fread(&p[i], 1, sizeof(p[i]), f) != sizeof(p[i]) || !pointer_valid(&p[i], id)) memset(&p[i], 0, sizeof(p[i]));
    }
    fclose(f);
}
static bool load_snapshot(const char* id, uint64_t key, manifest_t* m, bool verify_body) {
    char path[PATH_CAP];
    if (!object_path(path, id, 'm', key) || !read_exact(path, m, sizeof(*m)) ||
        !manifest_valid(m, id) || m->checksum != key || !m->body_size || !m->body_hash ||
        !object_path(path, id, 'b', m->body_hash)) return false;
    return verify_body ? file_hash(path, m->body_size, m->body_hash) : safe_file(path, m->body_size);
}
static bool load_published(const char* id, manifest_t* m, bool verify_body) {
    pointer_t p[2]; pointers(id, p);
    unsigned first = p[1].sequence > p[0].sequence ? 1 : 0;
    return (p[first].sequence && load_snapshot(id, p[first].manifest_hash, m, verify_body)) ||
           (p[1 - first].sequence && load_snapshot(id, p[1 - first].manifest_hash, m, verify_body));
}
static bool chapter_open(const manifest_t* m, const chapter_record_t* r, FILE** output, chapter_header_t* h) {
    char path[PATH_CAP]; *output = NULL;
    if (!r->key || !object_path(path, m->book_id, 'c', r->key) ||
        !safe_file(path, sizeof(*h) + (uint64_t)r->source_size + r->text_size)) return false;
    FILE* f = fopen(path, "rb"); if (!f) return false;
    bool okay = fread(h, 1, sizeof(*h), f) == sizeof(*h) && h->magic == CHAPTER_MAGIC &&
        h->version == FORMAT && !h->reserved && h->source_size == r->source_size && h->text_size == r->text_size &&
        h->source_hash == r->source_hash && h->text_hash == r->text_hash && h->kind == r->kind &&
        h->checksum == hash(h, offsetof(chapter_header_t, checksum));
    if (!okay) { fclose(f); return false; }
    *output = f; return true;
}
static bool read_all(int fd, void* buffer, size_t size) {
    uint8_t* p = buffer; size_t off = 0;
    while (off < size) {
        ssize_t n = read(fd, p + off, size - off);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}
/* fd variant used by the streaming exporter. A stdio FILE costs an
 * internal-heap buffer plus a lock semaphore for every open, and closing one
 * from the export worker double-released that state (tlsf_free assert inside
 * __bufio_close). A raw fd keeps the heap flat and closes exactly once. */
static bool chapter_open_fd(const manifest_t* m, const chapter_record_t* r, int* output, chapter_header_t* h) {
    char path[PATH_CAP]; *output = -1;
    if (!r->key || !object_path(path, m->book_id, 'c', r->key) ||
        !safe_file(path, sizeof(*h) + (uint64_t)r->source_size + r->text_size)) return false;
    int fd = open(path, O_RDONLY); if (fd < 0) return false;
    bool okay = read_all(fd, h, sizeof(*h)) && h->magic == CHAPTER_MAGIC &&
        h->version == FORMAT && !h->reserved && h->source_size == r->source_size && h->text_size == r->text_size &&
        h->source_hash == r->source_hash && h->text_hash == r->text_hash && h->kind == r->kind &&
        h->checksum == hash(h, offsetof(chapter_header_t, checksum));
    if (!okay) { close(fd); return false; }
    *output = fd; return true;
}
static bool chapter_verify(const manifest_t* m, const chapter_record_t* r) {
    chapter_header_t h; FILE* f; uint64_t a, b;
    if (!chapter_open(m, r, &f, &h)) return false;
    bool okay = stream_hash(f, h.source_size, &a, NULL) && a == h.source_hash &&
                stream_hash(f, h.text_size, &b, NULL) && b == h.text_hash;
    if (fclose(f)) okay = false;
    return okay;
}

/* The converter is also the mapping engine. Each emitted scalar carries the
 * original UTF-16 position of its source token. No source-byte/local-byte
 * proportional scaling exists. Block separators belong to the actual tag's
 * opening '<'; source tokens that emit nothing deliberately cannot map. */
typedef struct {
    FILE* write; FILE* compare;
    uint8_t* compare_buffer;
    size_t compare_at, compare_used;
    uint32_t compare_left;
    uint32_t output_size, target, result;
    uint64_t output_hash;
    bool map, reverse, found, last_newline;
} sink_t;
static bool emit(sink_t* s, uint32_t cp, uint32_t native) {
    uint8_t b[4]; size_t n;
    if (cp < 0x80) { b[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 63); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); n = 4; }
    if (s->output_size > FOLIO_WEREAD_CHAPTER_BYTES - n) return false;
    if (s->map && !s->found && (s->reverse ? native : s->output_size) == s->target) {
        s->result = s->reverse ? s->output_size : native; s->found = true;
    }
    if (s->write && fwrite(b, 1, n, s->write) != n) return false;
    if (s->compare) {
        /* Read converted text in blocks rather than locking a FILE for every
         * Unicode scalar. Every byte is still compared, including past target. */
        if(!s->compare_buffer)return false;
        size_t at=0;
        while(at<n){
            if(s->compare_at==s->compare_used){
                size_t want=s->compare_left<4096u?s->compare_left:4096u;
                if(!want || fread(s->compare_buffer,1,want,s->compare)!=want)return false;
                s->compare_at=0;s->compare_used=want;s->compare_left-=(uint32_t)want;
            }
            size_t take=s->compare_used-s->compare_at;if(take>n-at)take=n-at;
            if(memcmp(b+at,s->compare_buffer+s->compare_at,take))return false;
            s->compare_at+=take;at+=take;
        }
    }
    s->output_hash = hash_bytes(s->output_hash, b, n); s->output_size += (uint32_t)n;
    s->last_newline = cp == '\n'; return true;
}
static bool newline(sink_t* s, uint32_t native) { return !s->output_size || s->last_newline || emit(s, '\n', native); }
static bool block_tag(const char* tag) {
    const char* names[] = {"p", "div", "br", "hr", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "tr", "section", "article", "pre"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(tag, names[i])) return true;
    return false;
}
static bool skipped_tag(const char* tag) { return !strcmp(tag, "head") || !strcmp(tag, "script") || !strcmp(tag, "style"); }
static bool entity(const uint8_t* p, size_t n, uint32_t* cp) {
    if (n > 16 || n < 2 || p[n - 1] != ';') return false;
    if (*p == '#') {
        unsigned base = 10; size_t i = 1; uint32_t value = 0;
        if (i < n - 1 && (p[i] == 'x' || p[i] == 'X')) { base = 16; ++i; }
        if (i == n - 1) return false;
        for (; i < n - 1; ++i) {
            unsigned d = p[i] >= '0' && p[i] <= '9' ? p[i] - '0' :
                p[i] >= 'a' && p[i] <= 'f' ? p[i] - 'a' + 10 : p[i] >= 'A' && p[i] <= 'F' ? p[i] - 'A' + 10 : 99;
            if (d >= base || value > (0x10FFFFu - d) / base) return false;
            value = value * base + d;
        }
        if (!value || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) ||
            (value < 32 && value != 9 && value != 10 && value != 13)) return false;
        *cp = value; return true;
    }
    static const struct { const char* name; uint32_t cp; } entities[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''},
        {"nbsp;", 0xA0}, {"shy;", 0xAD}, {"ensp;", 0x2002}, {"emsp;", 0x2003},
        {"thinsp;", 0x2009}, {"ndash;", 0x2013}, {"mdash;", 0x2014}, {"lsquo;", 0x2018},
        {"rsquo;", 0x2019}, {"ldquo;", 0x201C}, {"rdquo;", 0x201D}, {"hellip;", 0x2026},
        {"middot;", 0xB7}, {"copy;", 0xA9}, {"reg;", 0xAE}, {"bull;", 0x2022}
    };
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i)
        if (strlen(entities[i].name) == n && !memcmp(p, entities[i].name, n)) { *cp = entities[i].cp; return true; }
    return false;
}
#ifdef ESP_PLATFORM
static void convert_fail(size_t pos, size_t size, const uint8_t* raw, uint32_t kind, int reason) {
    static const char hex[] = "0123456789abcdef";
    char before[3 * 16 + 1], after[3 * 16 + 1];
    size_t lo = pos > 16 ? 16 : pos, hi = size - pos > 16 ? 16 : size - pos, n = 0;
    before[0] = after[0] = 0;
    for (size_t i = 0; i < lo; ++i) { before[n++] = hex[raw[pos - lo + i] >> 4]; before[n++] = hex[raw[pos - lo + i] & 15]; }
    before[n] = 0; n = 0;
    for (size_t i = 0; i < hi; ++i) { after[n++] = hex[raw[pos + i] >> 4]; after[n++] = hex[raw[pos + i] & 15]; }
    after[n] = 0;
    ESP_LOGW("folio_wr_cache", "convert failed: reason=%d kind=%u pos=%u/%u before=%s after=%s",
        reason, (unsigned)kind, (unsigned)pos, (unsigned)size, before, after);
}
#define CONVERT_FAIL(n) (convert_fail(pos, size, raw, kind, n), false)
#else
#define CONVERT_FAIL(n) false
#endif
static bool convert(const uint8_t* raw, size_t size, uint32_t kind, sink_t* sink) {
    size_t pos = 0, checkpoint = 0; uint32_t native = 0;
    unsigned skip_depth = 0; char skip_name[32] = {0}; bool previous_cr = false;
    sink->output_hash = HASH_INIT;
    while (pos < size) {
        if (pos >= checkpoint) { if (cancelled()) { return CONVERT_FAIL(1); } cooperate(); checkpoint = pos + 4096; }
        uint32_t cp; size_t width;
        if (!scalar(raw + pos, size - pos, &cp, &width) || (cp < 32 && cp != 9 && cp != 10 && cp != 13)) { return CONVERT_FAIL(2); }
        uint32_t start = native;
        if (kind == FOLIO_WEREAD_XHTML && cp == '<') {
            // CDATA is legal XML. Inside <style>/<script>/<head> the payload is not
            // reading text, so skip it; elsewhere it is literal character data and
            // must be emitted instead of losing the whole chapter.
            if (size - pos >= 9 && !memcmp(raw + pos, "<![CDATA[", 9)) {
                size_t end = pos + 9;
                while (end + 2 < size && memcmp(raw + end, "]]>", 3)) {
                    if (end - pos > 65536) { return CONVERT_FAIL(3); }
                    ++end;
                }
                if (end + 2 >= size) { return CONVERT_FAIL(3); }
                const size_t open_end = pos + 9, close_start = end, limit = end + 3;
                while (pos < limit) {
                    if (!scalar(raw + pos, limit - pos, &cp, &width)) { return CONVERT_FAIL(11); }
                    bool text = pos >= open_end && pos < close_start;
                    if (text && !skip_depth && !(cp == '\n' && previous_cr) &&
                        !emit(sink, cp == '\r' ? '\n' : cp, start)) { return CONVERT_FAIL(14); }
                    if (text) { previous_cr = cp == '\r'; }
                    native += cp > 0xFFFF ? 2 : 1; pos += width;
                }
                previous_cr = false; continue;
            }
            size_t end = pos + 1; uint8_t quote = 0; bool comment = size - pos >= 4 && !memcmp(raw + pos, "<!--", 4);
            if (comment) {
                end = pos + 4;
                while (end + 2 < size && memcmp(raw + end, "-->", 3)) ++end;
                if (end + 2 >= size) { return CONVERT_FAIL(4); }
                end += 3;
            } else {
                while (end < size) {
                    uint8_t ch = raw[end++];
                    if (quote) { if (ch == quote) quote = 0; }
                    else if (ch == '\'' || ch == '"') quote = ch;
                    else if (ch == '>') break;
                }
                if (end > size || raw[end - 1] != '>' || quote) { return CONVERT_FAIL(5); }
            }
            if (end - pos > 8192) { return CONVERT_FAIL(6); }
            if (!comment && raw[pos + 1] != '!' && raw[pos + 1] != '?') {
                size_t at = pos + 1; bool closing = raw[at] == '/'; if (closing) ++at;
                char name[32] = {0}; size_t nn = 0;
                while (at < end && (isalnum(raw[at]) || raw[at] == ':' || raw[at] == '-')) {
                    if (nn + 1 >= sizeof(name)) { return CONVERT_FAIL(7); }
                    name[nn++] = (char)tolower(raw[at++]);
                }
                if (!nn || (raw[at] != '>' && raw[at] != '/' && !isspace(raw[at]))) { return CONVERT_FAIL(8); }
                bool self = end >= 2 && raw[end - 2] == '/';
                if (skip_depth) {
                    if (!strcmp(name, skip_name)) {
                        if (closing) --skip_depth;
                        else if (!self && ++skip_depth > 64) { return CONVERT_FAIL(9); }
                    }
                } else if (!closing && !self && skipped_tag(name)) {
                    skip_depth = 1; memcpy(skip_name, name, sizeof(name));
                } else if (block_tag(name) && !newline(sink, start)) { return CONVERT_FAIL(10); }
            }
            while (pos < end) {
                if (!scalar(raw + pos, end - pos, &cp, &width)) { return CONVERT_FAIL(11); }
                native += cp > 0xFFFF ? 2 : 1; pos += width;
            }
            previous_cr = false; continue;
        }
        if (kind == FOLIO_WEREAD_XHTML && cp == '&' && !skip_depth) {
            size_t end = pos + 1;
            while (end < size && end - pos <= 17 && raw[end] != ';') ++end;
            if (end >= size || raw[end] != ';' || !entity(raw + pos + 1, end - pos, &cp)) { return CONVERT_FAIL(12); }
            if (!emit(sink, cp, start)) { return CONVERT_FAIL(13); }
            native += (uint32_t)(end + 1 - pos); pos = end + 1; previous_cr = false; continue;
        }
        if (!skip_depth && !(pos == 0 && cp == 0xFEFF)) {
            if (!(cp == '\n' && previous_cr) && !emit(sink, cp == '\r' ? '\n' : cp, start)) { return CONVERT_FAIL(14); }
        }
        previous_cr = cp == '\r'; native += cp > 0xFFFF ? 2 : 1; pos += width;
    }
    if (skip_depth) { return CONVERT_FAIL(15); }
    if (sink->map && !sink->found && (sink->reverse ? native : sink->output_size) == sink->target) {
        sink->result = sink->reverse ? sink->output_size : native; sink->found = true;
    }
    return !sink->compare || !ferror(sink->compare);
}

static bool resume_path(char path[PATH_CAP], const char* id, unsigned slot) {
    char name[32]; int n = snprintf(name, sizeof(name), "resume%u.bin", slot);
    return n > 0 && n < (int)sizeof(name) && named_path(path, id, name);
}
static bool referenced(const manifest_t* m, char type, uint64_t key) {
    if (!m) return false;
    if (type == 'm') return m->checksum == key;
    if (type == 'b') return m->body_hash == key;
    for (size_t i = 0; i < m->count; ++i) if (m->records[i].key == key) return true;
    return false;
}
static bool object_name(const char* name, char* type, uint64_t* key) {
    size_t n = strlen(name);
    if (n != 22 || name[1] != '-' || name[18] != '.') return false;
    if ((name[0] == 'c' && strcmp(name + 19, "wrc")) ||
        (name[0] == 'm' && strcmp(name + 19, "idx")) ||
        (name[0] == 'b' && strcmp(name + 19, "txt")) ||
        (name[0] != 'c' && name[0] != 'm' && name[0] != 'b')) return false;
    uint64_t h = 0;
    for (size_t i = 2; i < 18; ++i) {
        unsigned d = name[i] >= '0' && name[i] <= '9' ? name[i] - '0' :
            name[i] >= 'a' && name[i] <= 'f' ? name[i] - 'a' + 10 : 99;
        if (d > 15) return false;
        h = (h << 4) | d;
    }
    *type = name[0]; *key = h; return h != 0;
}
/* Retain both published pointer targets, plus the current resumable job. A
 * torn new pointer or checkpoint can therefore still use its predecessor. */
static bool collect(const manifest_t* current) {
    pointer_t p[2]; pointers(current->book_id, p);
    manifest_t* old[2] = {NULL, NULL}; bool okay = true;
    for (size_t i = 0; i < 2; ++i) if (p[i].sequence) {
        old[i] = allocate(sizeof(manifest_t));
        if (!old[i]) { okay = false; break; }
        if (!load_snapshot(current->book_id, p[i].manifest_hash, old[i], false)) { free(old[i]); old[i] = NULL; }
    }
    char dirpath[PATH_CAP]; DIR* dir = NULL;
    if (okay && book_dir(dirpath, current->book_id)) dir = opendir(dirpath);
    if (!dir) okay = false;
    struct dirent* entry; unsigned count = 0;
    while (okay && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (++count > 2048 || cancelled()) { okay = false; break; }
        char type; uint64_t key;
        bool removable = !strcmp(entry->d_name, "chapter.part") || !strcmp(entry->d_name, "body.part") ||
                         !strcmp(entry->d_name, "manifest.part");
        if (object_name(entry->d_name, &type, &key)) removable = !referenced(current, type, key) &&
            !referenced(old[0], type, key) && !referenced(old[1], type, key);
        if (removable) {
            char path[PATH_CAP]; int n = snprintf(path, sizeof(path), "%s/%s", dirpath, entry->d_name);
            if (n <= 0 || n >= (int)sizeof(path) || !folio_storage_validate_parent(path, NULL, 0) || unlink(path)) okay = false;
        }
        if (!(count % 32)) cooperate();
    }
    if (dir) closedir(dir);
    free(old[0]); free(old[1]); return okay;
}
static bool usage(const char* id, uint64_t* out) {
    char root[PATH_CAP]; if (!book_dir(root, id)) return false;
    DIR* dir = opendir(root); if (!dir) return false;
    uint64_t total = 0; unsigned count = 0; bool okay = true; struct dirent* item;
    while ((item = readdir(dir)) != NULL) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        char path[PATH_CAP]; struct stat st;
        int n = snprintf(path, sizeof(path), "%s/%s", root, item->d_name);
        if (++count > 2048 || n <= 0 || n >= (int)sizeof(path) || cache_lstat(path, &st) ||
            !S_ISREG(st.st_mode) || st.st_size < 0 || cancelled()) { okay = false; break; }
        total += (uint64_t)st.st_size;
    }
    closedir(dir); *out = total; return okay;
}
static bool quota(const char* id, uint64_t additional) {
    uint64_t total = 0;
    return usage(id, &total) && total + additional <= FOLIO_WEREAD_BOOK_BYTES;
}
static uint64_t resume_bytes(const manifest_t* m) {
    return sizeof(resume_header_t) + (uint64_t)m->count * sizeof(chapter_record_t);
}
static bool write_resume(const char* path, const manifest_t* m) {
    resume_header_t h; memset(&h, 0, sizeof(h));
    h.magic = RESUME_MAGIC; h.version = FORMAT; h.sequence = m->sequence; h.count = m->count;
    h.body_hash = m->body_hash; h.body_size = m->body_size;
    memcpy(h.book_id, m->book_id, sizeof(h.book_id));
    memcpy(h.title, m->title, sizeof(h.title));
    memcpy(h.author, m->author, sizeof(h.author));
    uint64_t digest = hash_bytes(hash(&h, offsetof(resume_header_t, checksum)), m->records,
                                 (size_t)m->count * sizeof(chapter_record_t));
    h.checksum = digest;
    FILE* f = fopen(path, "wb");
    bool okay = f && fwrite(&h, 1, sizeof(h), f) == sizeof(h) &&
                fwrite(m->records, sizeof(chapter_record_t), m->count, f) == m->count;
    return close_write(f, okay);
}
static bool load_resume_file(const char* path, const char* id, manifest_t* out) {
    if (!folio_storage_validate_parent(path, NULL, 0)) return false;
    struct stat st;
    if (cache_lstat(path, &st) || !ordinary_file(&st)) return false;
    FILE* f = fopen(path, "rb"); if (!f) return false;
    bool okay = false;
    if ((uint64_t)st.st_size == sizeof(manifest_t)) {
        okay = fread(out, 1, sizeof(*out), f) == sizeof(*out) && !ferror(f);
    } else {
        resume_header_t h;
        uint64_t expected = 0;
        if (fread(&h, 1, sizeof(h), f) == sizeof(h) && !ferror(f) && h.magic == RESUME_MAGIC &&
            h.version == FORMAT && !h.reserved && h.count && h.count <= FOLIO_WEREAD_CHAPTER_LIMIT) {
            expected = sizeof(h) + (uint64_t)h.count * sizeof(chapter_record_t);
        }
        if (expected && (uint64_t)st.st_size == expected) {
            uint64_t digest = hash_bytes(hash(&h, offsetof(resume_header_t, checksum)), NULL, 0);
            memset(out, 0, sizeof(*out));
            out->magic = MANIFEST_MAGIC; out->version = FORMAT; out->sequence = h.sequence; out->count = h.count;
            out->body_hash = h.body_hash; out->body_size = h.body_size;
            memcpy(out->book_id, h.book_id, sizeof(out->book_id));
            memcpy(out->title, h.title, sizeof(out->title));
            memcpy(out->author, h.author, sizeof(out->author));
            if (fread(out->records, sizeof(chapter_record_t), h.count, f) == h.count && !ferror(f)) {
                digest = hash_bytes(digest, out->records, (size_t)h.count * sizeof(chapter_record_t));
                if (digest == h.checksum) {
                    out->checksum = hash(out, offsetof(manifest_t, checksum));
                    okay = true;
                }
            }
        }
    }
    if (fclose(f)) okay = false;
    return okay && manifest_valid(out, id);
}
static bool checkpoint(folio_weread_cache_writer_t* w) {
    if (w->manifest.sequence == UINT32_MAX) return false;
    char path[PATH_CAP]; unsigned slot = w->resume_slot == 0 ? 1 : 0;
    if (!resume_path(path, w->manifest.book_id, slot) || !writable_path(path)) return false;
    struct stat st; uint64_t add = cache_lstat(path, &st) == 0 ? 0 : resume_bytes(&w->manifest);
    if (w->used_bytes + add > FOLIO_WEREAD_BOOK_BYTES) return false;
    ++w->manifest.sequence; w->manifest.checksum = hash(&w->manifest, offsetof(manifest_t, checksum));
    bool okay = write_resume(path, &w->manifest);
    if (!okay) { --w->manifest.sequence; w->manifest.checksum = hash(&w->manifest, offsetof(manifest_t, checksum)); return false; }
    w->used_bytes += add; w->resume_slot = (int)slot; return true;
}
static manifest_t* load_resume(const char* id, int* slot) {
    manifest_t* best = allocate(sizeof(manifest_t)); manifest_t* next = allocate(sizeof(manifest_t));
    if (!best || !next) { free(best); free(next); return NULL; }
    *slot = -1;
    for (unsigned i = 0; i < 2; ++i) {
        char path[PATH_CAP];
        if (resume_path(path, id, i) && load_resume_file(path, id, next) &&
            (*slot < 0 || next->sequence > best->sequence)) { memcpy(best, next, sizeof(*best)); *slot = (int)i; }
    }
    free(next);
    if (*slot < 0 && !load_published(id, best, false)) { free(best); return NULL; }
    return best;
}

folio_weread_cache_writer_t* folio_weread_cache_begin(const char* id, const char* title, const char* author,
    const folio_weread_chapter_t* chapters, size_t count, char* error, size_t cap) {
    clear_error(error, cap);
    if (!id_valid(id) || !text_valid(title, 128, false) || !text_valid(author, 128, true) ||
        !chapters || !count || count > FOLIO_WEREAD_CHAPTER_LIMIT) {
        fail(error, cap, "微信读书目录或书籍标识无效。"); return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!memchr(chapters[i].uid, 0, 64) || !id_valid(chapters[i].uid) || !text_valid(chapters[i].title, 128, false)) {
            fail(error, cap, "章节标识或标题无效。"); return NULL;
        }
        for (size_t j = 0; j < i; ++j) if (!strcmp(chapters[i].uid, chapters[j].uid)) {
            fail(error, cap, "目录含有重复章节。"); return NULL;
        }
    }
    if (!folio_storage_lock(WAIT_MS)) { fail(error, cap, "TF 卡正忙。"); return NULL; }
    folio_weread_cache_writer_t* w = NULL; char dir[PATH_CAP];
    if (writer_active || !root_ready() || !book_dir(dir, id) || !directory(dir) || cancelled()) goto failed;
    w = allocate(sizeof(*w)); if (!w) goto failed;
    w->resume_slot = -1; w->manifest.magic = MANIFEST_MAGIC; w->manifest.version = FORMAT;
    w->manifest.count = (uint32_t)count; copy_text(w->manifest.book_id, 64, id);
    copy_text(w->manifest.title, 128, title); copy_text(w->manifest.author, 128, author);
    int old_slot; manifest_t* old = load_resume(id, &old_slot);
    if (old) { w->resume_slot = old_slot; w->manifest.sequence = old->sequence; }
    for (size_t i = 0; i < count; ++i) {
        if (old) for (size_t j = 0; j < old->count; ++j)
            if (!strcmp(chapters[i].uid, old->records[j].chapter.uid) && old->records[j].key &&
                chapter_verify(old, &old->records[j])) { w->manifest.records[i] = old->records[j]; break; }
        copy_text(w->manifest.records[i].chapter.uid, 64, chapters[i].uid);
        copy_text(w->manifest.records[i].chapter.title, 128, chapters[i].title);
    }
    free(old);
    if (!usage(id, &w->used_bytes)) w->used_bytes = 0;
    if (cancelled() || !collect(&w->manifest) || !checkpoint(w)) goto failed;
    writer_active = true; folio_storage_unlock(); return w;
failed:
    free(w); folio_storage_unlock(); fail(error, cap, cancelled() ? "缓存操作已取消。" : "无法准备缓存，工作者正忙、空间不足或 TF 卡不可用。"); return NULL;
}
bool folio_weread_cache_has_chapter(const folio_weread_cache_writer_t* w, size_t index) {
    if (!w || !folio_storage_lock(WAIT_MS)) return false;
    bool available = index < w->manifest.count && w->manifest.records[index].key != 0;
    folio_storage_unlock();
    return available;
}
bool folio_weread_cache_write_chapter(folio_weread_cache_writer_t* w, size_t index,
    const uint8_t* raw, size_t size, folio_weread_content_kind_t kind, char* error, size_t cap) {
    clear_error(error, cap);
    if (!w || index >= w->manifest.count || !raw || !size || size > FOLIO_WEREAD_CHAPTER_BYTES ||
        (kind != FOLIO_WEREAD_TEXT && kind != FOLIO_WEREAD_XHTML)) return fail(error, cap, "章节内容或大小无效。");
    LOCK(); bool okay = false; char path[PATH_CAP], part[PATH_CAP];
    sink_t sink = {0};
    if (!convert(raw, size, kind, &sink)) { fail(error, cap, "章节编码或 XHTML 不受支持，未保存。"); goto done; }
    chapter_record_t r = w->manifest.records[index];
    r.source_size = (uint32_t)size; r.text_size = sink.output_size; r.kind = (uint32_t)kind;
    r.source_hash = hash(raw, size); r.text_hash = sink.output_hash;
    if (!r.source_hash || !r.text_hash) goto io; // Zero is reserved for an absent fingerprint.
    r.key = hash_bytes(hash(r.chapter.uid, strlen(r.chapter.uid)), &r.source_hash, sizeof(r.source_hash));
    r.key = hash_bytes(r.key, &r.kind, sizeof(r.kind)); if (!r.key) r.key = 1;
    uint64_t total = 0;
    for (size_t i = 0; i < w->manifest.count; ++i) {
        const chapter_record_t* c = i == index ? &r : &w->manifest.records[i];
        total += c->source_size + (uint64_t)c->text_size * 2;
    }
    /* collect() re-reads both published manifests (119 KB each) to decide what
     * is unreferenced. Doing that for every chapter cost more than the chapter
     * itself; begin() and publish() still reclaim space. */
    if (total > FOLIO_WEREAD_BOOK_BYTES ||
        !object_path(path, w->manifest.book_id, 'c', r.key)) goto io;
    struct stat st;
    bool exists = cache_lstat(path, &st) == 0;
    if (!exists || !chapter_verify(&w->manifest, &r)) {
        uint64_t bytes = sizeof(chapter_header_t) + size + r.text_size;
        if (w->used_bytes + bytes > FOLIO_WEREAD_BOOK_BYTES) goto io;
        if (!named_path(part, w->manifest.book_id, "chapter.part") || !writable_path(part)) goto io;
        chapter_header_t h = {0}; h.magic = CHAPTER_MAGIC; h.version = FORMAT; h.source_size = r.source_size;
        h.text_size = r.text_size; h.source_hash = r.source_hash; h.text_hash = r.text_hash; h.kind = r.kind;
        h.checksum = hash(&h, offsetof(chapter_header_t, checksum));
        FILE* f = fopen(part, "wb"); if (!f) goto io;
        sink_t output = {.write = f};
        bool wrote = fwrite(&h, 1, sizeof(h), f) == sizeof(h) && memory_transfer(f, (uint8_t*)raw, size, true) &&
                     convert(raw, size, kind, &output) && output.output_size == h.text_size && output.output_hash == h.text_hash;
        if (!close_write(f, wrote) || cancelled() ||
            (exists && (!folio_storage_validate_parent(path, NULL, 0) || unlink(path))) || rename(part, path)) {
            unlink(part); goto io;
        }
    }
    if (!exists) w->used_bytes += sizeof(chapter_header_t) + (uint64_t)size + r.text_size;
    chapter_record_t previous = w->manifest.records[index]; w->manifest.records[index] = r;
    if (!checkpoint(w)) { w->manifest.records[index] = previous; goto io; }
    okay = true; goto done;
io:
    fail(error, cap, cancelled() ? "缓存操作已取消。" : "章节未提交：TF 卡写入失败、内容损坏或超过每书 32 MiB 缓存上限。");
done:
    folio_storage_unlock(); return okay;
}

static bool published_matches(const manifest_t* current, uint32_t body_size) {
    manifest_t* published = allocate(sizeof(*published));
    if (!published) return false;
    // Compare before the expensive body verification. Journal sequence and
    // derived body/checksum fields are not part of the requested content.
    bool same = !cancelled() && load_published(current->book_id, published, false) &&
        published->magic == current->magic && published->version == current->version &&
        published->count == current->count && published->reserved == current->reserved &&
        published->body_size == body_size &&
        !memcmp(published->book_id, current->book_id, sizeof(current->book_id)) &&
        !memcmp(published->title, current->title, sizeof(current->title)) &&
        !memcmp(published->author, current->author, sizeof(current->author)) &&
        !memcmp(published->records, current->records, sizeof(current->records));
    char body_path[PATH_CAP];
    if (same) same = object_path(body_path, published->book_id, 'b', published->body_hash) &&
        file_hash(body_path, published->body_size, published->body_hash);
    // Never trust begin()'s earlier checks: a damaged raw/text object must not
    // become acceptable merely because the derived published body is intact.
    for (size_t i = 0; same && i < current->count; ++i)
        if (current->records[i].key)
            same = !cancelled() && chapter_verify(current, &current->records[i]);
    free(published);
    return same && !cancelled();
}

bool folio_weread_cache_publish(folio_weread_cache_writer_t* w, char* out, size_t out_cap, char* error, size_t cap) {
    if (out && out_cap) *out = 0;
    clear_error(error, cap);
    char destination[PATH_CAP];
    if (!w || !out || !book_path(destination, w->manifest.book_id) || strlen(destination) >= out_cap)
        return fail(error, cap, "缓存发布参数无效。");
    LOCK(); bool okay = false; char part[PATH_CAP], final[PATH_CAP]; FILE* body = NULL;
    char hidden[PATH_CAP];struct stat hidden_info;
    if(named_path(hidden,w->manifest.book_id,"entry.wrb") && cache_lstat(hidden,&hidden_info)==0)copy_text(destination,sizeof(destination),hidden);
    uint64_t bytes = 0;
    for (size_t i = 0; i < w->manifest.count; ++i) if (w->manifest.records[i].key && w->manifest.records[i].text_size)
        bytes += w->manifest.records[i].text_size + 1u;
    if (!bytes || bytes > FOLIO_WEREAD_BOOK_BYTES || cancelled()) goto done;
    if (published_matches(&w->manifest, (uint32_t)bytes)) {
        book_path(out, w->manifest.book_id); okay = true; goto done;
    }
    // Missing, changed or damaged publications use the existing repair path.
    if (cancelled() || !collect(&w->manifest) ||
        !quota(w->manifest.book_id, bytes + sizeof(manifest_t))) goto done;
    if (!named_path(part, w->manifest.book_id, "body.part") || !writable_path(part)) goto done;
    body = fopen(part, "wb"); if (!body) goto done;
    bool wrote = true;
    for (size_t i = 0; wrote && i < w->manifest.count; ++i) {
        const chapter_record_t* r = &w->manifest.records[i]; if (!r->key || !r->text_size) continue;
        FILE* f; chapter_header_t h; uint64_t a, b;
        if (!chapter_open(&w->manifest, r, &f, &h)) { wrote = false; break; }
        wrote = stream_hash(f, h.source_size, &a, NULL) && a == h.source_hash &&
                stream_hash(f, h.text_size, &b, body) && b == h.text_hash && fputc('\n', body) != EOF;
        if (fclose(f)) wrote = false;
    }
    if (!close_write(body, wrote)) { body = NULL; unlink(part); goto done; }
    body = NULL; FILE* f = fopen(part, "rb"); uint64_t body_hash = 0;
    if (!f) goto done;
    bool read = stream_hash(f, (uint32_t)bytes, &body_hash, NULL); if (fclose(f)) read = false;
    if (!read || !object_path(final, w->manifest.book_id, 'b', body_hash)) goto done;
    struct stat st;
    if (cache_lstat(final, &st) == 0) {
        if (file_hash(final, (uint32_t)bytes, body_hash)) unlink(part);
        else if (!folio_storage_validate_parent(final, NULL, 0) || unlink(final) || rename(part, final)) goto done;
    }
    else if (rename(part, final)) goto done;
    manifest_t* published = allocate(sizeof(*published)); if (!published) goto done;
    memcpy(published, &w->manifest, sizeof(*published)); published->body_size = (uint32_t)bytes; published->body_hash = body_hash;
    // Resume sequence is journal bookkeeping, not content identity. Repeating
    // an unchanged download must preserve the published revision and progress.
    published->sequence = 1;
    published->checksum = hash(published, offsetof(manifest_t, checksum));
    bool prepared = object_path(final, published->book_id, 'm', published->checksum);
    bool existing_manifest = prepared && cache_lstat(final, &st) == 0;
    bool manifest_matches = false;
    if (existing_manifest) {
        manifest_t* check = allocate(sizeof(*check));
        manifest_matches = check && read_exact(final, check, sizeof(*check)) && !memcmp(check, published, sizeof(*check));
        free(check);
    }
    if (prepared && !manifest_matches) {
        prepared = named_path(part, published->book_id, "manifest.part") && writable_path(part);
        f = prepared ? fopen(part, "wb") : NULL;
        prepared = f && close_write(f, fwrite(published, 1, sizeof(*published), f) == sizeof(*published));
        if (prepared) prepared = !cancelled() &&
            (!existing_manifest || (folio_storage_validate_parent(final, NULL, 0) && unlink(final) == 0)) && rename(part, final) == 0;
    }
    if (!prepared) { free(published); goto done; }
    pointer_t p[2]; pointers(published->book_id, p);
    unsigned active = p[1].sequence > p[0].sequence ? 1 : 0;
    if (p[active].sequence && p[active].manifest_hash == published->checksum) {
        free(published); book_path(out,w->manifest.book_id); okay = true; goto done;
    }
    unsigned target = p[0].sequence > p[1].sequence ? 1 : 0;
    uint32_t seq = p[0].sequence > p[1].sequence ? p[0].sequence : p[1].sequence;
    if (seq == UINT32_MAX || !writable_path(destination) || cancelled()) { free(published); goto done; }
    if (cache_lstat(destination, &st) == 0 && (!S_ISREG(st.st_mode) || st.st_size > (off_t)(2 * sizeof(pointer_t)))) {
        free(published); goto done;
    }
    pointer_t pointer = {0}; pointer.magic = POINTER_MAGIC; pointer.version = FORMAT; pointer.sequence = seq + 1;
    copy_text(pointer.book_id, sizeof(pointer.book_id), published->book_id); pointer.manifest_hash = published->checksum;
    pointer.checksum = hash(&pointer, offsetof(pointer_t, checksum));
    f = fopen(destination, "r+b"); if (!f && errno == ENOENT) f = fopen(destination, "w+b");
    okay = f && close_write(f, fseek(f, (long)(target * sizeof(pointer_t)), SEEK_SET) == 0 &&
                           fwrite(&pointer, 1, sizeof(pointer), f) == sizeof(pointer));
    free(published);
    if (okay) book_path(out,w->manifest.book_id);
done:
    if (body) fclose(body);
    if (!okay) fail(error, cap, cancelled() ? "缓存发布已取消。" : "无法发布缓存：无可读章节、文件损坏、空间不足或 TF 卡写入失败。");
    folio_storage_unlock(); return okay;
}
void folio_weread_cache_close(folio_weread_cache_writer_t* w) {
    if (!w) return;
    if (folio_storage_lock(UINT32_MAX)) { writer_active = false; free(w); folio_storage_unlock(); }
}
bool folio_weread_cache_book_id(const char* path,char* out,size_t cap) {
    if(out&&cap)*out=0;
    return folio_wr_path_id(path,out,cap);
}
bool folio_weread_cache_path(const char* id, char* path, size_t cap) {
    if (path && cap) *path = 0;
    char full[PATH_CAP];
    if (!path || !book_path(full, id) || strlen(full) >= cap) return false;
    copy_text(path, cap, full); return true;
}
static manifest_t* open_manifest(const char* path, char* error, size_t cap, bool full) {
    char id[64];
    if (!folio_weread_cache_book_id(path, id, sizeof(id))) { fail(error, cap, "微信读书缓存路径无效。"); return NULL; }
    manifest_t* m = allocate(sizeof(*m));
    if (!m) { fail(error, cap, "内存不足，无法读取微信读书缓存。"); return NULL; }
    if (!load_published(id, m, full)) { free(m); fail(error, cap, "微信读书缓存不完整或已损坏，请重新缓存。"); return NULL; }
    return m;
}
bool folio_weread_cache_prepare(const char* path, folio_epub_t* result, char* error, size_t cap) {
    clear_error(error, cap); if (result) memset(result, 0, sizeof(*result));
    if (!result) return fail(error, cap, "阅读缓存参数无效。");
    LOCK(); manifest_t* m = open_manifest(path, error, cap, true); bool okay = false;
    if (!m) goto done;
    for (size_t i = 0; i < m->count; ++i) if (m->records[i].key && m->records[i].text_size) ++result->chapter_count;
    if (!result->chapter_count) goto done;
    result->chapters = allocate(result->chapter_count * sizeof(*result->chapters));
    if (!result->chapters) { fail(error, cap, "内存不足，无法读取目录。"); goto done; }
    uint32_t offset = 0; size_t at = 0;
    for (size_t i = 0; i < m->count; ++i) {
        const chapter_record_t* r = &m->records[i]; if (!r->key || !r->text_size) continue;
        result->chapters[at].offset = offset; copy_text(result->chapters[at].title, 128, r->chapter.title);
        offset += r->text_size + 1; ++at;
    }
    okay = offset == m->body_size && object_path(result->text_path, m->book_id, 'b', m->body_hash);
    if (okay) copy_text(result->title, sizeof(result->title), m->title);
done:
    free(m);
    if (!okay) { free(result->chapters); memset(result, 0, sizeof(*result)); if (error && cap && !*error) fail(error, cap, "缓存正文与目录不一致。"); }
    folio_storage_unlock(); return okay;
}
bool folio_weread_cache_metadata(const char* path, char* title, size_t title_cap, char* author, size_t author_cap,
    char* error, size_t cap) {
    clear_error(error, cap); if (title && title_cap) *title = 0; if (author && author_cap) *author = 0;
    LOCK(); manifest_t* m = open_manifest(path, error, cap, false);
    if (m) { copy_text(title, title_cap, m->title); copy_text(author, author_cap, m->author); }
    bool okay = m != NULL; free(m); folio_storage_unlock(); return okay;
}
uint64_t folio_weread_cache_revision(const char* path) {
    if (!folio_storage_lock(WAIT_MS)) return 0;
    manifest_t* m = open_manifest(path, NULL, 0, true); uint64_t result = m ? m->checksum : 0;
    free(m); folio_storage_unlock(); return result;
}
size_t folio_weread_cache_chapter_count(const char* path) {
    if (!folio_storage_lock(WAIT_MS)) return 0;
    manifest_t* m = open_manifest(path, NULL, 0, false); size_t count = 0;
    if (m) for (size_t i = 0; i < m->count; ++i)
        if (m->records[i].key && m->records[i].text_size) ++count;
    free(m); folio_storage_unlock(); return count;
}
bool folio_weread_cache_chapter_available(const char* path, const char* uid) {
    if (!id_valid(uid) || !folio_storage_lock(WAIT_MS)) return false;
    manifest_t* m = open_manifest(path, NULL, 0, false); bool found = false;
    if (m) for (size_t i = 0; i < m->count; ++i) if (!strcmp(uid, m->records[i].chapter.uid)) {
        found = m->records[i].key && chapter_verify(m, &m->records[i]); break;
    }
    free(m); folio_storage_unlock(); return found;
}
bool folio_weread_cache_presence(const char* path, const char* const* uids, size_t count, bool* present) {
    if (!uids || !present || count > FOLIO_WEREAD_CHAPTER_LIMIT) return false;
    memset(present, 0, count * sizeof(*present));
    if (!folio_storage_lock(WAIT_MS)) return false;
    manifest_t* m = open_manifest(path, NULL, 0, false);
    if (m) for (size_t i = 0; i < count; ++i) {
        if (!uids[i] || !id_valid(uids[i])) continue;
        for (size_t j = 0; j < m->count; ++j) if (!strcmp(uids[i], m->records[j].chapter.uid)) {
            present[i] = m->records[j].key && m->records[j].text_size;
            break;
        }
    }
    bool okay = m != NULL;
    free(m); folio_storage_unlock(); return okay;
}
static bool map_chapter(const manifest_t* m, const chapter_record_t* r, uint32_t target, bool reverse, uint32_t* result) {
    chapter_header_t h; FILE* f; if (!chapter_open(m, r, &f, &h)) return false;
    uint8_t* raw = allocate(h.source_size);uint8_t* compare=allocate(4096);
    bool okay = raw != NULL && compare != NULL;
    if (okay) okay = memory_transfer(f, raw, h.source_size, false) && hash(raw, h.source_size) == h.source_hash;
    sink_t sink = {.compare = f, .compare_buffer=compare, .compare_left=h.text_size,
                  .target = target, .map = true, .reverse = reverse};
    if (okay) okay = convert(raw, h.source_size, h.kind, &sink) && sink.found &&
                     sink.output_size == h.text_size && sink.output_hash == h.text_hash &&
                     !sink.compare_left && sink.compare_at==sink.compare_used;
    if (fclose(f)) okay = false;
    free(compare);free(raw); if (okay) *result = sink.result; return okay;
}
bool folio_weread_cache_local_to_cloud(const char* path, uint32_t offset, folio_weread_position_t* position,
    char* error, size_t cap) {
    clear_error(error, cap); if (position) memset(position, 0, sizeof(*position));
    if (!position) return fail(error, cap, "位置参数无效。");
    LOCK(); manifest_t* m = open_manifest(path, error, cap, true); bool okay = false;
    if (m) {
        uint32_t start = 0;
        for (size_t i = 0; i < m->count; ++i) {
            const chapter_record_t* r = &m->records[i]; if (!r->key || !r->text_size) continue;
            if (offset >= start && offset <= start + r->text_size) {
                okay = map_chapter(m, r, offset - start, false, &position->offset_utf16);
                if (okay) { copy_text(position->book_id, 64, m->book_id); copy_text(position->chapter_uid, 64, r->chapter.uid);
                    position->source_size = r->source_size; position->source_hash = r->source_hash; position->kind = (folio_weread_content_kind_t)r->kind; }
                break;
            }
            start += r->text_size + 1;
        }
    }
    free(m); if (!okay && error && cap && !*error) fail(error, cap, "当前位置无法精确对应云端，未上传。");
    folio_storage_unlock(); return okay;
}
bool folio_weread_cache_cloud_to_local(const char* path, const folio_weread_position_t* position,
    uint32_t* offset, char* error, size_t cap) {
    clear_error(error, cap); if (offset) *offset = 0;
    if (!position || !offset || !memchr(position->book_id, 0, 64) || !id_valid(position->book_id) ||
        !memchr(position->chapter_uid, 0, 64) || !id_valid(position->chapter_uid)) return fail(error, cap, "云端位置参数无效。");
    LOCK(); manifest_t* m = open_manifest(path, error, cap, true); bool okay = false;
    if (m && !strcmp(position->book_id, m->book_id)) {
        uint32_t start = 0;
        for (size_t i = 0; i < m->count; ++i) {
            const chapter_record_t* r = &m->records[i]; if (!r->key || !r->text_size) continue;
            if (!strcmp(position->chapter_uid, r->chapter.uid)) {
                bool same = !position->source_hash || (position->source_hash == r->source_hash &&
                    position->source_size == r->source_size && (uint32_t)position->kind == r->kind);
                uint32_t local;
                okay = same && map_chapter(m, r, position->offset_utf16, true, &local);
                if (okay) *offset = start + local;
                break;
            }
            start += r->text_size + 1;
        }
    }
    free(m); if (!okay && error && cap && !*error) fail(error, cap, "云端位置未缓存、正文已改变或不能精确映射。");
    folio_storage_unlock(); return okay;
}
bool folio_weread_cache_matches_raw(const folio_weread_position_t* position, const uint8_t* raw, size_t size,
    folio_weread_content_kind_t kind) {
    return position && raw && size && size <= FOLIO_WEREAD_CHAPTER_BYTES && size == position->source_size &&
           kind == position->kind && position->source_hash && hash(raw, size) == position->source_hash;
}

#include "folio_weread_export.inc"
