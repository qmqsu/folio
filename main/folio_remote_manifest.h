#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "folio_update.h"

#define FOLIO_REMOTE_MANIFEST_MAX 4096u
#define FOLIO_REMOTE_ORIGIN "https://qmqsu.github.io/folio/"
#define FOLIO_REMOTE_LAYOUT "folio-16m-factory-storage-ota-v2"
#define FOLIO_REMOTE_HARDWARE "RDP-G01-W"

typedef struct {
    folio_update_info_t image;
    uint32_t sequence;
    char channel[8], url[192], notes[513];
} folio_remote_manifest_t;

// Verifies raw payload bytes with the compiled release key (PSA P256/SHA256).
// Strict JSON, duplicate/unknown fields, integers, UTF-8, canonical base64,
// channel, exact image URL, device/layout and monotonic sequence all checked.
// Output is zeroed on any failure. No storage, HTTP or flash operations.
bool folio_remote_manifest_verify(const uint8_t* envelope, size_t length,
    const char* channel, uint32_t current_sequence, folio_remote_manifest_t* out,
    char* error, size_t error_cap);
const char* folio_remote_manifest_url(const char* channel);
