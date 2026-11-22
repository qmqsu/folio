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
static bool guard_live, guard_task_fail, request_send_fail, request_send_fail_image_once, missing_socket, guard_resource_fail;
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
static bool tls_live, complete_response = true, unknown_length, fail_open, fail_open_primary_once, fail_read, extra_byte;
static bool fail_read_primary_once, incomplete_primary_once, server_error_primary_once, check_mirror_progress;
static bool corrupt_primary_prefix, track_retry_paths;
static bool cancel_open, cancel_read, cancel_write, cancel_end, lose_card, mutate_begin, shorten_body, close_cancel;
static bool cancel_global, default_tls_checked, drop_power_write, drop_power_end;
static time_t test_utc = 1800000000;
static int response_status = 200, active_status = 200;
static int64_t length_delta, free_bytes = 10000000;
static int64_t read_time_delta;
static size_t fail_read_once_at;   // one mid-body read failure at/after this offset
static bool expire_connection_once;
static bool ignore_range;          // server answers a ranged request with 200 + full body
static uint32_t resume_from;       // pending Range offset requested by seek
static uint32_t open_offsets[64];
static unsigned char* response;
static size_t response_size, response_offset;
static bool response_mirror, response_image;
static unsigned mirror_progress_checks;
static char open_urls[16][192];
static unsigned char* manifest_bytes;
static size_t manifest_size;
static unsigned char* image_bytes;
static size_t image_size;
static const char* fixtures;
static char candidate[256];
static char primary_stage_path[FOLIO_ONLINE_PATH_CAP], backup_stage_path[FOLIO_ONLINE_PATH_CAP];
static unsigned char primary_partial[4096];
static size_t primary_partial_size;
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
    if (lstat(path, &st) != 0 && errno==ENOENT && strstr(path,"/folio-fonts/")) return true;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(error, cap, "Unsafe staged file"); return false; }
    if (track_retry_paths) {
        if (!primary_stage_path[0]) snprintf(primary_stage_path, sizeof(primary_stage_path), "%s", path);
        else if (!strcmp(primary_stage_path, path)) assert(!backup_stage_path[0]);
        else {
            if (!backup_stage_path[0]) snprintf(backup_stage_path, sizeof(backup_stage_path), "%s", path);
            assert(!strcmp(backup_stage_path, path));
        }
    }
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
    (void)ctx; assert(!storage_depth && !tls_live); tls_live = true;
    response_offset = resume_from; resume_from = 0;
    if (opens < sizeof(open_offsets) / sizeof(open_offsets[0])) open_offsets[opens] = (uint32_t)response_offset;
    bool mirror = !strncmp(url, FOLIO_REMOTE_CDN_ORIGIN, strlen(FOLIO_REMOTE_CDN_ORIGIN));
    response_mirror = mirror;
    const char *font=strstr(url,"/main/assets/fonts/folio_");
    assert(font || mirror || !strncmp(url, FOLIO_REMOTE_ORIGIN, strlen(FOLIO_REMOTE_ORIGIN)));
    if (mirror) assert(!strcmp(url, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json") ||
        !strcmp(url, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin"));
    assert(opens < sizeof(open_urls) / sizeof(open_urls[0]));
    snprintf(open_urls[opens], sizeof(open_urls[opens]), "%s", url); ++opens;
    bool image = strstr(url, "/firmware/") != NULL;
    response_image = image;
    response = image ? image_bytes : manifest_bytes; response_size = image ? image_size : manifest_size;
    if(font) {
        const char *name=strrchr(font,'/')+1;
        free(image_bytes);image_bytes=read_file(name,&image_size);
        response=image_bytes;response_size=image_size;
    }
    active_status = response_status;
    if (!mirror && server_error_primary_once) { active_status = 500; server_error_primary_once = false; }
    if (response_offset && ignore_range) {
        // Server ignored Range: 200 with the whole body, from byte zero.
        response_offset = 0;   // open_offsets keeps the offset that was requested
        *status = 200;
        *length = (int64_t)response_size;
    } else if (response_offset) {
        // A ranged request is answered with exactly the missing bytes.
        *status = 206;
        *length = (int64_t)response_size - (int64_t)response_offset;
    } else {
        *status = active_status; *length = unknown_length ? -1 : (int64_t)response_size + length_delta;
    }
    if (mirror && image && check_mirror_progress) {
        folio_online_snapshot_t snapshot; folio_online_update_get_snapshot(&snapshot);
        assert(!snapshot.received); ++mirror_progress_checks;
    }
    if (shorten_body && response_size > 10) response_size -= 10;
    bool fail = fail_open || (fail_open_primary_once && !mirror);
    if (!mirror) fail_open_primary_once = false;
    if (fail) {
        if (cancel_open) folio_online_update_cancel();
        snprintf(error, cap, "TLS injected failure"); return false;
    }
    return true;
}
static int transport_read(void* ctx, uint8_t* bytes, size_t capacity) {
    (void)ctx; assert(tls_live && !storage_depth && folio_online_update_busy()); ++reads;
    atomic_fetch_add(&ticks, read_time_delta);
    if (fail_read_once_at && response_offset >= fail_read_once_at) {
        if (expire_connection_once) { atomic_fetch_add(&ticks, 301000000); expire_connection_once = false; }
        fail_read_once_at = 0;
        return -1;
    }
    if ((fail_read || (fail_read_primary_once && !response_mirror)) && response_offset) {
        if (!response_mirror) {
            fail_read_primary_once = false;
            // Buffered downloads may not have committed a chunk yet, so the
            // staged file can legitimately still be empty here.
            if (track_retry_paths && response_image && primary_stage_path[0]) {
                FILE* f = fopen(primary_stage_path, "rb");
                if (f) {
                    assert(fseek(f, 0, SEEK_END) == 0);
                    long size = ftell(f);
                    assert(size >= 0 && (size_t)size <= sizeof(primary_partial));
                    primary_partial_size = (size_t)size;
                    assert(fseek(f, 0, SEEK_SET) == 0);
                    if (primary_partial_size) {
                        assert(fread(primary_partial, 1, primary_partial_size, f) == primary_partial_size);
                    }
                    fclose(f);
                }
            }
        }
        return -1;
    }
    if (response_offset >= response_size) {
        if (extra_byte && response_offset == response_size) { *bytes = 0; ++response_offset; return 1; }
        return 0;
    }
    if (response_image && response_offset == 0 && capacity > 3000) capacity = 3000;
    size_t count = response_size - response_offset, offset = response_offset; if (count > capacity) count = capacity;
    memcpy(bytes, response + offset, count); response_offset += count;
    if (response_image && !response_mirror && corrupt_primary_prefix && offset == 0) bytes[0] ^= 1;
    if (cancel_read) folio_online_update_cancel();
    if (lose_card) present_card = false;
    return (int)count;
}
static bool transport_complete(void* ctx) {
    (void)ctx; assert(tls_live && !storage_depth);
    if (!response_mirror && incomplete_primary_once) { incomplete_primary_once = false; return false; }
    return complete_response;
}
static void transport_close(void* ctx) {
    (void)ctx; assert(tls_live && !storage_depth); tls_live = false; ++closes;
    if (close_cancel) folio_online_update_cancel();
}
static bool transport_seek(void* ctx, uint32_t offset) {
    (void)ctx; assert(!storage_depth && !tls_live); resume_from = offset; return true;
}
static const folio_online_transport_t transport = {NULL, transport_open, transport_read, transport_complete, transport_close, transport_seek};
// Default HTTPS config is tested too; only the network plumbing is injected.
int esp_crt_bundle_attach(void* ctx) { (void)ctx; return 0; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    assert(config->crt_bundle_attach == esp_crt_bundle_attach && !config->skip_cert_common_name_check &&
        config->disable_auto_redirect && config->max_redirection_count == 0 && config->timeout_ms == 20000 &&
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
    bool fail_image = request_send_fail_image_once && strstr(http_config.url, "/firmware/") &&
        !strncmp(http_config.url, FOLIO_REMOTE_ORIGIN, strlen(FOLIO_REMOTE_ORIGIN));
    if(fail_image)request_send_fail_image_once=false;
    if(request_send_fail||fail_image){esp_http_client_close(c);return ESP_FAIL;}
    return ESP_OK;
}
int esp_http_client_get_socket(esp_http_client_handle_t c) { assert(c); return missing_socket ? -1 : http_pair[0]; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { assert(c); return (int64_t)response_size; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { assert(c); return active_status; }
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
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    assert(c && !tls_live);
    // A primary->mirror retry destroys the first client within one worker job.
    if(guard_live){assert(pthread_join(guard_thread,NULL)==0);guard_live=false;}
    return ESP_OK;
}
bool folio_io_cancelled(void) { return cancel_global; }
const esp_partition_t* esp_ota_get_running_partition(void) { return &running; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* p) { assert(!p); return &target; }
const esp_partition_t* esp_partition_find_first(unsigned type, unsigned subtype, const char* label) { assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_OTA && !label); return &metadata; }
static void patch_candidate(void) { FILE* f = fopen(candidate, "r+b"); assert(f && fseek(f, 8192, SEEK_SET) == 0 && fputc(0xaa,f) != EOF && fclose(f) == 0); }
esp_err_t esp_ota_begin(const esp_partition_t* p, size_t size, esp_ota_handle_t* handle) {
    assert(p == &target && size == image_size && !tls_live && storage_depth && !ota_live); ++begins; ota_live = true; ota_size = 0; *handle = 1;
    if (mutate_begin) patch_candidate();
    return ESP_OK;
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
    guard_task_fail=request_send_fail=request_send_fail_image_once=missing_socket=guard_resource_fail=false;assert(!guard_resources);
    present_card = power_ok = connected = complete_response = true;
    fail_crypto_init=fail_hash_setup=fail_hash_finish=false;
    lock_fail = disk_full = sync_fail = unknown_length = fail_open = fail_open_primary_once = fail_read = extra_byte = false;
    fail_read_primary_once = incomplete_primary_once = server_error_primary_once = check_mirror_progress = false;
    corrupt_primary_prefix = track_retry_paths = false;
    primary_stage_path[0] = backup_stage_path[0] = 0; primary_partial_size = 0;
    cancel_open = cancel_read = cancel_write = cancel_end = lose_card = mutate_begin = shorten_body = close_cancel = false;
    cancel_global = default_tls_checked = task_fail = drop_power_write = drop_power_end = false; test_utc=1800000000;
    response_status = active_status = 200; response_mirror = response_image = false; mirror_progress_checks = 0;
    length_delta = read_time_delta = 0; free_bytes = 10000000;
    fail_read_once_at = 0; resume_from = 0; ignore_range = false;
    memset(open_offsets, 0, sizeof(open_offsets));
    select_manifest("valid"); select_image("online.bin"); candidate[0] = 0;
    folio_online_hooks_t hooks = { .card_ready = card_ready, .storage_lock = storage_lock, .storage_unlock = storage_unlock,
        .install_allowed = install_allowed, .cancelled = shutdown_cancelled, .network_ready = net_ready, .transport = defaults ? NULL : &transport };
    assert(folio_online_update_init(&hooks));
}
static void expect_open(unsigned index, const char* url) {
    assert(index < opens && !strcmp(open_urls[index], url));
}
static void check_good(void) {
    char error[192]; assert(folio_online_update_start_check("preview", error, sizeof(error)));
    folio_online_snapshot_t s = wait_for(FOLIO_ONLINE_AVAILABLE); assert(s.sequence == 8032001 && !strcmp(s.version,"0.8.32-dev.1"));
    // A checked candidate is not downloaded and cannot skip the explicit download state.
    assert(!s.received && !s.candidate_path[0] && !begins && !boots);
    assert(!folio_online_update_start_install(error, sizeof(error)) && !begins && !boots);
}
static void check_stable_good(void) {
    select_manifest("wrong-channel"); // Same valid signed release, with channel=stable.
    char error[192]; assert(folio_online_update_start_check("stable", error, sizeof(error)));
    folio_online_snapshot_t s = wait_for(FOLIO_ONLINE_AVAILABLE); assert(s.sequence == 8032001 && !strcmp(s.version,"0.8.32-dev.1"));
}
static void download_good(void) {
    char error[192]; check_good(); assert(folio_online_update_start_download(error, sizeof(error)));
    folio_online_snapshot_t s = wait_for(FOLIO_ONLINE_READY); assert(s.received == image_size && s.total == image_size && s.candidate_path[0]);
    // READY is only a verified staging state; installation still requires a separate call.
    assert(!begins && !boots);
    snprintf(candidate,sizeof(candidate),"%s",s.candidate_path);
    assert(strstr(candidate,"/.folio/online-update/0.8.32-dev.1/"));
}
static void assert_candidate_image(const char* path) {
    FILE* f = fopen(path, "rb"); assert(f && fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f); assert(size >= 0 && (size_t)size == image_size && fseek(f, 0, SEEK_SET) == 0);
    unsigned char* actual = malloc(image_size); assert(actual);
    assert(fread(actual, 1, image_size, f) == image_size && fclose(f) == 0);
    assert(!memcmp(actual, image_bytes, image_size)); free(actual);
}
static void test_cdn_mapping(void) {
    const char* version = "0.8.32-dev.1";
    char output[192];
    assert(folio_remote_cdn_url(FOLIO_REMOTE_ORIGIN "channels/stable.json", NULL, output, sizeof(output)));
    assert(!strcmp(output, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json"));
    assert(folio_remote_cdn_url(FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin", version, output, sizeof(output)));
    assert(!strcmp(output, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin"));
    const struct { const char* url; const char* version; } invalid[] = {
        {FOLIO_REMOTE_ORIGIN "channels/preview.json", NULL},
        {FOLIO_REMOTE_ORIGIN "channels/stable.json?x=1", NULL},
        {FOLIO_REMOTE_ORIGIN "channels/stable.json#x", NULL},
        {FOLIO_REMOTE_ORIGIN "channels/stable.json", "0.8.32-dev.1"},
        {FOLIO_REMOTE_ORIGIN "other/stable.json", NULL},
        {"https://evil.example/folio/channels/stable.json", NULL},
        {"https://qmqsu.github.io.evil.example/folio/channels/stable.json", NULL},
        {"https://qmqsu.github.io@evil.example/folio/channels/stable.json", NULL},
        {"https://qmqsu.github.io:443/folio/channels/stable.json", NULL},
        {"http://qmqsu.github.io/folio/channels/stable.json", NULL},
        {FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json", NULL},
        {FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin", NULL},
        {FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin?x=1", "0.8.32-dev.1"},
        {FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin#x", "0.8.32-dev.1"},
        {FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin", "0.8.32-dev.2"},
        {FOLIO_REMOTE_ORIGIN "firmware/../evil/folio.bin", "../evil"},
        {FOLIO_REMOTE_ORIGIN "firmware/%2e%2e/folio.bin", "%2e%2e"},
        {FOLIO_REMOTE_ORIGIN "firmware/a/b/folio.bin", "a/b"},
        {FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1?x=1/folio.bin", "0.8.32-dev.1?x=1"},
        {FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1\\evil/folio.bin", "0.8.32-dev.1\\evil"},
        {FOLIO_REMOTE_ORIGIN "firmware//folio.bin", ""},
        {FOLIO_REMOTE_ORIGIN "firmware/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/folio.bin", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        {"https://evil.example/folio/firmware/0.8.32-dev.1/folio.bin", "0.8.32-dev.1"},
        {FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin", "0.8.32-dev.1"},
    };
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        output[0] = 'x'; assert(!folio_remote_cdn_url(invalid[i].url, invalid[i].version, output, sizeof(output)));
        assert(output[0] == 'x');
    }
    assert(!folio_remote_cdn_url(NULL, NULL, output, sizeof(output)));
    assert(!folio_remote_cdn_url(FOLIO_REMOTE_ORIGIN "channels/stable.json", NULL, NULL, sizeof(output)));
    assert(!folio_remote_cdn_url(FOLIO_REMOTE_ORIGIN "channels/stable.json", NULL, output, 8));
    puts("Online CDN mapping: exact stable/signed-image URLs only; host/path/query/fragment/traversal/version aliases rejected");
}
static void test_cdn_fallback(void) {
    char error[192];
    setup(false); fail_open_primary_once = true; check_stable_good();
    assert(opens == 2 && closes == 2);
    expect_open(0, FOLIO_REMOTE_ORIGIN "channels/stable.json");
    expect_open(1, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json");

    setup(false); fail_read_primary_once = true; check_stable_good();
    assert(opens == 2 && closes == 2);
    expect_open(1, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json");

    setup(false); incomplete_primary_once = true; check_stable_good();
    assert(opens == 2 && closes == 2);
    expect_open(1, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json");

    setup(false); server_error_primary_once = true; check_stable_good();
    assert(opens == 2 && closes == 2);
    expect_open(1, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json");

    setup(false); select_manifest("wrong-channel"); response_status = 404;
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_NO_RELEASE);
    assert(opens == 1); expect_open(0, FOLIO_REMOTE_ORIGIN "channels/stable.json");

    setup(false); select_manifest("wrong-channel"); assert(manifest_size > 90);
    assert(manifest_bytes[manifest_size-2] == '"' && manifest_bytes[manifest_size-1] == '}');
    uint8_t* signature = manifest_bytes + manifest_size - 90; assert(signature[-1] == '"');
    *signature = *signature == 'A' ? 'B' : 'A';
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 1 && !begins && !boots);

    setup(false); select_manifest("duplicate-schema");
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 1); // Signed JSON/schema rejection is not a transport failure.

    setup(false); select_manifest("wrong-channel"); response_status = 302;
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 1);

    setup(false); select_manifest("wrong-channel");
    length_delta = (int64_t)FOLIO_REMOTE_MANIFEST_MAX + 1 - (int64_t)manifest_size;
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 1);

    setup(false); select_manifest("wrong-channel"); fail_read = true;
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 2); expect_open(1, FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json");

    setup(false); select_manifest("wrong-channel"); read_time_delta = 31000000;
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 2); // Both the primary and mirror timed out independently.

    setup(false); fail_open_primary_once = cancel_open = true; select_manifest("wrong-channel");
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED);
    assert(opens == 1); expect_open(0, FOLIO_REMOTE_ORIGIN "channels/stable.json");

    setup(false); fail_open_primary_once = close_cancel = true; select_manifest("wrong-channel");
    assert(folio_online_update_start_check("stable",error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED);
    assert(opens == 1); expect_open(0, FOLIO_REMOTE_ORIGIN "channels/stable.json");

    setup(false); check_stable_good(); fail_open_primary_once = true;
    assert(folio_online_update_start_download(error,sizeof(error))); folio_online_snapshot_t ready = wait_for(FOLIO_ONLINE_READY);
    assert(ready.received == image_size && ready.total == image_size && opens == 3 && closes == 3);
    expect_open(1, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    expect_open(2, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    assert_candidate_image(ready.candidate_path);

    // A mid-body failure that already delivered bytes resumes the SAME staged
    // file with a Range request instead of starting a second complete attempt.
    setup(false); check_stable_good(); fail_read_once_at = (size_t)(image_size / 2);
    assert(folio_online_update_start_download(error,sizeof(error))); ready = wait_for(FOLIO_ONLINE_READY);
    assert(ready.received == image_size && opens == 3 && closes == 3);
    expect_open(1, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    expect_open(2, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    assert(open_offsets[1] == 0 && open_offsets[2] > 0 && open_offsets[2] < image_size);
    assert_candidate_image(ready.candidate_path);

    // A slow but productive connection may reach its deadline mid-image.
    setup(false); check_stable_good(); fail_read_once_at = image_size / 2; expire_connection_once = true;
    assert(folio_online_update_start_download(error,sizeof(error))); ready = wait_for(FOLIO_ONLINE_READY);
    assert(opens == 3 && open_offsets[2] > 0 && ready.received == image_size);
    expect_open(2, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    assert_candidate_image(ready.candidate_path);

    // An ignored Range reuses the full response and appends only the missing suffix.
    setup(false); check_stable_good(); fail_read_once_at = (size_t)(image_size / 2);
    ignore_range = true;
    assert(folio_online_update_start_download(error,sizeof(error))); ready = wait_for(FOLIO_ONLINE_READY);
    assert(opens == 3 && ready.received == image_size);
    assert(open_offsets[2] > 0);
    expect_open(2, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    assert_candidate_image(ready.candidate_path);

    // Resuming must not launder a corrupted prefix: the signed SHA still refuses.
    setup(false); check_stable_good(); fail_read_once_at = (size_t)(image_size / 2);
    corrupt_primary_prefix = true;
    assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 3 && !begins && !boots);

    setup(false); check_stable_good(); incomplete_primary_once = true;
    assert(folio_online_update_start_download(error,sizeof(error))); ready = wait_for(FOLIO_ONLINE_READY);
    assert(opens == 3 && closes == 3);
    expect_open(2, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    assert_candidate_image(ready.candidate_path);

    setup(false); check_stable_good(); server_error_primary_once = true;
    assert(folio_online_update_start_download(error,sizeof(error))); ready = wait_for(FOLIO_ONLINE_READY);
    assert(opens == 3 && closes == 3);
    expect_open(2, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    assert_candidate_image(ready.candidate_path);

    setup(false); check_stable_good(); response_status = 404;
    assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 2); expect_open(1, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");

    setup(false); check_stable_good(); response_status = 302;
    assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 2);

    setup(false); check_stable_good(); fail_read = true;
    assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    // Primary, one useless resume, then the CDN and its own resume: every
    // attempt keeps failing, so nothing is installed.
    assert(opens == 5); expect_open(3, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");

    setup(false); check_stable_good(); fail_open_primary_once = true; image_bytes[500] ^= 1;
    assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 3 && !begins && !boots);
    expect_open(2, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");

    setup(false); check_stable_good(); fail_open = true;
    assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED);
    assert(opens == 3 && !begins && !boots);
    expect_open(1, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    expect_open(2, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    puts("Online CDN fallback: open/read/incomplete/5xx retry; a delivering attempt resumes with Range, a stalled one restarts on the CDN");
}
static void test_check_failures(void) {
    char error[192]; setup(false);
    assert(!folio_online_update_start_check("beta",error,sizeof(error)) && *error);
    assert(!folio_online_update_start_download(error,sizeof(error)) && !folio_online_update_start_install(error,sizeof(error)));
    setup(false); response_status = 404; assert(folio_online_update_start_check("preview",error,sizeof(error))); wait_for(FOLIO_ONLINE_NO_RELEASE);
    const char* up_to_date[] = {"equal-sequence", "older-sequence"};
    for (size_t i = 0; i < 2; ++i) {
        setup(false); select_manifest(up_to_date[i]);
        assert(folio_online_update_start_check("preview", error, sizeof(error)));
        wait_for(FOLIO_ONLINE_NO_RELEASE);
        assert(!folio_online_update_start_download(error, sizeof(error)));
        assert(!folio_online_update_start_install(error, sizeof(error)));
    }
    const char* invalid[] = {"bad-signature","foreign-key","wrong-chip","wrong-layout","duplicate-schema","oversize-envelope"};
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
        if(i==2)assert(opens==1); // preview has no CDN mapping, even for an open failure.
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
        if (i==10) read_time_delta=301000000;
        if (i==11) response_status=302;
        if (i==12) fail_open=true;
        if (i==13) image_bytes[500]^=1;
        if (i==14) select_image("different.bin");
        assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED); assert(!begins && !boots);
        // Transport failures now resume first (i=0, i=8) and only fall back to a
        // clean CDN attempt when the partial cannot be continued (i=9, i=12).
        static const unsigned expected_opens[15] = {5, 2, 2, 2, 2, 2, 2, 2, 5, 3, 3, 2, 3, 2, 2};
        assert(opens == expected_opens[i]); // Anything else never retries or writes another slot.
    }
    const char* signed_bad[]={"wrong-hash","signed-bad-chip-image","signed-wrong-version-image"};
    for(unsigned i=0;i<3;++i){
        setup(false); select_manifest(signed_bad[i]); check_good();
        if(i==1)select_image("bad-chip.bin");
        if(i==2)select_image("different.bin");
        assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_FAILED); assert(!begins && !boots && opens==2);
    }
    setup(false); check_good(); cancel_read=true; assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED); assert(opens==2);
    setup(false); check_good(); close_cancel=true; assert(folio_online_update_start_download(error,sizeof(error))); wait_for(FOLIO_ONLINE_CANCELLED); assert(opens==2);
    setup(false); unknown_length=true; download_good(); assert(!begins && !boots);
    puts("Online download: 64 KiB batching + Range resume + per-source budget; interrupt/oversize/hash/chip/version/space/write/fsync/card/lock/timeout/redirect/cancel tested");
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
    setup(true); request_send_fail_image_once = true; download_good(); assert(default_tls_checked);
    assert(opens == 3 && closes == 3);
    expect_open(0, FOLIO_REMOTE_ORIGIN "channels/preview.json");
    expect_open(1, FOLIO_REMOTE_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    expect_open(2, FOLIO_REMOTE_CDN_ORIGIN "firmware/0.8.32-dev.1/folio.bin");
    // Mutating unrelated legacy sidecars cannot influence signed installation.
    FILE* f=fopen(FOLIO_SD_ROOT "/update/folio.bin.sha256","wb"); assert(f); assert(fputs("attacker sidecar",f)>=0 && fclose(f)==0);
    assert(folio_online_update_start_install(error,sizeof(error))); wait_for(FOLIO_ONLINE_INSTALLED);
    assert(begins==1 && boots==1 && ends==1 && writes>1 && !aborts);
    assert(!folio_online_update_start_check("preview",error,sizeof(error)));
    puts("Online install: mutation/power/cancel retain boot; default-TLS primary-open failure uses exact CDN then signature/SHA-bound install succeeds");
}
void test_remote_manifests(const char* root);
int main(int argc,char** argv){
    assert(argc==2); fixtures=argv[1];
    assert(mkdir(FOLIO_SD_ROOT,0700)==0 && mkdir(FOLIO_SD_ROOT "/update",0700)==0);
    FILE* f=fopen(FOLIO_SD_ROOT "/update/folio.bin","wb"); assert(f && fputs("user-owned old update",f)>=0 && fclose(f)==0);
    test_remote_manifests(fixtures); test_cdn_mapping(); test_cdn_fallback();
    char error[192];
    for(unsigned face=1;face<=3;++face) {
        setup(false);assert(folio_online_start_font(face,error,sizeof(error)));
        folio_online_snapshot_t result=wait_for(FOLIO_ONLINE_FONT_READY);
        assert(result.received==result.total && opens==(face==3?2:1) && !boots);
        assert(!folio_online_update_start_install(error,sizeof(error)));
    }
    for(unsigned fail=0;fail<4;++fail) {
        setup(false);
        FILE *old=fopen(FOLIO_SD_ROOT "/folio-fonts/folio_hei.ttf","wb");
        assert(old && fputs("old",old)>=0 && fclose(old)==0);
        if(fail==0)free_bytes=1024;
        if(fail==1)fail_read=true;
        if(fail==2)fail_hash_finish=true;
        if(fail==3)cancel_read=true;
        assert(folio_online_start_font(1,error,sizeof(error)));
        wait_for(fail==3?FOLIO_ONLINE_CANCELLED:FOLIO_ONLINE_FAILED);
        struct stat st;
        assert(stat(FOLIO_SD_ROOT "/folio-fonts/folio_hei.ttf",&st)==0 && st.st_size==3);
        assert(access(FOLIO_SD_ROOT "/folio-fonts/folio_hei.ttf.part",F_OK)!=0 && !boots);
    }
    puts("Font download: three real fonts, round symbols, cancellation, space, read and hash failures passed");
    test_check_failures(); test_download_failures(); test_install_failures();
    char sentinel[64]={0}; f=fopen(FOLIO_SD_ROOT "/update/folio.bin","rb"); assert(f && fread(sentinel,1,63,f)==21 && fclose(f)==0 && !strcmp(sentinel,"user-owned old update"));
    free(manifest_bytes); free(image_bytes);
    puts("Online OTA tests passed under ASan/UBSan; real host crypto, no device, network, production private key or GitHub access");
    return 0;
}
