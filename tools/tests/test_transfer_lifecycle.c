#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "transfer_sdk.h"
#define settimeofday fake_settimeofday
#define time fake_time
static int fake_settimeofday(const struct timeval* value,const void* zone);
static time_t fake_time(time_t* out);
#define ESP_PLATFORM 1
#include "../../main/net/folio_transfer.c"
#undef ESP_PLATFORM

static sntp_sync_time_cb_t ntp_callback;
static unsigned ntp_starts,ntp_stops;
static const char* ntp_servers[3];
void esp_sntp_setoperatingmode(int mode){assert(mode==SNTP_OPMODE_POLL);}
void esp_sntp_setservername(unsigned index,const char* name){assert(index<3);ntp_servers[index]=name;}
void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb){ntp_callback=cb;}
void esp_sntp_init(void){++ntp_starts;}
void esp_sntp_stop(void){++ntp_stops;}

static int fail_http_stop,fail_wifi_stop,fail_wifi_deinit,fail_unregister,fail_loop_delete,fail_set_clock,fail_connect;
static time_t test_utc;
static int64_t test_monotonic=1000000;
static uint32_t test_rtc;
static unsigned set_clock_calls,http_start_calls;
static int selected_mode;
static unsigned netif_creates,netif_destroys,wifi_init_calls,wifi_stop_calls,wifi_deinit_calls,http_stop_calls,nvs_writes,random_counter,delete_calls;
static bool entropy,driver_started;
static event_callback_t callback;
static wifi_config_t configured_ap,configured_sta;
static const char* request_body;
static const char* request_query;
static const char* request_name;
static const char* request_directory;
static char last_delete_path[256],last_validation_path[256];
static unsigned receive_timeouts,receive_calls;
static bool stop_on_receive;
static char response_status[64];
static const char* request_token;
static const char* request_origin;
static const char* request_host;
static char response_body[16384],last_list_path[256];
static size_t response_used;
static unsigned session_routes;
static uint8_t persisted_profiles[1024],pending_profiles[1024];
static size_t persisted_profile_size,pending_profile_size;
static bool fail_nvs_commit,validation_ok;
bool folio_transfer_saved_only(const char* path,folio_upload_kind_t kind){(void)path;return folio_upload_saved_only(kind);}

static int fake_settimeofday(const struct timeval* value,const void* zone){(void)zone;++set_clock_calls;if(fail_set_clock)return -1;test_utc=value->tv_sec;return 0;}
static time_t fake_time(time_t* out){if(out)*out=test_utc;return test_utc;}
uint32_t folio_power_unix_time(void){return test_rtc;}

