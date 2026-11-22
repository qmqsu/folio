#include "folio_transfer.h"
#include "folio_network.h"
#include "folio_power.h"
#include "folio_upload.h"
#include "folio_transfer_guard.h"
#include "folio_storage.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "bootloader_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "folio_transfer_page.inc"
#ifdef ESP_PLATFORM
#include "esp_sntp.h"
#include "esp_log.h"
_Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS>=3,"Folio requires three SNTP server slots");
#endif
static atomic_bool s_scan_done;
static bool s_time_sync;

static httpd_handle_t s_server;
static esp_netif_t *s_ap, *s_sta;
static esp_event_handler_instance_t s_wifi_handler, s_ip_handler;
static bool s_wifi_initialized, s_wifi_started, s_own_loop;
static atomic_bool s_sta_requested;
static atomic_uint s_retries;
static atomic_bool s_stopping;
static atomic_bool s_station_only;
static _Atomic(TaskHandle_t) s_http_task;
static int64_t s_request_deadline;
static portMUX_TYPE s_mux=portMUX_INITIALIZER_UNLOCKED;
static folio_transfer_status_t s_status;
static char s_token[33], s_ap_ip[16], s_sta_ip[16];

typedef struct { char ssid[33], password[65]; } credentials_t;
static credentials_t s_ram_credentials;
static bool s_have_credentials, s_network_running, s_connecting;
static uint32_t s_pending_utc;
static int64_t s_connect_deadline, s_pending_utc_at;
#define FOLIO_UTC_MIN UINT32_C(1704067200)
#define FOLIO_UTC_MAX UINT32_C(4102444799)
__attribute__((weak,noinline)) bool folio_main_io_cancelled(void) { return false; }
bool folio_io_cancelled(void) {
    if(xTaskGetCurrentTaskHandle()!=s_http_task) return folio_main_io_cancelled();
    return atomic_load(&s_stopping) || (s_request_deadline && esp_timer_get_time()>s_request_deadline);
}
static void wipe(void* memory,size_t count) { volatile unsigned char* p=memory; while(count--) *p++=0; }
static void set_message(const char* text) {
    portENTER_CRITICAL(&s_mux); snprintf(s_status.message,sizeof(s_status.message),"%s",text); portEXIT_CRITICAL(&s_mux);
}
void folio_transfer_get_status(folio_transfer_status_t* out) {
    if (!out) return;
    portENTER_CRITICAL(&s_mux); *out=s_status; portEXIT_CRITICAL(&s_mux);
}
void folio_network_get_status(folio_network_status_t* out) {
    if(!out)return;
    time_t now=time(NULL);
    int64_t monotonic=esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if(s_connecting && s_connect_deadline && monotonic>=s_connect_deadline) {
        s_connecting=false;s_sta_requested=false;
        snprintf(s_status.message,sizeof(s_status.message),"Wi-Fi 连接超时，请检查网络后重试。");
    }
    *out=(folio_network_status_t){.running=s_network_running,.connecting=s_connecting,
        .connected=s_status.sta_connected,.has_credentials=s_have_credentials,
        .clock_valid=now>=FOLIO_UTC_MIN && now<=FOLIO_UTC_MAX};
    snprintf(out->message,sizeof(out->message),"%s",s_status.message);
    portEXIT_CRITICAL(&s_mux);
}
bool folio_network_take_utc(uint32_t* utc) {
    if(!utc)return false;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    uint64_t adjusted=s_pending_utc;
    if(adjusted && now>s_pending_utc_at)adjusted+=(uint64_t)(now-s_pending_utc_at)/1000000;
    *utc=adjusted<=FOLIO_UTC_MAX?(uint32_t)adjusted:0;s_pending_utc=0;
    portEXIT_CRITICAL(&s_mux);
    return *utc!=0;
}
static bool set_utc(uint32_t utc,bool pending) {
    if(utc<FOLIO_UTC_MIN || utc>FOLIO_UTC_MAX)return false;
    const struct timeval value={.tv_sec=(time_t)utc,.tv_usec=0};
    if(settimeofday(&value,NULL)!=0)return false;
    if(pending){int64_t now=esp_timer_get_time();portENTER_CRITICAL(&s_mux);s_pending_utc=utc;s_pending_utc_at=now;portEXIT_CRITICAL(&s_mux);}
    return true;
}
static bool parse_utc(const char* value,uint32_t* utc) {
    if(!value || strlen(value)!=10)return false;
    uint64_t parsed=0;
    for(unsigned i=0;i<10;++i){if(value[i]<'0'||value[i]>'9')return false;parsed=parsed*10+(unsigned)(value[i]-'0');}
    if(parsed<FOLIO_UTC_MIN || parsed>FOLIO_UTC_MAX)return false;
    *utc=(uint32_t)parsed;return true;
}
static void random_hex(char out[33]) {
    unsigned char bytes[16]; static const char digits[]="0123456789abcdef";
    esp_fill_random(bytes,sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);++i) { out[i*2]=digits[bytes[i]>>4]; out[i*2+1]=digits[bytes[i]&15]; }
    out[32]=0; wipe(bytes,sizeof(bytes));
}
static bool credentials_valid(const credentials_t* c) {
    size_t ssid=strnlen(c->ssid,sizeof(c->ssid)), password=strnlen(c->password,sizeof(c->password));
    if(!ssid || ssid>32 || password>63 || (password && password<8)) return false;
    for(size_t i=0;i<ssid;++i) if((unsigned char)c->ssid[i]<32 || c->ssid[i]==127) return false;
    for(size_t i=0;i<password;++i) if((unsigned char)c->password[i]<32 || (unsigned char)c->password[i]>126) return false;
    return true;
}
#define WIFI_PROFILE_LIMIT 8
#define WIFI_PROFILE_MAGIC UINT32_C(0x46575031)
typedef struct {uint32_t magic;uint8_t count,active,reserved[2];credentials_t items[WIFI_PROFILE_LIMIT];} wifi_profiles_t;
static bool profiles_load(wifi_profiles_t* profiles) {
    memset(profiles,0,sizeof(*profiles));profiles->magic=WIFI_PROFILE_MAGIC;
    if(nvs_flash_init()!=ESP_OK)return false;
    nvs_handle_t h;esp_err_t result=nvs_open("folio_wifi",NVS_READONLY,&h);
    if(result==ESP_ERR_NVS_NOT_FOUND)return true;
    if(result!=ESP_OK)return false;
    size_t size=sizeof(*profiles);result=nvs_get_blob(h,"profiles",profiles,&size);
    if(result==ESP_ERR_NVS_NOT_FOUND){
        memset(profiles,0,sizeof(*profiles));profiles->magic=WIFI_PROFILE_MAGIC;
        size=sizeof(credentials_t);result=nvs_get_blob(h,"sta",&profiles->items[0],&size);
        if(result==ESP_OK && size==sizeof(credentials_t) && credentials_valid(&profiles->items[0]))profiles->count=1;
        else if(result!=ESP_ERR_NVS_NOT_FOUND){nvs_close(h);return false;}
        nvs_close(h);return true;
    }
    nvs_close(h);
    if(result!=ESP_OK || size!=sizeof(*profiles) || profiles->magic!=WIFI_PROFILE_MAGIC || profiles->count>WIFI_PROFILE_LIMIT ||
       (profiles->count && profiles->active>=profiles->count))return false;
    for(unsigned i=0;i<profiles->count;++i){
        if(!credentials_valid(&profiles->items[i]))return false;
        for(unsigned j=0;j<i;++j)if(!strcmp(profiles->items[i].ssid,profiles->items[j].ssid))return false;
    }
    return true;
}
static bool profiles_save(const wifi_profiles_t* profiles) {
    nvs_handle_t h;if(nvs_open("folio_wifi",NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t result=nvs_set_blob(h,"profiles",profiles,sizeof(*profiles));
    if(result==ESP_OK)result=nvs_commit(h);
    nvs_close(h);return result==ESP_OK;
}
static bool credentials_save(const credentials_t* credentials) {
    wifi_profiles_t profiles;bool okay=profiles_load(&profiles);
    if(okay){
        unsigned i=0;while(i<profiles.count && strcmp(profiles.items[i].ssid,credentials->ssid))++i;
        if(i==WIFI_PROFILE_LIMIT)okay=false;
        else if(i<profiles.count && profiles.active==i && !memcmp(&profiles.items[i],credentials,sizeof(*credentials)))okay=true;
        else {if(i==profiles.count)++profiles.count;profiles.active=i;profiles.items[i]=*credentials;okay=profiles_save(&profiles);}
    }
    wipe(&profiles,sizeof(profiles));return okay;
}
static bool credentials_load(credentials_t* credentials) {
    portENTER_CRITICAL(&s_mux);bool in_ram=s_have_credentials;if(in_ram)*credentials=s_ram_credentials;portEXIT_CRITICAL(&s_mux);
    if(in_ram)return true;
    wifi_profiles_t profiles;bool okay=profiles_load(&profiles) && profiles.count;
    if(okay)*credentials=profiles.items[profiles.active];
    wipe(&profiles,sizeof(profiles));return okay;
}
bool folio_network_saved_password(const char* ssid,char* password,size_t cap) {
    if(password && cap)password[0]=0;
    if(!ssid)return false;
    wifi_profiles_t profiles;bool found=false;
    if(profiles_load(&profiles))for(unsigned i=0;i<profiles.count;++i)if(!strcmp(ssid,profiles.items[i].ssid)){
        if(password && cap)snprintf(password,cap,"%s",profiles.items[i].password);
        found=true;break;
    }
    wipe(&profiles,sizeof(profiles));return found;
}
bool folio_network_forget(const char* ssid,char* error,size_t cap) {
    if(!ssid || !folio_network_stop()){snprintf(error,cap,"网络正在关闭，请稍后再试。");return false;}
    wifi_profiles_t profiles;bool okay=profiles_load(&profiles);
    if(okay)for(unsigned i=0;i<profiles.count;++i)if(!strcmp(ssid,profiles.items[i].ssid)){
        memmove(&profiles.items[i],&profiles.items[i+1],(profiles.count-i-1)*sizeof(credentials_t));
        --profiles.count;wipe(&profiles.items[profiles.count],sizeof(credentials_t));
        if(profiles.active>i)--profiles.active;else if(profiles.active==i)profiles.active=0;
        okay=profiles_save(&profiles);break;
    }
    wipe(&profiles,sizeof(profiles));
    if(!okay){snprintf(error,cap,"未能忘记网络，请重试。");return false;}
    portENTER_CRITICAL(&s_mux);
    if(!strcmp(s_ram_credentials.ssid,ssid)){wipe(&s_ram_credentials,sizeof(s_ram_credentials));s_have_credentials=false;}
    portEXIT_CRITICAL(&s_mux);return true;
}
bool folio_transfer_forget_wifi(void) {
    if(s_server || s_ap || s_sta || s_wifi_initialized || s_wifi_started || s_own_loop)return false;
    if(nvs_flash_init()!=ESP_OK)return false;
    nvs_handle_t h;esp_err_t result=nvs_open("folio_wifi",NVS_READWRITE,&h);
    if(result!=ESP_OK)return false;
    const char* keys[]={"profiles","sta"};
    for(unsigned i=0;i<2 && result==ESP_OK;++i){result=nvs_erase_key(h,keys[i]);if(result==ESP_ERR_NVS_NOT_FOUND)result=ESP_OK;}
    if(result==ESP_OK)result=nvs_commit(h);
    nvs_close(h);
    if(result!=ESP_OK)return false;
    portENTER_CRITICAL(&s_mux);wipe(&s_ram_credentials,sizeof(s_ram_credentials));s_have_credentials=false;portEXIT_CRITICAL(&s_mux);
    return true;
}
static bool station_configure(const credentials_t* credentials,bool remember,char* error,size_t cap) {
    if(!credentials_valid(credentials)) { snprintf(error,cap,"网络名需 1–32 字节；密码为空或 8–63 个英文字符。"); return false; }
    wifi_config_t wifi={0};
    memcpy(wifi.sta.ssid,credentials->ssid,strlen(credentials->ssid));
    memcpy(wifi.sta.password,credentials->password,strlen(credentials->password));
    wifi.sta.threshold.authmode=credentials->password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
    wifi.sta.pmf_cfg.capable=true;
    wifi.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
    s_sta_requested=false;
    if(s_wifi_started) esp_wifi_disconnect();
    esp_err_t result=esp_wifi_set_config(WIFI_IF_STA,&wifi); wipe(&wifi,sizeof(wifi));
    if(result!=ESP_OK || (remember && !credentials_save(credentials))) { snprintf(error,cap,"无法配置 Wi-Fi 或保存密码。"); return false; }
    int64_t deadline=esp_timer_get_time()+INT64_C(30000000);
    portENTER_CRITICAL(&s_mux);s_ram_credentials=*credentials;s_have_credentials=true;s_connecting=true;s_connect_deadline=deadline;s_status.sta_connected=false;s_status.sta_url[0]=0;s_sta_ip[0]=0;portEXIT_CRITICAL(&s_mux);
    s_retries=0; s_sta_requested=true;
    if(s_wifi_started && esp_wifi_connect()!=ESP_OK) { s_sta_requested=false;portENTER_CRITICAL(&s_mux);s_connecting=false;portEXIT_CRITICAL(&s_mux);snprintf(error,cap,"无法开始连接 Wi-Fi，请重试。"); return false; }
    set_message(s_station_only?"正在连接 Wi-Fi。":"正在连接 Wi-Fi，小纸热点仍可使用。"); return true;
}
static void connect_station(void) {
    if(esp_wifi_connect()!=ESP_OK) {
        s_sta_requested=false;portENTER_CRITICAL(&s_mux);s_connecting=false;portEXIT_CRITICAL(&s_mux);
        set_message("无法开始连接 Wi-Fi，请重试。");
    }
}
static void disconnect_message(unsigned reason) {
    const char* description="Wi-Fi 连接中断，请重试";
    switch(reason) {
        case WIFI_REASON_NO_AP_FOUND:description="未找到网络，请检查距离和 2.4 GHz 网络";break;
        case WIFI_REASON_AUTH_EXPIRE:description="Wi-Fi 认证超时，请靠近路由器重试";break;
        case WIFI_REASON_AUTH_FAIL:description="网络认证未通过，请核对密码与路由器设置";break;
        case WIFI_REASON_ASSOC_FAIL:description="路由器未接受连接，请稍后重试";break;
        case WIFI_REASON_HANDSHAKE_TIMEOUT:case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            description="网络握手超时，请检查信号与路由器设置";break;
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            description="网络加密方式不兼容，请检查路由器设置";break;
        default:break;
    }
    char message[128];snprintf(message,sizeof(message),"%s（%u）",description,reason);set_message(message);
}
static void network_event(void* arg,esp_event_base_t base,int32_t id,void* data) {
    (void)arg;
    if(atomic_load(&s_stopping)) return;
    if(base==WIFI_EVENT && id==WIFI_EVENT_SCAN_DONE)atomic_store(&s_scan_done,true);
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START && s_sta_requested)connect_station();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        unsigned reason=data?((wifi_event_sta_disconnected_t*)data)->reason:0;
#ifdef ESP_PLATFORM
        ESP_LOGW("folio_wifi","STA disconnected: reason=%u, retry=%u",reason,(unsigned)s_retries);
#endif
        int64_t deadline=esp_timer_get_time()+INT64_C(30000000);
        portENTER_CRITICAL(&s_mux);if(!s_connect_deadline)s_connect_deadline=deadline;s_status.sta_connected=false;s_connecting=s_sta_requested; s_status.sta_url[0]=0; s_sta_ip[0]=0; portEXIT_CRITICAL(&s_mux);
        if(s_sta_requested && s_retries++<3)connect_station();
        else if(s_sta_requested) {s_sta_requested=false;portENTER_CRITICAL(&s_mux);s_connecting=false;portEXIT_CRITICAL(&s_mux);disconnect_message(reason);}
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        if(!s_sta_requested)return;
        ip_event_got_ip_t* event=data; char ip[16],url[128];
        snprintf(ip,sizeof(ip),IPSTR,IP2STR(&event->ip_info.ip));
        if(s_station_only)url[0]=0;else snprintf(url,sizeof(url),"http://%s/",ip);
        portENTER_CRITICAL(&s_mux); snprintf(s_sta_ip,sizeof(s_sta_ip),"%s",ip); snprintf(s_status.sta_url,sizeof(s_status.sta_url),"%s",url); s_status.sta_connected=true;s_connecting=false;s_connect_deadline=0; portEXIT_CRITICAL(&s_mux);
        s_retries=0; set_message(s_station_only?"Wi-Fi 已连接。":"Wi-Fi 已连接，两个网址均可传书。");
    }
}
static void response_headers(httpd_req_t* req) {
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"X-Frame-Options","DENY");
}
static esp_err_t json_error(httpd_req_t* req,const char* status,const char* message) {
    // Internal messages only: escape quotes/control bytes rather than echo request content.
    char buffer[768]; size_t used=0;
    used+=(size_t)snprintf(buffer,sizeof(buffer),"{\"error\":\"");
    for(const unsigned char* p=(const unsigned char*)message;*p && used+7<sizeof(buffer);++p) {
        if(*p=='"' || *p=='\\') { buffer[used++]='\\'; buffer[used++]=(char)*p; }
        else if(*p<32) used+=(size_t)snprintf(buffer+used,sizeof(buffer)-used,"\\u%04x",*p);
        else buffer[used++]=(char)*p;
    }
    buffer[used++]='"'; buffer[used++]='}'; buffer[used]=0;
    response_headers(req); httpd_resp_set_status(req,status); httpd_resp_set_type(req,"application/json; charset=utf-8");
    httpd_resp_send(req,buffer,(ssize_t)used);
    // Close on errors so an unread request body cannot be reused as a new request.
    return ESP_FAIL;
}
static bool host_allowed(httpd_req_t* req) {
    char host[48], origin[64]={0}, sta[16];
    if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK) return false;
    portENTER_CRITICAL(&s_mux); snprintf(sta,sizeof(sta),"%s",s_sta_ip); portEXIT_CRITICAL(&s_mux);
    if(httpd_req_get_hdr_value_len(req,"Origin") && httpd_req_get_hdr_value_str(req,"Origin",origin,sizeof(origin))!=ESP_OK) return false;
    return folio_transfer_host_guard(host,origin,s_ap_ip,sta);
}
static bool authorized(httpd_req_t* req,bool token_required) {
    s_http_task=xTaskGetCurrentTaskHandle();
    s_request_deadline=esp_timer_get_time()+INT64_C(300000000);
    if(atomic_load(&s_stopping) || !host_allowed(req)) { json_error(req,"403 Forbidden","访问来源无效，请使用小纸显示的网址。"); return false; }
    if(token_required) {
        char token[33];
        if(httpd_req_get_hdr_value_len(req,"X-Folio-Token")!=32 || httpd_req_get_hdr_value_str(req,"X-Folio-Token",token,sizeof(token))!=ESP_OK) { json_error(req,"403 Forbidden","会话尚未就绪，请刷新传书网页。"); return false; }
        bool matches=folio_transfer_token_guard(token,s_token);
        wipe(token,sizeof(token));
        if(!matches) { json_error(req,"403 Forbidden","本次会话已失效，请刷新传书网页。"); return false; }
    }
    return true;
}
static bool form_field(const char* form,const char* key,char* out,size_t cap) {
    char encoded[FOLIO_UPLOAD_ENCODED_NAME_CAP];
    if(httpd_query_key_value(form,key,encoded,sizeof(encoded))!=ESP_OK)return false;
    return folio_transfer_decode(encoded,out,cap,true);
}
static bool body(httpd_req_t* req,char* buffer,size_t cap) {
    if(!req->content_len || req->content_len>=cap)return false;
    size_t read=0;
    while(read<req->content_len && !folio_io_cancelled()) {
        int n=httpd_req_recv(req,buffer+read,req->content_len-read);
        if(n<=0)return false;
        read+=(size_t)n;
    }
    buffer[read]=0; return read==req->content_len && !memchr(buffer,0,read);
}
static esp_err_t page_handler(httpd_req_t* req) {
    if(!authorized(req,false))return ESP_FAIL;
    response_headers(req); httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req,transfer_page,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t success(httpd_req_t* req) {
    response_headers(req); httpd_resp_set_type(req,"application/json"); return httpd_resp_send(req,"{\"ok\":true}",HTTPD_RESP_USE_STRLEN);
}
static const char* result_status(folio_upload_result_t result) {
    if(result==FOLIO_UPLOAD_CONFLICT)return "409 Conflict";
    if(result==FOLIO_UPLOAD_TOO_LARGE)return "413 Content Too Large";
    if(result==FOLIO_UPLOAD_IO)return "507 Insufficient Storage";
    if(result==FOLIO_UPLOAD_INVALID)return "422 Unprocessable Content";
    return "400 Bad Request";
}
static void copy_utf8(char* out,size_t cap,const char* text) {
    size_t n=strlen(text);if(n>=cap){n=cap-1;while(n && ((unsigned char)text[n]&0xc0)==0x80)--n;}
    memcpy(out,text,n);out[n]=0;
}
static void record_upload(const char* name,size_t size,folio_upload_result_t result,bool saved_only,const char* message) {
    folio_transfer_record_t record={.size=size>UINT32_MAX?UINT32_MAX:(uint32_t)size,.result=(uint8_t)result,.saved_only=saved_only};
    copy_utf8(record.name,sizeof(record.name),name);copy_utf8(record.message,sizeof(record.message),message);
    portENTER_CRITICAL(&s_mux);
    memmove(s_status.recent+1,s_status.recent,(FOLIO_TRANSFER_RECENT_CAP-1)*sizeof(record));
    s_status.recent[0]=record;if(s_status.recent_count<FOLIO_TRANSFER_RECENT_CAP)++s_status.recent_count;
    portEXIT_CRITICAL(&s_mux);
}
static esp_err_t upload_error(httpd_req_t* req,const char* name,folio_upload_result_t result,const char* status,const char* message) {
    record_upload(name,req->content_len,result,false,message);set_message(message);
    return json_error(req,status,message);
}
static esp_err_t upload_handler(httpd_req_t* req) {
    if(!authorized(req,true))return ESP_FAIL;
    char encoded[FOLIO_UPLOAD_ENCODED_NAME_CAP],name[FOLIO_UPLOAD_NAME_CAP],nonce[33],error[160]={0};
    if(httpd_req_get_hdr_value_str(req,"X-Folio-Name",encoded,sizeof(encoded))!=ESP_OK || !folio_transfer_decode(encoded,name,sizeof(name),false))
        return upload_error(req,"无效文件名",FOLIO_UPLOAD_BAD_NAME,"400 Bad Request","文件名编码无效，请重命名后重试。");
    char target[FOLIO_UPLOAD_PATH_CAP];folio_upload_kind_t kind;
    if(!folio_upload_name(name,&kind,NULL))return upload_error(req,"无效文件名",FOLIO_UPLOAD_BAD_NAME,"400 Bad Request","文件名无效或文件类型暂不支持。");
    char directory[16];snprintf(directory,sizeof(directory),"%s",folio_upload_directory(kind));
    if(httpd_req_get_hdr_value_len(req,"X-Folio-Directory") &&
       (httpd_req_get_hdr_value_str(req,"X-Folio-Directory",directory,sizeof(directory))!=ESP_OK ||
        !folio_upload_in_directory(name,directory,NULL)))return upload_error(req,name,FOLIO_UPLOAD_BAD_NAME,"400 Bad Request","文件类型与上传目录不匹配。");
    if(!strcmp(directory,"wallpapers"))snprintf(directory,sizeof(directory),"picture");
    int n=snprintf(target,sizeof(target),FOLIO_SD_ROOT "/%s/%s",directory,name);
    if(n<0 || (size_t)n>=sizeof(target))return upload_error(req,name,FOLIO_UPLOAD_BAD_NAME,"400 Bad Request","文件名过长，请缩短后重试。");
    if(!folio_storage_lock(1000))return upload_error(req,name,FOLIO_UPLOAD_IO,"503 Service Unavailable","TF 卡正在使用，请稍后重试。");
    random_hex(nonce);folio_upload_t* upload=NULL;
    folio_upload_result_t result=folio_storage_validate_parent(target,error,sizeof(error))?
        folio_upload_begin_in_directory(FOLIO_SD_ROOT,name,directory,req->content_len,nonce,folio_transfer_validate,&upload):FOLIO_UPLOAD_BAD_NAME;
    size_t capacity=16384, buffered=0;
    char* chunk=result==FOLIO_UPLOAD_OK?malloc(capacity):NULL;
    if(result==FOLIO_UPLOAD_OK && !chunk){capacity=4096;chunk=malloc(capacity);}
    if(result==FOLIO_UPLOAD_OK && !chunk)result=FOLIO_UPLOAD_IO;
    portENTER_CRITICAL(&s_mux);s_status.busy=result==FOLIO_UPLOAD_OK;s_status.received=0;s_status.total=(uint32_t)req->content_len;portEXIT_CRITICAL(&s_mux);
    int64_t started=esp_timer_get_time(), last_data=started;
    size_t remaining=req->content_len;
    while(!result && remaining) {
        int64_t now=esp_timer_get_time();
        if(atomic_load(&s_stopping) || now-last_data>INT64_C(300000000) || now-started>INT64_C(1800000000)){result=FOLIO_UPLOAD_TRUNCATED;break;}
        size_t available=capacity-buffered;
        int n=httpd_req_recv(req,chunk+buffered,remaining<available?remaining:available);
        if(n==HTTPD_SOCK_ERR_TIMEOUT)continue;
        if(n<=0 || (size_t)n>remaining || (size_t)n>available){result=FOLIO_UPLOAD_TRUNCATED;break;}
        remaining-=(size_t)n;buffered+=(size_t)n;
        last_data=esp_timer_get_time();
        if(buffered==capacity || !remaining){
            result=folio_upload_write(upload,chunk,buffered);
            buffered=0;
        }
        portENTER_CRITICAL(&s_mux);s_status.received=(uint32_t)(req->content_len-remaining);portEXIT_CRITICAL(&s_mux);
    }
    free(chunk);
    if(!result && atomic_load(&s_stopping))result=FOLIO_UPLOAD_TRUNCATED;
    if(!result){set_message("正在校验上传文件。");result=folio_upload_finish(&upload,error,sizeof(error));}
    else folio_upload_abort(&upload);
    bool saved_only=!result && folio_transfer_saved_only(target,kind);
    folio_storage_unlock();
    portENTER_CRITICAL(&s_mux);s_status.busy=false;if(!result)++s_status.completed;portEXIT_CRITICAL(&s_mux);
    if(result)return upload_error(req,name,result,result_status(result),error[0]?error:folio_upload_message(result));
    const char* message=kind==FOLIO_UPLOAD_TXT && saved_only?"已原样保存；请转换为 UTF-8 或 GBK 后阅读。":folio_upload_is_image(kind)?"已保存，能否设为壁纸以设备图像支持为准。":
        saved_only?"已原样保存；设备暂不支持阅读此格式。":
        (kind==FOLIO_UPLOAD_TXT || kind==FOLIO_UPLOAD_EPUB)?"已保存；阅读兼容性以设备支持为准。":"上传完成。";
    record_upload(name,req->content_len,FOLIO_UPLOAD_OK,saved_only,message);set_message(message);
    response_headers(req);httpd_resp_set_type(req,"application/json; charset=utf-8");
    char reply[256];snprintf(reply,sizeof(reply),"{\"ok\":true,\"saved_only\":%s,\"message\":\"%s\"}",saved_only?"true":"false",message);
    return httpd_resp_send(req,reply,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t json_string_chunk(httpd_req_t* req,const char* text) {
    char escaped[1024];size_t used=0;escaped[used++]='"';
    for(const unsigned char* p=(const unsigned char*)text;*p;++p) {
        if(used+7>=sizeof(escaped))return ESP_FAIL;
        if(*p=='"'||*p=='\\'){escaped[used++]='\\';escaped[used++]=(char)*p;}
        else if(*p<32)used+=(size_t)snprintf(escaped+used,sizeof(escaped)-used,"\\u%04x",*p);
        else escaped[used++]=(char)*p;
    }
    escaped[used++]='"';return httpd_resp_send_chunk(req,escaped,(ssize_t)used);
}
static esp_err_t files_handler(httpd_req_t* req) {
    if(!authorized(req,true))return ESP_FAIL;
    char query[80],kind[16],number[16],error[128];size_t offset=0;
    if(httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK || httpd_query_key_value(query,"kind",kind,sizeof(kind))!=ESP_OK || (strcmp(kind,"books")&&strcmp(kind,"fonts")&&strcmp(kind,"update")&&strcmp(kind,"picture")&&strcmp(kind,"wallpapers")))return json_error(req,"400 Bad Request","文件类别无效，请刷新页面。");
    if(!strcmp(kind,"wallpapers"))snprintf(kind,sizeof(kind),"picture");
    if(httpd_query_key_value(query,"offset",number,sizeof(number))==ESP_OK) { char* end; unsigned long value=strtoul(number,&end,10); if(!number[0]||*end||value>FOLIO_STORAGE_ENTRY_LIMIT)return json_error(req,"400 Bad Request","页码无效，请刷新文件列表。");offset=(size_t)value; }
    folio_storage_entry_t* entries=calloc(16,sizeof(*entries)); if(!entries)return json_error(req,"503 Service Unavailable","内存不足，请关闭传书后重试。");
    char path[FOLIO_STORAGE_PATH_CAP];snprintf(path,sizeof(path),FOLIO_SD_ROOT "/%s",kind);size_t count=0,total=0;
    bool ok=folio_storage_list(path,offset,entries,16,&count,&total,error,sizeof(error));
    if(!ok){free(entries);return json_error(req,"503 Service Unavailable",error);}
    response_headers(req);httpd_resp_set_type(req,"application/json; charset=utf-8");
    esp_err_t result=httpd_resp_send_chunk(req,"{\"files\":[",10);bool comma=false;
    for(size_t i=0;i<count && result==ESP_OK;++i) {
        if(entries[i].directory || !folio_upload_in_directory(entries[i].name,kind,NULL))continue;
        const char* prefix=comma?",{\"name\":" : "{\"name\":";comma=true;
        result=httpd_resp_send_chunk(req,prefix,HTTPD_RESP_USE_STRLEN);
        if(result==ESP_OK)result=json_string_chunk(req,entries[i].name);
        char tail[64];snprintf(tail,sizeof(tail),",\"bytes\":%llu}",(unsigned long long)entries[i].bytes);
        if(result==ESP_OK)result=httpd_resp_send_chunk(req,tail,HTTPD_RESP_USE_STRLEN);
    }
    free(entries);
    if(result==ESP_OK)result=httpd_resp_send_chunk(req,offset+count<total?"],\"more\":true}":"],\"more\":false}",HTTPD_RESP_USE_STRLEN);
    if(result==ESP_OK)result=httpd_resp_send_chunk(req,NULL,0);
    return result;
}
static esp_err_t delete_handler(httpd_req_t* req) {
    if(!authorized(req,true))return ESP_FAIL;
    char form[FOLIO_UPLOAD_FORM_CAP],name[FOLIO_UPLOAD_NAME_CAP],directory[16],path[FOLIO_UPLOAD_PATH_CAP],error[128];folio_upload_kind_t kind;
    if(!body(req,form,sizeof(form)) || !form_field(form,"name",name,sizeof(name)) ||
       !form_field(form,"kind",directory,sizeof(directory)) || !folio_upload_in_directory(name,directory,&kind))return json_error(req,"400 Bad Request","文件与目录不匹配，请刷新列表后重试。");
    if(!strcmp(directory,"wallpapers"))snprintf(directory,sizeof(directory),"picture");
    if(kind==FOLIO_UPLOAD_TTF)return json_error(req,"403 Forbidden","请在小纸的文件管理器中删除字体。");
    int n=snprintf(path,sizeof(path),FOLIO_SD_ROOT "/%s/%s",directory,name);
    if(n<0 || (size_t)n>=sizeof(path))return json_error(req,"400 Bad Request","文件名过长，请缩短后重试。");
    if(!folio_storage_lock(1000))return json_error(req,"503 Service Unavailable","TF 卡正在使用，请稍后重试。");
    folio_storage_preview_t preview={0};
    bool ok=folio_storage_validate_parent(path,error,sizeof(error)) && folio_storage_preview_delete(path,&preview,error,sizeof(error));
    if(ok && preview.directory){ok=false;snprintf(error,sizeof(error),"传书页面仅可删除单个文件。");}
    if(ok)ok=folio_storage_commit_delete(&preview,error,sizeof(error));
    folio_storage_cancel_delete();folio_storage_unlock();
    if(!ok)return json_error(req,"409 Conflict",error);
    portENTER_CRITICAL(&s_mux);++s_status.completed;portEXIT_CRITICAL(&s_mux);
    return success(req);
}
static esp_err_t wifi_handler(httpd_req_t* req) {
    if(!authorized(req,true))return ESP_FAIL;
    char form[512],remember[4],utc_text[12],error[160]={0};credentials_t credentials={0};uint32_t utc=0;
    bool parsed=body(req,form,sizeof(form)) && form_field(form,"ssid",credentials.ssid,sizeof(credentials.ssid)) && form_field(form,"password",credentials.password,sizeof(credentials.password)) && form_field(form,"remember",remember,sizeof(remember));
    if(parsed && strcmp(remember,"0") && strcmp(remember,"1"))parsed=false;
    if(parsed && httpd_query_key_value(form,"utc",utc_text,sizeof(utc_text))==ESP_OK)parsed=parse_utc(utc_text,&utc);
    else if(parsed && strstr(form,"utc="))parsed=false;
    wipe(form,sizeof(form));
    bool ok=parsed && station_configure(&credentials,!strcmp(remember,"1"),error,sizeof(error));
    wipe(&credentials,sizeof(credentials));
    if(!ok)return json_error(req,"400 Bad Request",parsed?error:"Wi-Fi 信息无效，请检查后重试。");
    if(utc && !set_utc(utc,true))return json_error(req,"500 Internal Server Error","Wi-Fi 已配置，但校时失败，请再次校时。");
    return success(req);
}
static esp_err_t session_handler(httpd_req_t* req) {
    // Same-origin bootstrap, not a login. No CORS, cookies or URL credentials.
    if(!authorized(req,false))return ESP_FAIL;
    if(req->content_len)return json_error(req,"400 Bad Request","会话请求不接受请求正文。");
    response_headers(req);httpd_resp_set_type(req,"application/json; charset=utf-8");
    char reply[64];snprintf(reply,sizeof(reply),"{\"token\":\"%s\"}",s_token);
    return httpd_resp_send(req,reply,HTTPD_RESP_USE_STRLEN);
}
static esp_err_t time_handler(httpd_req_t* req) {
    if(!authorized(req,true))return ESP_FAIL;
    char form[64],value[12];uint32_t utc;
    if(!body(req,form,sizeof(form)) || !form_field(form,"utc",value,sizeof(value)) || !parse_utc(value,&utc))
        return json_error(req,"400 Bad Request","时间无效，请检查手机或电脑的日期时间。");
    if(!set_utc(utc,true))return json_error(req,"500 Internal Server Error","校时失败，请重试。");
    return success(req);
}
static esp_err_t status_handler(httpd_req_t* req) {
    if(!authorized(req,true))return ESP_FAIL;
    folio_transfer_status_t status;folio_transfer_get_status(&status);
    response_headers(req);httpd_resp_set_type(req,"application/json; charset=utf-8");
    esp_err_t result=httpd_resp_send_chunk(req,"{\"sta_url\":",11);
    if(result==ESP_OK)result=json_string_chunk(req,status.sta_url);
    if(result==ESP_OK)result=httpd_resp_send_chunk(req,",\"message\":",11);
    if(result==ESP_OK)result=json_string_chunk(req,status.message);
    char counters[192];snprintf(counters,sizeof(counters),",\"busy\":%s,\"received\":%lu,\"total\":%lu,\"completed\":%lu,\"recent_count\":%u,\"recent\":[",status.busy?"true":"false",(unsigned long)status.received,(unsigned long)status.total,(unsigned long)status.completed,status.recent_count);
    if(result==ESP_OK)result=httpd_resp_send_chunk(req,counters,HTTPD_RESP_USE_STRLEN);
    for(unsigned i=0;result==ESP_OK && i<status.recent_count;++i) {
        const folio_transfer_record_t* record=&status.recent[i];
        result=httpd_resp_send_chunk(req,i?",{\"name\":" : "{\"name\":",HTTPD_RESP_USE_STRLEN);
        if(result==ESP_OK)result=json_string_chunk(req,record->name);
        snprintf(counters,sizeof(counters),",\"size\":%lu,\"result\":%u,\"saved_only\":%s,\"message\":",(unsigned long)record->size,record->result,record->saved_only?"true":"false");
        if(result==ESP_OK)result=httpd_resp_send_chunk(req,counters,HTTPD_RESP_USE_STRLEN);
        if(result==ESP_OK)result=json_string_chunk(req,record->message);
        if(result==ESP_OK)result=httpd_resp_send_chunk(req,"}",1);
    }
    if(result==ESP_OK)result=httpd_resp_send_chunk(req,"]}",2);
    if(result==ESP_OK)result=httpd_resp_send_chunk(req,NULL,0);
    wipe(&status,sizeof(status));return result;
}

static bool prepare_directories(char* error,size_t cap) {
    if(!folio_storage_lock(1000)){snprintf(error,cap,"TF 卡正在使用，请稍后重试。");return false;}
    const char* names[]={"books","fonts","update","picture"};bool ok=true;
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]) && ok;++i) {
        char path[FOLIO_STORAGE_PATH_CAP];snprintf(path,sizeof(path),FOLIO_SD_ROOT "/%s",names[i]);
        ok=folio_storage_validate_parent(path,error,cap);
        if(ok && mkdir(path,0777)!=0 && errno!=EEXIST)ok=false;
        struct stat st;if(ok && (stat(path,&st)!=0 || !S_ISDIR(st.st_mode)))ok=false;
        if(ok)ok=folio_upload_clean_staging(path);
    }
    folio_storage_unlock();
    if(!ok && !error[0])snprintf(error,cap,"无法准备传书目录，请检查 TF 卡。");
    return ok;
}

