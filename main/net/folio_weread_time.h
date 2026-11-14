#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t generation, position, percent;
    uint64_t book_revision;
    uint64_t pending, confirmed, uncertain;
    uint32_t inflight;
    uint8_t owner[32]; // SHA-256 account identifier; never a login secret.
    bool enabled;
    // Last explicitly downloaded cloud total. Never added to pending/local activity.
    uint32_t cloud_seconds, cloud_updated_at;
    bool cloud_valid;
} folio_weread_time_t;
// Caller holds the shared storage lock. path must be a canonical SD .wrb path;
// every sidecar parent and existing target is checked before I/O.
// Missing pair is empty; corrupt pair fails closed. Inflight is quarantined as
// uncertain after a restart and can never become an automatic retry.
bool folio_weread_time_load(const char* path, folio_weread_time_t* record);
// Rejects corrupted existing slots, stale generations and owner replacement.
bool folio_weread_time_save(const char* path, folio_weread_time_t* record);
bool folio_weread_time_bind(folio_weread_time_t* record, const uint8_t owner[32]);
void folio_weread_time_add(folio_weread_time_t* record, uint32_t seconds);
// Persist before sending. A failed save means no request may be sent.
bool folio_weread_time_begin(const char* path, folio_weread_time_t* record);
typedef enum { FOLIO_WR_TIME_ACCEPTED, FOLIO_WR_TIME_REJECTED, FOLIO_WR_TIME_UNKNOWN } folio_wr_time_result_t;
bool folio_weread_time_finish(const char* path, folio_weread_time_t* record,
                              folio_wr_time_result_t result);
#ifdef __cplusplus
}
#endif