esp_err_t esp_event_loop_create_default(void){return ESP_OK;}
esp_err_t esp_event_loop_delete_default(void){return fail_loop_delete?ESP_FAIL:ESP_OK;}
esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int32_t id,event_callback_t cb,void* arg,esp_event_handler_instance_t* out){(void)id;(void)arg;if(b==WIFI_EVENT)callback=cb;*out=(void*)2;return ESP_OK;}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b,int32_t id,esp_event_handler_instance_t p){(void)b;(void)id;(void)p;return fail_unregister?ESP_FAIL:ESP_OK;}
esp_err_t esp_netif_init(void){return ESP_OK;}
esp_netif_t* esp_netif_create_default_wifi_ap(void){++netif_creates;return calloc(1,sizeof(esp_netif_t));}
esp_netif_t* esp_netif_create_default_wifi_sta(void){++netif_creates;return calloc(1,sizeof(esp_netif_t));}
void esp_netif_destroy_default_wifi(esp_netif_t* p){++netif_destroys;free(p);}
esp_err_t esp_netif_get_ip_info(esp_netif_t* p,esp_netif_ip_info_t* info){assert(p);info->ip.addr=0xc0a80401;return ESP_OK;}
esp_err_t esp_wifi_init(const wifi_init_config_t* c){assert(!c->nvs_enable);++wifi_init_calls;return ESP_OK;}
esp_err_t esp_wifi_deinit(void){++wifi_deinit_calls;if(fail_wifi_deinit)return ESP_FAIL;return ESP_OK;}
esp_err_t esp_wifi_set_storage(int storage){assert(storage==WIFI_STORAGE_RAM);return ESP_OK;}
esp_err_t esp_wifi_set_mode(int mode){assert(mode==WIFI_MODE_APSTA||mode==WIFI_MODE_STA);selected_mode=mode;return ESP_OK;}
esp_err_t esp_wifi_set_config(int which,const wifi_config_t* c){if(which==WIFI_IF_AP)configured_ap=*c;else configured_sta=*c;return ESP_OK;}
esp_err_t esp_wifi_start(void){assert(!entropy);driver_started=true;if(callback)callback(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);return ESP_OK;}
esp_err_t esp_wifi_stop(void){++wifi_stop_calls;if(fail_wifi_stop)return ESP_FAIL;driver_started=false;return ESP_OK;}
esp_err_t esp_wifi_connect(void){return fail_connect?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_disconnect(void){return ESP_OK;}
esp_err_t esp_wifi_set_ps(int m){assert(m==WIFI_PS_NONE);return ESP_OK;}
void esp_fill_random(void* ptr,size_t count){assert(entropy||driver_started);unsigned char* p=ptr;while(count--)*p++=(unsigned char)++random_counter;}
int64_t esp_timer_get_time(void){return test_monotonic;}
void bootloader_random_enable(void){assert(!driver_started);entropy=true;}
void bootloader_random_disable(void){entropy=false;}
esp_err_t nvs_flash_init(void){return ESP_OK;}
esp_err_t nvs_open(const char* n,int mode,nvs_handle_t* out){(void)n;*out=1;if(mode==NVS_READWRITE){memcpy(pending_profiles,persisted_profiles,sizeof(pending_profiles));pending_profile_size=persisted_profile_size;}return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char* n,void* p,size_t* len){(void)h;if(strcmp(n,"profiles")||!persisted_profile_size)return ESP_ERR_NVS_NOT_FOUND;if(*len<persisted_profile_size)return ESP_FAIL;memcpy(p,persisted_profiles,persisted_profile_size);*len=persisted_profile_size;return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char* n,const void* p,size_t len){(void)h;assert(!strcmp(n,"profiles") && len<=sizeof(pending_profiles));memcpy(pending_profiles,p,len);pending_profile_size=len;++nvs_writes;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;if(fail_nvs_commit)return ESP_FAIL;memcpy(persisted_profiles,pending_profiles,sizeof(persisted_profiles));persisted_profile_size=pending_profile_size;return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char* n){(void)h;if(strcmp(n,"profiles"))return ESP_ERR_NVS_NOT_FOUND;pending_profile_size=0;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t httpd_start(httpd_handle_t* out,const httpd_config_t* c){assert(c->max_req_hdr_len>=2048 && c->recv_wait_timeout==1 && c->send_wait_timeout==1);++http_start_calls;*out=(void*)3;return ESP_OK;}
esp_err_t httpd_stop(httpd_handle_t h){assert(h==(void*)3);++http_stop_calls;return fail_http_stop?ESP_FAIL:ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t* r){assert(h&&r->handler);assert(strcmp(r->uri,"/api/login"));if(!strcmp(r->uri,"/api/session")){assert(r->method==HTTP_GET && r->handler==session_handler);++session_routes;}return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t* r,const char* a,const char* b){(void)r;(void)b;assert(strncmp(a,"Access-Control-",15));return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t* r,const char* value){(void)r;snprintf(response_status,sizeof(response_status),"%s",value);return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t* r,const char* t){(void)r;(void)t;return ESP_OK;}
esp_err_t httpd_resp_send_chunk(httpd_req_t* r,const char* p,ssize_t n){(void)r;if(!p)return ESP_OK;if(n==HTTPD_RESP_USE_STRLEN)n=(ssize_t)strlen(p);assert(response_used+(size_t)n<sizeof(response_body));memcpy(response_body+response_used,p,(size_t)n);response_used+=(size_t)n;response_body[response_used]=0;return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t* r,const char* p,ssize_t n){response_used=0;return httpd_resp_send_chunk(r,p,n);}
size_t httpd_req_get_hdr_value_len(httpd_req_t* r,const char* name){(void)r;return !strcmp(name,"X-Folio-Token")?strlen(request_token?request_token:s_token):!strcmp(name,"Origin")&&request_origin?strlen(request_origin):!strcmp(name,"X-Folio-Directory")&&request_directory?strlen(request_directory):0;}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r,const char* name,char* out,size_t cap){(void)r;const char* value=!strcmp(name,"Host")?(request_host?request_host:"192.168.4.1"):!strcmp(name,"X-Folio-Token")?(request_token?request_token:s_token):!strcmp(name,"Origin")?request_origin:!strcmp(name,"X-Folio-Name")?request_name:!strcmp(name,"X-Folio-Directory")?request_directory:NULL;if(!value||strlen(value)>=cap)return ESP_FAIL;snprintf(out,cap,"%s",value);return ESP_OK;}
esp_err_t httpd_req_get_url_query_str(httpd_req_t* r,char* out,size_t cap){(void)r;if(!request_query||strlen(request_query)>=cap)return ESP_FAIL;snprintf(out,cap,"%s",request_query);return ESP_OK;}
esp_err_t httpd_query_key_value(const char* form,const char* key,char* out,size_t cap){size_t n=strlen(key);while(*form){if(!strncmp(form,key,n)&&form[n]=='='){form+=n+1;size_t len=strcspn(form,"&");if(len>=cap)return ESP_FAIL;memcpy(out,form,len);out[len]=0;return ESP_OK;}const char* next=strchr(form,'&');if(!next)break;form=next+1;}return ESP_FAIL;}
static size_t receive_limit;
int httpd_req_recv(httpd_req_t* r,char* out,size_t cap){(void)r;++receive_calls;if(stop_on_receive){atomic_store(&s_stopping,true);return HTTPD_SOCK_ERR_TIMEOUT;}if(receive_timeouts){--receive_timeouts;return HTTPD_SOCK_ERR_TIMEOUT;}if(!request_body)return 0;size_t n=strlen(request_body);if(n>cap)n=cap;if(receive_limit && n>receive_limit)n=receive_limit;memcpy(out,request_body,n);request_body+=n;return (int)n;}
bool folio_storage_lock(uint32_t ms){(void)ms;return true;}
void folio_storage_unlock(void){}
bool folio_storage_validate_parent(const char* p,char* error,size_t cap){(void)p;(void)error;(void)cap;return true;}
bool folio_storage_list(const char* p,size_t o,folio_storage_entry_t* entries,size_t limit,size_t* count,size_t* total,char* e,size_t cap){snprintf(last_list_path,sizeof(last_list_path),"%s",p);(void)o;(void)entries;(void)limit;(void)e;(void)cap;*count=*total=0;return true;}
bool folio_storage_preview_delete(const char* p,folio_storage_preview_t* preview,char* e,size_t cap){snprintf(last_delete_path,sizeof(last_delete_path),"%s",p);(void)e;(void)cap;++delete_calls;memset(preview,0,sizeof(*preview));return true;}
bool folio_storage_commit_delete(const folio_storage_preview_t* p,char* e,size_t cap){(void)p;(void)e;(void)cap;return true;}
void folio_storage_cancel_delete(void){}
bool folio_transfer_validate(const char* p,folio_upload_kind_t k,char* e,size_t cap){snprintf(last_validation_path,sizeof(last_validation_path),"%s",p);(void)k;(void)e;(void)cap;return validation_ok;}

int main(void){
    assert(mkdir(FOLIO_SD_ROOT,0700)==0);char error[160];folio_transfer_status_t state;
    assert(folio_transfer_stop());
    assert(!folio_network_start_station(error,sizeof(error)) && !wifi_init_calls);
    // First boot/default unencrypted NVS must still offer a protected hotspot.
    folio_transfer_config_t automatic={.use_saved=true};
    assert(folio_transfer_start(&automatic,error,sizeof(error)));folio_transfer_get_status(&state);
    assert(state.running && !strcmp(state.ssid,"Folio") && !strcmp(state.ap_password,"foliomuki") && !strcmp(state.url,"http://192.168.4.1/"));
    assert(!state.access_code[0] && !strchr(state.url,'#'));
    httpd_req_t session_req={0};request_body=NULL;receive_calls=0;
    request_origin="https://evil.example";
    assert(session_handler(&session_req)==ESP_FAIL);request_origin=NULL;
    assert(session_handler(&session_req)==ESP_OK && !receive_calls && session_routes==1);
    assert(strstr(response_body,s_token) && !strstr(response_body,"foliomuki"));
    const char* bad_origins[]={"null","http://evil.example","http://192.168.4.1.evil","http://192.168.4.1:81","http://192.168.4.1/"};
    for(unsigned i=0;i<sizeof(bad_origins)/sizeof(bad_origins[0]);++i){request_origin=bad_origins[i];assert(session_handler(&session_req)==ESP_FAIL && !strcmp(response_status,"403 Forbidden"));}
    request_origin=NULL;request_host="evil.example";assert(session_handler(&session_req)==ESP_FAIL);request_host=NULL;
    request_origin="http://192.168.4.1";assert(session_handler(&session_req)==ESP_OK);request_origin=NULL;
    session_req.content_len=1;assert(session_handler(&session_req)==ESP_FAIL && !receive_calls);session_req.content_len=0;
    esp_err_t (*protected[])(httpd_req_t*)={upload_handler,delete_handler,wifi_handler,time_handler,files_handler,status_handler};
    for(unsigned i=0;i<sizeof(protected)/sizeof(protected[0]);++i){request_token="";assert(protected[i](&session_req)==ESP_FAIL && !receive_calls);request_token="00000000000000000000000000000000";assert(protected[i](&session_req)==ESP_FAIL && !receive_calls);}
    request_token=NULL;
    char first_token[33];memcpy(first_token,s_token,sizeof(first_token));
    assert(configured_ap.ap.authmode==WIFI_AUTH_WPA2_PSK && configured_ap.ap.max_connection==2 && nvs_writes==0);
    char first_password[17];memcpy(first_password,state.ap_password,sizeof(first_password));unsigned before=netif_creates;
    assert(folio_transfer_start(NULL,error,sizeof(error)) && netif_creates==before);
    fail_http_stop=1;unsigned stopped=wifi_stop_calls,stop_attempts=http_stop_calls;
    assert(!folio_transfer_stop() && s_server && atomic_load(&s_stopping) && wifi_stop_calls==stopped);
    assert(http_stop_calls==stop_attempts+3);
    assert(!folio_transfer_start(NULL,error,sizeof(error)) && netif_creates==before);
    fail_http_stop=0;assert(folio_transfer_stop());assert(!s_server && !s_ap && !s_sta && !driver_started);
    assert(folio_transfer_start(NULL,error,sizeof(error)));folio_transfer_get_status(&state);assert(!strcmp(first_password,state.ap_password) && strcmp(first_token,s_token));
    before=netif_creates;fail_wifi_stop=1;assert(!folio_transfer_stop() && !s_server && s_ap && s_wifi_initialized);
    assert(!folio_transfer_start(NULL,error,sizeof(error)) && netif_creates==before);
    fail_wifi_stop=0;assert(folio_transfer_stop());
    assert(folio_transfer_start(NULL,error,sizeof(error)));before=netif_creates;fail_wifi_deinit=1;
    assert(!folio_transfer_stop() && !s_server && s_wifi_initialized && s_ap);
    assert(!folio_transfer_start(NULL,error,sizeof(error)) && netif_creates==before);fail_wifi_deinit=0;assert(folio_transfer_stop());
    folio_transfer_config_t saved={.ssid="Home",.password="test-password",.remember=true};
    assert(folio_transfer_start(&saved,error,sizeof(error)) && nvs_writes==1);
    assert(folio_transfer_stop() && folio_transfer_forget_wifi());nvs_writes=0;
    saved.remember=false;assert(folio_transfer_start(&saved,error,sizeof(error)));
    // A forged/misplaced category cannot redirect deletion into another folder.
    request_body="name=book.epub&kind=update";httpd_req_t req={.content_len=strlen(request_body)};
    assert(delete_handler(&req)==ESP_FAIL && !delete_calls && !strcmp(response_status,"400 Bad Request"));
    request_body="name=font.ttf&kind=fonts";req.content_len=strlen(request_body);
    assert(delete_handler(&req)==ESP_FAIL && !delete_calls && !strcmp(response_status,"403 Forbidden"));
    request_body="name=book.epub&kind=books";req.content_len=strlen(request_body);
    assert(delete_handler(&req)==ESP_OK && delete_calls==1);
    request_body="name=art.png&kind=wallpapers";req.content_len=strlen(request_body);
    assert(delete_handler(&req)==ESP_OK && !strcmp(last_delete_path,FOLIO_SD_ROOT "/picture/art.png"));
    request_directory="wallpapers";request_name="art.png";request_body="hello";req.content_len=5;
    assert(upload_handler(&req)==ESP_FAIL && !strcmp(response_status,"422 Unprocessable Content"));
    assert(strstr(last_validation_path,"/picture/.folio-upload-"));
    request_name="folio.bin";receive_calls=0;
    assert(upload_handler(&req)==ESP_FAIL && !receive_calls && !strcmp(response_status,"400 Bad Request"));
    request_directory="../wallpapers";request_name="art.png";
    assert(upload_handler(&req)==ESP_FAIL && !receive_calls && !strcmp(response_status,"400 Bad Request"));
    request_directory=NULL;
    // Receive timeout is a retry opportunity; cancellation still exits next loop.
    request_name="wire.txt";request_body="hello";req.content_len=5;receive_timeouts=2;receive_calls=0;
    assert(upload_handler(&req)==ESP_FAIL && receive_calls==3 && !strcmp(response_status,"422 Unprocessable Content"));
    // Full real path capacity, percent-expanded headers and form fields must agree.
    char long_name[256],encoded_name[FOLIO_UPLOAD_ENCODED_NAME_CAP],delete_form[FOLIO_UPLOAD_FORM_CAP];
    size_t max_name=255-strlen(FOLIO_SD_ROOT)-strlen("/picture/");assert(max_name>120);
    size_t used=0;while(used+3+8<=max_name){memcpy(long_name+used,"书",3);used+=3;}
    while(used+8<max_name)long_name[used++]='a';
    memcpy(long_name+used," %+.PNG",8);used+=7;assert(used==max_name-1);
    // Fill the last spare byte while retaining valid UTF-8.
    memmove(long_name+1,long_name,used+1);long_name[0]='a';++used;assert(used==max_name);
    for(size_t i=0;i<used;++i)snprintf(encoded_name+i*3,4,"%%%02X",(unsigned char)long_name[i]);
    validation_ok=true;request_name=encoded_name;request_directory="picture";request_body="hello";req.content_len=5;
    assert(upload_handler(&req)==ESP_OK && strstr(response_body,"\"saved_only\":true") && strstr(response_body,"已保存"));
    char actual[256];snprintf(actual,sizeof(actual),FOLIO_SD_ROOT "/picture/%s",long_name);assert(strlen(actual)==255 && access(actual,F_OK)==0);
    request_body="hello";assert(upload_handler(&req)==ESP_FAIL && !strcmp(response_status,"409 Conflict"));
    snprintf(delete_form,sizeof(delete_form),"name=%s&kind=wallpapers",encoded_name);request_body=delete_form;req.content_len=strlen(delete_form);
    assert(delete_handler(&req)==ESP_OK && !strcmp(last_delete_path,actual));
    request_query="kind=wallpapers";response_used=0;assert(files_handler(&req)==ESP_OK && !strcmp(last_list_path,FOLIO_SD_ROOT "/picture"));request_query=NULL;
    char overflow_name[257];snprintf(overflow_name,sizeof(overflow_name),"b%s",long_name);request_name=overflow_name;req.content_len=5;receive_calls=0;
    assert(upload_handler(&req)==ESP_FAIL && !receive_calls && !strcmp(response_status,"400 Bad Request"));
    request_name="%2e%2e%2fsecret.txt";assert(upload_handler(&req)==ESP_FAIL);folio_transfer_get_status(&state);
    assert(!strcmp(state.recent[0].name,"无效文件名") && state.recent[0].result==FOLIO_UPLOAD_BAD_NAME && state.recent[0].message[0]);
    assert(state.recent_count==5 && !strcmp(state.recent[3].name,long_name) && !state.recent[3].result && state.recent[3].saved_only);
    response_used=0;assert(status_handler(&req)==ESP_OK && strstr(response_body,"\"recent_count\":5") && strstr(response_body,"\"saved_only\":true") && !strstr(response_body,s_token));
    request_directory=NULL;request_name="wire-short.txt";request_body="he";req.content_len=5;
    assert(upload_handler(&req)==ESP_FAIL);folio_transfer_get_status(&state);assert(state.recent[0].result==FOLIO_UPLOAD_TRUNCATED);
    request_name="saved.MOBI";request_body="hello";req.content_len=5;assert(upload_handler(&req)==ESP_OK && strstr(response_body,"\"saved_only\":true") && strstr(response_body,"暂不支持阅读"));
    static char body[32770], uploaded[sizeof(body)];
    for(size_t i=0;i<sizeof(body)-1;++i)body[i]=(char)('a'+i%26);
    receive_limit=731;request_body=body;request_name="batched.txt";req.content_len=sizeof(body)-1;
    assert(upload_handler(&req)==ESP_OK);
    FILE* upload_file=fopen(FOLIO_SD_ROOT "/books/batched.txt","rb");assert(upload_file);
    assert(fread(uploaded,1,sizeof(uploaded),upload_file)==sizeof(body)-1);
    assert(!memcmp(uploaded,body,sizeof(body)-1));fclose(upload_file);receive_limit=0;
    validation_ok=false;
    request_name="abort.txt";request_body="hello";req.content_len=5;stop_on_receive=true;receive_calls=0;
    assert(upload_handler(&req)==ESP_FAIL && receive_calls==1 && atomic_load(&s_stopping));stop_on_receive=false;
    assert(folio_transfer_stop());assert(netif_creates==netif_destroys);assert(folio_transfer_stop());
    folio_transfer_get_status(&state);assert(!state.running && !state.ap_password[0] && !state.url[0]);
    assert(folio_transfer_forget_wifi());folio_network_status_t network;folio_network_get_status(&network);
    assert(!network.has_credentials && !network.running && !folio_network_start_station(error,sizeof(error)));
    assert(folio_transfer_start(NULL,error,sizeof(error)));
    request_body="utc=1793102400";req.content_len=strlen(request_body);unsigned calls=set_clock_calls;
    request_token="00000000000000000000000000000000";
    assert(time_handler(&req)==ESP_FAIL && set_clock_calls==calls);request_token=NULL;
    request_origin="https://evil.example";
    assert(time_handler(&req)==ESP_FAIL && set_clock_calls==calls);request_origin=NULL;
    const char* bad_times[]={"utc=0","utc=-1793102400","utc=9999999999","utc=1704067199","utc=4102444800","utc=1793102400x","utc=1793102400%00"};
    for(unsigned i=0;i<sizeof(bad_times)/sizeof(bad_times[0]);++i){request_body=bad_times[i];req.content_len=strlen(request_body);assert(time_handler(&req)==ESP_FAIL && set_clock_calls==calls);}
    request_body="utc=1704067200";req.content_len=strlen(request_body);assert(time_handler(&req)==ESP_OK);
    request_body="utc=4102444799";req.content_len=strlen(request_body);assert(time_handler(&req)==ESP_OK);
    fail_set_clock=1;request_body="utc=1793102400";req.content_len=strlen(request_body);
    assert(time_handler(&req)==ESP_FAIL && !strcmp(response_status,"500 Internal Server Error"));fail_set_clock=0;
    request_body="ssid=ReadingRoom&password=private-pass&remember=x&utc=1793102400";req.content_len=strlen(request_body);
    assert(wifi_handler(&req)==ESP_FAIL && !s_have_credentials);
    request_body="ssid=ReadingRoom&password=private-pass&remember=0&utc=9999999999999";req.content_len=strlen(request_body);
    assert(wifi_handler(&req)==ESP_FAIL && !s_have_credentials);
    request_body="ssid=ReadingRoom&password=private-pass&remember=0&utc=1793102400";req.content_len=strlen(request_body);
    assert(wifi_handler(&req)==ESP_OK && test_utc==1793102400 && !nvs_writes);
    uint32_t pending;assert(folio_network_take_utc(&pending) && pending==1793102400 && !folio_network_take_utc(&pending));
    folio_network_get_status(&network);assert(network.has_credentials && network.connecting && network.clock_valid);
    before=netif_creates;assert(!folio_network_start_station(error,sizeof(error)) && netif_creates==before);
    assert(!folio_transfer_forget_wifi());
    assert(folio_transfer_stop());folio_network_get_status(&network);assert(network.has_credentials && !network.running);
    test_utc=0;test_rtc=0;assert(!folio_network_start_station(error,sizeof(error)) && !s_sta);
    test_rtc=1793102400;unsigned servers=http_start_calls;before=netif_creates;
    assert(folio_network_start_station(error,sizeof(error)) && selected_mode==WIFI_MODE_STA && !s_ap && !s_server);
    assert(netif_creates==before+1 && http_start_calls==servers && test_utc==test_rtc);
    assert(!strcmp((const char*)configured_sta.sta.ssid,"ReadingRoom") && !strcmp((const char*)configured_sta.sta.password,"private-pass"));
    before=netif_creates;assert(folio_network_start_station(error,sizeof(error)) && netif_creates==before);
    assert(!folio_transfer_start(NULL,error,sizeof(error)) && netif_creates==before);
    ip_event_got_ip_t ip={.ip_info.ip.addr=0xc0a80106};callback(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
    folio_network_get_status(&network);assert(network.running && network.connected && !network.connecting);
    unsigned existing_inits=wifi_init_calls;test_utc=0;test_rtc=0;
    assert(!folio_network_start_station(error,sizeof(error)) && wifi_init_calls==existing_inits);
    test_rtc=1793102400;
    assert(folio_network_start_station(error,sizeof(error)) && wifi_init_calls==existing_inits && test_utc==test_rtc);
    assert(configured_sta.sta.scan_method==WIFI_ALL_CHANNEL_SCAN && configured_sta.sta.sort_method==WIFI_CONNECT_AP_BY_SIGNAL);
    assert(folio_network_sync_time(error,sizeof(error)) && ntp_starts==1 && s_time_sync);
    assert(!strcmp(ntp_servers[0],"ntp.aliyun.com") && !strcmp(ntp_servers[1],"ntp1.aliyun.com") && !strcmp(ntp_servers[2],"time.cloudflare.com"));
    struct timeval ntp_time={.tv_sec=1793102400};ntp_callback(&ntp_time);
    assert(folio_network_take_utc(&pending) && pending==1793102400);
    ntp_callback(&ntp_time);folio_network_cancel_time();assert(ntp_stops==1 && !folio_network_take_utc(&pending));
    ntp_callback(&ntp_time);assert(!folio_network_take_utc(&pending));
    folio_transfer_get_status(&state);assert(!state.running && !state.url[0] && !state.sta_url[0]);
    wifi_event_sta_disconnected_t disconnect={.reason=WIFI_REASON_HANDSHAKE_TIMEOUT};
    for(unsigned i=0;i<4;++i)callback(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&disconnect);
    folio_network_get_status(&network);assert(!network.connecting && !network.connected);
    assert(strstr(network.message,"204") && !strstr(network.message,"检查密码"));
    before=wifi_init_calls;assert(folio_network_connect(error,sizeof(error)) && wifi_init_calls==before+1);
    folio_network_get_status(&network);assert(network.connecting);
    fail_wifi_stop=1;assert(!folio_network_stop());before=netif_creates;
    assert(!folio_network_start_station(error,sizeof(error)) && !folio_transfer_start(NULL,error,sizeof(error)) && netif_creates==before);
    fail_wifi_stop=0;assert(folio_network_stop());
    assert(folio_network_start_station(error,sizeof(error)));fail_unregister=1;
    assert(!folio_network_stop() && s_wifi_handler && s_wifi_started);fail_unregister=0;
    fail_loop_delete=1;assert(!folio_network_stop() && s_own_loop && !s_wifi_started);
    assert(!folio_network_start_station(error,sizeof(error)));fail_loop_delete=0;assert(folio_network_stop());
    assert(folio_network_start_station(error,sizeof(error)));test_monotonic+=INT64_C(31000000);
    folio_network_get_status(&network);assert(!network.connecting && !network.connected && !s_sta_requested);
    callback(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);folio_network_get_status(&network);assert(!network.connected);
    assert(folio_network_stop());fail_connect=1;assert(folio_network_start_station(error,sizeof(error)));
    folio_network_get_status(&network);assert(!network.connecting && !network.connected);fail_connect=0;assert(folio_network_stop());
    assert(folio_transfer_forget_wifi());folio_network_get_status(&network);assert(!network.has_credentials);
    assert(!folio_network_start_station(error,sizeof(error)) && netif_creates==netif_destroys && !nvs_writes);
    test_rtc=test_utc=0;
    assert(folio_network_scan_start(error,sizeof(error)) && !s_ap && !s_server && !s_sta_requested);
    folio_network_ap_t aps[16];size_t ap_count=99;
    assert(!folio_network_scan_result(aps,16,&ap_count));
    callback(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,NULL);
    assert(folio_network_scan_result(aps,16,&ap_count) && ap_count==2);
    assert(!strcmp(aps[0].ssid,"Home") && aps[0].secured && !aps[1].secured);
    assert(!folio_network_scan_result(aps,16,&ap_count));
    assert(!folio_network_configure("Home","short",error,sizeof(error)));
    before=wifi_init_calls;
    assert(folio_network_configure("Home","simple-pass",error,sizeof(error)) && s_wifi_initialized && wifi_init_calls==before);
    assert(folio_network_connect(error,sizeof(error)) && s_sta_requested && !s_ap && !s_server);
    assert(folio_network_stop() && netif_creates==netif_destroys);
    char restored[65];assert(folio_network_saved_password("Home",restored,sizeof(restored)) && !strcmp(restored,"simple-pass"));
    // Simulated reboot loses RAM but reloads the selected network from NVS.
    wipe(&s_ram_credentials,sizeof(s_ram_credentials));s_have_credentials=false;
    assert(folio_network_connect(error,sizeof(error)) && !strcmp((char*)configured_sta.sta.ssid,"Home"));
    assert(folio_network_stop());
    assert(folio_network_configure("Guest","guest-pass",error,sizeof(error)));
    fail_nvs_commit=true;assert(!folio_network_configure("Home","changed-pass",error,sizeof(error)));fail_nvs_commit=false;
    assert(folio_network_saved_password("Home",restored,sizeof(restored)) && !strcmp(restored,"simple-pass"));
    assert(folio_network_forget("Home",error,sizeof(error)) && !folio_network_saved_password("Home",restored,sizeof(restored)));
    assert(folio_network_saved_password("Guest",restored,sizeof(restored)) && !strcmp(restored,"guest-pass"));
    for(unsigned i=0;i<7;++i){char name[16];snprintf(name,sizeof(name),"Saved%u",i);assert(folio_network_configure(name,"password8",error,sizeof(error)));}
    assert(!folio_network_configure("Ninth","password8",error,sizeof(error)) && folio_network_saved_password("Guest",restored,sizeof(restored)));
    persisted_profiles[0]^=0x80;
    assert(!folio_network_saved_password("Guest",restored,sizeof(restored)) && !folio_network_configure("Guest","new-pass",error,sizeof(error)));
    persisted_profiles[0]^=0x80;
    assert(folio_transfer_forget_wifi() && !folio_network_saved_password("Guest",restored,sizeof(restored)));
    printf("transfer lifecycle: no-code session/Host/Origin/token/no-CORS, fixed AP/rotating token, picture aliases, 255-byte UTF8 paths/percent-header/delete-form, save-only responses/recent ring, disconnect, Wi-Fi persistence/SNTP/resource ownership passed\n");
    return 0;
}

esp_err_t esp_wifi_scan_start(const void* c,bool block){(void)c;assert(!block);return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t* n,wifi_ap_record_t* r){
    assert(*n>=3);*n=3;
    strcpy((char*)r[0].ssid,"Home");r[0].authmode=WIFI_AUTH_WPA2_PSK;r[0].rssi=-40;
    r[1]=r[0];strcpy((char*)r[2].ssid,"Guest");r[2].authmode=WIFI_AUTH_OPEN;return ESP_OK;
}
uint32_t esp_random(void){uint32_t x;esp_fill_random(&x,sizeof(x));return x;}
