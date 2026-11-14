#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = 0x105;
enum { HTTP_EVENT_ON_HEADER = 1, HTTP_TRANSPORT_OVER_SSL = 2, HTTP_METHOD_GET = 3, HTTP_METHOD_POST = 4 };
struct esp_http_client_event_t { int event_id; void* user_data; const char* header_key; const char* header_value; };
struct esp_http_client_config_t {
 const char* url; const char* user_agent; int timeout_ms, buffer_size, buffer_size_tx, transport_type;
 bool disable_auto_redirect, skip_cert_common_name_check, save_client_session; const char* common_name;
 int (*crt_bundle_attach)(void*); int (*event_handler)(esp_http_client_event_t*); void* user_data;
};
struct fake_client;
using esp_http_client_handle_t = fake_client*;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
int esp_http_client_close(esp_http_client_handle_t);
int esp_http_client_cleanup(esp_http_client_handle_t);
int esp_http_client_set_header(esp_http_client_handle_t,const char*,const char*);
int esp_http_client_delete_header(esp_http_client_handle_t,const char*);
int esp_http_client_set_url(esp_http_client_handle_t,const char*);
int esp_http_client_set_method(esp_http_client_handle_t,int);
int esp_http_client_open(esp_http_client_handle_t,int);
int esp_http_client_set_timeout_ms(esp_http_client_handle_t,int);
int esp_http_client_write(esp_http_client_handle_t,const char*,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);

constexpr int ESP_ERR_HTTP_EAGAIN=0x7007;

int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,int*,int*);
