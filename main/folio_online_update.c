#include "folio_online_update.h"
#include "folio_remote_manifest.h"
#include "folio_online_guard.h"
#include "folio_storage.h"
#include "net/folio_network.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ONLINE_CHUNK 4096u
#define ONLINE_HTTP_TIMEOUT_MS 5000
#define ONLINE_MANIFEST_BUDGET_US INT64_C(30000000)
#define ONLINE_IMAGE_BUDGET_US INT64_C(180000000)
#define ONLINE_STAGE_ROOT FOLIO_SD_ROOT "/.folio/online-update"

typedef enum { JOB_CHECK, JOB_DOWNLOAD, JOB_INSTALL } job_t;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized, s_cancel;
static void* s_worker_task;
static folio_online_hooks_t s_hooks;
static folio_online_transport_t s_transport;
static folio_online_snapshot_t s_snapshot;
// Worker-only trusted state, retained between operations. Never loaded from SD.
static struct {
    uint8_t envelope[FOLIO_REMOTE_MANIFEST_MAX];
    size_t envelope_size;
    folio_remote_manifest_t manifest;
    folio_update_info_t preview;
    char channel[8], path[FOLIO_ONLINE_PATH_CAP];
    job_t job;
} s_work;
typedef struct {
    esp_http_client_handle_t client;
    folio_online_guard_t guard;
    int64_t deadline_us;
    char mirror_url[192];
    bool bad_encoding, guard_failed;
} https_t;
static https_t s_https;
static bool s_connection_open;

