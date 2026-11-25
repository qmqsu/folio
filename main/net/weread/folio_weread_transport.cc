#include "folio_weread_client.h"
#ifndef FOLIO_WEREAD_HOST_TEST
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_tls_errors.h>
#include <algorithm>
#include <cstring>
#include <strings.h>
#include <new>
#include <stdexcept>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_heap_caps.h>
#endif

namespace folio::weread {
namespace {
constexpr char kHost[] = "https://weread.qq.com/";
constexpr char kUa[] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
struct Context { HttpResponse* response; CancelCheck cancelled; int64_t deadline; };
// The event callback has no per-call argument of its own, and the client handle
// outlives a single request, so the active context is tracked here. Requests are
// strictly sequential, which is what the client contract already requires.
Context* g_context = nullptr;
bool active(Context* c) {
    if (c->response->failure != ErrorCode::None) return false;
    if (c->cancelled()) { c->response->failure = ErrorCode::Cancelled; return false; }
    if (esp_timer_get_time() >= c->deadline) { c->response->failure = ErrorCode::Timeout; return false; }
    return true;
}
int remaining_timeout(Context* c, int maximum) {
    if (!active(c)) return 0;
    return static_cast<int>(std::min<int64_t>(maximum,
        std::max<int64_t>(1, (c->deadline - esp_timer_get_time() + 999) / 1000)));
}
esp_err_t event(esp_http_client_event_t* ev) {
    Context* c = g_context;
    if (!c || !active(c)) return ESP_FAIL;
    if (ev->event_id == HTTP_EVENT_ON_HEADER && ev->header_key && ev->header_value &&
        strcasecmp(ev->header_key, "Set-Cookie") == 0) {
        if (std::strlen(ev->header_value) > 2048 || c->response->set_cookies.size() >= 16) {
            c->response->failure = ErrorCode::Limit; return ESP_FAIL;
        }
        try { c->response->set_cookies.emplace_back(ev->header_value); }
        catch (...) { c->response->failure = ErrorCode::NoMemory; return ESP_FAIL; }
    }
    return ESP_OK;
}
/* A WeRead session is one burst of small requests to a single host: shelf, TOC,
 * then one request per chapter, then progress. Building a fresh TLS connection
 * per request made every chapter pay a handshake it did not need, and a
 * 42-chapter book paid it 42 times. The connection survives between requests
 * while it is healthy; anything that looks like a dead socket drops it and the
 * request is retried once on a fresh connection. */
struct Connection {
    esp_http_client_handle_t handle = nullptr;
    bool reusable = false;
    int64_t used = 0;  /* esp_timer microseconds of the last completed request */
};
/* The server closes idle keep-alive sockets itself. Dropping the connection
 * after a quiet spell avoids paying a failed request (and its timeout) before
 * reconnecting. */
constexpr int64_t kIdleMicroseconds = 20000000;
Connection g_connection;
void drop_connection() {
    if (g_connection.handle) { esp_http_client_close(g_connection.handle); esp_http_client_cleanup(g_connection.handle); }
    g_connection.handle = nullptr;
    g_connection.reusable = false;
    g_connection.used = 0;
}
bool set_header(esp_http_client_handle_t h, const char* name, const char* value) {
    return esp_http_client_set_header(h, name, value) == ESP_OK;
}
// An absent optional header must be removed, never left over from the previous
// request on the same connection: a stale Cookie would reach the wrong account.
bool optional_header(esp_http_client_handle_t h, const char* name, const char* value) {
    if (value && *value) return set_header(h, name, value);
    esp_err_t err = esp_http_client_delete_header(h, name);
    return err == ESP_OK || err == ESP_ERR_NOT_FOUND;
}
bool configure(esp_http_client_handle_t h, const HttpRequest& q) {
    return esp_http_client_set_method(h, q.method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET) == ESP_OK &&
           set_header(h, "Accept", "application/json, text/plain, */*") &&
           set_header(h, "Origin", "https://weread.qq.com") &&
           set_header(h, "Accept-Encoding", "identity") &&
           set_header(h, "Cache-Control", "no-cache") &&
           optional_header(h, "Cookie", q.cookie.c_str()) &&
           optional_header(h, "Referer", q.referer.c_str()) &&
           (q.method != "POST" || set_header(h, "Content-Type", "application/json;charset=UTF-8"));
}
void reset_response(HttpResponse* r) {
    r->status = 0; r->failure = ErrorCode::None; r->phase = RequestPhase::Setup;
    r->received_bytes = 0; r->sent = false;
    r->body.clear(); r->set_cookies.clear();
}
}
void EspTransportDisconnect() noexcept { drop_connection(); }
void EspTransport(const HttpRequest& q, HttpResponse* r, CancelCheck cancelled, void*) {
    if (!r) return;
#ifdef ESP_PLATFORM
    struct Trace {
        HttpResponse* response;const char* kind;const char* stage="setup";size_t bytes=0;
        int attempt=0;
        int64_t started=esp_timer_get_time();
        ~Trace(){
            ESP_LOGI("folio_wr_http","%s %s: result=%d http=%d bytes=%u ms=%ld attempt=%d internal=%u largest=%u psram=%u",
                kind,stage,(int)response->failure,response->status,(unsigned)bytes,
                (long)((esp_timer_get_time()-started)/1000),attempt,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    } trace{r,q.reader_tokens?"reader-page":q.url.find("/web/book/chapter/")!=std::string::npos?"chapter":
        q.url.find("/web/shelf/sync")!=std::string::npos?"shelf":
        q.url.find("/web/book/chapterInfos")!=std::string::npos?"toc":
        q.url.find("getProgress")!=std::string::npos?"progress":
        q.url.find("getLoginInfo")!=std::string::npos?"login-poll":"api"};
#endif
    try {
        // Exact origin and no user-info, custom ports, or redirect targets. Cookie stays here.
        if (q.url.rfind(kHost, 0) != 0 || q.url.find_first_of("\r\n") != std::string::npos ||
            (!q.referer.empty() && q.referer != "https://weread.qq.com" && q.referer.rfind(kHost, 0) != 0) ||
            (q.method != "GET" && q.method != "POST") || q.body.size() > 8192 || q.cookie.size() > 2200 ||
            q.max_response > (q.url == "https://weread.qq.com/web/shelf/sync" ? kMaxShelfBytes : 768u * 1024u) || !q.timeout_ms || q.timeout_ms > (q.consume ? 180000u : 60000u)) {
            r->failure = ErrorCode::InvalidArgument; return;
        }
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(q.timeout_ms) * 1000;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool stale = g_connection.reusable &&
                esp_timer_get_time() - g_connection.used > kIdleMicroseconds;
            const bool reuse = attempt == 0 && g_connection.handle && g_connection.reusable && !stale;
#ifdef ESP_PLATFORM
            trace.attempt = attempt;
            trace.stage = "setup"; trace.bytes = 0;
#endif
            if (!reuse) drop_connection();
            Context ctx{r, cancelled, deadline};
            struct Guard {
                Context* ctx;
                explicit Guard(Context* c) : ctx(c) { g_context = c; }
                ~Guard() {
                    g_context = nullptr;
                    if (ctx->response->phase != RequestPhase::Complete) drop_connection();
                }
            } guard(&ctx);
            if (!active(&ctx)) return;
            esp_http_client_handle_t h = g_connection.handle;
            if (h) {
                if (esp_http_client_set_url(h, q.url.c_str()) != ESP_OK) { drop_connection(); continue; }
            } else {
                esp_http_client_config_t config = {};
                config.url = q.url.c_str(); config.user_agent = kUa;
                config.timeout_ms = remaining_timeout(&ctx, q.reader_tokens?20000:10000);
                config.buffer_size = 4096; config.buffer_size_tx = 4096;
                config.transport_type = HTTP_TRANSPORT_OVER_SSL;
                config.disable_auto_redirect = true;
                config.skip_cert_common_name_check = false;
                config.common_name = "weread.qq.com";
                config.crt_bundle_attach = esp_crt_bundle_attach;
                // ESP-IDF retains the ticket in this client handle, not in NVS.
                config.save_client_session = true;
                config.event_handler = event; config.user_data = nullptr;
                h = esp_http_client_init(&config);
                if (!h) { r->failure = ErrorCode::NoMemory; return; }
                g_connection.handle = h; g_connection.reusable = false;
            }
            // The per-request deadline covers the handshake; once the connection
            // is up, short reads keep cancellation responsive.
            // Only reads can replay after a stale socket. /web/book/read and
            // login renewal may already have changed server state, even when
            // their response was lost. Never replay those writes.
            const bool read_only = q.method == "GET" ||
                q.url.rfind("https://weread.qq.com/web/book/chapter/", 0) == 0 ||
                q.url == "https://weread.qq.com/web/book/chapterInfos";
            bool retryable = reuse && read_only;
            auto io_timeout = [&](int maximum) {
                int ms = remaining_timeout(&ctx, maximum);
                if (!ms) return false;
                if (esp_http_client_set_timeout_ms(h, ms) == ESP_OK) return true;
                r->failure = ErrorCode::Transport; return false;
            };
            auto failed = [&](ErrorCode code) {
                if (!active(&ctx)) return;
                r->failure = code;
            };
            if (!configure(h, q)) {
                r->failure = ErrorCode::NoMemory; drop_connection(); return;
            }
            if (!io_timeout(q.reader_tokens?20000:10000)) return;
            if (!active(&ctx)) return;
            // A stale kept-alive socket may fail here or on the first read; both
            // are retried once on a fresh connection.
            r->sent = true;
            r->phase = RequestPhase::Connect;
#ifdef ESP_PLATFORM
            trace.stage="connect";
#endif
            if (esp_http_client_open(h, static_cast<int>(q.body.size())) != ESP_OK) {
                int tls_code=0,tls_flags=0;
                esp_err_t tls_result=esp_http_client_get_and_clear_last_tls_error(h,&tls_code,&tls_flags);
#ifdef ESP_PLATFORM
                ESP_LOGW("folio_wr_http","TLS open failed: result=%d code=%d flags=%d",tls_result,tls_code,tls_flags);
#endif
                if (retryable) { reset_response(r); drop_connection(); continue; }
                // ESP-IDF 6.1 / Mbed TLS 4 maps allocation failures to PSA -141.
                if (active(&ctx)) r->failure = tls_code==-0x008d?ErrorCode::NoMemory:
                    tls_result==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME?ErrorCode::NameResolution:
                    tls_result==ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || tls_result==ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST?ErrorCode::Connection:
                    tls_flags?ErrorCode::Certificate:ErrorCode::Transport;
                return;
            }
            if (!active(&ctx)) return;
            r->phase = RequestPhase::Send;
            size_t sent = 0;
            bool sent_failed = false;
            while (sent < q.body.size()) {
                if (!io_timeout(3000)) return;
                const int n = esp_http_client_write(h, q.body.data() + sent, static_cast<int>(std::min<size_t>(4096, q.body.size() - sent)));
                if (n <= 0) { sent_failed = true; break; }
                sent += static_cast<size_t>(n);
            }
            if (sent_failed) {
                if (retryable) { reset_response(r); drop_connection(); continue; }
                failed(ErrorCode::Transport); return;
            }
            if (!active(&ctx)) return;
#ifdef ESP_PLATFORM
            trace.stage="headers";
#endif
            r->phase = RequestPhase::Headers;
            int64_t length;
            // Login polling can wait for the phone. EAGAIN is an idle socket, not
            // a failed request. Continue the SAME request, checking cancellation
            // between bounded reads; never resend a POST or its headers.
            do {
                if(!io_timeout(3000))return;
                length=esp_http_client_fetch_headers(h);
            } while(length == -ESP_ERR_HTTP_EAGAIN);
            if (length < 0) {
                if (retryable) { reset_response(r); drop_connection(); continue; }
                failed(ErrorCode::Transport); return;
            }
            r->status = esp_http_client_get_status_code(h);
            if (!active(&ctx)) return;
            // Never follow Location; in particular never resend credentials across origins.
            if (r->status >= 300 && r->status < 400) return;
            const size_t limit = q.reader_tokens ? 4 * 1024 * 1024 : q.max_response;
            if (length > static_cast<int64_t>(limit)) { r->failure = ErrorCode::Limit; return; }
            if (!q.reader_tokens && !q.consume && length > 0) r->body.reserve(static_cast<size_t>(length));
            std::string window, ps, pc, token;
            // Fields are <=256 bytes; a 512-byte overlap preserves split fields.
            // Avoid the old 12 KiB internal-RAM reserve while TLS is also active.
            if (q.reader_tokens) window.reserve(512 + 4096);
#ifdef ESP_PLATFORM
            trace.stage="body";
#endif
            r->phase = RequestPhase::Body;
            size_t received = 0; char buffer[4096];
            bool body_failed = false;
            while (active(&ctx)) {
                if (!io_timeout(3000)) return;
                const int n = esp_http_client_read(h, buffer, sizeof(buffer));
                // IDF reports a temporarily idle body with EAGAIN too. Preserve
                // received bytes and the SAME request; never resend a chapter POST.
                if (n == -ESP_ERR_HTTP_EAGAIN) continue;
                if (n < 0) {
                    if (retryable && received == 0) { reset_response(r); drop_connection(); body_failed = true; break; }
                    failed(ErrorCode::Transport);
                    return;
                }
                if (n == 0) break;
                if (static_cast<size_t>(n) > limit - received) { r->failure = ErrorCode::Limit; return; }
                received += static_cast<size_t>(n);
                r->received_bytes=received;
#ifdef ESP_PLATFORM
                trace.bytes=received;
#endif
                if (q.reader_tokens) {
                    window.append(buffer, n);
                    if (ps.empty()) ExtractReaderField(window, "psvts", &ps);
                    if (pc.empty()) ExtractReaderField(window, "pclts", &pc);
                    if (token.empty()) ExtractReaderField(window, "token", &token);
                    if (!ps.empty() && !pc.empty() && !token.empty() && r->status == 200) {
                        if (!active(&ctx)) return;
                        if (esp_http_client_is_complete_data_received(h)) break;
                        r->body = "{\"psvts\":\"" + ps + "\",\"pclts\":\"" + pc + "\",\"token\":\"" + token + "\"}";
                        // This is a metadata probe, never a chapter body. Discard
                        // unread HTML and close its socket before returning it.
                        drop_connection();
                        r->phase = RequestPhase::Complete;
#ifdef ESP_PLATFORM
                        trace.stage = "tokens-ready";
#endif
                        return;
                    }
                    if (window.size() > 512) window.erase(0, window.size() - 512);
                } else if (q.consume) {
                    if (r->status >= 200 && r->status < 300) {
                        r->failure = q.consume(q.consume_context, buffer, static_cast<size_t>(n));
                        if (r->failure != ErrorCode::None) return;
                    }
                } else r->body.append(buffer, n);
            }
            if (body_failed) { reset_response(r); continue; }
            if (!active(&ctx)) return;
            if (!esp_http_client_is_complete_data_received(h)) {
                if (retryable && (!q.consume || !received)) { reset_response(r); drop_connection(); continue; }
                r->failure = ErrorCode::Transport; return;
            }
#ifdef ESP_PLATFORM
            trace.stage="done";
#endif
            if (q.reader_tokens) r->body = "{\"psvts\":\"" + ps + "\",\"pclts\":\"" + pc + "\",\"token\":\"" + token + "\"}";
            r->phase = RequestPhase::Complete;
            // The response was fully consumed, so the socket is clean for reuse.
            g_connection.reusable = true;
            g_connection.used = esp_timer_get_time();
            return;
        }
        if (r->failure == ErrorCode::None) r->failure = ErrorCode::Transport;
    } catch (const std::length_error&) { r->failure = ErrorCode::Limit; }
      catch (const std::bad_alloc&) { r->failure = ErrorCode::NoMemory; }
      catch (...) { r->failure = ErrorCode::Transport; }
}
} // namespace folio::weread
#else
namespace folio::weread {
void EspTransportDisconnect() noexcept {}
void EspTransport(const HttpRequest&, HttpResponse* r, CancelCheck, void*) { if (r) r->failure = ErrorCode::Transport; }
}
#endif
