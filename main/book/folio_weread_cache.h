#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "folio_epub.h"

#define FOLIO_WEREAD_ID_CAP 64
#define FOLIO_WEREAD_CHAPTER_LIMIT 512
#define FOLIO_WEREAD_CHAPTER_BYTES (512u * 1024u)
#define FOLIO_WEREAD_BOOK_BYTES (32u * 1024u * 1024u)

typedef enum { FOLIO_WEREAD_TEXT = 0, FOLIO_WEREAD_XHTML = 1 } folio_weread_content_kind_t;
typedef struct {
    char uid[FOLIO_WEREAD_ID_CAP];
    char title[FOLIO_EPUB_TITLE_CAP];
} folio_weread_chapter_t;
typedef struct folio_weread_cache_writer folio_weread_cache_writer_t;
typedef struct {
    char book_id[FOLIO_WEREAD_ID_CAP];
    char chapter_uid[FOLIO_WEREAD_ID_CAP];
    uint32_t offset_utf16, source_size;
    uint64_t source_hash;
    folio_weread_content_kind_t kind;
} folio_weread_position_t;

/* IDs accept only ASCII letters, digits, '_' and '-', at most 63 bytes. Paths
 * for cache files are generated from book IDs, never remote URLs. Public EPUB
 * names use a sanitized title plus the encoded stable ID.
 * One download worker only. Close an open reader before updating that book.
 * Every public disk operation takes the shared recursive storage lock.
 * Chapter commits survive restart independently of publish. Begin reuses
 * verified chapters with matching UIDs; it cannot detect remote edits by itself.
 * Force re-download to refresh changed content. */
folio_weread_cache_writer_t* folio_weread_cache_begin(
    const char* book_id, const char* title, const char* author,
    const folio_weread_chapter_t* chapters, size_t count, char* error, size_t cap);
bool folio_weread_cache_has_chapter(const folio_weread_cache_writer_t* writer, size_t index);
bool folio_weread_cache_write_chapter(folio_weread_cache_writer_t* writer, size_t index,
    const uint8_t* raw, size_t size, folio_weread_content_kind_t kind, char* error, size_t cap);
/* Publishes all completed, nonempty chapters in TOC order, including partial
 * books. Output .wrb uses two checksummed pointer slots; immutable text and
 * manifest files are durable before the inactive pointer slot is changed.
 * The 32 MiB per-book disk limit includes staging and retained old snapshots. */
bool folio_weread_cache_publish(folio_weread_cache_writer_t* writer,
    char* path, size_t path_cap, char* error, size_t cap);
void folio_weread_cache_close(folio_weread_cache_writer_t* writer);

/* Result uses the existing reader's UTF-8 text/TOC interface. Free it with
 * folio_epub_free(). A changed published body invalidates saved reader offsets. */
bool folio_weread_cache_prepare(const char* path, folio_epub_t* result, char* error, size_t cap);
bool folio_weread_cache_metadata(const char* path, char* title, size_t title_cap,
    char* author, size_t author_cap, char* error, size_t cap);
bool folio_weread_cache_book_id(const char* path, char* book_id, size_t capacity);
bool folio_weread_cache_path(const char* book_id, char* path, size_t capacity);
uint64_t folio_weread_cache_revision(const char* path);
/* Chapters with text in the published cache. Reads only the manifest, unlike
 * cache_prepare() which materializes the whole book. */
size_t folio_weread_cache_chapter_count(const char* path);
bool folio_weread_cache_chapter_available(const char* path, const char* chapter_uid);
/* Fast UI hints from one checksummed manifest. Does not read/hash chapter bodies;
 * opening, downloading, exporting and sync must still use verified operations. */
bool folio_weread_cache_presence(const char* path, const char* const* chapter_uids,
    size_t count, bool* present);

/* Exact scalar-boundary mapping only: no percentage approximation. Raw XHTML
 * coordinates count UTF-16 units including markup. UTF-8 continuation bytes,
 * surrogate halves, entity interiors, and omitted markup are not positions.
 * The same converter verifies the stored source and text before mapping.
 * source_hash is an integrity/version fingerprint, not remote authentication.
 * A cloud position with hash=0 uses the current cached source; callers must
 * first confirm that source still matches the remote chapter. Nonzero hashes
 * must match and protect restoration across cache updates. */
bool folio_weread_cache_local_to_cloud(const char* path, uint32_t byte_offset,
    folio_weread_position_t* position, char* error, size_t cap);
bool folio_weread_cache_cloud_to_local(const char* path, const folio_weread_position_t* position,
    uint32_t* byte_offset, char* error, size_t cap);
bool folio_weread_cache_matches_raw(const folio_weread_position_t* position,
    const uint8_t* raw, size_t size, folio_weread_content_kind_t kind);

/* Publish a portable EPUB with a cover and only verified downloaded chapters.
 * Legacy pointer/cover files move to the per-book hidden directory only after
 * the EPUB has been durably published. Originals survive any failed export. */
bool folio_weread_cache_export(const char* path,char* output,size_t output_cap,char* error,size_t cap);
/* Optional observer for long exports. Weak no-op by default so the cache works
* without a network layer; the WeRead worker overrides it to show progress and
* keep the idle timer alive while a book is packaged. Called once per chapter. */
void folio_weread_export_progress(size_t done, size_t total);
bool folio_weread_cache_public_path(const char* path,char* output,size_t cap);
void folio_weread_cache_migrate(void);