static void error_text(char* out, size_t cap, const char* text) {
    if (out && cap) snprintf(out, cap, "%s", text);
}
static bool signed_version_token(const char* version) {
    if (!version || !*version || *version == '.' || *version == '-' || *version == '_') return false;
    size_t length = 0;
    for (const char* p = version; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return false;
        if (++length >= sizeof(s_work.manifest.image.version)) return false;
    }
    return true;
}
bool folio_remote_cdn_url(const char* primary_url, const char* signed_version,
    char* output, size_t output_cap) {
    if (!primary_url || !output || !output_cap) return false;
    char expected[192], mapped[192];
    int n;
    if (!signed_version) {
        n = snprintf(expected, sizeof(expected), "%s", FOLIO_REMOTE_ORIGIN "channels/stable.json");
    } else {
        if (!signed_version_token(signed_version)) return false;
        n = snprintf(expected, sizeof(expected), FOLIO_REMOTE_ORIGIN "firmware/%s/folio.bin", signed_version);
    }
    if (n <= 0 || (size_t)n >= sizeof(expected) || strcmp(primary_url, expected)) return false;
    if (!signed_version) n = snprintf(mapped, sizeof(mapped), "%s", FOLIO_REMOTE_CDN_ORIGIN "channels/stable.json");
    else n = snprintf(mapped, sizeof(mapped), FOLIO_REMOTE_CDN_ORIGIN "firmware/%s/folio.bin", signed_version);
    if (n <= 0 || (size_t)n >= sizeof(mapped) || (size_t)n >= output_cap) return false;
    memcpy(output, mapped, (size_t)n + 1);
    return true;
}
static bool cancel_flag(void* ignored) {
    (void)ignored;
    portENTER_CRITICAL(&s_mux); bool cancel = s_cancel; portEXIT_CRITICAL(&s_mux);
    return cancel;
}
static bool cancelled(void* ignored) {
    return cancel_flag(ignored) || (s_hooks.cancelled && s_hooks.cancelled(s_hooks.context));
}
static void progress(uint32_t received, uint32_t total) {
    portENTER_CRITICAL(&s_mux);
    s_snapshot.received = received; s_snapshot.total = total; ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_mux);
}
static bool card(char* error, size_t cap) {
    if (cancelled(NULL)) { error_text(error, cap, "在线更新已取消，当前固件保留。"); return false; }
    if (!s_hooks.card_ready(s_hooks.context)) { error_text(error, cap, "请插入可用的 TF 卡后再进行在线更新。"); return false; }
    return true;
}
static bool lock_card(char* error, size_t cap) {
    if (!card(error, cap)) return false;
    if (!s_hooks.storage_lock(s_hooks.context, 1000)) { error_text(error, cap, "TF 卡正忙，更新已停止。"); return false; }
    if (!card(error, cap)) { s_hooks.storage_unlock(s_hooks.context); return false; }
    return true;
}
static void unlock_card(void) { s_hooks.storage_unlock(s_hooks.context); }
static bool network_ready(void) {
    if (s_hooks.network_ready) return s_hooks.network_ready(s_hooks.context);
    folio_network_status_t status; folio_network_get_status(&status); return status.connected;
}
static bool connection_ready(char* error, size_t cap) {
    if (!card(error, cap)) return false;
    time_t now = time(NULL);
    if (now < (time_t)1704067200 || (int64_t)now >= INT64_C(4102444800)) {
        error_text(error, cap, "设备时间无效，请先校准日期时间；不会跳过 HTTPS 验证。"); return false;
    }
    int64_t deadline = esp_timer_get_time() + ONLINE_MANIFEST_BUDGET_US;
    while (!network_ready()) {
        if (!card(error, cap)) return false;
        if (esp_timer_get_time() >= deadline) { error_text(error, cap, "Wi-Fi 尚未连接，请先在设置中连接网络。"); return false; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}
static esp_err_t https_event(esp_http_client_event_t* event) {
    https_t* state = event->user_data;
    if (state && event->event_id == HTTP_EVENT_ON_CONNECTED) {
        int fd = esp_http_client_get_socket(event->client);
        // Bind even when fcntl fails, so request_send remains interruptible.
        bool guarded = folio_online_guard_bind(&state->guard, fd);
        int flags = fd >= 0 ? fcntl(fd, F_GETFL, 0) : -1;
        // Async is for the handshake only. Restore blocking I/O after TLS is
        // established: ordinary split TLS records must not become EAGAIN HTTP
        // failures. SO_RCVTIMEO remains set; the guard interrupts slow-drip loops.
        if (!guarded || flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) state->guard_failed = true;
    }
    // IDF request_send can close internally on a failed write. DISCONNECTED is
    // dispatched synchronously BEFORE transport_close: detach before fd reuse.
    if (state && event->event_id == HTTP_EVENT_DISCONNECTED) folio_online_guard_detach(&state->guard);
    if (state && event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "Content-Encoding") && strcasecmp(event->header_value, "identity")) state->bad_encoding = true;
    return ESP_OK;
}
static bool https_open(void* context, const char* url, int* status, int64_t* length, char* error, size_t cap) {
    https_t* state = context; state->bad_encoding = state->guard_failed = false;
    // Primary URLs are exact constants or reconstructed from a verified version
    // token. A CDN URL is accepted only after http_begin maps it exactly.
    bool mapped_mirror = state->mirror_url[0] && !strcmp(url, state->mirror_url);
    if (strncmp(url, FOLIO_REMOTE_ORIGIN, strlen(FOLIO_REMOTE_ORIGIN)) && !mapped_mirror) {
        error_text(error, cap, "更新地址不在受信任来源内。"); return false;
    }
    if (!folio_online_guard_begin(&state->guard, state->deadline_us, cancel_flag, NULL)) {
        error_text(error, cap, "无法启动在线超时保护任务。"); return false;
    }
    esp_http_client_config_t config = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false, .disable_auto_redirect = true,
        .max_redirection_count = 0, .timeout_ms = ONLINE_HTTP_TIMEOUT_MS,
        .transport_type = HTTP_TRANSPORT_OVER_SSL, .keep_alive_enable = false,
        .is_async = true, .addr_type = HTTP_ADDR_TYPE_INET,
        .buffer_size = ONLINE_CHUNK, .buffer_size_tx = 1024,
        .user_agent = "Folio/online-update", .event_handler = https_event, .user_data = state
    };
    state->client = esp_http_client_init(&config);
    if (!state->client || esp_http_client_set_header(state->client, "Accept-Encoding", "identity") != ESP_OK) {
        error_text(error, cap, "HTTPS 初始化失败。"); return false;
    }
    // Async TLS returns between handshake steps; the guard handles HTTP's
    // internal header/body loops after CONNECTED. DNS is still lwIP's bounded
    // synchronous A lookup (3 servers * 7 s with this fixed SDK/config).
    esp_err_t result;
    do {
        if (cancelled(NULL) || esp_timer_get_time() >= state->deadline_us ||
            folio_online_guard_interrupted(&state->guard)) {
            error_text(error, cap, "HTTPS 连接已取消或超时。"); return false;
        }
        result = esp_http_client_open(state->client, 0);
        if (result == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (result == ESP_ERR_HTTP_EAGAIN);
    if (result != ESP_OK || state->guard_failed || cancelled(NULL) ||
        esp_timer_get_time() >= state->deadline_us || folio_online_guard_interrupted(&state->guard)) {
        error_text(error, cap, "HTTPS 连接、证书验证失败或请求已取消/超时。"); return false;
    }
    int64_t fetched = esp_http_client_fetch_headers(state->client);
    if (fetched < 0 || state->bad_encoding || folio_online_guard_interrupted(&state->guard)) { error_text(error, cap, "更新服务器响应头无效。"); return false; }
    *status = esp_http_client_get_status_code(state->client);
    *length = esp_http_client_is_chunked_response(state->client) ? -1 : esp_http_client_get_content_length(state->client);
    return true;
}
static int https_read(void* context, uint8_t* bytes, size_t capacity) {
    https_t* state = context; return esp_http_client_read(state->client, (char*)bytes, (int)capacity);
}
static bool https_complete(void* context) {
    https_t* state = context; return esp_http_client_is_complete_data_received(state->client);
}
static void https_close(void* context) {
    https_t* state = context;
    // Stop joins the last shutdown call before worker-owned close/cleanup.
    folio_online_guard_stop(&state->guard);
    if (state->client) { (void)esp_http_client_close(state->client); (void)esp_http_client_cleanup(state->client); state->client = NULL; }
}
static bool open_connection(const char* url, int* status, int64_t* length, char* error, size_t cap) {
    // The transport contract requires close after every open, even on failure.
    s_connection_open = true;
    return s_transport.open(s_transport.context, url, status, length, error, cap);
}
static void close_connection(void) {
    if (!s_connection_open) return;
    s_connection_open = false;
    s_transport.close(s_transport.context);
}
static bool http_begin(const char* primary, const char* mirror, bool use_mirror,
    int* status, int64_t* length, int64_t deadline, char* error, size_t cap) {
    *status = 0; *length = -1;
    const char* url = use_mirror ? mirror : primary;
    if (s_transport.open == https_open) {
        s_https.deadline_us = deadline; s_https.mirror_url[0] = 0;
        if (use_mirror) snprintf(s_https.mirror_url, sizeof(s_https.mirror_url), "%s", mirror);
    }
    if (!open_connection(url, status, length, error, cap)) {
        if (error && cap && !*error) error_text(error, cap, "HTTPS 请求失败。");
        return false;
    }
    return true;
}
static bool response_ok(int status, char* error, size_t cap) {
    if (status == 200) return true;
    error_text(error, cap, status >= 300 && status < 400 ? "不允许更新服务器重定向，更新已拒绝。" : "更新服务器返回错误，当前固件保留。");
    return false;
}
static bool within_budget(int64_t deadline, char* error, size_t cap) {
    if (!card(error, cap)) return false;
    if (esp_timer_get_time() > deadline) { error_text(error, cap, "在线更新超时，当前固件保留。"); return false; }
    return true;
}
static bool verify_manifest(folio_remote_manifest_t* result, char* error, size_t cap) {
    return folio_remote_manifest_verify(s_work.envelope, s_work.envelope_size, s_work.channel,
        FOLIO_RELEASE_SEQUENCE, result, error, cap);
}
static folio_online_state_t check_manifest(char* error, size_t cap) {
    if (!connection_ready(error, cap)) return FOLIO_ONLINE_FAILED;
    const char* primary = folio_remote_manifest_url(s_work.channel);
    char mirror[192];
    bool can_mirror = folio_remote_cdn_url(primary, NULL, mirror, sizeof(mirror));
    int status; int64_t length;
    int64_t deadline = esp_timer_get_time() + ONLINE_MANIFEST_BUDGET_US;
    bool okay = false, retryable = false, use_mirror = false;
    size_t used = 0; uint8_t buffer[512];
    for (;;) {
        if (use_mirror && !within_budget(deadline, error, cap)) { okay = false; break; }
        used = 0; retryable = false;
        okay = http_begin(primary, mirror, use_mirror, &status, &length, deadline, error, cap);
        if (!okay) retryable = true;
        if (okay && length < -1) { error_text(error, cap, "服务器文件长度无效。"); okay = false; }
        if (okay && status == 404) {
            close_connection();
            error_text(error, cap, "此通道暂未发布在线更新。"); return FOLIO_ONLINE_NO_RELEASE;
        }
        if (okay && status != 200) {
            retryable = status >= 500 && status <= 599;
            okay = response_ok(status, error, cap);
        }
        if (okay && (length == 0 || length > FOLIO_REMOTE_MANIFEST_MAX)) {
            error_text(error, cap, "发布清单大小无效或超过 4096 字节。"); okay = false;
        }
        while (okay) {
            if (!within_budget(deadline, error, cap)) { okay = false; break; }
            int n = s_transport.read(s_transport.context, buffer, sizeof(buffer));
            if (n < 0 || (size_t)n > sizeof(buffer)) {
                error_text(error, cap, "发布清单下载中断。"); okay = false; retryable = true; break;
            }
            if (!n) {
                okay = s_transport.complete(s_transport.context) && (length < 0 || (uint64_t)length == used);
                if (!okay) { error_text(error, cap, "发布清单长度不符或连接中断。"); retryable = true; }
                break;
            }
            if ((size_t)n > sizeof(s_work.envelope) - used) {
                error_text(error, cap, "发布清单超过 4096 字节。"); okay = false; break;
            }
            memcpy(s_work.envelope + used, buffer, (size_t)n); used += (size_t)n;
        }
        if (okay) break;
        // One exact CDN retry shares the operation's original total deadline.
        if (!retryable || use_mirror || !can_mirror || cancelled(NULL)) break;
        close_connection();
        if (cancelled(NULL)) break;
        use_mirror = true;
        if (error && cap) *error = 0;
    }
    close_connection();
    if (okay && !within_budget(deadline, error, cap)) okay = false;
    s_work.envelope_size = okay ? used : 0;
    // Signature/JSON validation is deliberately outside the retry loop.
    if (okay) okay = verify_manifest(&s_work.manifest, error, cap);
    if (!okay) return FOLIO_ONLINE_FAILED;
    portENTER_CRITICAL(&s_mux);
    s_snapshot.sequence = s_work.manifest.sequence;
    snprintf(s_snapshot.version, sizeof(s_snapshot.version), "%s", s_work.manifest.image.version);
    snprintf(s_snapshot.notes, sizeof(s_snapshot.notes), "%s", s_work.manifest.notes);
    s_snapshot.total = s_work.manifest.image.size;
    portEXIT_CRITICAL(&s_mux);
    error_text(error, cap, "已验证发布者签名，可下载新固件。");
    return FOLIO_ONLINE_AVAILABLE;
}
#ifdef ESP_PLATFORM
// ESP FAT VFS has neither symlinks nor lstat. This branch is FAT-specific;
// host filesystems retain lstat so existing symlink directories are rejected.
#define online_lstat stat
#else
#define online_lstat lstat
#endif
static bool safe_directory(const char* path, bool create) {
    struct stat st;
    if (online_lstat(path, &st) == 0) return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    if (!create || errno != ENOENT || mkdir(path, 0700) != 0) return false;
    return online_lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}
// Creates only new, unique attempt files. Never truncates/deletes any old file.
static bool create_stage(char* error, size_t cap) {
    if (!lock_card(error, cap)) return false;
    uint64_t capacity = 0, free_bytes = 0;
    bool okay = folio_storage_space(&capacity, &free_bytes, error, cap);
    if (okay && free_bytes < (uint64_t)s_work.manifest.image.size + 65536) {
        error_text(error, cap, "TF 卡空间不足，至少需固件大小加 64 KiB 空余。"); okay = false;
    }
    char version_path[FOLIO_ONLINE_PATH_CAP], attempt[FOLIO_ONLINE_PATH_CAP];
    int n = snprintf(version_path, sizeof(version_path), ONLINE_STAGE_ROOT "/%s", s_work.manifest.image.version);
    if (okay) okay = n > 0 && (size_t)n < sizeof(version_path) && safe_directory(FOLIO_SD_ROOT, false) &&
        safe_directory(FOLIO_SD_ROOT "/.folio", true) && safe_directory(ONLINE_STAGE_ROOT, true) && safe_directory(version_path, true);
    bool created = false;
    for (unsigned tries = 0; okay && tries < 8 && !created; ++tries) {
        n = snprintf(attempt, sizeof(attempt), "%s/%08lx", version_path, (unsigned long)esp_random());
        if (n <= 0 || (size_t)n >= sizeof(attempt)) { okay = false; break; }
        if (mkdir(attempt, 0700) == 0) created = true;
        else if (errno != EEXIST) okay = false;
    }
    bool opened = false;
    if (okay && created) {
        n = snprintf(s_work.path, sizeof(s_work.path), "%s/folio.bin", attempt);
        if (n > 0 && (size_t)n < sizeof(s_work.path)) {
            int fd = open(s_work.path, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) opened = close(fd) == 0;
        }
    }
    if (!opened && error && cap && !*error) error_text(error, cap, "无法创建独立更新暂存目录，旧文件保持不变。");
    unlock_card(); return opened;
}
// No descriptor escapes this short lock. Cancellation/lock-timeout cleanup
// therefore never waits forever or accesses an unmounted volume to close it.
static bool stage_write(const uint8_t* bytes, size_t count, uint32_t offset,
    bool sync, char* error, size_t cap) {
    if (!lock_card(error, cap)) return false;
    bool okay = folio_storage_validate_parent(s_work.path, error, cap);
    int fd = okay ? open(s_work.path, O_WRONLY | O_APPEND) : -1;
    struct stat status;
    if (fd < 0) okay = false;
    if (okay) okay = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size == (off_t)offset;
    if (okay && count) okay = write(fd, bytes, count) == (ssize_t)count;
    if (okay && sync) okay = fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) okay = false;
    unlock_card();
    if (!okay && error && cap && !*error) error_text(error, cap, "TF 卡写入失败、空间不足或暂存文件已变化。");
    return okay;
}
static folio_online_state_t download_image(char* error, size_t cap) {
    if (!verify_manifest(&s_work.manifest, error, cap) || !connection_ready(error, cap)) return FOLIO_ONLINE_FAILED;
    const char* primary = s_work.manifest.url;
    char mirror[192];
    bool can_mirror = folio_remote_cdn_url(primary, s_work.manifest.image.version, mirror, sizeof(mirror));
    int status; int64_t length;
    int64_t deadline = esp_timer_get_time() + ONLINE_IMAGE_BUDGET_US;
    uint8_t* buffer = malloc(ONLINE_CHUNK);
    if (!buffer) {
        error_text(error, cap, "下载缓冲内存不足。"); memset(&s_work.preview, 0, sizeof(s_work.preview));
        return FOLIO_ONLINE_FAILED;
    }
    bool okay = false, retryable = false, use_mirror = false, stage_exists = false;
    uint32_t received = 0;
    for (;;) {
        if (use_mirror && !within_budget(deadline, error, cap)) { okay = false; break; }
        received = 0; retryable = false;
        okay = http_begin(primary, mirror, use_mirror, &status, &length, deadline, error, cap);
        if (!okay) retryable = true;
        if (okay && length < -1) { error_text(error, cap, "服务器文件长度无效。"); okay = false; }
        if (okay && status != 200) {
            retryable = status >= 500 && status <= 599;
            okay = response_ok(status, error, cap);
        }
        if (okay && length >= 0 && (uint64_t)length != s_work.manifest.image.size) {
            error_text(error, cap, "服务器固件长度与已签名清单不一致。"); okay = false;
        }
        // Resolve/connect/headers finish before creating even an empty attempt.
        if (okay && !within_budget(deadline, error, cap)) okay = false;
        if (okay && !stage_exists) {
            okay = create_stage(error, cap);
            stage_exists = okay;
        }
        while (okay) {
            if (!within_budget(deadline, error, cap)) { okay = false; break; }
            int n = s_transport.read(s_transport.context, buffer, ONLINE_CHUNK);
            if (n < 0 || n > (int)ONLINE_CHUNK) {
                error_text(error, cap, "固件下载中断，当前固件保留。"); okay = false; retryable = true; break;
            }
            if (!n) {
                okay = s_transport.complete(s_transport.context) && received == s_work.manifest.image.size;
                if (!okay) { error_text(error, cap, "固件下载不完整，当前固件保留。"); retryable = true; }
                break;
            }
            if ((uint32_t)n > s_work.manifest.image.size - received) {
                error_text(error, cap, "下载数据超过已签名的固件大小。"); okay = false; break;
            }
            if (!stage_write(buffer, (size_t)n, received, false, error, cap)) { okay = false; break; }
            received += (uint32_t)n; progress(received, s_work.manifest.image.size);
            vTaskDelay(1);
        }
        if (okay) break;
        // The signed URL is never rewritten; only this attempt can use the CDN.
        if (!retryable || use_mirror || !can_mirror || cancelled(NULL)) break;
        close_connection();
        if (cancelled(NULL)) break;
        // Keep the old partial untouched. A retry gets a new unique attempt
        // path, and only that new complete file can reach validation/install.
        if (stage_exists && !create_stage(error, cap)) break;
        received = 0; progress(0, s_work.manifest.image.size);
        use_mirror = true;
        if (error && cap) *error = 0;
    }
    // The complete TLS session is destroyed BEFORE validation or installation.
    close_connection(); free(buffer);
    if (okay && !stage_write(NULL, 0, received, true, error, cap)) okay = false;
    if (okay && !within_budget(deadline, error, cap)) okay = false;
    // SHA256 and ESP image validation never trigger another network attempt.
    if (okay && lock_card(error, cap)) {
        const folio_update_control_t control = {.cancelled = cancelled};
        okay = folio_update_inspect_file(s_work.path, &s_work.manifest.image, &s_work.preview, &control, error, cap);
        unlock_card();
    } else okay = false;
    if (!okay) { memset(&s_work.preview, 0, sizeof(s_work.preview)); return FOLIO_ONLINE_FAILED; }
    portENTER_CRITICAL(&s_mux);
    snprintf(s_snapshot.candidate_path, sizeof(s_snapshot.candidate_path), "%s", s_work.path);
    portEXIT_CRITICAL(&s_mux);
    error_text(error, cap, "固件已下载并验证，请确认电量后安装。");
    return FOLIO_ONLINE_READY;
}
static bool install_cancelled(void* ignored) {
    (void)ignored;
    char reason[FOLIO_ONLINE_MESSAGE_CAP];
    return cancelled(NULL) || !s_hooks.install_allowed(s_hooks.context, reason, sizeof(reason));
}
static folio_online_state_t install_image(char* error, size_t cap) {
    // The envelope is immutable worker-owned RAM, not a trusted-looking sidecar.
    folio_remote_manifest_t signed_manifest;
    if (!verify_manifest(&signed_manifest, error, cap) || !card(error, cap)) return FOLIO_ONLINE_FAILED;
    if (!s_hooks.install_allowed(s_hooks.context, error, cap)) {
        if (error && cap && !*error) error_text(error, cap, "电量或安装授权不足，当前固件保留。");
        return FOLIO_ONLINE_FAILED;
    }
    if (!lock_card(error, cap)) return FOLIO_ONLINE_FAILED;
    const folio_update_control_t control = {.cancelled = install_cancelled};
    folio_update_info_t actual;
    bool okay = folio_storage_validate_parent(s_work.path, error, cap) &&
        folio_update_inspect_file(s_work.path, &signed_manifest.image, &actual, &control, error, cap);
    if (okay) okay = s_hooks.install_allowed(s_hooks.context, error, cap) && !cancelled(NULL);
    if (okay) okay = folio_update_install_file(s_work.path, &actual, &control, error, cap);
    unlock_card();
    if (!okay) {
        if (error && cap && !*error) error_text(error, cap, "安装安全检查未通过，当前固件保留。");
        return FOLIO_ONLINE_FAILED;
    }
    error_text(error, cap, "新固件安装完成，请重新启动设备。");
    return FOLIO_ONLINE_INSTALLED;
}
static void worker(void* unused) {
    (void)unused;
    portENTER_CRITICAL(&s_mux); s_worker_task = xTaskGetCurrentTaskHandle(); portEXIT_CRITICAL(&s_mux);
    char error[FOLIO_ONLINE_MESSAGE_CAP] = {0}; folio_online_state_t state;
    if (s_work.job == JOB_CHECK) state = check_manifest(error, sizeof(error));
    else if (s_work.job == JOB_DOWNLOAD) state = download_image(error, sizeof(error));
    else state = install_image(error, sizeof(error));
    if (state != FOLIO_ONLINE_INSTALLED && cancelled(NULL)) {
        state = FOLIO_ONLINE_CANCELLED; error_text(error, sizeof(error), "在线更新已取消，当前固件保留。");
    }
    portENTER_CRITICAL(&s_mux);
    s_worker_task = NULL; s_snapshot.state = state; s_snapshot.busy = false; ++s_snapshot.revision;
    snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s", error);
    if (state == FOLIO_ONLINE_FAILED || state == FOLIO_ONLINE_CANCELLED) s_snapshot.candidate_path[0] = 0;
    portEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}
bool folio_online_update_init(const folio_online_hooks_t* hooks) {
    if (!hooks || !hooks->card_ready || !hooks->storage_lock || !hooks->storage_unlock || !hooks->install_allowed) return false;
    if (hooks->transport && (!hooks->transport->open || !hooks->transport->read || !hooks->transport->complete || !hooks->transport->close)) return false;
    portENTER_CRITICAL(&s_mux);
    if (s_snapshot.busy || s_snapshot.state == FOLIO_ONLINE_INSTALLED) { portEXIT_CRITICAL(&s_mux); return false; }
    s_hooks = *hooks;
    if (hooks->transport) s_transport = *hooks->transport;
    else s_transport = (folio_online_transport_t){&s_https, https_open, https_read, https_complete, https_close};
    memset(&s_work, 0, sizeof(s_work)); memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_connection_open = false; s_initialized = true; s_cancel = false;
    portEXIT_CRITICAL(&s_mux); return true;
}
static bool launch(job_t job, const char* channel, char* error, size_t cap) {
    error_text(error, cap, "");
    portENTER_CRITICAL(&s_mux);
    bool allowed = s_initialized && !s_snapshot.busy && s_snapshot.state != FOLIO_ONLINE_INSTALLED &&
        (job == JOB_CHECK || (job == JOB_DOWNLOAD && s_snapshot.state == FOLIO_ONLINE_AVAILABLE) ||
            (job == JOB_INSTALL && s_snapshot.state == FOLIO_ONLINE_READY));
    if (!allowed) { portEXIT_CRITICAL(&s_mux); error_text(error, cap, "在线更新正忙或当前操作尚未就绪。"); return false; }
    if (job == JOB_CHECK) {
        memset(&s_work, 0, sizeof(s_work));
        uint32_t revision = s_snapshot.revision;
        memset(&s_snapshot, 0, sizeof(s_snapshot)); s_snapshot.revision = revision;
        snprintf(s_work.channel, sizeof(s_work.channel), "%s", channel);
        snprintf(s_snapshot.channel, sizeof(s_snapshot.channel), "%s", channel);
    }
    s_cancel = false; s_work.job = job; s_snapshot.busy = true; ++s_snapshot.revision;
    s_snapshot.state = job == JOB_CHECK ? FOLIO_ONLINE_CHECKING : job == JOB_DOWNLOAD ? FOLIO_ONLINE_DOWNLOADING : FOLIO_ONLINE_INSTALLING;
    snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s", job == JOB_CHECK ? "正在检查签名发布清单" : job == JOB_DOWNLOAD ? "正在下载新固件" : "正在复验并安装，请保持供电");
    portEXIT_CRITICAL(&s_mux);
    if (xTaskCreate(worker, "folio_online", 16384, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_snapshot.busy = false; s_snapshot.state = FOLIO_ONLINE_FAILED;
        snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s", "在线更新任务内存不足。"); ++s_snapshot.revision;
        portEXIT_CRITICAL(&s_mux);
        error_text(error, cap, "在线更新任务内存不足。"); return false;
    }
    return true;
}
bool folio_online_update_start_check(const char* channel, char* error, size_t cap) {
    if (!folio_remote_manifest_url(channel)) { error_text(error, cap, "更新通道无效。"); return false; }
    return launch(JOB_CHECK, channel, error, cap);
}
bool folio_online_update_start_download(char* error, size_t cap) { return launch(JOB_DOWNLOAD, NULL, error, cap); }
bool folio_online_update_start_install(char* error, size_t cap) { return launch(JOB_INSTALL, NULL, error, cap); }
void folio_online_update_cancel(void) { portENTER_CRITICAL(&s_mux); if (s_snapshot.busy) s_cancel = true; portEXIT_CRITICAL(&s_mux); }
bool folio_online_update_busy(void) { portENTER_CRITICAL(&s_mux); bool busy = s_snapshot.busy; portEXIT_CRITICAL(&s_mux); return busy; }
bool folio_online_update_is_worker(void) {
    void* current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_mux); bool worker = s_worker_task && s_worker_task == current; portEXIT_CRITICAL(&s_mux);
    return worker;
}
bool folio_online_update_cancel_requested(void) { return s_initialized && cancelled(NULL); }
void folio_online_update_get_snapshot(folio_online_snapshot_t* snapshot) {
    if (!snapshot) return;
    portENTER_CRITICAL(&s_mux); *snapshot = s_snapshot; portEXIT_CRITICAL(&s_mux);
}
