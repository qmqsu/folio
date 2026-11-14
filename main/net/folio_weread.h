#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOLIO_WR_PAGE_SIZE 6
typedef enum {
    FOLIO_WR_OFF, FOLIO_WR_CONNECTING, FOLIO_WR_LOGIN, FOLIO_WR_OTP,
    FOLIO_WR_SHELF, FOLIO_WR_BOOK, FOLIO_WR_WORK, FOLIO_WR_SYNC,
    FOLIO_WR_ERROR, FOLIO_WR_DOWNLOAD_ERROR
} folio_weread_state_t;
typedef struct {
    char title[128], subtitle[128];
    bool cached;
} folio_weread_item_t;
typedef struct {
    uint32_t revision;
    folio_weread_state_t state;
    bool running, busy, logged_in, can_read, sync_local, sync_cloud;
    bool time_enabled, login_required;
    bool cloud_time_available, cloud_time_saved;
    uint32_t cloud_total_seconds, saved_cloud_seconds;
    uint32_t completed, total, pending_seconds, uncertain_seconds;
    size_t page, count, item_count;
    char title[128], message[192], qr_url[256], local_path[256];
    char local_position[128], cloud_position[128];
    folio_weread_item_t items[FOLIO_WR_PAGE_SIZE];
} folio_weread_status_t;
typedef enum {
    FOLIO_WR_REFRESH, FOLIO_WR_NEW_QR, FOLIO_WR_SUBMIT_OTP,
    FOLIO_WR_SELECT_BOOK, FOLIO_WR_PAGE, FOLIO_WR_DOWNLOAD_CHAPTER,
    FOLIO_WR_DOWNLOAD_ALL, FOLIO_WR_CHECK_SYNC, FOLIO_WR_USE_CLOUD,
    FOLIO_WR_UPLOAD_POSITION, FOLIO_WR_ENABLE_TIME, FOLIO_WR_DISABLE_TIME,
    FOLIO_WR_UPLOAD_TIME, FOLIO_WR_LOGOUT, FOLIO_WR_CANCEL,
    FOLIO_WR_RETRY_DOWNLOAD, FOLIO_WR_RETURN_TOC, FOLIO_WR_RELOGIN, FOLIO_WR_UPLOAD_SYNC,
    FOLIO_WR_RETRY, FOLIO_WR_RETURN_SHELF
} folio_weread_action_t;

// UI-thread lifecycle. Async worker owns the protocol client and SD mutations.
// stop cancels cooperatively; false means the worker still owns I/O. Never
// unmount/format SD, start transfer, or shut down before stop returns true.
bool folio_weread_start(char* error, size_t capacity);
// Start from a downloaded WeRead EPUB after saving/closing the local reader.
bool folio_weread_start_sync(const char* path, char* error, size_t capacity);
bool folio_weread_stop(void);
// True while the worker owns the network or the SD card. Callers that do heavy
// card work (library rescan, cache migration) must skip it: the worker already
// holds the storage lock for its own export.
bool folio_weread_busy(void);
void folio_weread_get_status(folio_weread_status_t* status);
bool folio_weread_command(folio_weread_action_t action, size_t index, const char* value);
// Called only with the worker stopped, after successfully saving/closing reader.
// These record the actual active delta from folio_activity, not wall-clock time.
void folio_weread_record(const char* path, uint32_t byte_position, uint32_t percent,
                         uint32_t active_seconds);
bool folio_weread_flush(void);
bool folio_weread_io_cancelled(void);
// A confirmed cloud-to-local action requests an ordinary local reader seek.
bool folio_weread_take_open(char* path, size_t capacity, uint32_t* position);
bool folio_weread_local_position(const char* path, uint32_t* position);

#ifdef __cplusplus
}
#endif