bool folio_transfer_start(const folio_transfer_config_t* config,char* error,size_t cap) {
    char fallback[160];if(!error||!cap){error=fallback;cap=sizeof(fallback);}error[0]=0;
    if(s_server || s_ap || s_sta || s_wifi_initialized || s_wifi_started || s_own_loop) {
        if(s_server && !atomic_load(&s_stopping))return true;
        snprintf(error,cap,"上次传书尚未停止，请重试关闭。");return false;
    }
    if(!prepare_directories(error,cap))return false;
    credentials_t credentials={0};
    if(config && config->use_saved && (!config->ssid || !config->ssid[0])) {
        if(!credentials_load(&credentials))wipe(&credentials,sizeof(credentials));
    } else if(config && config->ssid && config->ssid[0]) {
        if(strlen(config->ssid)>32 || (config->password && strlen(config->password)>63)){snprintf(error,cap,"网络名或密码过长，请缩短后重试。");return false;}
        snprintf(credentials.ssid,sizeof(credentials.ssid),"%s",config->ssid);snprintf(credentials.password,sizeof(credentials.password),"%s",config->password?config->password:"");
    }
    atomic_store(&s_stopping,false);s_station_only=false;memset(&s_status,0,sizeof(s_status));s_sta_ip[0]=0;s_sta_requested=false;s_retries=0;s_connecting=false;
    // At this point Folio owns no RF/ADC session. Enable hardware entropy only
    // while generating secrets, and disable it before the Wi-Fi driver starts.
    bootloader_random_enable();random_hex(s_token);bootloader_random_disable();
    snprintf(s_status.ssid,sizeof(s_status.ssid),"Folio");
    snprintf(s_status.ap_password,sizeof(s_status.ap_password),"foliomuki");
    s_status.access_code[0]=0; // Retained only for source compatibility.
    esp_err_t result=esp_netif_init();if(result!=ESP_OK)goto fail;
    result=esp_event_loop_create_default();if(result==ESP_OK)s_own_loop=true;else if(result!=ESP_ERR_INVALID_STATE)goto fail;
    s_ap=esp_netif_create_default_wifi_ap();s_sta=esp_netif_create_default_wifi_sta();if(!s_ap||!s_sta)goto fail;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();init.nvs_enable=false;result=esp_wifi_init(&init);if(result!=ESP_OK)goto fail;s_wifi_initialized=true;
    if(esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK || esp_wifi_set_mode(WIFI_MODE_APSTA)!=ESP_OK)goto fail;
    wifi_config_t ap={0};memcpy(ap.ap.ssid,s_status.ssid,strlen(s_status.ssid));ap.ap.ssid_len=(uint8_t)strlen(s_status.ssid);
    memcpy(ap.ap.password,s_status.ap_password,strlen(s_status.ap_password));ap.ap.authmode=WIFI_AUTH_WPA2_PSK;ap.ap.max_connection=2;ap.ap.channel=1;ap.ap.pmf_cfg.capable=true;
    result=esp_wifi_set_config(WIFI_IF_AP,&ap);wipe(&ap,sizeof(ap));if(result!=ESP_OK)goto fail;
    if(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,network_event,NULL,&s_wifi_handler)!=ESP_OK || esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,network_event,NULL,&s_ip_handler)!=ESP_OK)goto fail;
    if(credentials.ssid[0] && !station_configure(&credentials,config&&config->remember,error,cap))goto fail;
    wipe(&credentials,sizeof(credentials));
    if(esp_wifi_start()!=ESP_OK)goto fail;
    s_wifi_started=true;
    if(esp_wifi_set_ps(WIFI_PS_NONE)!=ESP_OK)goto fail;
    portENTER_CRITICAL(&s_mux);s_network_running=true;portEXIT_CRITICAL(&s_mux);
    esp_netif_ip_info_t info;if(esp_netif_get_ip_info(s_ap,&info)!=ESP_OK)goto fail;
    snprintf(s_ap_ip,sizeof(s_ap_ip),IPSTR,IP2STR(&info.ip));snprintf(s_status.url,sizeof(s_status.url),"http://%s/",s_ap_ip);
    httpd_config_t server=HTTPD_DEFAULT_CONFIG();server.stack_size=12288;server.max_req_hdr_len=2048;server.max_uri_handlers=10;server.max_open_sockets=3;server.recv_wait_timeout=1;server.send_wait_timeout=1;server.lru_purge_enable=true;
    if(httpd_start(&s_server,&server)!=ESP_OK)goto fail;
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=page_handler},
        {.uri="/api/session",.method=HTTP_GET,.handler=session_handler},
        {.uri="/api/upload",.method=HTTP_POST,.handler=upload_handler},
        {.uri="/api/files",.method=HTTP_GET,.handler=files_handler},
        {.uri="/api/delete",.method=HTTP_POST,.handler=delete_handler},
        {.uri="/api/wifi",.method=HTTP_POST,.handler=wifi_handler},
        {.uri="/api/time",.method=HTTP_POST,.handler=time_handler},
        {.uri="/api/status",.method=HTTP_GET,.handler=status_handler}
    };
    for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i)if(httpd_register_uri_handler(s_server,&routes[i])!=ESP_OK)goto fail;
    portENTER_CRITICAL(&s_mux);s_status.running=true;portEXIT_CRITICAL(&s_mux);
    if(!s_sta_requested)set_message("请连接小纸热点，扫描二维码或打开完整网址。");
    return true;
