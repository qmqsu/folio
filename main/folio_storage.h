#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
#define FOLIO_STORAGE_PATH_CAP 256
#define FOLIO_STORAGE_ENTRY_LIMIT 4096
#define FOLIO_STORAGE_DEPTH_LIMIT 24

typedef enum {
    FOLIO_STORAGE_BOOK, FOLIO_STORAGE_FONT,
    FOLIO_STORAGE_CACHE, FOLIO_STORAGE_OTHER
} folio_storage_category_t;

typedef struct {
    uint64_t capacity_bytes, used_bytes, free_bytes;
    /* Logical file lengths; FAT allocation/metadata can make their sum differ
     * from used_bytes. Capacity/free come from the caller's fresh SD info. */
    uint64_t book_bytes, font_bytes, cache_bytes, other_bytes;
    size_t file_count, directory_count;
} folio_storage_summary_t;

typedef struct {
    char path[FOLIO_STORAGE_PATH_CAP];
    char name[FOLIO_STORAGE_PATH_CAP];
    bool directory;
    uint64_t bytes; /* A directory's immediate stat length is reported as zero. */
    int64_t modified;
    folio_storage_category_t category;
} folio_storage_entry_t;

typedef struct {
    char path[FOLIO_STORAGE_PATH_CAP];
    bool directory;
    uint64_t bytes;
    size_t file_count, directory_count; /* Includes the target directory. */
    uint64_t token; /* Opaque, one use; a newer preview invalidates this token. */
} folio_storage_preview_t;

/* Shared recursive lock. Upload must hold it throughout write/validate/rename;
 * UI holds it around closing/reloading SD users plus a destructive operation.
 * Public storage I/O functions also acquire it, with a bounded timeout. */
bool folio_storage_lock(uint32_t timeout_ms);
void folio_storage_unlock(void);
/* Optional application hooks: define bool folio_storage_cancelled(void) for
 * shutdown, or bool folio_io_cancelled(void) for the current upload task.
 * Long operations check these weak hooks and stop with an explicit error. */

/* Accepts only absolute paths inside FOLIO_SD_ROOT. Repeated '/' and '.' are
 * normalized; every '..', backslash, control character and symlink is rejected.
 * normalize performs no I/O. validate_parent requires an existing safe parent,
 * and rejects an existing symlink/special-file target. It creates nothing. */
bool folio_storage_normalize(const char* path, char* output, size_t output_cap,
                             char* error, size_t error_cap);
bool folio_storage_validate_parent(const char* path, char* error, size_t error_cap);
/* Fresh usable FAT volume capacity/free space, serialized with uploads. */
bool folio_storage_space(uint64_t* capacity_bytes, uint64_t* free_bytes,
                         char* error, size_t error_cap);
bool folio_storage_scan(uint64_t capacity_bytes, uint64_t free_bytes,
                        folio_storage_summary_t* result, char* error, size_t error_cap);
/* Stable directory-first, bytewise-name order. count may be zero at end.
 * A failed call resets count/total; callers must discard entries on failure. */
bool folio_storage_list(const char* path, size_t offset,
                        folio_storage_entry_t* entries, size_t entry_capacity,
                        size_t* count, size_t* total, char* error, size_t error_cap);
/* Device browser: hide internal dot entries and covers paired with a book unless requested. */
bool folio_storage_list_view(const char* path, size_t offset, bool show_internal,
                        folio_storage_entry_t* entries, size_t entry_capacity,
                        size_t* count, size_t* total, char* error, size_t error_cap);
/* Preview fingerprints all target contents, including file bytes. Commit
 * repeats the scan before deleting; changed/missing/unreadable targets fail
 * closed. Root deletion is prohibited. No cleanup outside the chosen subtree.
 * I/O failure during deletion may leave a partially deleted subtree, reported
 * as failure; there is no FAT transaction/rollback. Caller must refresh UI. */
bool folio_storage_preview_delete(const char* path, folio_storage_preview_t* result,
                                  char* error, size_t error_cap);
bool folio_storage_commit_delete(const folio_storage_preview_t* preview,
                                 char* error, size_t error_cap);
void folio_storage_cancel_delete(void);

#define FOLIO_PICTURE_DIRECTORY "picture"
#define FOLIO_PICTURE_ROOT FOLIO_SD_ROOT "/" FOLIO_PICTURE_DIRECTORY
#define FOLIO_LEGACY_WALLPAPER_ROOT FOLIO_SD_ROOT "/wallpapers"
/* Prepare picture; rename a sole safe legacy wallpapers directory without
 * replacement. If both exist, return false with a retained/unmigrated warning:
 * neither directory is merged/deleted, and both remain readable below.
 * A saved legacy choice is journaled before rename for restart recovery.
 * Call after mounting FAT, before exposing uploads; never format on failure. */
bool folio_storage_prepare_wallpapers(char* error,size_t error_cap);
/* PNG/JPEG candidates in picture plus retained wallpapers. Original names are
 * unchanged (duplicates allowed); full path is the identity. Sorted by name,
 * then path. Decode still validates contents. No migration occurs on reads. */
bool folio_storage_wallpapers(size_t offset,folio_storage_entry_t* entries,size_t capacity,
                             size_t* count,size_t* total,char* error,size_t error_cap);
/* Full absolute path in/out, including retained legacy paths. Two checksummed
 * slots keep directory-qualified choices and read v1 basename-only records.
 * Missing/unsafe legacy choices never fall through to a same-name picture.
 * Migration recovery verifies image bytes; images are never modified. */
bool folio_storage_wallpaper_load(char* path,size_t capacity);
bool folio_storage_wallpaper_save(const char* path,char* error,size_t error_cap);
/* Legacy shutdown fallback only: never return a source larger than 2 MiB, or
 * hash a larger migrating selection. Menu load above retains the full limit. */
bool folio_storage_wallpaper_load_legacy(char* path,size_t capacity);
/* Publish a prepared snapshot before its V2 selection. pixels is packed gray4,
 * row-major, high nibble first, stride=(width+1)/2; max dimensions 684x1216.
 * The caller owns pixels and must have decoded this source successfully.
 * Preparation failures retain the active choice/cache. Once selection commit
 * starts, write/sync/readback failure reports uncertainty, not atomic rollback.
 * Serializes storage; does not provide an SD/NVS or physical FAT transaction. */
bool folio_storage_wallpaper_save_prepared(const char* path,const uint8_t* pixels,
    unsigned width,unsigned height,char* error,size_t error_cap);
/* Read only the latest bound snapshot; caller frees *pixels with free().
 * Outputs reset on failure. No EPD dependency or large-source read/hash.
 * Source deletion/size/mtime change invalidates it. Matching metadata is NOT
 * proof of identical current contents: this is a selection-time snapshot.
 * Legacy migration records cannot use this cache or cross-directory aliases. */
bool folio_storage_wallpaper_cache_load(uint8_t** pixels,unsigned* width,unsigned* height);
