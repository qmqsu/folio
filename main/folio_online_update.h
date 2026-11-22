#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef FOLIO_RELEASE_SEQUENCE
#define FOLIO_RELEASE_SEQUENCE 8032000u
#endif
#define FOLIO_ONLINE_PATH_CAP 256
#define FOLIO_ONLINE_MESSAGE_CAP 192

typedef enum {
    FOLIO_ONLINE_IDLE, FOLIO_ONLINE_CHECKING, FOLIO_ONLINE_AVAILABLE,
    FOLIO_ONLINE_DOWNLOADING, FOLIO_ONLINE_READY, FOLIO_ONLINE_INSTALLING,
    FOLIO_ONLINE_INSTALLED, FOLIO_ONLINE_NO_RELEASE, FOLIO_ONLINE_FAILED,
    FOLIO_ONLINE_CANCELLED, FOLIO_ONLINE_FONT_READY
} folio_online_state_t;

typedef struct {
    folio_online_state_t state;
    bool busy;
    uint32_t received, total, sequence, revision;
    char version[32], channel[8], notes[513];
    char candidate_path[FOLIO_ONLINE_PATH_CAP];
    char message[FOLIO_ONLINE_MESSAGE_CAP];
} folio_online_snapshot_t;

// Optional injected transport. open/read/complete/close run only on the worker.
// open must authenticate TLS+hostname, disable redirects and use bounded I/O.
// open may allocate even on failure: close is ALWAYS called after an open.
// content_length=-1 means chunked/unknown; read: >0 bytes, 0 EOF, -1 failure.
// complete must distinguish valid EOF from a truncated HTTP response.
// This seam replaces network I/O, never signature or image validation.
typedef struct {
    void* context;
    bool (*open)(void*, const char* url, int* status, int64_t* content_length,
                 char* error, size_t error_cap);
    int (*read)(void*, uint8_t* bytes, size_t capacity);
    bool (*complete)(void*);
    void (*close)(void*);
    // Optional resumable download: request the next body to start at offset for
    // the following open. Return false to report that the server cannot resume.
    // Absent means every attempt starts at zero (still valid, just slower).
    bool (*seek)(void*, uint32_t offset);
} folio_online_transport_t;

typedef struct {
    void* context;
    // All callbacks execute on the worker: no UI or PMU I2C calls here.
    bool (*card_ready)(void*);
    bool (*storage_lock)(void*, uint32_t timeout_ms);
    void (*storage_unlock)(void*);
    // Required; read a UI-refreshed power/authorization snapshot. Rechecked
    // immediately before installation. Return a user-facing reason on refusal.
    bool (*install_allowed)(void*, char* error, size_t error_cap);
    // Optional global shutdown/cancellation hook.
    bool (*cancelled)(void*);
    // Optional test/application seam; NULL uses existing STA status.
    bool (*network_ready)(void*);
    // NULL uses certificate-bundle HTTPS transport. Copied at init.
    const folio_online_transport_t* transport;
} folio_online_hooks_t;

// UI-thread lifecycle. init only while idle; callbacks must remain valid.
// No operation starts/stops Wi-Fi, mounts/unmounts SD, reboots or renders UI.
// UI owns STA lifecycle and exclusion with transfer/WeRead/local OTA/card
// changes. While busy, suppress sleep/unmount/shutdown; cancel and wait first.
// During network I/O NO storage lock is held. Storage chunks take short locks;
// final validation/install holds a lock after the HTTP client is CLOSED.
bool folio_online_update_init(const folio_online_hooks_t* hooks);
bool folio_online_start_font(unsigned face, char* error, size_t cap);
bool folio_online_update_start_check(const char* channel, char* error, size_t error_cap);
bool folio_online_update_start_download(char* error, size_t error_cap);
// Explicit UI confirmation only, after saving/closing other card consumers.
// The worker re-verifies its RAM-held signed envelope and the actual file;
// no mutable TF sidecar can authorize installation. Never reboots on success.
bool folio_online_update_start_install(char* error, size_t error_cap);
// Also call this on emergency/card loss, not only hooks.cancelled: it signals
// the socket-only guard without invoking application callbacks off-worker.
// Pre-socket DNS remains the fixed SDK's synchronous bounded IPv4 resolver;
// cancellation can wait for DNS/one connect step. No whole-operation latency
// guarantee is made under resolver/RTOS delay. Always wait for busy=false.
void folio_online_update_cancel(void);
bool folio_online_update_busy(void);
// Route the application's generic I/O cancellation hook to this worker instead
// of an unrelated WeRead task. hooks.cancelled must not call back into that hook.
bool folio_online_update_is_worker(void);
bool folio_online_update_cancel_requested(void);
void folio_online_update_get_snapshot(folio_online_snapshot_t* snapshot);