fail:
    wipe(&credentials,sizeof(credentials));if(!error[0])snprintf(error,cap,"无法启动 Wi-Fi 传书，请重试。");folio_transfer_stop();return false;
}
static bool network_start(bool require_clock,bool scan,char* error,size_t cap) {
    char fallback[160];if(!error||!cap){error=fallback;cap=sizeof(fallback);}error[0]=0;
    time_t now=time(NULL);
    if(require_clock && (now<FOLIO_UTC_MIN || now>FOLIO_UTC_MAX) && !set_utc(folio_power_unix_time(),false)) {
        snprintf(error,cap,"请先在日期与时间页面校准时间。");return false;
    }
    if(s_server || s_ap || s_sta || s_wifi_initialized || s_wifi_started || s_own_loop) {
        if(s_station_only && s_wifi_started && !atomic_load(&s_stopping)) {
            folio_network_status_t state;folio_network_get_status(&state);
            if(state.connected || state.connecting)return true;
            if(!folio_network_stop()){snprintf(error,cap,"网络尚未停止，请先关闭当前连接。");return false;}
        } else {snprintf(error,cap,"网络尚未停止，请先关闭当前连接。");return false;}
    }
    credentials_t credentials={0};
    if(!scan && !credentials_load(&credentials)) {wipe(&credentials,sizeof(credentials));snprintf(error,cap,"请先在设置的 Wi-Fi 页面连接网络。");return false;}
    atomic_store(&s_stopping,false);s_station_only=true;s_sta_requested=false;s_retries=0;
    portENTER_CRITICAL(&s_mux);memset(&s_status,0,sizeof(s_status));s_sta_ip[0]=0;s_connecting=false;portEXIT_CRITICAL(&s_mux);
    esp_err_t result=esp_netif_init();if(result!=ESP_OK)goto fail;
    result=esp_event_loop_create_default();if(result==ESP_OK)s_own_loop=true;else if(result!=ESP_ERR_INVALID_STATE)goto fail;
    s_sta=esp_netif_create_default_wifi_sta();if(!s_sta)goto fail;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();init.nvs_enable=false;
    if(esp_wifi_init(&init)!=ESP_OK)goto fail;
    s_wifi_initialized=true;
    if(esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK)goto fail;
    if(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,network_event,NULL,&s_wifi_handler)!=ESP_OK ||
       esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,network_event,NULL,&s_ip_handler)!=ESP_OK)goto fail;
    if(!scan && !station_configure(&credentials,false,error,cap))goto fail;
    wipe(&credentials,sizeof(credentials));
    if(esp_wifi_start()!=ESP_OK)goto fail;
    s_wifi_started=true;
    if(esp_wifi_set_ps(WIFI_PS_NONE)!=ESP_OK)goto fail;
    portENTER_CRITICAL(&s_mux);s_network_running=true;portEXIT_CRITICAL(&s_mux);
    return true;
