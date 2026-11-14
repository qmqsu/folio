#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // SoftAP is always available. Optional STA joins a trusted local network.
    // Empty ssid + use_saved tries stored credentials. Missing or
    // unavailable credentials fall back to SoftAP, including first startup.
    // RAM credentials from browser provisioning take priority over NVS.
    const char* ssid;
    const char* password;
    bool remember;
    bool use_saved;
} folio_transfer_config_t;
#define FOLIO_TRANSFER_RECENT_CAP 5
// Results use folio_upload_result_t numeric values; zero means saved successfully.
typedef struct {
    char name[256];
    uint32_t size;
    uint8_t result;
    bool saved_only;
    char message[96];
} folio_transfer_record_t;
typedef struct {
    bool running, busy, sta_connected;
    uint32_t received, total, completed;
    char ssid[33], ap_password[17], access_code[7];
    char url[128], sta_url[128]; // Public URLs; session token is never exposed here.
    char message[128];
    uint8_t recent_count;
    folio_transfer_record_t recent[FOLIO_TRANSFER_RECENT_CAP]; // Newest first; current session only.
} folio_transfer_status_t;

// UI task only: start/stop. Stop cancels work and stops HTTP/Wi-Fi. It is
// idempotent. A false result retains live handles in the stopping state for a
// later retry: the caller MUST NOT unmount/remount the SD, sleep, or destroy its
// users until stop succeeds. No credentials/session secrets are logged.
bool folio_transfer_start(const folio_transfer_config_t* config, char* error, size_t cap);
bool folio_transfer_stop(void);
void folio_transfer_get_status(folio_transfer_status_t* status);
// UI task only, while stopped. Clears RAM and all stored Wi-Fi profiles.
bool folio_transfer_forget_wifi(void);
