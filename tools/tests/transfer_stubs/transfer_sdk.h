#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTP_GET 0
#define HTTP_POST 1
#define WIFI_EVENT "wifi"
#define IP_EVENT "ip"
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_REASON_AUTH_EXPIRE 2
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_ASSOC_FAIL 203
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210
#define WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211
typedef struct{uint8_t reason;} wifi_event_sta_disconnected_t;
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_APSTA 3
#define WIFI_MODE_STA 1
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define pdMS_TO_TICKS(x) (x)
typedef void* TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)1;}
static inline void vTaskDelay(unsigned ticks){(void)ticks;}
typedef const char* esp_event_base_t;
typedef void* esp_event_handler_instance_t;
typedef void (*event_callback_t)(void*,esp_event_base_t,int32_t,void*);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_loop_delete_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int32_t,event_callback_t,void*,esp_event_handler_instance_t*);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t,int32_t,esp_event_handler_instance_t);
typedef struct{unsigned id;} esp_netif_t;
typedef struct{uint32_t addr;} ip4_addr_t;
typedef struct{ip4_addr_t ip;} esp_netif_ip_info_t;
typedef struct{esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)(((ip)->addr>>24)&255),(unsigned)(((ip)->addr>>16)&255),(unsigned)(((ip)->addr>>8)&255),(unsigned)((ip)->addr&255)
esp_err_t esp_netif_init(void);
esp_netif_t* esp_netif_create_default_wifi_ap(void);
esp_netif_t* esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(esp_netif_t*);
esp_err_t esp_netif_get_ip_info(esp_netif_t*,esp_netif_ip_info_t*);
typedef struct{bool capable;} pmf_t;
typedef struct{uint8_t ssid[32],password[64];struct{int authmode;}threshold;pmf_t pmf_cfg;int scan_method,sort_method;} wifi_sta_config_t;
typedef struct{uint8_t ssid[32],password[64],ssid_len,max_connection,channel;int authmode;pmf_t pmf_cfg;} wifi_ap_config_t;
typedef union{wifi_sta_config_t sta;wifi_ap_config_t ap;} wifi_config_t;
typedef struct{bool nvs_enable;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() (wifi_init_config_t){.nvs_enable=true}
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int,const wifi_config_t*);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
void esp_fill_random(void*,size_t);
int64_t esp_timer_get_time(void);
void bootloader_random_enable(void);
void bootloader_random_disable(void);
typedef unsigned nvs_handle_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char*);
void nvs_close(nvs_handle_t);
typedef void* httpd_handle_t;
typedef struct{size_t content_len;const char* uri;} httpd_req_t;
typedef struct{const char* uri;int method;esp_err_t(*handler)(httpd_req_t*);} httpd_uri_t;
typedef struct{size_t stack_size,max_req_hdr_len;unsigned max_uri_handlers,max_open_sockets,recv_wait_timeout,send_wait_timeout;bool lru_purge_enable;} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() (httpd_config_t){0}
esp_err_t httpd_start(httpd_handle_t*,const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_send(httpd_req_t*,const char*,ssize_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t*,const char*,ssize_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t*,const char*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t);
esp_err_t httpd_req_get_url_query_str(httpd_req_t*,char*,size_t);
esp_err_t httpd_query_key_value(const char*,const char*,char*,size_t);
int httpd_req_recv(httpd_req_t*,char*,size_t);

#define WIFI_EVENT_SCAN_DONE 4
typedef struct {uint8_t ssid[33];int8_t rssi;int authmode;} wifi_ap_record_t;
esp_err_t esp_wifi_scan_start(const void*,bool);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t*,wifi_ap_record_t*);
uint32_t esp_random(void);

#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 1
#define WIFI_PS_NONE 0
esp_err_t esp_wifi_set_ps(int mode);