fail:
    wipe(&credentials,sizeof(credentials));if(!error[0])snprintf(error,cap,"无法连接 Wi-Fi，请重试。");folio_transfer_stop();return false;
}
bool folio_network_start_station(char* error,size_t cap) {return network_start(true,false,error,cap);}
bool folio_network_connect(char* error,size_t cap) {return network_start(false,false,error,cap);}
bool folio_network_configure(const char* ssid,const char* password,char* error,size_t cap) {
    credentials_t c={0};
    if(!ssid||!password||strlen(ssid)>=sizeof(c.ssid)||strlen(password)>=sizeof(c.password)) {
        snprintf(error,cap,"网络名或密码过长。");return false;
    }
    snprintf(c.ssid,sizeof(c.ssid),"%s",ssid);snprintf(c.password,sizeof(c.password),"%s",password);
    if(!credentials_valid(&c)){wipe(&c,sizeof(c));snprintf(error,cap,"密码为空或 8–63 个英文字符。");return false;}
    if(!credentials_save(&c)){wipe(&c,sizeof(c));snprintf(error,cap,"密码未能保存；最多记住 8 个网络，请先忘记不用的网络再重试。");return false;}
    folio_network_status_t state;folio_network_get_status(&state);
    if(s_station_only && s_wifi_started && !atomic_load(&s_stopping) && !state.connected && !state.connecting && !s_sta_requested){
        bool okay=station_configure(&c,false,error,cap);wipe(&c,sizeof(c));return okay;
    }
    if(!folio_network_stop()){wipe(&c,sizeof(c));snprintf(error,cap,"正在关闭网络，请稍后重试。");return false;}
    portENTER_CRITICAL(&s_mux);s_ram_credentials=c;s_have_credentials=true;portEXIT_CRITICAL(&s_mux);
    wipe(&c,sizeof(c));return true;
}
bool folio_network_scan_start(char* error,size_t cap) {
    if(!folio_network_stop()){snprintf(error,cap,"正在关闭网络，请稍后重试。");return false;}
    if(!network_start(false,true,error,cap))return false;
    atomic_store(&s_scan_done,false);
    if(esp_wifi_scan_start(NULL,false)!=ESP_OK){snprintf(error,cap,"无法扫描网络，请重试。");return false;}
    return true;
}
bool folio_network_scan_result(folio_network_ap_t* items,size_t cap,size_t* count) {
    if(!atomic_exchange(&s_scan_done,false))return false;
    uint16_t n=16;wifi_ap_record_t* records=calloc(n,sizeof(*records));*count=0;
    if(!records)return true;
    if(esp_wifi_scan_get_ap_records(&n,records)==ESP_OK)for(unsigned i=0;i<n && *count<cap;++i) {
        if(!records[i].ssid[0])continue;
        bool duplicate=false;for(size_t j=0;j<*count;++j)if(!strcmp(items[j].ssid,(char*)records[i].ssid))duplicate=true;
        if(duplicate)continue;
        folio_network_ap_t* a=&items[(*count)++];memset(a,0,sizeof(*a));
        memcpy(a->ssid,records[i].ssid,32);a->signal=records[i].rssi;a->secured=records[i].authmode!=WIFI_AUTH_OPEN;
    }
    free(records);return true;
}
#ifdef ESP_PLATFORM
static void ntp_received(struct timeval* tv) {
    if(!tv || tv->tv_sec<FOLIO_UTC_MIN || tv->tv_sec>FOLIO_UTC_MAX)return;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    bool accepted=s_time_sync;
    if(accepted){s_pending_utc=(uint32_t)tv->tv_sec;s_pending_utc_at=now;}
    portEXIT_CRITICAL(&s_mux);
    if(accepted)ESP_LOGI("folio_wifi","Network time received");
}
#endif
bool folio_network_sync_time(char* error,size_t cap) {
    folio_network_status_t state;folio_network_get_status(&state);
    if(!state.connected){snprintf(error,cap,"请先连接 Wi-Fi，再同步时间。");return false;}
#ifdef ESP_PLATFORM
    folio_network_cancel_time();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,"ntp.aliyun.com");
    esp_sntp_setservername(1,"ntp1.aliyun.com");
    esp_sntp_setservername(2,"time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(ntp_received);
    portENTER_CRITICAL(&s_mux);s_time_sync=true;portEXIT_CRITICAL(&s_mux);
    esp_sntp_init();ESP_LOGI("folio_wifi","Network time request started");return true;
#else
    snprintf(error,cap,"主机预览不执行网络校时。");return false;
#endif
}
void folio_network_cancel_time(void) {
    portENTER_CRITICAL(&s_mux);
    bool was_syncing=s_time_sync;s_time_sync=false;
    if(was_syncing)s_pending_utc=0;
    portEXIT_CRITICAL(&s_mux);
#ifdef ESP_PLATFORM
    if(was_syncing)esp_sntp_stop();
#endif
}
bool folio_network_stop(void) {return folio_transfer_stop();}
bool folio_transfer_stop(void) {
    folio_network_cancel_time();
    atomic_store(&s_stopping,true);s_sta_requested=false;
    portENTER_CRITICAL(&s_mux);s_connecting=false;portEXIT_CRITICAL(&s_mux);
    if(s_server){
        bool stopped=false;
        for(unsigned attempt=0;attempt<3;++attempt){if(httpd_stop(s_server)==ESP_OK){stopped=true;break;}vTaskDelay(pdMS_TO_TICKS(25));}
        if(!stopped){set_message("传书尚未停止，请勿拔卡，再次尝试关闭。");return false;}
        s_server=NULL;
    }
    s_http_task=NULL;s_request_deadline=0;
    if(s_wifi_handler){if(esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,s_wifi_handler)!=ESP_OK){set_message("Wi-Fi 回调尚未关闭，请重试。");return false;}s_wifi_handler=NULL;}
    if(s_ip_handler){if(esp_event_handler_instance_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,s_ip_handler)!=ESP_OK){set_message("Wi-Fi 回调尚未关闭，请重试。");return false;}s_ip_handler=NULL;}
    if(s_wifi_started){if(esp_wifi_stop()!=ESP_OK){set_message("Wi-Fi 尚未停止，请再次尝试关闭。");return false;}s_wifi_started=false;}
    if(s_wifi_initialized){if(esp_wifi_deinit()!=ESP_OK){set_message("Wi-Fi 尚未关闭，请再次尝试关闭。");return false;}s_wifi_initialized=false;}
    if(s_ap){esp_netif_destroy_default_wifi(s_ap);s_ap=NULL;}
    if(s_sta){esp_netif_destroy_default_wifi(s_sta);s_sta=NULL;}
    if(s_own_loop){if(esp_event_loop_delete_default()!=ESP_OK){set_message("网络服务尚未关闭，请重试。");return false;}s_own_loop=false;}
    wipe(s_token,sizeof(s_token));s_ap_ip[0]=0;s_sta_ip[0]=0;
    portENTER_CRITICAL(&s_mux);wipe(&s_status,sizeof(s_status));s_network_running=false;s_connecting=false;portEXIT_CRITICAL(&s_mux);
    return true;
}
