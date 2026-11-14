#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "folio_weread_cache.h"

static bool cancel_now;
static int fail_write_after = -1, fail_sync_after = -1, fail_rename_after = -1;
static unsigned write_calls, sync_calls, rename_calls;
static size_t checked_write(const void* p, size_t n, size_t count, FILE* f) {
    ++write_calls;
    if (fail_write_after == 0) { fail_write_after = -1; errno = EIO; return 0; }
    if (fail_write_after > 0) --fail_write_after;
    return fwrite(p, n, count, f);
}
static int checked_sync(int fd) {
    ++sync_calls;
    if (fail_sync_after == 0) { fail_sync_after = -1; errno = EIO; return -1; }
    if (fail_sync_after > 0) --fail_sync_after;
    return fsync(fd);
}
static int checked_rename(const char* a, const char* b) {
    ++rename_calls;
    if (fail_rename_after == 0) { fail_rename_after = -1; errno = EIO; return -1; }
    if (fail_rename_after > 0) --fail_rename_after;
    return rename(a, b);
}
#define folio_io_cancelled test_io_cancelled
#define folio_storage_cancelled test_storage_cancelled
#define fwrite checked_write
#define fsync checked_sync
#define rename checked_rename
#include "../../main/book/folio_weread_cache.c"
#undef folio_io_cancelled
#undef folio_storage_cancelled
#undef fwrite
#undef fsync
#undef rename
bool test_io_cancelled(void) { return cancel_now; }
bool test_storage_cancelled(void) { return false; }

