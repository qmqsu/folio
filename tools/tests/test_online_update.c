#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "folio_online_update.h"
#include "folio_remote_manifest.h"
#include "folio_storage.h"
#include "net/folio_network.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/task.h"
#include "lwip/sys.h"

bool fail_crypto_init, fail_hash_setup, fail_hash_finish;
static pthread_t thread, guard_thread;
static bool guard_live, guard_task_fail, request_send_fail, missing_socket, guard_resource_fail;
static _Atomic unsigned guard_resources;
static _Thread_local sys_sem_t guard_syssem;
sys_sem_t* sys_thread_sem_get(void) { if(guard_resource_fail)return NULL;assert(!guard_syssem);guard_syssem=1;++guard_resources;return &guard_syssem; }
void sys_thread_sem_deinit(void) { assert(guard_syssem && guard_resources);guard_syssem=0;--guard_resources; }
static TaskFunction_t guard_function;
static void* guard_argument;
static _Thread_local bool in_guard;
static esp_http_client_config_t http_config;
static int http_pair[2];
static bool thread_live, task_fail;
static TaskFunction_t task_function;
static void* task_argument;
static _Atomic int64_t ticks;
static unsigned random_value = 1;
static unsigned storage_depth, locks, opens, closes, reads, begins, writes, aborts, boots, ends;
static bool present_card = true, power_ok = true, connected = true, lock_fail, disk_full, sync_fail;
static bool tls_live, complete_response = true, unknown_length, fail_open, fail_read, extra_byte;
static bool cancel_read, cancel_write, cancel_end, lose_card, mutate_begin, shorten_body, close_cancel;
static bool cancel_global, default_tls_checked, drop_power_write, drop_power_end;
static time_t test_utc = 1800000000;
static int response_status = 200;
static int64_t length_delta, free_bytes = 10000000;
static int64_t read_time_delta;
static unsigned char* response;
static size_t response_size, response_offset;
static unsigned char* manifest_bytes;
static size_t manifest_size;
static unsigned char* image_bytes;
static size_t image_size;
static const char* fixtures;
static char candidate[256];
static esp_partition_t running = {0x10000, 0x400000, ESP_PARTITION_TYPE_APP, 0};
static esp_partition_t target = {0x920000, 0x300000, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
static esp_partition_t metadata = {0x910000, 8192, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA};
static bool ota_live;
static uint32_t ota_size;

static void* task_run(void* ignored) { (void)ignored; task_function(task_argument); return NULL; }
static void* guard_run(void* ignored) { (void)ignored; in_guard=true; guard_function(guard_argument); return NULL; }
int xTaskCreate(TaskFunction_t function, const char* name, unsigned stack, void* arg, unsigned priority, void* handle) {
    assert(name && priority && !handle);
    if (!strcmp(name,"folio_http_guard")) {
        assert(stack>=3072 && !guard_live); if (guard_task_fail) return 0; guard_live=true; guard_function=function; guard_argument=arg;
        assert(pthread_create(&guard_thread,NULL,guard_run,NULL)==0); return 1;
    }
    assert(stack >= 8192 && !thread_live);
    if (task_fail) return 0;
    task_function = function; task_argument = arg; thread_live = true;
    assert(pthread_create(&thread, NULL, task_run, NULL) == 0); return 1;
}
void vTaskDelay(unsigned n) {
    if (in_guard) { struct timespec t={n/1000,(long)(n%1000)*1000000}; nanosleep(&t,NULL); }
    else { atomic_fetch_add(&ticks, (int64_t)n * 1000); sched_yield(); }
}
void vTaskDelete(void* task) { assert(!task); }
int64_t esp_timer_get_time(void) { return atomic_load(&ticks); }
time_t online_test_time(time_t* out) { if (out) *out=test_utc; return test_utc; }
uint32_t esp_random(void) { return ++random_value; }
static bool card_ready(void* ctx) { assert(!ctx); return present_card; }
static bool storage_lock(void* ctx, uint32_t timeout) {
    assert(!ctx && timeout && !storage_depth); if (lock_fail) return false;
    ++locks; ++storage_depth; return true;
}
static void storage_unlock(void* ctx) { assert(!ctx && storage_depth == 1); --storage_depth; }
static bool install_allowed(void* ctx, char* error, size_t cap) {
    assert(!ctx && !tls_live);
    if (!power_ok) snprintf(error, cap, "Power denied");
    return power_ok;
}
static bool shutdown_cancelled(void* ctx) { assert(!ctx); return cancel_global; }
static bool net_ready(void* ctx) { assert(!ctx); return connected; }
void folio_network_get_status(folio_network_status_t* status) { memset(status, 0, sizeof(*status)); status->connected = connected; }
bool folio_storage_space(uint64_t* capacity, uint64_t* available, char* error, size_t cap) {
    (void)error; (void)cap; assert(storage_depth); *capacity = 100000000; *available = (uint64_t)free_bytes; return true;
}
bool folio_storage_validate_parent(const char* path, char* error, size_t cap) {
    assert(storage_depth && !strncmp(path, FOLIO_SD_ROOT, strlen(FOLIO_SD_ROOT)));
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(error, cap, "Unsafe staged file"); return false; }
    return true;
}
ssize_t online_test_write(int fd, const void* bytes, size_t count) {
    assert(storage_depth);
    if (disk_full) { errno = ENOSPC; return -1; }
    return write(fd, bytes, count);
}
int online_test_fsync(int fd) { assert(storage_depth); if (sync_fail) { errno = EIO; return -1; } return fsync(fd); }
static void* read_file(const char* name, size_t* length) {
    char path[1024]; snprintf(path, sizeof(path), "%s/%s", fixtures, name);
    FILE* f = fopen(path, "rb"); assert(f && fseek(f, 0, SEEK_END) == 0); long n = ftell(f); assert(n > 0);
    assert(fseek(f, 0, SEEK_SET) == 0); void* data = malloc((size_t)n); assert(data);
    assert(fread(data, 1, (size_t)n, f) == (size_t)n && fclose(f) == 0); *length = (size_t)n; return data;
}
static bool transport_open(void* ctx, const char* url, int* status, int64_t* length, char* error, size_t cap) {
    (void)ctx; assert(!storage_depth && !tls_live); tls_live = true; ++opens; response_offset = 0;
    assert(!strncmp(url, FOLIO_REMOTE_ORIGIN, strlen(FOLIO_REMOTE_ORIGIN)));
    bool image = strstr(url, "/firmware/") != NULL;
    response = image ? image_bytes : manifest_bytes; response_size = image ? image_size : manifest_size;
    *status = response_status; *length = unknown_length ? -1 : (int64_t)response_size + length_delta;
    if (shorten_body && response_size > 10) response_size -= 10;
    if (fail_open) { snprintf(error, cap, "TLS injected failure"); return false; }
    return true;
}
static int transport_read(void* ctx, uint8_t* bytes, size_t capacity) {
    (void)ctx; assert(tls_live && !storage_depth && folio_online_update_busy()); ++reads;
    atomic_fetch_add(&ticks, read_time_delta);
    if (fail_read && response_offset) return -1;
    if (response_offset >= response_size) {
        if (extra_byte && response_offset == response_size) { *bytes = 0; ++response_offset; return 1; }
        return 0;
    }
    size_t count = response_size - response_offset; if (count > capacity) count = capacity;
    memcpy(bytes, response + response_offset, count); response_offset += count;
    if (cancel_read) folio_online_update_cancel();
    if (lose_card) present_card = false;
    return (int)count;
}
static bool transport_complete(void* ctx) { (void)ctx; assert(tls_live && !storage_depth); return complete_response; }
static void transport_close(void* ctx) {
    (void)ctx; assert(tls_live && !storage_depth); tls_live = false; ++closes;
    if (close_cancel) folio_online_update_cancel();
}
static const folio_online_transport_t transport = {NULL, transport_open, transport_read, transport_complete, transport_close};
// Default HTTPS config is tested too; only the network plumbing is injected.
int esp_crt_bundle_attach(void* ctx) { (void)ctx; return 0; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    assert(config->crt_bundle_attach == esp_crt_bundle_attach && !config->skip_cert_common_name_check &&
        config->disable_auto_redirect && config->max_redirection_count == 0 && config->timeout_ms == 5000 &&
        config->transport_type == HTTP_TRANSPORT_OVER_SSL && !config->keep_alive_enable && config->is_async && config->addr_type==HTTP_ADDR_TYPE_INET);
    http_config=*config; assert(socketpair(AF_UNIX,SOCK_STREAM,0,http_pair)==0);
    assert(fcntl(http_pair[0],F_SETFL,fcntl(http_pair[0],F_GETFL,0)|O_NONBLOCK)==0);
    assert(guard_resources==1);
    default_tls_checked = true;
    int status; int64_t length; char error[192]; assert(transport_open(NULL, config->url, &status, &length, error, sizeof(error)));
    return (void*)1;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char* key, const char* value) { assert(c && !strcmp(key,"Accept-Encoding") && !strcmp(value,"identity")); return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length) {
    assert(c && !length);
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_CONNECTED,.client=c,.user_data=http_config.user_data};
    assert(http_config.event_handler(&e)==ESP_OK);
    if(!missing_socket)assert(!(fcntl(http_pair[0],F_GETFL,0)&O_NONBLOCK));
    if(request_send_fail){esp_http_client_close(c);return ESP_FAIL;}
    return ESP_OK;
}
int esp_http_client_get_socket(esp_http_client_handle_t c) { assert(c); return missing_socket ? -1 : http_pair[0]; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { assert(c); return (int64_t)response_size; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { assert(c); return response_status; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { assert(c); return unknown_length; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { assert(c); return (int64_t)response_size + length_delta; }
int esp_http_client_read(esp_http_client_handle_t c, char* bytes, int n) { assert(c); return transport_read(NULL, (uint8_t*)bytes, (size_t)n); }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { assert(c); return transport_complete(NULL); }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    assert(c); if(!tls_live)return ESP_OK;
    esp_http_client_event_t e={.event_id=HTTP_EVENT_DISCONNECTED,.client=c,.user_data=http_config.user_data};
    assert(http_config.event_handler(&e)==ESP_OK);
    assert(close(http_pair[0])==0 && close(http_pair[1])==0); transport_close(NULL); return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { assert(c && !tls_live); return ESP_OK; }
bool folio_io_cancelled(void) { return cancel_global; }
const esp_partition_t* esp_ota_get_running_partition(void) { return &running; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* p) { assert(!p); return &target; }
const esp_partition_t* esp_partition_find_first(unsigned type, unsigned subtype, const char* label) { assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_OTA && !label); return &metadata; }
static void patch_candidate(void) { FILE* f = fopen(candidate, "r+b"); assert(f && fseek(f, 8192, SEEK_SET) == 0 && fputc(0xaa,f) != EOF && fclose(f) == 0); }
esp_err_t esp_ota_begin(const esp_partition_t* p, size_t size, esp_ota_handle_t* handle) {
    assert(p == &target && size == image_size && !tls_live && storage_depth && !ota_live); ++begins; ota_live = true; ota_size = 0; *handle = 1;
    if (mutate_begin) patch_candidate(); return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* bytes, size_t size) {
    assert(handle == 1 && bytes && ota_live && storage_depth && !tls_live); ++writes; ota_size += (uint32_t)size;
    if (cancel_write) folio_online_update_cancel();
    if (drop_power_write) power_ok=false;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) { assert(handle == 1 && ota_live && ota_size == image_size); ++ends; ota_live = false; if (cancel_end) folio_online_update_cancel(); if(drop_power_end)power_ok=false; return ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { assert(handle == 1 && ota_live); ++aborts; ota_live = false; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p) { assert(p == &target && !tls_live && storage_depth && !ota_live); ++boots; return ESP_OK; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* state) { assert(p == &running); *state = ESP_OTA_IMG_VALID; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
static folio_online_snapshot_t wait_for(folio_online_state_t expected) {
    assert(thread_live && pthread_join(thread, NULL) == 0); thread_live = false;
    if(guard_live){assert(pthread_join(guard_thread,NULL)==0);guard_live=false;}
    folio_online_snapshot_t snapshot; folio_online_update_get_snapshot(&snapshot);
    if (snapshot.state != expected) { fprintf(stderr, "State=%d expected=%d message=%s\n", snapshot.state, expected, snapshot.message); abort(); }
    assert(!snapshot.busy && !folio_online_update_busy() && !storage_depth && !tls_live && opens == closes && !ota_live);
    return snapshot;
}
static void select_manifest(const char* name) { free(manifest_bytes); char file[192]; snprintf(file, sizeof(file), "%s.json",name); manifest_bytes = read_file(file, &manifest_size); }
static void select_image(const char* name) { free(image_bytes); image_bytes = read_file(name, &image_size); }
static void setup(bool defaults) {
    assert(!thread_live && !tls_live && !ota_live && !storage_depth);
    opens = closes = reads = locks = begins = writes = aborts = boots = ends = 0; ticks = 0;
    guard_task_fail=request_send_fail=missing_socket=guard_resource_fail=false;assert(!guard_resources);
    present_card = power_ok = connected = complete_response = true;
    lock_fail = disk_full = sync_fail = unknown_length = fail_open = fail_read = extra_byte = false;
    cancel_read = cancel_write = cancel_end = lose_card = mutate_begin = shorten_body = close_cancel = false;
    cancel_global = default_tls_checked = task_fail = drop_power_write = drop_power_end = false; test_utc=1800000000;
    response_status = 200; length_delta = read_time_delta = 0; free_bytes = 10000000;
    select_manifest("valid"); select_image("online.bin"); candidate[0] = 0;
    folio_online_hooks_t hooks = { .card_ready = card_ready, .storage_lock = storage_lock, .storage_unlock = storage_unlock,
        .install_allowed = install_allowed, .cancelled = shutdown_cancelled, .network_ready = net_ready, .transport = defaults ? NULL : &transport };
    assert(folio_online_update_init(&hooks));
}
static void check_good(void) {
    char error[192]; assert(folio_online_update_start_check("preview", error, sizeof(error)));
    folio_online_snapshot_t s = wait_for(FOLIO_ONLINE_AVAILABLE); assert(s.sequence == 8032001 && !strcmp(s.version,"0.8.32-dev.1"));
}
static void download_good(void) {
    char error[192]; check_good(); assert(folio_online_update_start_download(error, sizeof(error)));
    folio_online_snapshot_t s = wait_for(FOLIO_ONLINE_READY); assert(s.received == image_size && s.total == image_size && s.candidate_path[0]);
    snprintf(candidate,sizeof(candidate),"%s",s.candidate_path);
    assert(strstr(candidate,"/.folio/online-update/0.8.32-dev.1/"));
}
static void test_check_failures(void) {
    char error[192]; setup(false);
    assert(!folio_online_update_start_check("beta",error,sizeof(error)) && *error);
    assert(!folio_online_update_start_download(error,sizeof(error)) && !folio_online_update_start_install(error,sizeof(error)));
    setup(false); response_status = 404; assert(folio_online_update_start_check("preview",error,sizeof(error))); wait_for(FOLIO_ONLINE_NO_RELEASE);
    const char* invalid[] = {"bad-signature","foreign-key","wrong-chip","wrong-layout","duplicate-schema","equal-sequence","older-sequence","oversize-envelope"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        setup(false); select_manifest(invalid[i]); assert(folio_online_update_start_check("preview",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED); assert(!begins && !boots);
    }
    for (unsigned i=0;i<12;++i) {
        setup(false);
        if (i==0) present_card=false;
        if (i==1) connected=false;
        if (i==2) fail_open=true;
        if (i==3) fail_read=true;
        if (i==4) response_status=302;
        if (i==5) complete_response=false;
        if (i==6) length_delta=1;
        if (i==7) read_time_delta=31000000;
        if (i==8) shorten_body=true;
        if (i==9) response_status=500;
        if (i==10) test_utc=0;
        if (i==11) test_utc=(time_t)4102444800;
        assert(folio_online_update_start_check("preview",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED); assert(!begins && !boots);
    }
    setup(false); cancel_read=true; assert(folio_online_update_start_check("preview",error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED);
    setup(false); task_fail=true; assert(!folio_online_update_start_check("preview",error,sizeof(error)) && !folio_online_update_busy());
    for(unsigned i=0;i<4;++i){
        setup(true);
        if(i==0)request_send_fail=true;
        if(i==1)guard_task_fail=true;
        if(i==2)missing_socket=true;
        if(i==3)guard_resource_fail=true;
        assert(folio_online_update_start_check("preview",error,sizeof(error)));wait_for(FOLIO_ONLINE_FAILED);assert(!begins && !boots && !guard_resources);
        if(i==3)assert(!opens);
    }
    puts("Online check: 404/no-release differs from forged/bad metadata; no card/network/TLS/redirect/timeout/length/EOF/cancel/task failures preserve slot");
}
static void test_download_failures(void) {
    char error[192];
    for (unsigned i=0;i<15;++i) {
        setup(false); check_good();
        if (i==0) fail_read=true;
        if (i==1) { extra_byte=true; unknown_length=true; }
        if (i==2) length_delta=1;
        if (i==3) free_bytes=1;
        if (i==4) disk_full=true;
        if (i==5) sync_fail=true;
        if (i==6) lock_fail=true;
        if (i==7) lose_card=true;
        if (i==8) { shorten_body=true; unknown_length=true; }
        if (i==9) complete_response=false;
        if (i==10) read_time_delta=181000000;
        if (i==11) response_status=302;
        if (i==12) fail_open=true;
        if (i==13) image_bytes[500]^=1;
        if (i==14) select_image("different.bin");
        assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED); assert(!begins && !boots);
    }
    const char* signed_bad[]={"wrong-hash","signed-bad-chip-image","signed-wrong-version-image"};
    for(unsigned i=0;i<3;++i){
        setup(false); select_manifest(signed_bad[i]); check_good();
        if(i==1)select_image("bad-chip.bin"); if(i==2)select_image("different.bin");
        assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED); assert(!begins && !boots);
    }
    setup(false); check_good(); cancel_read=true; assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED);
    setup(false); check_good(); close_cancel=true; assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED);
    setup(false); unknown_length=true; download_good(); assert(!begins && !boots);
    puts("Online download: interrupt/oversize/hash/chip/app-version/space/write/fsync/card/lock/timeout/redirect/cancel tested; chunked success verified by production parser");
}
static void test_install_failures(void) {
    char error[192];
    for(unsigned i=0;i<8;++i){
        setup(false); download_good();
        if(i==0)patch_candidate();
        if(i==1)power_ok=false;
        if(i==2)cancel_write=true;
        if(i==3)cancel_end=true;
        if(i==4)mutate_begin=true;
        if(i==5)cancel_global=true;
        if(i==6)drop_power_write=true;
        if(i==7)drop_power_end=true;
        assert(folio_online_update_start_install(error,sizeof(error)));
        wait_for((i==2||i==3||i==5)?FOLIO_ONLINE_CANCELLED:FOLIO_ONLINE_FAILED);
        assert(!boots);
        if(i==0||i==1||i==5)assert(!begins);
        if(i==2||i==4||i==6)assert(aborts==1);
        if(i==7)assert(ends==1);
    }
    setup(true); download_good(); assert(default_tls_checked);
    // Mutating unrelated legacy sidecars cannot influence signed installation.
    FILE* f=fopen(FOLIO_SD_ROOT "/update/folio.bin.sha256","wb"); assert(f); assert(fputs("attacker sidecar",f)>=0 && fclose(f)==0);
    assert(folio_online_update_start_install(error,sizeof(error))); wait_for(FOLIO_ONLINE_INSTALLED);
    assert(begins==1 && boots==1 && ends==1 && writes>1 && !aborts);
    assert(!folio_online_update_start_check("preview",error,sizeof(error)));
    puts("Online install: actual file/OTA-write mutation, power refusal, mid-write/end cancellation retain boot; signature-bound default-TLS-config success installs inactive slot");
}
void test_remote_manifests(const char* root);
int main(int argc,char** argv){
    assert(argc==2); fixtures=argv[1];
    assert(mkdir(FOLIO_SD_ROOT,0700)==0 && mkdir(FOLIO_SD_ROOT "/update",0700)==0);
    FILE* f=fopen(FOLIO_SD_ROOT "/update/folio.bin","wb"); assert(f && fputs("user-owned old update",f)>=0 && fclose(f)==0);
    test_remote_manifests(fixtures); test_check_failures(); test_download_failures(); test_install_failures();
    char sentinel[64]={0}; f=fopen(FOLIO_SD_ROOT "/update/folio.bin","rb"); assert(f && fread(sentinel,1,63,f)==21 && fclose(f)==0 && !strcmp(sentinel,"user-owned old update"));
    free(manifest_bytes); free(image_bytes);
    puts("Online OTA tests passed under ASan/UBSan; real host crypto, no device, network, production private key or GitHub access");
    return 0;
}
