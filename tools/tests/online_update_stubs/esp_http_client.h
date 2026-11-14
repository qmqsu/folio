#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef void* esp_http_client_handle_t;
typedef struct { int event_id; void* user_data; esp_http_client_handle_t client; char* header_key; char* header_value; } esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_EVENT_ON_CONNECTED 2
#define HTTP_EVENT_DISCONNECTED 3
#define HTTP_ADDR_TYPE_INET 2
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define HTTP_TRANSPORT_OVER_SSL 2
typedef struct {
 const char* url; int (*crt_bundle_attach)(void*); bool skip_cert_common_name_check;
 bool disable_auto_redirect; int max_redirection_count, timeout_ms, transport_type;
 bool is_async; int addr_type;
 bool keep_alive_enable; int buffer_size, buffer_size_tx; const char* user_agent;
 esp_err_t (*event_handler)(esp_http_client_event_t*); void* user_data;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char* key, const char* value);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t c);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char* data, int length);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);

int esp_http_client_get_socket(esp_http_client_handle_t c);
