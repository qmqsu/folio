#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"

static bool fail_alloc;
static int fail_write_after = -1;
static void* test_calloc(size_t count, size_t size) { return fail_alloc ? NULL : calloc(count, size); }
static void* test_heap_calloc(size_t count, size_t size, unsigned flags) {
    (void)flags; return test_calloc(count, size);
}
static size_t test_fwrite(const void* bytes, size_t size, size_t count, FILE* file) {
    if (fail_write_after == 0) { errno = EIO; return 0; }
    if (fail_write_after > 0) --fail_write_after;
    return fwrite(bytes, size, count, file);
}
#define calloc test_calloc
#define heap_caps_calloc test_heap_calloc
#define fwrite test_fwrite
#include "../../main/book/folio_catalog.c"
#undef calloc
#undef heap_caps_calloc
#undef fwrite

static unsigned metadata_reads;
static unsigned weread_metadata_reads;
bool folio_weread_cache_metadata(const char* path,char* title,size_t title_cap,
                                 char* author,size_t author_cap,char* error,size_t error_cap) {
    ++weread_metadata_reads;title[0]=author[0]=error[0]=0;
    if(strstr(path,"broken")){snprintf(error,error_cap,"damaged pointer");return false;}
    snprintf(title,title_cap,"云端书名");snprintf(author,author_cap,"林舟");return true;
}
void folio_pdf_metadata(const char* path,char* title,size_t title_cap,char* author,size_t author_cap) {
    (void)path;(void)title;(void)title_cap;(void)author;(void)author_cap;
}
bool folio_epub_metadata(const char* path, char* title, size_t title_cap,
                         char* author, size_t author_cap, char* error, size_t error_cap) {
    ++metadata_reads; title[0] = author[0] = error[0] = 0;
    const char* name = strrchr(path, '/') + 1;
    if (!strcmp(name, "损坏.epub")) { snprintf(error, error_cap, "bad ZIP"); return false; }
    const char* titles[] = {"平凡的世界", "三体", "旅行日记", "Jane Eyre"};
    const char* authors[] = {"路遥", "刘慈欣", "吕明", "Charlotte Brontë"};
    const char* names[] = {"平凡的世界.epub", "三体.epub", "旅行日记.epub", "简爱.epub"};
    for (unsigned i = 0; i < 4; ++i) if (!strcmp(name, names[i])) {
        snprintf(title, title_cap, "%s", titles[i]); snprintf(author, author_cap, "%s", authors[i]);
    }
    return true;
}
static void put_file(const char* path, const void* bytes, size_t size) {
    FILE* file = fopen(path, "wb"); assert(file);
    assert(fwrite(bytes, 1, size, file) == size && fclose(file) == 0);
}
static folio_book_t make_book(const char* title, const char* extension) {
    folio_book_t book = {0};
    snprintf(book.path, sizeof(book.path), FOLIO_SD_ROOT "/books/%s.%s", title, extension);
    snprintf(book.title, sizeof(book.title), "%s", title);
    put_file(book.path, "book", 4); book.size = 4;
    return book;
}
static void remove_slots(void) {
    for (unsigned i = 0; i < 2; ++i) assert(remove(catalog_slot(i)) == 0 || errno == ENOENT);
}
static folio_catalog_t* fresh(void) {
    remove_slots(); folio_catalog_t* catalog = folio_catalog_open();
    assert(catalog && !*folio_catalog_error(catalog) && !folio_catalog_dirty(catalog));
    return catalog;
}
static size_t select_books(const folio_catalog_t* catalog, const folio_book_t* books, size_t count,
                           folio_catalog_filter_t filter, folio_catalog_sort_t sort, const char* query, size_t* indexes) {
    folio_catalog_options_t options = {filter, sort, query};
    return folio_catalog_select(catalog, books, count, &options, indexes, FOLIO_LIBRARY_LIMIT);
}
static void match(folio_catalog_t* catalog, folio_book_t* books, const char* query, size_t expected) {
    size_t indexes[128];
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_TITLE, query, indexes) == 1);
    assert(indexes[0] == expected);
}
static void test_features(void) {
    folio_catalog_t* catalog = fresh();
    folio_book_t books[] = {make_book("平凡的世界", "epub"), make_book("三体", "epub"),
        make_book("南方来信", "txt"), make_book("旅行日记", "epub"), make_book("简爱", "epub"), make_book("山间日记", "txt")};
    folio_book_t original[6]; memcpy(original, books, sizeof(books));
    metadata_reads = 0;
    assert(folio_catalog_sync(catalog, books, 6, 0, false));
    assert(metadata_reads == 4 && catalog->count == 6 && folio_catalog_dirty(catalog));
    assert(folio_catalog_save(catalog) && !folio_catalog_dirty(catalog));
    assert(folio_catalog_sync(catalog, books, 6, 1793021760, false));
    assert(metadata_reads == 4 && !folio_catalog_dirty(catalog));
    assert(folio_catalog_sync(catalog, books, 6, 1793021760, true));
    assert(metadata_reads == 8 && !folio_catalog_dirty(catalog));
    const folio_catalog_metadata_t* metadata = folio_catalog_metadata(catalog, books[0].path);
    assert(metadata && !metadata->read && metadata->added_utc == 0 && !strcmp(metadata->author, "路遥"));
    assert(folio_catalog_metadata(catalog, books[2].path)->author[0] == 0);
    assert(!strcmp(folio_catalog_metadata(catalog, books[4].path)->title, "Jane Eyre"));
    size_t indexes[128];
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_TITLE, NULL, indexes) == 6);
    const size_t by_title[] = {4, 3, 2, 0, 1, 5}; assert(!memcmp(indexes, by_title, sizeof(by_title)));
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_AUTHOR, NULL, indexes) == 6);
    const size_t by_author[] = {4, 1, 0, 3, 2, 5}; assert(!memcmp(indexes, by_author, sizeof(by_author)));
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_ADDED, NULL, indexes) == 6);
    for (size_t i = 0; i < 6; ++i) assert(indexes[i] == 5 - i);
    match(catalog, books, "世界", 0); match(catalog, books, "ping fan de shi jie", 0);
    match(catalog, books, "p f d s j", 0); match(catalog, books, "SANti", 1);
    match(catalog, books, "lcx", 1); match(catalog, books, "luyao", 0);
    match(catalog, books, "jane", 4); match(catalog, books, "jianai", 4);
    match(catalog, books, "lvming", 3); match(catalog, books, "lÜ ming", 3);
    match(catalog, books, "lu:ming", 3); match(catalog, books, "吕", 3);
    match(catalog, books, "南方来信.txt", 2);
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_TITLE, "absent", indexes) == 0);
    assert(folio_catalog_touch(catalog, books[0].path, 100));
    assert(folio_catalog_touch(catalog, books[1].path, 90)); // Clock correction cannot reverse recency.
    assert(folio_catalog_touch(catalog, books[3].path, 0));
    assert(folio_catalog_touch(catalog, books[4].path, 80));
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_RECENT, FOLIO_CATALOG_SORT_RECENT, "", indexes) == 4);
    const size_t recent[] = {4, 3, 1, 0}; assert(!memcmp(indexes, recent, sizeof(recent)));
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_UNREAD, FOLIO_CATALOG_SORT_TITLE, "", indexes) == 2);
    assert(indexes[0] == 2 && indexes[1] == 5);
    folio_catalog_options_t options = {FOLIO_CATALOG_RECENT, FOLIO_CATALOG_SORT_RECENT, NULL};
    indexes[2] = 999;
    assert(folio_catalog_select(catalog, books, 6, &options, indexes, 2) == 2 && indexes[0] == 4 && indexes[1] == 3 && indexes[2] == 999);
    assert(memcmp(books, original, sizeof(books)) == 0);
    assert(folio_catalog_save(catalog)); folio_catalog_free(catalog); catalog = folio_catalog_open();
    assert(catalog && !*folio_catalog_error(catalog));
    assert(select_books(catalog, books, 6, FOLIO_CATALOG_RECENT, FOLIO_CATALOG_SORT_RECENT, NULL, indexes) == 4);
    assert(!memcmp(indexes, recent, sizeof(recent)));
    metadata = folio_catalog_metadata(catalog, books[3].path);
    assert(metadata->read && metadata->lastread_utc == 0 && metadata->lastread_order > 0);
    assert(folio_catalog_sync(catalog, books, 6, 1000, false) && metadata_reads == 8);
    put_file(books[0].path, "changed", 7);
    assert(folio_catalog_sync(catalog, books, 6, 1001, false) && metadata_reads == 9);
    assert(folio_catalog_metadata(catalog, books[0].path)->read); // Path identity is retained.
    assert(folio_catalog_save(catalog));
    folio_book_t malformed = make_book("损坏", "epub");
    assert(!folio_catalog_sync(catalog, &malformed, 1, 10, false));
    assert(*folio_catalog_error(catalog) && !strcmp(folio_catalog_metadata(catalog, malformed.path)->title, "损坏"));
    unsigned reads = metadata_reads;
    assert(folio_catalog_sync(catalog, &malformed, 1, 11, false) && metadata_reads == reads);
    assert(!folio_catalog_sync(catalog, &malformed, 1, 11, true) && metadata_reads == reads + 1);
    folio_catalog_free(catalog);
    puts("catalog features: metadata cache, nonmutating filters/sorts, raw/pinyin/initial search, >3 recents, unknown-clock persistence passed");
}
static void test_boundaries(void) {
    folio_catalog_t* catalog = fresh(); folio_book_t book = make_book("边界", "txt");
    assert(folio_catalog_sync(catalog, &book, 1, 123, false));
    size_t indexes[128]; char query[FOLIO_CATALOG_QUERY_CAP + 1]; memset(query, 'a', sizeof(query)); query[sizeof(query) - 1] = 0;
    assert(select_books(catalog, &book, 1, FOLIO_CATALOG_ALL, FOLIO_CATALOG_SORT_TITLE, query, indexes) == 0);
    assert(!folio_catalog_select(catalog, &book, 1, NULL, indexes, 0));
    assert(!folio_catalog_select(catalog, &book, FOLIO_LIBRARY_LIMIT + 1, NULL, indexes, 128));
    assert(!folio_catalog_sync(catalog, NULL, 1, 123, false));
    assert(!folio_catalog_sync(catalog, &book, FOLIO_LIBRARY_LIMIT + 1, 123, false));
    folio_book_t duplicate[] = {book, book}; assert(!folio_catalog_sync(catalog, duplicate, 2, 123, false));
    folio_book_t broken = book; memset(broken.title, 'x', sizeof(broken.title));
    assert(!folio_catalog_sync(catalog, &broken, 1, 123, false));
    const char* bad_paths[] = {"/outside.txt", FOLIO_SD_ROOT "/books/../escape.txt", FOLIO_SD_ROOT "/books/", FOLIO_SD_ROOT "/books/a/b.txt", FOLIO_SD_ROOT "/books/a\nb.txt"};
    for (unsigned i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i) {
        broken = book; snprintf(broken.path, sizeof(broken.path), "%s", bad_paths[i]);
        assert(!folio_catalog_sync(catalog, &broken, 1, 123, false));
        assert(!folio_catalog_touch(catalog, bad_paths[i], 123));
    }
    assert(folio_catalog_select(NULL, &book, 1, NULL, indexes, 128) == 1);
    assert(select_books(NULL, &book, 1, FOLIO_CATALOG_RECENT, FOLIO_CATALOG_SORT_RECENT, NULL, indexes) == 0);
    assert(select_books(NULL, &book, 1, FOLIO_CATALOG_UNREAD, FOLIO_CATALOG_SORT_RECENT, NULL, indexes) == 1);
    assert(folio_catalog_sync(catalog, NULL, 0, 123, false));
    assert(folio_catalog_metadata(catalog, book.path)); // Temporarily removed book keeps its state.
    catalog->order = UINT64_MAX; assert(!folio_catalog_touch(catalog, book.path, 0));
    assert(!folio_catalog_sync(catalog, &book, 1, 0, false));
    folio_catalog_free(catalog);
    fail_alloc = true; assert(folio_catalog_open() == NULL); fail_alloc = false;
    assert(!folio_catalog_save(NULL) && !folio_catalog_dirty(NULL) && *folio_catalog_error(NULL));
    puts("catalog boundaries: invalid arrays, paths, query/capacity, clock/order overflow and allocation failure passed");
}
static void test_weread_metadata(void) {
    folio_catalog_t* catalog=fresh();folio_book_t books[]={make_book("wr-123","wrb"),make_book("ordinary","txt")};
    unsigned epub_reads=metadata_reads;weread_metadata_reads=0;
    assert(folio_catalog_sync(catalog,books,2,123,false));
    const folio_catalog_metadata_t* m=folio_catalog_metadata(catalog,books[0].path);
    assert(m&&!strcmp(m->title,"云端书名")&&!strcmp(m->author,"林舟"));
    assert(weread_metadata_reads==1&&metadata_reads==epub_reads);
    size_t indexes[128];
    assert(select_books(catalog,books,2,FOLIO_CATALOG_ALL,FOLIO_CATALOG_SORT_AUTHOR,"linzhou",indexes)==1&&indexes[0]==0);
    assert(folio_catalog_sync(catalog,books,2,124,false)&&weread_metadata_reads==1);
    assert(folio_catalog_save(catalog));folio_catalog_free(catalog);catalog=folio_catalog_open();
    m=folio_catalog_metadata(catalog,books[0].path);assert(m&&!strcmp(m->title,"云端书名")&&!strcmp(m->author,"林舟"));
    assert(folio_catalog_sync(catalog,books,2,125,true)&&weread_metadata_reads==2);
    folio_book_t bad=make_book("wr-broken","wrb");
    assert(!folio_catalog_sync(catalog,&bad,1,126,false)&&weread_metadata_reads==3);
    m=folio_catalog_metadata(catalog,bad.path);assert(m&&!strcmp(m->title,"wr-broken")&&!m->author[0]&&*folio_catalog_error(catalog));
    folio_catalog_free(catalog);
    puts("catalog WeRead: cache metadata dispatch, author search, cached roundtrip and damaged-pointer fallback passed");
}
static void test_recovery(void) {
    folio_catalog_t* catalog = fresh(); folio_book_t book = make_book("恢复", "txt");
    assert(folio_catalog_sync(catalog, &book, 1, 123, false));
    assert(folio_catalog_save(catalog)); int first_slot = catalog->active_slot;
    assert(folio_catalog_touch(catalog, book.path, 124));
    fail_write_after = 1; assert(!folio_catalog_save(catalog)); fail_write_after = -1;
    assert(catalog->dirty && catalog->active_slot == first_slot && *catalog->error);
    folio_catalog_t* reopened = folio_catalog_open();
    assert(reopened && *folio_catalog_error(reopened) && !folio_catalog_metadata(reopened, book.path)->read);
    folio_catalog_free(reopened);
    assert(folio_catalog_save(catalog) && !catalog->dirty && !*catalog->error);
    unsigned latest = (unsigned)catalog->active_slot;
    // A syntactically complete but bit-corrupted latest record falls back.
    FILE* file = fopen(catalog_slot(latest), "r+b"); assert(file && fseek(file, 30, SEEK_SET) == 0);
    int byte = fgetc(file); assert(byte != EOF && fseek(file, -1, SEEK_CUR) == 0 && fputc(byte ^ 1, file) != EOF && fclose(file) == 0);
    folio_catalog_free(catalog); catalog = folio_catalog_open();
    assert(catalog && *catalog->error && !folio_catalog_metadata(catalog, book.path)->read);
    put_file(catalog_slot(0), "broken", 6); put_file(catalog_slot(1), "bad", 3);
    folio_catalog_free(catalog); catalog = folio_catalog_open();
    assert(catalog && catalog->count == 0 && *catalog->error);
    assert(folio_catalog_sync(catalog, &book, 1, 125, false));
    assert(folio_catalog_save(catalog));
    unsigned target = catalog->active_slot == 0 ? 1 : 0;
    assert(remove(catalog_slot(target)) == 0 && mkdir(catalog_slot(target), 0700) == 0);
    assert(folio_catalog_touch(catalog, book.path, 126));
    assert(!folio_catalog_save(catalog) && catalog->dirty && *catalog->error);
    reopened = folio_catalog_open();
    assert(reopened && *reopened->error && folio_catalog_metadata(reopened, book.path));
    folio_catalog_free(reopened);
    assert(rmdir(catalog_slot(target)) == 0 && folio_catalog_save(catalog));
    folio_catalog_free(catalog);
    puts("catalog recovery: interrupted write, corrupt latest/both slots, unwritable destination and retry passed");
}
static void test_capacity(void) {
    folio_catalog_t* catalog = fresh(); folio_book_t books[FOLIO_LIBRARY_LIMIT];
    for (unsigned batch = 0; batch < 3; ++batch) {
        for (unsigned i = 0; i < FOLIO_LIBRARY_LIMIT; ++i) {
            char name[32]; snprintf(name, sizeof(name), "book-%03u", batch * FOLIO_LIBRARY_LIMIT + i);
            books[i] = make_book(name, "txt");
        }
        assert(folio_catalog_sync(catalog, books, FOLIO_LIBRARY_LIMIT, 100 + batch, false));
        assert(catalog->count == (batch ? FOLIO_CATALOG_LIMIT : FOLIO_LIBRARY_LIMIT));
        for (unsigned i = 0; i < FOLIO_LIBRARY_LIMIT; ++i) assert(folio_catalog_metadata(catalog, books[i].path));
    }
    assert(!folio_catalog_metadata(catalog, FOLIO_SD_ROOT "/books/book-000.txt"));
    assert(folio_catalog_metadata(catalog, FOLIO_SD_ROOT "/books/book-128.txt"));
    // Keep old entries that occur late in the supplied array while admitting
    // new books first: eviction must protect the entire current scan up front.
    for (unsigned i = 0; i < FOLIO_LIBRARY_LIMIT; ++i) {
        char name[32]; snprintf(name, sizeof(name), "book-%03u", i < 64 ? 384 + i : 128 + i - 64);
        books[i] = make_book(name, "txt");
    }
    assert(folio_catalog_sync(catalog, books, FOLIO_LIBRARY_LIMIT, 104, false));
    for (unsigned i = 0; i < FOLIO_LIBRARY_LIMIT; ++i) assert(folio_catalog_metadata(catalog, books[i].path));
    assert(!folio_catalog_metadata(catalog, FOLIO_SD_ROOT "/books/book-192.txt"));
    assert(folio_catalog_save(catalog)); folio_catalog_free(catalog); catalog = folio_catalog_open();
    assert(catalog && !*catalog->error && catalog->count == FOLIO_CATALOG_LIMIT);
    size_t indexes[FOLIO_LIBRARY_LIMIT];
    assert(folio_catalog_select(catalog, books, FOLIO_LIBRARY_LIMIT, NULL, indexes, FOLIO_LIBRARY_LIMIT) == FOLIO_LIBRARY_LIMIT);
    for (unsigned i = 0; i < FOLIO_LIBRARY_LIMIT; ++i) assert(indexes[i] == (i < 64 ? i + 64 : i - 64));
    folio_catalog_free(catalog);
    puts("catalog limits: 128 visible books, 256 retained records, nonactive eviction and full-index roundtrip passed");
}
static void test_corrupt_records(void) {
    folio_catalog_t* catalog = fresh(); folio_book_t book = make_book("校验", "txt");
    assert(folio_catalog_sync(catalog, &book, 1, 123, false) && folio_catalog_save(catalog));
    uint8_t original[CATALOG_HEADER_SIZE + CATALOG_RECORD_SIZE + 4];
    FILE* file = fopen(catalog_slot(0), "rb"); assert(file);
    assert(fread(original, 1, sizeof(original), file) == sizeof(original) && fclose(file) == 0);
    folio_catalog_free(catalog);
    for (size_t cut = 0; cut < sizeof(original); cut += 7) {
        put_file(catalog_slot(0), original, cut); catalog = folio_catalog_open();
        assert(catalog && !catalog->count && *catalog->error); folio_catalog_free(catalog);
    }
    uint32_t random = 0x536c;
    for (unsigned i = 0; i < 160; ++i) {
        uint8_t mutated[sizeof(original)]; memcpy(mutated, original, sizeof(mutated));
        random = random * 1664525u + 1013904223u;
        size_t position = random % sizeof(mutated); mutated[position] ^= (uint8_t)(1u << (random % 8));
        put_file(catalog_slot(0), mutated, sizeof(mutated)); catalog = folio_catalog_open();
        assert(catalog && !catalog->count && *catalog->error); folio_catalog_free(catalog);
    }
    // Valid checksums never legitimize an unbounded count or invalid flags/path.
    const size_t invalid_fields[] = {12, CATALOG_HEADER_SIZE, CATALOG_HEADER_SIZE + 548};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t mutated[sizeof(original)]; memcpy(mutated, original, sizeof(mutated));
        write32(mutated + invalid_fields[i], UINT32_MAX);
        write32(mutated + sizeof(mutated) - 4, hash_bytes(HASH_START, mutated, sizeof(mutated) - 4));
        put_file(catalog_slot(0), mutated, sizeof(mutated)); catalog = folio_catalog_open();
        assert(catalog && !catalog->count && *catalog->error); folio_catalog_free(catalog);
    }
    puts("catalog corruption: truncations, 160 byte mutations and semantically invalid checksummed records rejected");
}
static void test_v1_reading_migration(void) {
    folio_catalog_t* catalog=fresh();folio_book_t book=make_book("旧版阅读", "txt");
    assert(folio_catalog_sync(catalog,&book,1,123,false));
    assert(folio_catalog_touch(catalog,book.path,456));
    assert(folio_catalog_save(catalog));
    uint8_t old[24+552+4]={0},record[CATALOG_RECORD_SIZE];
    write32(old,CATALOG_MAGIC);write32(old+4,1);write32(old+8,7);write32(old+12,1);write64(old+16,catalog->order);
    encode_entry(&catalog->entries[0],record);memcpy(old+24,record,552);
    write32(old+sizeof(old)-4,hash_bytes(HASH_START,old,sizeof(old)-4));
    folio_catalog_free(catalog);remove_slots();put_file(catalog_slot(0),old,sizeof(old));
    catalog=folio_catalog_open();assert(catalog && !*catalog->error);
    const folio_catalog_metadata_t* m=folio_catalog_metadata(catalog,book.path);
    assert(m && m->read && m->lastread_utc==456 && m->reading_seconds==0);
    assert(folio_catalog_add_reading(catalog,book.path,71));
    assert(folio_catalog_save(catalog));folio_catalog_free(catalog);catalog=folio_catalog_open();
    m=folio_catalog_metadata(catalog,book.path);assert(m && m->reading_seconds==71 && m->lastread_utc==456);
    assert(folio_catalog_add_reading(catalog,book.path,UINT64_MAX));assert(m->reading_seconds==UINT64_MAX);
    assert(!folio_catalog_add_reading(NULL,book.path,1));
    const char* relocated=FOLIO_SD_ROOT "/books/迁移后 [wr-61].epub";
    assert(folio_catalog_relocate(catalog,book.path,relocated)&&folio_catalog_save(catalog));
    folio_catalog_free(catalog);catalog=folio_catalog_open();m=folio_catalog_metadata(catalog,relocated);
    assert(m&&m->reading_seconds==UINT64_MAX&&m->lastread_utc==456&&m->read);
    folio_catalog_free(catalog);puts("catalog v1 migration: recency preserved, per-book seconds persist, overflow saturates");
}
static void test_pinyin_table(void) {
    size_t count = sizeof(catalog_pinyin_map) / sizeof(catalog_pinyin_map[0]);
    assert(count == 44348);
    for (size_t i = 0; i < count; ++i) {
        uint32_t value = catalog_pinyin_map[i];
        assert((value & 511) < sizeof(catalog_pinyin_syllables) / sizeof(catalog_pinyin_syllables[0]));
        assert(!i || catalog_pinyin_map[i - 1] >> 9 < value >> 9);
        assert(pinyin(value >> 9) == catalog_pinyin_syllables[value & 511]);
    }
    assert(!pinyin('A') && !pinyin(0x10ffff));
    assert(!strcmp(pinyin(0x4e2d), "zhong") && !strcmp(pinyin(0x5415), "lv"));
    puts("pinyin table: all 44,348 mappings sorted, bounded and binary-searchable passed");
}
int main(void) {
    assert(mkdir(FOLIO_SD_ROOT, 0700) == 0 && mkdir(FOLIO_SD_ROOT "/books", 0700) == 0);
    test_v1_reading_migration(); test_pinyin_table(); test_features(); test_weread_metadata(); test_boundaries(); test_recovery(); test_capacity(); test_corrupt_records();
    printf("Catalog tests passed; catalog RAM %zu bytes, flash pinyin table %zu bytes\n", sizeof(folio_catalog_t),
           sizeof(catalog_pinyin_map) + sizeof(catalog_pinyin_syllables));
    return 0;
}
