#pragma once
#include <sys/time.h>
#define SNTP_OPMODE_POLL 0
#define CONFIG_LWIP_SNTP_MAX_SERVERS 3
typedef void (*sntp_sync_time_cb_t)(struct timeval*);
void esp_sntp_setoperatingmode(int mode);
void esp_sntp_setservername(unsigned index,const char* name);
void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback);
void esp_sntp_init(void);
void esp_sntp_stop(void);