static char error[256], path[256];
static const folio_weread_chapter_t toc[] = {{"10", "第一章"}, {"20", "第二章"}, {"30", "第三章"}};
static void expect(bool okay) { if (!okay) fprintf(stderr, "CACHE: %s\n", error); assert(okay); }
static folio_weread_cache_writer_t* begin(const char* id) {
    folio_weread_cache_writer_t* w = folio_weread_cache_begin(id, "测试书", "作者", toc, 3, error, sizeof(error));
    if (!w) fprintf(stderr, "BEGIN: %s\n", error);
    assert(w); return w;
}
static void write_chapter(folio_weread_cache_writer_t* w, unsigned i, const char* raw, folio_weread_content_kind_t kind) {
    expect(folio_weread_cache_write_chapter(w, i, (const uint8_t*)raw, strlen(raw), kind, error, sizeof(error)));
}
static void publish(folio_weread_cache_writer_t* w) {
    expect(folio_weread_cache_publish(w, path, sizeof(path), error, sizeof(error)));
}
static char* body(void) {
    folio_epub_t p; expect(folio_weread_cache_prepare(path, &p, error, sizeof(error)));
    FILE* f = fopen(p.text_path, "rb"); assert(f); assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n >= 0);
    assert(!fseek(f, 0, SEEK_SET)); char* text = calloc((size_t)n + 1, 1); assert(text);
    assert(fread(text, 1, (size_t)n, f) == (size_t)n); assert(!fclose(f)); free(p.chapters); return text;
}
static uint32_t utf16_prefix(const char* raw, const char* marker) {
    const char* stop = strstr(raw, marker); assert(stop); uint32_t n = 0;
    for (const uint8_t* p = (const uint8_t*)raw; p < (const uint8_t*)stop;) {
        uint32_t cp; size_t size; assert(scalar(p, strlen((const char*)p), &cp, &size)); n += cp > 0xFFFF ? 2 : 1; p += size;
    }
    return n;
}
static void map_roundtrip(uint32_t local, uint32_t expected) {
    folio_weread_position_t position; uint32_t back;
    expect(folio_weread_cache_local_to_cloud(path, local, &position, error, sizeof(error)));
    assert(position.offset_utf16 == expected);
    expect(folio_weread_cache_cloud_to_local(path, &position, &back, error, sizeof(error))); assert(back == local);
}
static void test_exact_mapping(void) {
    const char* raw = "<?xml version='1.0'?><html><head><title>不可见</title></head><body><p>中A&amp;😀\r\nB&#x1F642;</p><!--备注--><p>后&nbsp;页</p></body></html>";
    folio_weread_cache_writer_t* w = begin("map"); write_chapter(w, 0, raw, FOLIO_WEREAD_XHTML); publish(w);
    char* text = body(); assert(!strcmp(text, "中A&😀\nB🙂\n后\xC2\xA0页\n\n"));
    const char* tokens[] = {"中", "A", "&amp;", "😀", "\r\n", "B&#", "&#x1F642;", "</p>", "后", "&nbsp;", "页"};
    const uint32_t local[] = {0, 3, 4, 5, 9, 10, 11, 15, 16, 19, 21};
    for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); ++i) map_roundtrip(local[i], utf16_prefix(raw, tokens[i]));
    folio_weread_position_t pos;
    assert(!folio_weread_cache_local_to_cloud(path, 1, &pos, error, sizeof(error)));
    expect(folio_weread_cache_local_to_cloud(path, 5, &pos, error, sizeof(error)));
    assert(folio_weread_cache_matches_raw(&pos, (const uint8_t*)raw, strlen(raw), FOLIO_WEREAD_XHTML));
    assert(!folio_weread_cache_matches_raw(&pos, (const uint8_t*)raw, strlen(raw), FOLIO_WEREAD_TEXT));
    uint32_t out; ++pos.offset_utf16; assert(!folio_weread_cache_cloud_to_local(path, &pos, &out, error, sizeof(error)));
    pos.offset_utf16 = utf16_prefix(raw, "&amp;") + 1; assert(!folio_weread_cache_cloud_to_local(path, &pos, &out, error, sizeof(error)));
    pos.offset_utf16 = utf16_prefix(raw, "不可见"); assert(!folio_weread_cache_cloud_to_local(path, &pos, &out, error, sizeof(error)));
    free(text); folio_weread_cache_close(w);
}
static void test_partial_resume_change(void) {
    folio_weread_cache_writer_t* w = begin("partial"); write_chapter(w, 2, "Tail😀", FOLIO_WEREAD_TEXT);
    assert(!folio_weread_cache_has_chapter(w, 0)); assert(folio_weread_cache_has_chapter(w, 2)); publish(w);
    uint64_t old_revision = folio_weread_cache_revision(path); assert(old_revision);
    folio_weread_position_t old; expect(folio_weread_cache_local_to_cloud(path, 4, &old, error, sizeof(error)));
    folio_epub_t p; expect(folio_weread_cache_prepare(path, &p, error, sizeof(error)));
    assert(p.chapter_count == 1 && !strcmp(p.chapters[0].title, "第三章") && !p.chapters[0].offset); free(p.chapters);
    folio_weread_cache_close(w); w = begin("partial"); assert(folio_weread_cache_has_chapter(w, 2));
    write_chapter(w, 0, "First\r\nLine", FOLIO_WEREAD_TEXT); publish(w);
    assert(folio_weread_cache_revision(path) != old_revision); uint32_t shifted;
    expect(folio_weread_cache_cloud_to_local(path, &old, &shifted, error, sizeof(error))); assert(shifted == 15);
    char* text = body(); assert(!strcmp(text, "First\nLine\nTail😀\n")); free(text);
    write_chapter(w, 2, "Changed😀", FOLIO_WEREAD_TEXT); publish(w);
    assert(!folio_weread_cache_cloud_to_local(path, &old, &shifted, error, sizeof(error)));
    assert(folio_weread_cache_chapter_available(path, "30")); assert(!folio_weread_cache_chapter_available(path, "20"));
    folio_weread_cache_close(w);
}
static void test_crash_and_corruption(void) {
    folio_weread_cache_writer_t* w = begin("crash"); write_chapter(w, 0, "Old body", FOLIO_WEREAD_TEXT); publish(w);
    char published_path[256]; strcpy(published_path, path); uint64_t revision = folio_weread_cache_revision(path);
    write_chapter(w, 1, "New body", FOLIO_WEREAD_TEXT);
    fail_sync_after = 0; assert(!folio_weread_cache_publish(w, path, sizeof(path), error, sizeof(error))); strcpy(path, published_path);
    assert(folio_weread_cache_revision(path) == revision);
    fail_rename_after = 0; assert(!folio_weread_cache_publish(w, path, sizeof(path), error, sizeof(error))); strcpy(path, published_path);
    assert(folio_weread_cache_revision(path) == revision);
    publish(w); uint64_t new_revision = folio_weread_cache_revision(path); assert(new_revision != revision);
    pointer_t pointers_before[2]; pointers("crash", pointers_before);
    unsigned latest = pointers_before[1].sequence > pointers_before[0].sequence ? 1 : 0;
    FILE* f = fopen(path, "r+b"); assert(f); assert(!fseek(f, (long)(latest * sizeof(pointer_t) + 8), SEEK_SET));
    assert(fputc(0xFD, f) != EOF); assert(!fclose(f));
    assert(folio_weread_cache_revision(path) == revision); // A torn pointer falls back.
    publish(w); assert(folio_weread_cache_revision(path) == new_revision);
    manifest_t* latest_manifest = open_manifest(path, error, sizeof(error), false); assert(latest_manifest);
    char body_path[256]; assert(object_path(body_path, "crash", 'b', latest_manifest->body_hash)); free(latest_manifest);
    f = fopen(body_path, "r+b"); assert(f); assert(fputc('?', f) != EOF); assert(!fclose(f));
    assert(folio_weread_cache_revision(path) == revision); // Bad derived text also falls back.
    publish(w); assert(folio_weread_cache_revision(path) == new_revision); // Verified source repairs it.
    char chapter_path[256]; object_path(chapter_path, "crash", 'c', w->manifest.records[0].key);
    f = fopen(chapter_path, "r+b"); assert(f); assert(!fseek(f, sizeof(chapter_header_t), SEEK_SET));
    assert(fputc('X', f) != EOF); assert(!fclose(f));
    folio_weread_position_t pos; assert(!folio_weread_cache_local_to_cloud(path, 0, &pos, error, sizeof(error)));
    assert(!folio_weread_cache_chapter_available(path, "10"));
    folio_weread_cache_close(w); w = begin("crash"); assert(!folio_weread_cache_has_chapter(w, 0)); assert(folio_weread_cache_has_chapter(w, 1));
    write_chapter(w, 0, "Old body", FOLIO_WEREAD_TEXT); publish(w);
    assert(folio_weread_cache_chapter_available(path, "10"));
    folio_weread_cache_close(w);
}
static void publish_without_writes(folio_weread_cache_writer_t* w) {
    unsigned writes = write_calls, syncs = sync_calls, renames = rename_calls;
    pointer_t before[2], after[2]; pointers(w->manifest.book_id, before);
    publish(w); pointers(w->manifest.book_id, after);
    assert(write_calls == writes && sync_calls == syncs && rename_calls == renames);
    assert(!memcmp(before, after, sizeof(before)));
}
static void damage_byte(const char* target, long offset) {
    FILE* f = fopen(target, "r+b"); assert(f);
    assert(!fseek(f, offset, SEEK_SET)); int old = fgetc(f); assert(old != EOF);
    assert(!fseek(f, offset, SEEK_SET)); assert(fputc(old ^ 1, f) != EOF); assert(!fclose(f));
}
static void test_publish_reuse_integrity(void) {
    folio_weread_cache_writer_t* w = begin("reuse");
    write_chapter(w, 0, "First", FOLIO_WEREAD_TEXT);
    write_chapter(w, 1, "Second", FOLIO_WEREAD_TEXT); publish(w);
    uint64_t revision = folio_weread_cache_revision(path); assert(revision);
    write_chapter(w, 0, "First", FOLIO_WEREAD_TEXT);
    publish_without_writes(w); assert(folio_weread_cache_revision(path) == revision);
    // A fresh writer has a different resume sequence but identical content.
    folio_weread_cache_close(w); w = begin("reuse");
    publish_without_writes(w); assert(folio_weread_cache_revision(path) == revision);
    write_chapter(w, 0, "Changed", FOLIO_WEREAD_TEXT); publish(w);
    assert(folio_weread_cache_revision(path) != revision);
    char* text = body(); assert(!strcmp(text, "Changed\nSecond\n")); free(text);
    // Book title, author, and even an undownloaded chapter title are content.
    folio_weread_chapter_t renamed[3]; memcpy(renamed, toc, sizeof(renamed));
    for (unsigned change = 0; change < 3; ++change) {
        revision = folio_weread_cache_revision(path); folio_weread_cache_close(w);
        if (change == 2) strcpy(renamed[2].title, "第三章（新版）");
        w = folio_weread_cache_begin("reuse", "改名", change ? "新作者" : "作者",
            renamed, 3, error, sizeof(error)); assert(w); publish(w);
        assert(folio_weread_cache_revision(path) != revision);
    }
    revision = folio_weread_cache_revision(path);
    manifest_t* m = open_manifest(path, error, sizeof(error), true); assert(m);
    assert(!strcmp(m->title, "改名") && !strcmp(m->author, "新作者") &&
           !strcmp(m->records[2].chapter.title, "第三章（新版）"));
    char manifest_path[256], body_path[256], published_path[256]; strcpy(published_path, path);
    assert(object_path(manifest_path, "reuse", 'm', m->checksum));
    assert(object_path(body_path, "reuse", 'b', m->body_hash)); free(m);
    // Same-size damage must be repaired, not accepted as an unchanged cache.
    damage_byte(body_path, 0); unsigned writes = write_calls; publish(w);
    assert(write_calls > writes && folio_weread_cache_revision(path) == revision);
    text = body(); assert(!strcmp(text, "Changed\nSecond\n")); free(text);
    damage_byte(manifest_path, (long)offsetof(manifest_t, title)); writes = write_calls; publish(w);
    assert(write_calls > writes && folio_weread_cache_revision(path) == revision);
    // Intact published text cannot mask corrupt raw or converted chapter bytes.
    char chapter_path[256]; assert(object_path(chapter_path, "reuse", 'c', w->manifest.records[0].key));
    for (unsigned converted = 0; converted < 2; ++converted) {
        damage_byte(chapter_path, (long)sizeof(chapter_header_t) +
            (converted ? w->manifest.records[0].source_size : 0));
        assert(!folio_weread_cache_publish(w, path, sizeof(path), error, sizeof(error)) && !path[0]);
        strcpy(path, published_path); assert(folio_weread_cache_revision(path) == revision);
        assert(!folio_weread_cache_chapter_available(path, "10"));
        write_chapter(w, 0, "Changed", FOLIO_WEREAD_TEXT);
        publish_without_writes(w); assert(folio_weread_cache_revision(path) == revision);
    }
    cancel_now = true;
    assert(!folio_weread_cache_publish(w, path, sizeof(path), error, sizeof(error)) && !path[0]);
    cancel_now = false; strcpy(path, published_path);
    publish_without_writes(w); folio_weread_cache_close(w);
}
static void test_bounds_and_failures(void) {
    assert(!folio_weread_cache_begin("../evil", "x", "", toc, 3, error, sizeof(error)));
    assert(!folio_weread_cache_begin("valid", "x", "", toc, 513, error, sizeof(error)));
    folio_weread_chapter_t duplicate[2] = {toc[0], toc[0]};
    assert(!folio_weread_cache_begin("dupe", "x", "", duplicate, 2, error, sizeof(error)));
    folio_weread_cache_writer_t* w = begin("bounds");
    assert(!folio_weread_cache_begin("busy", "x", "", toc, 3, error, sizeof(error)));
    uint8_t* large = malloc(FOLIO_WEREAD_CHAPTER_BYTES + 1); assert(large); memset(large, 'a', FOLIO_WEREAD_CHAPTER_BYTES + 1);
    assert(!folio_weread_cache_write_chapter(w, 0, large, FOLIO_WEREAD_CHAPTER_BYTES + 1, FOLIO_WEREAD_TEXT, error, sizeof(error)));
    expect(folio_weread_cache_write_chapter(w, 0, large, FOLIO_WEREAD_CHAPTER_BYTES, FOLIO_WEREAD_TEXT, error, sizeof(error)));
    free(large);
    const char* bad[] = {"<p>bad &unknown;</p>", "<p>bad &#xD800;</p>", "<p>bad &#0;</p>", "<p a='unterminated>bad", "<!--unfinished", "<head>unfinished", "<![CDATA[unterminated", "\xF0\x80\x80\x80"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!folio_weread_cache_write_chapter(w, 1, (const uint8_t*)bad[i], strlen(bad[i]), FOLIO_WEREAD_XHTML, error, sizeof(error)));
    fail_write_after = 0; assert(!folio_weread_cache_write_chapter(w, 1, (const uint8_t*)"Retry", 5, FOLIO_WEREAD_TEXT, error, sizeof(error)));
    assert(!folio_weread_cache_has_chapter(w, 1));
    fail_sync_after = 0; assert(!folio_weread_cache_write_chapter(w, 1, (const uint8_t*)"Retry", 5, FOLIO_WEREAD_TEXT, error, sizeof(error)));
    cancel_now = true; assert(!folio_weread_cache_write_chapter(w, 1, (const uint8_t*)"Retry", 5, FOLIO_WEREAD_TEXT, error, sizeof(error))); cancel_now = false;
    write_chapter(w, 1, "Retry", FOLIO_WEREAD_TEXT); publish(w);
    write_chapter(w, 2, "Full", FOLIO_WEREAD_TEXT);
    folio_weread_cache_close(w);
    /* The writer measures the book directory when it starts and then accounts
     * its own bytes, so a directory that is already over budget must fail on the
     * first write of the next session. */
    char quota_path[256]; assert(named_path(quota_path, "bounds", "reserved.bin"));
    FILE* f = fopen(quota_path, "wb"); assert(f); assert(!ftruncate(fileno(f), FOLIO_WEREAD_BOOK_BYTES)); assert(!fclose(f));
    assert(!folio_weread_cache_begin("bounds", "x", "", toc, 3, error, sizeof(error)));
    assert(!unlink(quota_path));
    w = begin("bounds");
    write_chapter(w, 2, "Full", FOLIO_WEREAD_TEXT);
    folio_weread_cache_close(w);
    char id[64]; assert(!folio_weread_cache_book_id(BOOK_ROOT "/../wr-bounds.wrb", id, sizeof(id)));
    assert(!folio_weread_cache_book_id(BOOK_ROOT "/wr-../evil.wrb", id, sizeof(id)));
}
static void test_checkpoint_torn(void) {
    folio_weread_cache_writer_t* w = begin("resume"); write_chapter(w, 0, "First", FOLIO_WEREAD_TEXT);
    int first_slot = w->resume_slot;
    write_chapter(w, 1, "Second", FOLIO_WEREAD_TEXT);
    int second_slot = w->resume_slot; assert(second_slot != first_slot);
    char checkpoint_path[256]; resume_path(checkpoint_path, "resume", (unsigned)second_slot);
    FILE* f = fopen(checkpoint_path, "wb"); assert(f); assert(fwrite("torn", 1, 4, f) == 4); assert(!fclose(f));
    folio_weread_cache_close(w); w = begin("resume"); assert(folio_weread_cache_has_chapter(w, 0)); assert(!folio_weread_cache_has_chapter(w, 1));
    write_chapter(w, 1, "Second", FOLIO_WEREAD_TEXT); publish(w); folio_weread_cache_close(w);
}
static void test_revision_and_maximum_toc(void) {
    folio_weread_cache_writer_t* w = begin("revision"); write_chapter(w, 0, "<p>A</p>", FOLIO_WEREAD_XHTML); publish(w);
    uint64_t first = folio_weread_cache_revision(path); folio_weread_position_t old;
    expect(folio_weread_cache_local_to_cloud(path, 0, &old, error, sizeof(error)));
    publish(w); assert(folio_weread_cache_revision(path) == first);
    folio_weread_cache_close(w); w = begin("revision"); publish(w); assert(folio_weread_cache_revision(path) == first);
    write_chapter(w, 0, "<p class='changed'>A</p>", FOLIO_WEREAD_XHTML); publish(w);
    assert(folio_weread_cache_revision(path) != first); uint32_t offset;
    assert(!folio_weread_cache_cloud_to_local(path, &old, &offset, error, sizeof(error)));
    for (unsigned i = 0; i < 10; ++i) { char text[32]; snprintf(text, sizeof(text), "Changed %u", i); write_chapter(w, 0, text, FOLIO_WEREAD_TEXT); publish(w); }
    assert(collect(&w->manifest));
    char root[256]; book_dir(root, "revision"); DIR* dir = opendir(root); assert(dir);
    unsigned chapters = 0; struct dirent* item;
    while ((item = readdir(dir))) if (!strncmp(item->d_name, "c-", 2)) ++chapters;
    closedir(dir); assert(chapters <= 2); folio_weread_cache_close(w);
    folio_weread_chapter_t* many = calloc(FOLIO_WEREAD_CHAPTER_LIMIT, sizeof(*many)); assert(many);
    for (size_t i = 0; i < FOLIO_WEREAD_CHAPTER_LIMIT; ++i) {
        snprintf(many[i].uid, sizeof(many[i].uid), "uid%zu", i); snprintf(many[i].title, sizeof(many[i].title), "Chapter %zu", i);
    }
    w = folio_weread_cache_begin("max_toc", "Many", "", many, FOLIO_WEREAD_CHAPTER_LIMIT, error, sizeof(error)); assert(w);
    write_chapter(w, FOLIO_WEREAD_CHAPTER_LIMIT - 1, "Last", FOLIO_WEREAD_TEXT); publish(w);
    folio_epub_t p; expect(folio_weread_cache_prepare(path, &p, error, sizeof(error)));
    assert(p.chapter_count == 1 && !strcmp(p.chapters[0].title, "Chapter 511")); free(p.chapters);
    folio_weread_cache_close(w); free(many);
}
static void test_paths_and_invalid_pointer(void) {
    char upper[256], lower[256], id[64];
    assert(folio_weread_cache_path("BookA", upper, sizeof(upper)));
    assert(folio_weread_cache_path("booka", lower, sizeof(lower)));
    assert(strcmp(upper, lower) && strcasecmp(upper, lower));
    assert(folio_weread_cache_book_id(upper, id, sizeof(id)) && !strcmp(id, "BookA"));
    assert(!folio_weread_cache_book_id(BOOK_ROOT "/wr-610062.wrb", id, sizeof(id)));
    assert(!folio_weread_cache_book_id(BOOK_ROOT "/wr-2e2e.wrb", id, sizeof(id)));
    assert(!folio_weread_cache_path("bad/path", lower, sizeof(lower)));
    assert(!folio_weread_cache_path("valid", lower, 3));
    folio_weread_cache_writer_t* w = begin("pointer"); write_chapter(w, 0, "Valid text", FOLIO_WEREAD_TEXT); publish(w);
    FILE* f = fopen(path, "ab"); assert(f); char bad[200] = {0}; assert(fwrite(bad, 1, sizeof(bad), f) == sizeof(bad)); assert(!fclose(f));
    assert(!folio_weread_cache_publish(w, lower, sizeof(lower), error, sizeof(error)));
    assert(!folio_weread_cache_revision(path)); folio_weread_cache_close(w);
    assert(!unlink(path));
    assert(!symlink("/private/tmp", path));
    folio_epub_t p; assert(!folio_weread_cache_prepare(path, &p, error, sizeof(error))); assert(!unlink(path));
    char victim[256]; snprintf(victim, sizeof(victim), FOLIO_SD_ROOT "/untouched.txt");
    f = fopen(victim, "wb"); assert(f); assert(fwrite("Keep", 1, 4, f) == 4); assert(!fclose(f));
    assert(!link(victim, path));
    w = begin("pointer"); assert(!folio_weread_cache_publish(w, lower, sizeof(lower), error, sizeof(error)));
    folio_weread_cache_close(w); char kept[5] = {0}; f = fopen(victim, "rb"); assert(f);
    assert(fread(kept, 1, 4, f) == 4 && !strcmp(kept, "Keep")); assert(!fclose(f)); assert(!unlink(path));
}
static void test_converter_adversarial(void) {
    uint32_t state = 0x945A3211u;
    uint8_t bytes[128];
    for (unsigned trial = 0; trial < 2000; ++trial) {
        size_t n = trial % sizeof(bytes) + 1;
        for (size_t i = 0; i < n; ++i) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; bytes[i] = (uint8_t)state; }
        sink_t sink = {0}; (void)convert(bytes, n, trial & 1u, &sink);
    }
    const uint8_t bom[] = {0xEF, 0xBB, 0xBF, 'A', '\r', '\n', 0xF0, 0x9F, 0x99, 0x82};
    sink_t sink = {.map = true, .target = 2}; assert(convert(bom, sizeof(bom), FOLIO_WEREAD_TEXT, &sink));
    assert(sink.found && sink.result == 4 && sink.output_size == 6);
    /* CDATA is literal character data outside a skipped element: its payload is
     * kept verbatim, while inside <style> the payload is not reading text. */
    const char* sample = "<p>A<![CDATA[<b>B&amp;</b>]]>C</p><style><![CDATA[p{color:red}]]></style>";
    FILE* out = tmpfile(); assert(out);
    sink_t cdata = {.write = out};
    assert(convert((const uint8_t*)sample, strlen(sample), FOLIO_WEREAD_XHTML, &cdata));
    rewind(out); char got[128] = {0};
    assert(fread(got, 1, sizeof(got) - 1, out) > 0); assert(!fclose(out));
    assert(strstr(got, "A<b>B&amp;</b>C") && !strstr(got, "color:red"));
}
static void test_epub_export_recovery(void) {
    folio_weread_cache_writer_t* w=begin("export");
    write_chapter(w,0,"<p>甲&amp;乙</p>\n\n<p>第二段</p>",FOLIO_WEREAD_XHTML);publish(w);folio_weread_cache_close(w);
    char legacy[256],epub[256],hidden[256],backup[256],id[64];strcpy(legacy,path);
    assert(!folio_wr_path_id(BOOK_ROOT "/../wr-61.wrb",id,sizeof(id)));
    assert(!folio_wr_path_id(BOOK_ROOT "/bad [wr-2f].epub",id,sizeof(id)));
    uint64_t revision=folio_weread_cache_revision(path);
    fail_sync_after=0;assert(!folio_weread_cache_export(path,epub,sizeof(epub),error,sizeof(error)));assert(!access(legacy,F_OK));
    expect(folio_weread_cache_export(path,epub,sizeof(epub),error,sizeof(error)));
    assert(access(legacy,F_OK)!=0 && folio_weread_cache_revision(epub)==revision);
    assert(folio_wr_aux(epub,"entry.wrb",hidden,sizeof(hidden))&&!access(hidden,F_OK));
    assert(folio_wr_aux(epub,"export.bak",backup,sizeof(backup)));
    // A failed replacement must restore the previously complete EPUB.
    fail_rename_after=1;assert(!folio_weread_cache_export(epub,path,sizeof(path),error,sizeof(error)));
    assert(!access(epub,F_OK));
    // Simulated loss between backup rename and publication is recovered on scan.
    assert(!rename(epub,backup));folio_weread_cache_migrate();assert(!access(epub,F_OK)&&access(backup,F_OK)!=0);
    assert(!unlink(epub));folio_weread_cache_migrate();assert(access(epub,F_OK)!=0); // No resurrection after deletion.
}
static uint16_t zip_le16(const unsigned char* p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t zip_le32(const unsigned char* p) { return zip_le16(p)|((uint32_t)zip_le16(p+2)<<16); }
static char* export_member(const char* epub,const char* name) {
    FILE* f=fopen(epub,"rb");assert(f);
    unsigned char header[30];char member[256];char* result=NULL;
    while(fread(header,1,sizeof(header),f)==sizeof(header) && zip_le32(header)==0x04034b50u){
        size_t length=zip_le16(header+26),extra=zip_le16(header+28);uint32_t bytes=zip_le32(header+18);
        assert(length<sizeof(member) && zip_le16(header+8)==0);
        assert(fread(member,1,length,f)==length);member[length]=0;assert(!fseek(f,(long)extra,SEEK_CUR));
        if(!strcmp(member,name)){
            result=calloc((size_t)bytes+1,1);assert(result);
            assert(fread(result,1,bytes,f)==bytes);break;
        }
        assert(!fseek(f,(long)bytes,SEEK_CUR));
    }
    assert(!fclose(f));assert(result);return result;
}
static void test_export_markup_complete(void) {
    // Long identifiers and cover markup used to exceed a 192-byte formatter:
    // ZIP CRCs were valid but the embedded XML was silently truncated.
    char id[64];memset(id,'a',sizeof(id)-1);id[sizeof(id)-1]=0;
    folio_weread_cache_writer_t* w=begin(id);
    write_chapter(w,0,"<p>正文</p>",FOLIO_WEREAD_XHTML);publish(w);folio_weread_cache_close(w);
    char cover[256],epub[256];assert(folio_wr_aux(path,"cover.png",cover,sizeof(cover)));
    char fixture[1024];snprintf(fixture,sizeof(fixture),"%s",__FILE__);
    char* base=strrchr(fixture,'/');assert(base);snprintf(base+1,(size_t)(fixture+sizeof(fixture)-base-1),"fixtures/ui-cover-light.png");
    FILE* source=fopen(fixture,"rb");assert(source);FILE* f=fopen(cover,"wb");assert(f);
    unsigned char image[1024];size_t n;
    while((n=fread(image,1,sizeof(image),source)))assert(fwrite(image,1,n,f)==n);
    assert(!ferror(source));assert(!fclose(source));assert(!fclose(f));
    expect(folio_weread_cache_export(path,epub,sizeof(epub),error,sizeof(error)));
    char* xml=export_member(epub,"OEBPS/cover.xhtml");
    assert(strstr(xml,"src=\"cover.png\""));
    assert(strstr(xml,"style=\"max-width:100%;max-height:100%\"/></div></body></html>"));free(xml);
    xml=export_member(epub,"OEBPS/toc.ncx");
    assert(strstr(xml,id) && strstr(xml,"name=\"dtb:maxPageNumber\" content=\"0\"/></head><docTitle><text>"));
    assert(strstr(xml,"</text></docTitle><navMap>") && strstr(xml,"</navMap></ncx>"));free(xml);
    xml=export_member(epub,"OEBPS/content.opf");
    assert(strstr(xml,id) && strstr(xml,"</dc:identifier><dc:description>已下载 1 / 3 章</dc:description>"));
    assert(strstr(xml,"<itemref idref=\"cover\"/>") && strstr(xml,"</package>"));free(xml);
    // Future oversized formatted fields must fail instead of publishing bad XML.
    unsigned char buffer[256];wr_out_t out={.fd=-1,.okay=true,.buf=buffer,.cap=sizeof(buffer)};
    wr_printf(&out,"%0256u",1u);assert(!out.okay && out.used==0);
}
static void test_mapping_block_boundaries(void) {
    const size_t prefix=4095,tail=4096;
    char* text=malloc(prefix+7+tail+1);assert(text);memset(text,'A',prefix);
    memcpy(text+prefix,"中😀",7);memset(text+prefix+7,'B',tail);text[prefix+7+tail]=0;
    folio_weread_cache_writer_t* w=begin("map_blocks");
    write_chapter(w,0,text,FOLIO_WEREAD_TEXT);publish(w);
    map_roundtrip((uint32_t)prefix,(uint32_t)prefix);
    map_roundtrip((uint32_t)prefix+3,(uint32_t)prefix+1);
    map_roundtrip((uint32_t)prefix+7+tail,(uint32_t)prefix+3+tail);
    // Corruption after the requested anchor must still reject the mapping,
    // including a byte split across two compare-buffer reads.
    char chapter_path[256];assert(object_path(chapter_path,"map_blocks",'c',w->manifest.records[0].key));
    FILE* f=fopen(chapter_path,"r+b");assert(f);
    long damaged=(long)sizeof(chapter_header_t)+w->manifest.records[0].source_size+(long)prefix+1;
    assert(!fseek(f,damaged,SEEK_SET));assert(fputc('?',f)!=EOF);assert(!fclose(f));
    folio_weread_position_t position;
    assert(!folio_weread_cache_local_to_cloud(path,0,&position,error,sizeof(error)));
    f=fopen(chapter_path,"r+b");assert(f);assert(!fseek(f,damaged,SEEK_SET));
    assert(fputc((unsigned char)text[prefix+1],f)!=EOF);assert(!fclose(f));
    map_roundtrip(0,0);free(text);folio_weread_cache_close(w);
}
int main(void) {
    assert(!mkdir(FOLIO_SD_ROOT, 0777));
    test_exact_mapping(); test_partial_resume_change(); test_crash_and_corruption(); test_bounds_and_failures(); test_checkpoint_torn();
    test_revision_and_maximum_toc(); test_publish_reuse_integrity();
    test_paths_and_invalid_pointer(); test_converter_adversarial(); test_epub_export_recovery();test_export_markup_complete();
    test_mapping_block_boundaries();
    puts("WeRead cache: exact UTF16/UTF8 mapping, resume, snapshots, corruption and limits passed");
    return 0;
}
