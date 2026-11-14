#pragma once
#include "esp_http_client.h"
constexpr int ESP_ERR_NVS_NOT_FOUND=0x1102;
enum nvs_open_mode_t {NVS_READONLY=0,NVS_READWRITE=1};
using nvs_handle_t=unsigned;
extern "C" {
esp_err_t nvs_open(const char*,nvs_open_mode_t,nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char*);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_flash_init();
}
