#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool running, connecting, connected, has_credentials, clock_valid;
    char message[128];
} folio_network_status_t;

// UI task only. Uses RAM credentials or the selected internal-NVS Wi-Fi profile.
// Starts only STA, with no AP or HTTP server.
// Call stop successfully before switching between station and transfer modes.
bool folio_network_start_station(char* error, size_t cap);
void folio_network_get_status(folio_network_status_t* status);
// Same ownership/failed-stop contract as folio_transfer_stop. RAM credentials
// Saved native Wi-Fi profiles survive restart; forgetting removes them.
bool folio_network_stop(void);
// Browser time is authenticated, bounded UTC. HTTP sets the system clock;
// the UI takes this pending value to update the PMU without cross-task I2C.
bool folio_network_take_utc(uint32_t* utc);
typedef struct {char ssid[33];int8_t signal;bool secured;} folio_network_ap_t;
// Native settings persist up to eight profiles in internal NVS, never on TF.
bool folio_network_saved_password(const char* ssid,char* password,size_t capacity);
bool folio_network_forget(const char* ssid,char* error,size_t capacity);
bool folio_network_configure(const char* ssid,const char* password,char* error,size_t cap);
bool folio_network_connect(char* error,size_t cap);
bool folio_network_scan_start(char* error,size_t cap);
bool folio_network_scan_result(folio_network_ap_t* items,size_t cap,size_t* count);
bool folio_network_sync_time(char* error,size_t cap);
void folio_network_cancel_time(void);
