#include "folio_online_update.h"
#include "folio_remote_manifest.h"
#include "folio_online_guard.h"
#include "folio_storage.h"
#include "book/folio_font_assets.h"
#include "psa/crypto.h"
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
// One card transaction per 64 KiB instead of one per network chunk: slow SD
// cards spend most of their time in open/close/FAT metadata, not in the write.
#define ONLINE_WRITE_BATCH (64u * 1024u)
// Resumes are progress-driven; this only bounds a pathological flapping link.
#define ONLINE_MAX_RESUMES 8u
// A 5 s socket timeout aborted whole firmware downloads on slow links: a 3 MiB
// image needs ~55 s at 55 kB/s, so one stalled TCP window ended the transfer.
// The total budget below still bounds the operation, and cancel stays instant.
#define ONLINE_HTTP_TIMEOUT_MS 20000
#define ONLINE_MANIFEST_BUDGET_US INT64_C(30000000)
// Room for a slow primary attempt plus one CDN retry at that same rate.
#define ONLINE_IMAGE_BUDGET_US INT64_C(300000000)
#define ONLINE_SOURCE_BUDGET_US INT64_C(900000000)
#define ONLINE_STAGE_ROOT FOLIO_SD_ROOT "/.folio/online-update"

typedef enum { JOB_CHECK, JOB_DOWNLOAD, JOB_INSTALL, JOB_FONT } job_t;
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
    unsigned font_index;
    job_t job;
} s_work;
typedef struct {
    esp_http_client_handle_t client;
    folio_online_guard_t guard;
    int64_t deadline_us;
    char mirror_url[192];
    uint32_t range_start;
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
static bool https_seek(void* context, uint32_t offset) {
    https_t* state = context;
    state->range_start = offset;
    return true;
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
    // Resuming asks only for the bytes that are missing. The request body is
    // appended to the same staged file, and the complete file still has to pass
    // the signed SHA-256 and ESP image checks before it can be installed.
    if (state->range_start) {
        char range[32];
        int n = snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)state->range_start);
        if (n <= 0 || (size_t)n >= sizeof(range) ||
            esp_http_client_set_header(state->client, "Range", range) != ESP_OK) {
            error_text(error, cap, "HTTPS 初始化失败。"); return false;
        }
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
static bool http_begin(const char* primary, const char* mirror, bool use_mirror, uint32_t range,
    int* status, int64_t* length, int64_t deadline, char* error, size_t cap) {
    *status = 0; *length = -1;
    const char* url = use_mirror ? mirror : primary;
    if (range && (!s_transport.seek || !s_transport.seek(s_transport.context, range))) {
        error_text(error, cap, "更新服务器不支持断点续传。");
        return false;
    }
    if (!range && s_transport.seek) (void)s_transport.seek(s_transport.context, 0);
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
        used = 0; retryable = false;
        okay = http_begin(primary, mirror, use_mirror, 0, &status, &length, deadline, error, cap);
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
            if (!within_budget(deadline, error, cap)) {
                okay = false;
                retryable = esp_timer_get_time() > deadline;
                break;
            }
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
        // Give the mirror its own bounded attempt after a primary timeout.
        if (!retryable || use_mirror || !can_mirror || cancelled(NULL)) break;
        close_connection();
        if (cancelled(NULL)) break;
        use_mirror = true;
        deadline = esp_timer_get_time() + ONLINE_MANIFEST_BUDGET_US;
        if (error && cap) *error = 0;
    }
    close_connection();
    if (okay && !within_budget(deadline, error, cap)) okay = false;
    s_work.envelope_size = okay ? used : 0;
    // Signature/JSON validation is deliberately outside the retry loop.
    if (!okay || !folio_remote_manifest_verify(s_work.envelope, used, s_work.channel,
        0, &s_work.manifest, error, cap)) return FOLIO_ONLINE_FAILED;
    if (s_work.manifest.sequence <= FOLIO_RELEASE_SEQUENCE) {
        memset(&s_work.manifest, 0, sizeof(s_work.manifest));
        s_work.envelope_size = 0;
        error_text(error, cap, "当前通道没有更高版本。");
        return FOLIO_ONLINE_NO_RELEASE;
    }
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
    const uint32_t total = s_work.manifest.image.size;
    const char* primary = s_work.manifest.url;
    char mirror[192];
    bool can_mirror = folio_remote_cdn_url(primary, s_work.manifest.image.version, mirror, sizeof(mirror));
    int status; int64_t length;
    // Read directly into the remaining space in the card write batch.
    uint8_t* batch = malloc(ONLINE_WRITE_BATCH);
    if (!batch) {
        error_text(error, cap, "下载缓冲内存不足。"); memset(&s_work.preview, 0, sizeof(s_work.preview));
        return FOLIO_ONLINE_FAILED;
    }
    bool okay = false, retryable = false, use_mirror = false, stage_exists = false;
    uint32_t written = 0;   // bytes already committed to the staged file
    uint32_t filled = 0;    // bytes held in the batch buffer
    unsigned resumes = 0, stalled = 0;
    // Each source gets its own budget: the primary used to consume the whole
    // deadline, so the CDN fallback never got a turn.
    int64_t source_deadline = esp_timer_get_time() + ONLINE_SOURCE_BUDGET_US;
    int64_t deadline = esp_timer_get_time() + ONLINE_IMAGE_BUDGET_US;
    for (;;) {
        const uint32_t resume_at = written;
        uint32_t skip = 0;
        retryable = false;
        if (!within_budget(deadline, error, cap)) break;
        okay = http_begin(primary, mirror, use_mirror, resume_at, &status, &length, deadline, error, cap);
        if (!okay) retryable = true;
        if (okay && length < -1) { error_text(error, cap, "服务器文件长度无效。"); okay = false; }
        if (okay && resume_at) {
            if (status == 200) {
                // Some mirrors return the full body. Consume its prefix without
                // appending it twice. The signed hash covers the assembled file.
                skip = resume_at;
            } else if (status != 206 || length < 0 || (uint64_t)length != (uint64_t)total - resume_at) {
                error_text(error, cap, "断点续传响应与已签名清单不一致。"); okay = false;
            }
        } else if (okay && status != 200) {
            retryable = status >= 500 && status <= 599;
            okay = response_ok(status, error, cap);
        }
        if (okay && status == 200 && length >= 0 && (uint64_t)length != total) {
            error_text(error, cap, "服务器固件长度与已签名清单不一致。"); okay = false;
        }
        // Resolve/connect/headers finish before creating even an empty attempt.
        if (okay && !within_budget(deadline, error, cap)) okay = false;
        if (okay && !stage_exists) {
            okay = create_stage(error, cap);
            stage_exists = okay;
        }
        while (okay) {
            if (!within_budget(deadline, error, cap)) {
                okay = false;
                retryable = esp_timer_get_time() > deadline;
                break;
            }
            size_t remaining = ONLINE_WRITE_BATCH - filled;
            size_t capacity = remaining < ONLINE_CHUNK ? remaining : ONLINE_CHUNK;
            if (skip && capacity > skip) capacity = skip;
            int n = s_transport.read(s_transport.context, batch + filled, capacity);
            if (esp_timer_get_time() >= deadline) {
                error_text(error, cap, "本次下载连接超时。"); okay = false; retryable = true; break;
            }
            if (n < 0 || (size_t)n > capacity) {
                error_text(error, cap, "固件下载中断，当前固件保留。"); okay = false; retryable = true; break;
            }
            if (!n) {
                okay = s_transport.complete(s_transport.context) && written + filled == total;
                if (!okay) { error_text(error, cap, "固件下载不完整，当前固件保留。"); retryable = true; }
                break;
            }
            if (skip) {
                skip -= (uint32_t)n;
                continue;
            }
            if ((uint32_t)n > total - (written + filled)) {
                error_text(error, cap, "下载数据超过已签名的固件大小。"); okay = false; break;
            }
            filled += (uint32_t)n;
            if (filled == ONLINE_WRITE_BATCH) {
                if (!stage_write(batch, filled, written, false, error, cap)) { okay = false; break; }
                written += filled; filled = 0;
            }
            progress(written + filled, total);
            vTaskDelay(1);
        }
        if (okay) break;
        if (cancelled(NULL)) break;
        close_connection();
        // Commit the tail so the staged file matches the resume offset we use.
        if (stage_exists && filled) {
            if (!stage_write(batch, filled, written, false, error, cap)) break;
            written += filled; filled = 0;
        }
        const uint32_t got = written;
        if (got == resume_at) ++stalled; else stalled = 0;
        if (!retryable) break;
        // Continue the same file when this attempt actually delivered bytes.
        const bool resumable = stage_exists && got > 0 && got < total &&
            s_transport.seek && resumes < ONLINE_MAX_RESUMES && stalled == 0 && esp_timer_get_time() < source_deadline;
        if (resumable) {
            ++resumes;
            deadline = esp_timer_get_time() + ONLINE_IMAGE_BUDGET_US;
            if (deadline > source_deadline) deadline = source_deadline;
            if (error && cap) *error = 0;
            continue;
        }
        if (use_mirror || !can_mirror) break;
        // No usable partial: one clean attempt on the mirror, on a new file.
        // When no file exists yet the next iteration creates one, exactly like
        // the first attempt, so stage_exists must not be forced true here.
        if (stage_exists && !create_stage(error, cap)) break;
        written = 0; filled = 0; use_mirror = true;
        resumes = 0; stalled = 0;
        source_deadline = esp_timer_get_time() + ONLINE_SOURCE_BUDGET_US;
        deadline = esp_timer_get_time() + ONLINE_IMAGE_BUDGET_US;
        if (error && cap) *error = 0;
    }
    // The complete TLS session is destroyed BEFORE validation or installation.
    close_connection();
    if (okay && filled) {
        okay = stage_write(batch, filled, written, false, error, cap);
        written += filled; filled = 0;
    }
    free(batch);
    if (okay && !stage_write(NULL, 0, written, true, error, cap)) okay = false;
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
static bool font_file_matches(const char *path, const folio_font_asset_t *face, uint8_t *buffer) {
    psa_hash_operation_t hash=PSA_HASH_OPERATION_INIT;
    FILE *file=fopen(path,"rb");
    uint8_t digest[32]; size_t n=0;
    bool okay=file && psa_crypto_init()==PSA_SUCCESS && psa_hash_setup(&hash,PSA_ALG_SHA_256)==PSA_SUCCESS;
    size_t remaining=face->length;
    while (okay && remaining) {
        size_t chunk=remaining<16384?remaining:16384;
        okay=!cancelled(NULL) && fread(buffer,1,chunk,file)==chunk && psa_hash_update(&hash,buffer,chunk)==PSA_SUCCESS;
        remaining-=chunk;
    }
    okay=okay && fgetc(file)==EOF && !ferror(file) &&
        psa_hash_finish(&hash,digest,sizeof(digest),&n)==PSA_SUCCESS && n==32 && !memcmp(digest,face->sha256,32);
    if (file && fclose(file)!=0) okay=false;
    psa_hash_abort(&hash);
    return okay;
}

// Sources are pinned alongside their SHA-256 values in the firmware.
static bool download_font_file(unsigned asset, uint32_t prior, uint32_t total, char *error, size_t cap) {
    const folio_font_asset_t *face = &card_faces[asset];
    const char *root = FOLIO_SD_ROOT "/folio-fonts";
    if (!lock_card(error, cap)) return false;
    uint64_t capacity, available;
    char backup[FOLIO_ONLINE_PATH_CAP];
    snprintf(backup,sizeof(backup),"%s.old",face->path);
    struct stat st;
    bool okay = safe_directory(root,true) && folio_storage_validate_parent(face->path,error,cap) &&
        folio_storage_validate_parent(backup,error,cap);
    if (okay && online_lstat(backup,&st)==0) {
        okay=S_ISREG(st.st_mode);
        if (okay && online_lstat(face->path,&st)!=0)
            okay=errno==ENOENT && rename(backup,face->path)==0;
    }
    uint8_t *buffer=malloc(16384);
    if (!buffer) { error_text(error,cap,"内存不足，无法下载字体。");okay=false; }
    if (okay && online_lstat(face->path,&st)==0) {
        okay=S_ISREG(st.st_mode);
        if (okay && st.st_size==(off_t)face->length && font_file_matches(face->path,face,buffer)) {
            (void)unlink(backup);
            progress(prior+face->length,total);unlock_card();free(buffer);return true;
        }
    }
    okay=okay && folio_storage_space(&capacity,&available,error,cap);
    if (okay && available < face->length + 65536) {
        error_text(error,cap,"TF 卡空间不足，无法保存字体。");
        okay = false;
    }
    snprintf(s_work.path,sizeof(s_work.path),"%s.part",face->path);
    if (okay) okay = folio_storage_validate_parent(s_work.path,error,cap);
    if (okay) {
        int fd = open(s_work.path,O_WRONLY|O_CREAT|O_TRUNC,0600);
        okay = fd >= 0 && close(fd) == 0;
    }
    unlock_card();
    if (!okay) { if (!*error) error_text(error,cap,"无法创建字体下载文件。"); free(buffer); return false; }
    char url[192];
    snprintf(url,sizeof(url),"https://testingcf.jsdelivr.net/gh/qmqsu/folio@v0.8.46/main/assets/fonts/folio_%s.ttf",face->id);
    char raw[192];
    snprintf(raw,sizeof(raw),"https://raw.githubusercontent.com/qmqsu/folio/v0.8.46/main/assets/fonts/folio_%s.ttf",face->id);
    okay = false;
    for (unsigned attempt=0; buffer && attempt<2 && !cancelled(NULL); ++attempt) {
        if (attempt) {
            if (!lock_card(error,cap)) break;
            int fd=open(s_work.path,O_WRONLY|O_TRUNC);
            bool reset=fd>=0 && close(fd)==0;
            unlock_card();
            if (!reset) break;
        }
        error[0]=0;
        int status; int64_t length;
        int64_t deadline=esp_timer_get_time()+ONLINE_SOURCE_BUDGET_US;
        okay=http_begin(NULL,attempt?raw:url,true,0,&status,&length,deadline,error,cap) &&
            response_ok(status,error,cap) && (length<0 || (uint64_t)length==face->length);
        size_t received=0;
        while (okay && received<face->length) {
            size_t used=0, want=face->length-received;
            if (want>16384) want=16384;
            while (used<want) {
                if (!card(error,cap) || !within_budget(deadline,error,cap)) break;
                int n=s_transport.read(s_transport.context,buffer+used,want-used);
                if (n<=0 || (size_t)n>want-used) break;
                used+=(size_t)n;
            }
            okay=used==want && stage_write(buffer,used,received,received+used==face->length,error,cap);
            if (okay) {received+=used; progress(prior+received,total);}
        }
        if (okay) okay=s_transport.read(s_transport.context,buffer,1)==0 && s_transport.complete(s_transport.context);
        close_connection();
        if (okay) break;
    }
    if (okay && lock_card(error,cap)) {
        // Hash the actual staged file before exposing it under its final name.
        okay=font_file_matches(s_work.path,face,buffer);
        if (okay && !cancelled(NULL)) {
            // FAT may require moving the previous file before replacing it.
            if (rename(s_work.path,face->path)!=0) {
                okay=folio_storage_validate_parent(backup,error,cap) && rename(face->path,backup)==0;
                if (okay) {
                    okay=rename(s_work.path,face->path)==0;
                    if (okay) unlink(backup); else (void)rename(backup,face->path);
                }
            }
        } else okay=false;
        unlock_card();
    } else okay=false;
    free(buffer);
    if (!okay) {
        if (s_hooks.card_ready(s_hooks.context) && s_hooks.storage_lock(s_hooks.context,1000)) {
            (void)unlink(s_work.path); unlock_card();
        }
        if (!*error) error_text(error,cap,"字体下载或完整性校验失败，请重试。");
    }
    return okay;
}
static folio_online_state_t download_font(char *error,size_t cap) {
    unsigned face=s_work.font_index-1;
    uint32_t total=card_faces[face].length+(face==2?card_faces[3].length:0);
    progress(0,total);
    if (!connection_ready(error,cap)) return FOLIO_ONLINE_FAILED;
    if (face==2 && !download_font_file(3,0,total,error,cap)) return FOLIO_ONLINE_FAILED;
    if (!download_font_file(face,face==2?card_faces[3].length:0,total,error,cap)) return FOLIO_ONLINE_FAILED;
    error_text(error,cap,"下载完成，可启用字体。");
    return FOLIO_ONLINE_FONT_READY;
}

static void worker(void* unused) {
    (void)unused;
    portENTER_CRITICAL(&s_mux); s_worker_task = xTaskGetCurrentTaskHandle(); portEXIT_CRITICAL(&s_mux);
    char error[FOLIO_ONLINE_MESSAGE_CAP] = {0}; folio_online_state_t state;
    if (s_work.job == JOB_CHECK) state = check_manifest(error, sizeof(error));
    else if (s_work.job == JOB_DOWNLOAD) state = download_image(error, sizeof(error));
    else if (s_work.job == JOB_FONT) state = download_font(error, sizeof(error));
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
    else s_transport = (folio_online_transport_t){&s_https, https_open, https_read, https_complete, https_close, https_seek};
    memset(&s_work, 0, sizeof(s_work)); memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_connection_open = false; s_initialized = true; s_cancel = false;
    portEXIT_CRITICAL(&s_mux); return true;
}
static bool launch(job_t job, unsigned face, const char* channel, char* error, size_t cap) {
    error_text(error, cap, "");
    portENTER_CRITICAL(&s_mux);
    bool allowed = s_initialized && !s_snapshot.busy && s_snapshot.state != FOLIO_ONLINE_INSTALLED &&
        (job == JOB_FONT || job == JOB_CHECK || (job == JOB_DOWNLOAD && s_snapshot.state == FOLIO_ONLINE_AVAILABLE) ||
            (job == JOB_INSTALL && s_snapshot.state == FOLIO_ONLINE_READY));
    if (!allowed) { portEXIT_CRITICAL(&s_mux); error_text(error, cap, "在线更新正忙或当前操作尚未就绪。"); return false; }
    if (job == JOB_CHECK || job == JOB_FONT) {
        memset(&s_work, 0, sizeof(s_work));
        uint32_t revision = s_snapshot.revision;
        memset(&s_snapshot, 0, sizeof(s_snapshot)); s_snapshot.revision = revision;
        snprintf(s_work.channel, sizeof(s_work.channel), "%s", channel);
        snprintf(s_snapshot.channel, sizeof(s_snapshot.channel), "%s", channel);
    }
    s_work.font_index=face;
    s_cancel = false; s_work.job = job; s_snapshot.busy = true; ++s_snapshot.revision;
    s_snapshot.state = job == JOB_CHECK ? FOLIO_ONLINE_CHECKING : job != JOB_INSTALL ? FOLIO_ONLINE_DOWNLOADING : FOLIO_ONLINE_INSTALLING;
    snprintf(s_snapshot.message, sizeof(s_snapshot.message), "%s", job == JOB_CHECK ? "正在检查签名发布清单" : job == JOB_FONT ? "正在下载字体" : job == JOB_DOWNLOAD ? "正在下载新固件" : "正在复验并安装，请保持供电");
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
    return launch(JOB_CHECK, 0, channel, error, cap);
}
bool folio_online_update_start_download(char* error, size_t cap) { return launch(JOB_DOWNLOAD, 0, NULL, error, cap); }
bool folio_online_update_start_install(char* error, size_t cap) { return launch(JOB_INSTALL, 0, NULL, error, cap); }
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

bool folio_online_start_font(unsigned face, char* error, size_t cap) {
    return face>=1 && face<=3 && launch(JOB_FONT,face,"",error,cap);
}
