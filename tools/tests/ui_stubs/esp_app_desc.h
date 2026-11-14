#pragma once
// Host stand-in for the SDK descriptor; production reads the running app.
typedef struct { char version[32]; } esp_app_desc_t;
const esp_app_desc_t* esp_app_get_description(void);
