#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOLIO_UPDATE_MAX_SIZE (3u * 1024u * 1024u)
#define FOLIO_UPDATE_SHA_FILE_MAX 160u

typedef struct {
    char version[32];
    uint32_t size;
    uint8_t sha256[32];
    uint64_t token; // Binds the preview fields; install rechecks the actual bytes.
} folio_update_info_t;

// True only when a separate OTA application slot and OTA metadata exist.
// This does not mean an update file has been found or validated.
bool folio_update_available(void);
// Read-only preview of /sdcard/update/folio.bin and folio.bin.sha256.
// Hash file: exactly one SHA-256, optionally followed by folio.bin (sha256sum).
bool folio_update_inspect(folio_update_info_t* info, char* error, size_t error_cap);
// Install only after explicit UI confirmation. Streams to an inactive OTA slot,
// revalidates preview hash/version/size, and changes the boot target only after
// esp_ota_end succeeds. Does not reboot or erase factory/NVS/book storage.
bool folio_update_install(const folio_update_info_t* info, char* error, size_t error_cap);
// Call after initialization/rendering is healthy. Only pending verification is
// marked valid; ready=false leaves rollback armed. Never reboots or invalidates.
bool folio_update_confirm_boot(bool ready);
// Upload staging check: complete bounded ESP32-S3/Folio image structure and
// internal checksum/hash; arbitrary local temp path, no sidecar or flash writes.
bool folio_update_validate_image(const char* path, char* error, size_t error_cap);

// Optional task-local cancellation, in addition to the legacy application hook.
typedef struct {
    bool (*cancelled)(void* context);
    void* context;
} folio_update_control_t;
// No sidecar is read. Caller must authenticate expected version/size/hash first.
// Holds no storage lock: caller owns the lock throughout inspect/install.
bool folio_update_inspect_file(const char* path, const folio_update_info_t* expected,
    folio_update_info_t* info, const folio_update_control_t* control, char* error, size_t error_cap);
// Rechecks structure, preview and the bytes actually written before boot switch.
// This is a byte-integrity primitive, NOT publisher authentication by itself.
bool folio_update_install_file(const char* path, const folio_update_info_t* info,
    const folio_update_control_t* control, char* error, size_t error_cap);
