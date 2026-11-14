#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "folio_reader.h"

#define FOLIO_CATALOG_LIMIT 256
#define FOLIO_CATALOG_QUERY_CAP 128
#define FOLIO_CATALOG_AUTHOR_CAP 128

typedef struct folio_catalog folio_catalog_t;
typedef enum { FOLIO_CATALOG_ALL, FOLIO_CATALOG_RECENT, FOLIO_CATALOG_UNREAD } folio_catalog_filter_t;
typedef enum { FOLIO_CATALOG_SORT_RECENT, FOLIO_CATALOG_SORT_TITLE,
               FOLIO_CATALOG_SORT_AUTHOR, FOLIO_CATALOG_SORT_ADDED } folio_catalog_sort_t;
typedef struct {
    folio_catalog_filter_t filter;
    folio_catalog_sort_t sort;
    const char* search; // NULL/empty: no search; UTF-8, at most QUERY_CAP-1 bytes.
} folio_catalog_options_t;
typedef struct {
    char title[FOLIO_BOOK_TITLE_CAP];
    char author[FOLIO_CATALOG_AUTHOR_CAP]; // Empty when the book has no known author.
    uint32_t added_utc, lastread_utc; // 0: device clock was not calibrated.
    uint64_t added_order, lastread_order; // Persistent local event order.
    uint64_t reading_seconds; // Counted from v2 onward; older records start at zero.
    bool read; // Successfully opened at least once; not an inferred completion.
} folio_catalog_metadata_t;

// Bounded, PSRAM-first state; missing files start an empty catalog. Corrupt or
// unreadable records leave error() set while retaining any valid older slot.
folio_catalog_t* folio_catalog_open(void);
void folio_catalog_free(folio_catalog_t* catalog); // No implicit I/O.
bool folio_catalog_save(folio_catalog_t* catalog);
bool folio_catalog_dirty(const folio_catalog_t* catalog);
const char* folio_catalog_error(const folio_catalog_t* catalog);

// Records actual local discovery time/order, caches read-only EPUB metadata.
// Does not reorder/mutate books. refresh_metadata also rechecks unchanged EPUBs.
// Existing paths retain read/discovery history when their file contents change.
// Sync is bounded to FOLIO_LIBRARY_LIMIT and performs no persistence writes.
bool folio_catalog_sync(folio_catalog_t* catalog, const folio_book_t* books,
                        size_t count, uint32_t utc, bool refresh_metadata);
// Call only after a successful open. Updates read status/order even with utc=0.
bool folio_catalog_add_reading(folio_catalog_t* catalog, const char* path, uint64_t seconds);
bool folio_catalog_touch(folio_catalog_t* catalog, const char* path, uint32_t utc);
const folio_catalog_metadata_t* folio_catalog_metadata(const folio_catalog_t* catalog,
                                                       const char* path);
// Ordered indexes into the ORIGINAL supplied books. Recent includes every known
// opened book, not only the home page's three MRU entries. Unknown authors sort
// last. Title/author ordering uses normalized single-character Mandarin readings.
// Search matches title, filename or author: original UTF-8 (ASCII case folded),
// toneless pinyin or initials. Pinyin uses one Unihan kMandarin default reading;
// phrase-dependent polyphonic readings are not inferred. ü/u:/v normalize to v.
// Returns indexes written, bounded by capacity; oversized/invalid inputs yield 0.
size_t folio_catalog_select(const folio_catalog_t* catalog, const folio_book_t* books,
                            size_t count, const folio_catalog_options_t* options,
                            size_t* indexes, size_t capacity);

bool folio_catalog_relocate(folio_catalog_t* catalog,const char* old_path,const char* new_path);
