// Protocol portions adapted from danleegle/wxBook, MIT (LICENSE.wxBook).
// Position enter/report sequencing independently follows CrossMux author docs/source.
#include "folio_weread_client.h"
#include "folio_weread_crypto.h"
#include "vendor/cjson/cJSON.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#ifndef FOLIO_WEREAD_HOST_TEST
#include <esp_random.h>
#endif

namespace folio::weread {
#ifdef FOLIO_WEREAD_TEST_CLOCK
uint64_t TestMonotonicMilliseconds();
#endif
namespace {
constexpr char kHost[] = "https://weread.qq.com";
constexpr char kUa[] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr char kDefaultToken[] = "3c5c8717f3daf09iop3423zafeqoi";
uint64_t monotonic_milliseconds() {
#ifdef FOLIO_WEREAD_TEST_CLOCK
    return TestMonotonicMilliseconds();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
struct RequestBudget {
    uint64_t deadline;
    explicit RequestBudget(uint32_t milliseconds) : deadline(monotonic_milliseconds() + milliseconds) {}
    uint32_t remaining() const {
        uint64_t now = monotonic_milliseconds();
        return now < deadline ? static_cast<uint32_t>(deadline - now) : 0;
    }
};
void wipe(void* ptr, size_t n) noexcept { volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr); while (n--) *p++ = 0; }
void wipe(std::string& s) noexcept { if (!s.empty()) wipe(s.data(), s.size()); s.clear(); }
bool fail(Error* e, ErrorCode code, const char* text, int http = 0, int server = 0) noexcept {
    if (e) { *e = {}; e->code = code; e->http_status = http; e->server_code = server; std::snprintf(e->message, sizeof(e->message), "%s", text); }
    return false;
}
void clear(Error* e) noexcept { if (e) *e = {}; }
uint32_t random_value() {
#ifdef FOLIO_WEREAD_HOST_TEST
    return 271828;
#else
    return esp_random();
#endif
}
bool safe_id(const std::string& s) {
    return !s.empty() && s.size() <= 63 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
bool safe_token(const std::string& s, size_t cap) {
    return !s.empty() && s.size() < cap && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c > 32 && c < 127 && c != ';' && c != '"' && c != '\\' && c != ','; });
}
struct JsonDeleter { void operator()(cJSON* p) const { cJSON_Delete(p); } };
using Json = std::unique_ptr<cJSON, JsonDeleter>;
cJSON* field(cJSON* j, const char* k) { return cJSON_GetObjectItemCaseSensitive(j, k); }
bool json_shape(const std::string& s) {
    unsigned depth = 0; bool quote = false, escape = false;
    for (unsigned char c : s) {
        if (quote) { if (escape) escape = false; else if (c == '\\') escape = true; else if (c == '"') quote = false; else if (c < 32) return false; }
        else if (c == '"') quote = true;
        else if (c == '{' || c == '[') { if (++depth > 32) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return !quote && depth == 0;
}
Json parse(const std::string& s) {
    if (!json_shape(s) || s.find("\\u0000") != std::string::npos) return {};
    const char* end = nullptr;
    Json j(cJSON_ParseWithLengthOpts(s.data(), s.size(), &end, false));
    if (!j || !end) return {};
    while (end < s.data() + s.size() && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end != s.data() + s.size()) return {};
    return j;
}
bool number(cJSON* v, uint32_t* out) {
    if (cJSON_IsNumber(v) && std::isfinite(v->valuedouble) && v->valuedouble >= 0 &&
        v->valuedouble <= UINT32_MAX && std::floor(v->valuedouble) == v->valuedouble) {
        *out = static_cast<uint32_t>(v->valuedouble); return true;
    }
    if (cJSON_IsString(v) && v->valuestring && *v->valuestring) {
        uint64_t n = 0;
        for (const unsigned char* p = reinterpret_cast<unsigned char*>(v->valuestring); *p; ++p) {
            if (*p < '0' || *p > '9' || (n = n * 10 + *p - '0') > UINT32_MAX) return false;
        }
        *out = static_cast<uint32_t>(n); return true;
    }
    return false;
}
int error_code(cJSON* j) {
    cJSON* f = field(j, "errcode"); if (!f) f = field(j, "errCode");
    if (cJSON_IsNumber(f) && f->valuedouble >= INT32_MIN && f->valuedouble <= INT32_MAX) return f->valueint;
    return 0;
}
std::string string(cJSON* j, const char* k, size_t limit) {
    cJSON* v = field(j, k);
    if (cJSON_IsString(v) && v->valuestring) {
        const size_t n = std::strlen(v->valuestring);
        if (n > limit) throw std::length_error("field");
        return std::string(v->valuestring, n);
    }
    uint32_t n;
    if (number(v, &n)) return std::to_string(n);
    return {};
}
bool server_ok(cJSON* j, Error* e) {
    const int code = error_code(j);
    if (!code) return true;
    return fail(e, code == -2012 ? ErrorCode::SessionExpired : ErrorCode::Permission,
                code == -2012 ? "登录已过期，请重新扫码" : "服务端拒绝访问，可能需要购买或授权", 0, code);
}
bool response_ok(const HttpResponse& r, Error* e) {
    if (r.failure != ErrorCode::None) {
        if(r.failure==ErrorCode::Transport || r.failure==ErrorCode::Timeout){
            const char* phase=r.phase==RequestPhase::Connect?"安全连接未完成":
                r.phase==RequestPhase::Send?"请求发送中断":
                r.phase==RequestPhase::Headers?"等待服务器响应失败":
                r.phase==RequestPhase::Body?"正文数据接收中断":nullptr;
            if(phase){
                char message[160];
                if(r.phase==RequestPhase::Body)std::snprintf(message,sizeof(message),"%s（HTTP %d，已收 %u 字节）",phase,r.status,(unsigned)r.received_bytes);
                else std::snprintf(message,sizeof(message),"%s%s，请重试",phase,r.failure==ErrorCode::Timeout?"：超时":"");
                return fail(e,r.failure,message,r.status);
            }
        }
        const char* msg = r.failure == ErrorCode::Cancelled ? "请求已取消" :
                          r.failure == ErrorCode::Timeout ? "网络请求超时" :
                          r.failure == ErrorCode::NameResolution ? "域名解析失败，请检查网络或换手机热点重试" :
                          r.failure == ErrorCode::Connection ? "无法连接微信读书服务器，请稍后重试" :
                          r.failure == ErrorCode::Certificate ? "服务器证书验证失败，请检查日期与网络" :
                          r.failure == ErrorCode::Limit ? "响应超过设备限制" :
                          r.failure == ErrorCode::Malformed ? "书架响应格式无效" :
                          r.failure == ErrorCode::Storage ? "书架写入失败，请检查 TF 卡空间" :
                          r.failure == ErrorCode::NoMemory ? "运行内存不足" : "安全连接或网络请求失败";
        return fail(e, r.failure, msg, r.status);
    }
    if (r.status >= 200 && r.status < 300) return true;
    if (r.status == 401) return fail(e, ErrorCode::SessionExpired, "登录已过期，请重新扫码", r.status);
    if (r.status == 403) return fail(e, ErrorCode::Permission, "无权读取，请检查书籍授权", r.status);
    if (r.status >= 300 && r.status < 400) return fail(e, ErrorCode::Http, "服务器要求跳转，已停止请求", r.status);
    return fail(e, ErrorCode::Http, "微信读书请求失败", r.status);
}
std::string escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out;
}
using Params = std::vector<std::pair<std::string, std::string>>;
std::string params_json(const Params& params) {
    std::string s = "{";
    for (const auto& p : params) {
        if (s.size() > 1) s += ',';
        const bool numeric = p.first == "ct" || p.first == "sc" || p.first == "st" || p.first == "r" ||
            p.first == "ci" || p.first == "co" || p.first == "pr" || p.first == "rt" || p.first == "ts" || p.first == "rn";
        s += "\"" + p.first + "\":";
        s += numeric ? p.second : "\"" + escape(p.second) + "\"";
    }
    return s + '}';
}
std::string app_id() {
    std::string prefix; size_t start = 0; int words = 0;
    const std::string ua = kUa;
    for (size_t i = 0; i <= ua.size() && words < 12; ++i) {
        if (i == ua.size() || ua[i] == ' ' || ua[i] == '\t') {
            if (i > start) { prefix += std::to_string((i - start) % 10); ++words; } start = i + 1;
        }
    }
    uint32_t hash = 0;
    for (uint8_t c : ua) hash = static_cast<uint32_t>((0x83ULL * hash + c) & 0x7fffffff);
    return "wb" + prefix + "h" + std::to_string(hash);
}
bool clock_ready(Error* e) { return std::time(nullptr) >= 1704067200 || fail(e, ErrorCode::Clock, "请先联网校准时间"); }
bool progress(cJSON* item, const std::string& id, Progress* out) {
    if (!cJSON_IsObject(item)) return false;
    const std::string got = string(item, "bookId", 63);
    if (!got.empty() && got != id) return false;
    Progress p; p.book_id = id; p.chapter_uid = string(item, "chapterUid", 63);
    if (!safe_id(p.chapter_uid)) return false;
    p.has_offset = number(field(item, "chapterOffset"), &p.chapter_offset);
    if (!p.has_offset) return false; // Never manufacture a byte/percentage approximation.
    number(field(item, "updateTime"), &p.update_time);
    p.has_reading_time = number(field(item, "readingTime"), &p.reading_seconds);
    if (!p.has_reading_time)
        p.has_reading_time = number(field(item, "recordReadingTime"), &p.reading_seconds);
    cJSON* percent = field(item, "progress");
    if (cJSON_IsNumber(percent) && std::isfinite(percent->valuedouble) && percent->valuedouble >= 0 && percent->valuedouble <= 100)
        p.percent = static_cast<int>(percent->valuedouble);
    *out = std::move(p); return true;
}
#define WR_CATCH_BOOL(e) catch (const std::bad_alloc&) { return fail(e, ErrorCode::NoMemory, "运行内存不足"); } catch (const std::length_error&) { return fail(e, ErrorCode::Limit, "响应字段超过设备限制"); } catch (...) { return fail(e, ErrorCode::Malformed, "微信读书响应无法解析"); }
} // namespace

Session::~Session(){wipe(this,sizeof(*this));}
bool Session::Valid() const noexcept {
    auto valid=[](const char* p,size_t cap,bool id){
        size_t n=strnlen(p,cap);if(n==cap)return false;
        for(size_t i=0;i<n;++i){unsigned char c=p[i];
            if(id){if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;}
            else if(c<=32||c>=127||c==';'||c=='"'||c=='\\'||c==',')return false;
        }return true;
    };
    return vid[0]&&(skey[0]||gid[0])&&valid(vid,sizeof(vid),true)&&valid(skey,sizeof(skey),false)&&valid(gid,sizeof(gid),false)&&valid(rt,sizeof(rt),false);
}
bool Client::ExportSession(Session* out) const noexcept {
    if(!out)return false;
    *out=Session{};
    if(!LoggedIn())return false;
    std::snprintf(out->vid,sizeof(out->vid),"%s",auth_.vid);
    std::snprintf(out->skey,sizeof(out->skey),"%s",auth_.skey);
    std::snprintf(out->gid,sizeof(out->gid),"%s",auth_.gid);
    std::snprintf(out->rt,sizeof(out->rt),"%s",auth_.rt);
    return out->Valid();
}
bool Client::RestoreSession(const Session& saved) noexcept {
    if(!saved.Valid())return false;
    Logout();auth_=saved;return true;
}
HttpRequest::~HttpRequest() { wipe(cookie); }
HttpResponse::~HttpResponse() { wipe(body); for (auto& c : set_cookies) wipe(c); }
Client::Client(Transport t, void* ctx) noexcept : transport_(t), transport_context_(ctx) {}
Client::~Client() { Logout(); }
bool Client::LoggedIn() const noexcept { return auth_.vid[0] && (auth_.skey[0] || auth_.gid[0]); }
void Client::Disconnect() noexcept {
    wipe(token_book_); wipe(psvts_); wipe(pclts_); wipe(reader_token_); token_acquired_at_ = 0;
    if (transport_ == EspTransport) EspTransportDisconnect();
}
void Client::Logout() noexcept { Disconnect(); wipe(&auth_, sizeof(auth_)); }
bool Client::Ready(const std::string& id, Error* e) {
    if (!LoggedIn()) return fail(e, ErrorCode::NotLoggedIn, "请先扫码登录微信读书");
    if (!safe_id(id)) return fail(e, ErrorCode::InvalidArgument, "书籍编号无效");
    if (cancelled_()) return fail(e, ErrorCode::Cancelled, "请求已取消");
    return true;
}
bool ExtractReaderField(const std::string& window, const char* key, std::string* value) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = window.find(needle, pos)) != std::string::npos) {
        size_t p = pos + needle.size(); pos = p;
        while (p < window.size() && std::isspace(static_cast<unsigned char>(window[p]))) ++p;
        if (p == window.size() || window[p++] != ':') continue;
        while (p < window.size() && std::isspace(static_cast<unsigned char>(window[p]))) ++p;
        if (p == window.size() || window[p++] != '"') continue;
        const size_t end = window.find('"', p);
        if (end == std::string::npos) continue;
        const std::string found = window.substr(p, end - p);
        if (safe_token(found, 256)) { *value = found; return true; }
    }
    return false;
}
bool Client::AbsorbCookies(const HttpResponse& r, Error* e) {
    Auth next = auth_;
    for (const auto& cookie : r.set_cookies) {
        if (cookie.size() > 2048) { wipe(&next, sizeof(next)); return fail(e, ErrorCode::Limit, "登录凭据超过设备限制"); }
        const size_t eq = cookie.find('='); if (eq == std::string::npos) continue;
        const std::string name = cookie.substr(0, eq);
        const size_t semi = cookie.find(';', eq + 1);
        const std::string value = cookie.substr(eq + 1, semi == std::string::npos ? semi : semi - eq - 1);
        char* dst = nullptr; size_t cap = 0;
        if (name == "wr_vid") { dst = next.vid; cap = sizeof(next.vid); }
        if (name == "wr_skey") { dst = next.skey; cap = sizeof(next.skey); }
        if (name == "wr_gid") { dst = next.gid; cap = sizeof(next.gid); }
        if (name == "wr_rt") { dst = next.rt; cap = sizeof(next.rt); }
        if (!dst) continue;
        if (!value.empty() && !safe_token(value, cap)) { wipe(&next, sizeof(next)); return fail(e, ErrorCode::Malformed, "登录凭据格式无效"); }
        if (name == "wr_vid" && !value.empty() && !safe_id(value)) { wipe(&next, sizeof(next)); return fail(e, ErrorCode::Malformed, "登录账户编号无效"); }
        std::snprintf(dst, cap, "%s", value.c_str());
    }
    // An authenticated request must never silently switch accounts.
    if (auth_.vid[0] && std::strcmp(auth_.vid, next.vid) != 0) {
        wipe(&next, sizeof(next)); Logout(); return fail(e, ErrorCode::SessionExpired, "登录账户发生变化，请重新扫码");
    }
    auth_ = next; wipe(&next, sizeof(next)); return true;
}
// Frame top-level fields and individual array elements across HTTP reads.
// cJSON still validates each complete value, including ignored metadata.
class ShelfStream {
public:
    explicit ShelfStream(ShelfSink& target) : sink(target) {}

    ErrorCode feed(const char* bytes, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            char c = bytes[i];
            if (!token.empty()) {
                if (!quoted && depth == 0 && (space(c) || c == ',' || c == '}' || c == ']')) {
                    if (!finish_token()) return error;
                } else {
                    if (!append(c)) return error;
                    continue;
                }
            }
            if (space(c)) continue;
            switch (state) {
                case Start: if (c != '{') return ErrorCode::Malformed; state = Key; break;
                case Key:
                    if (c != '"' || !append(c)) return ErrorCode::Malformed;
                    break;
                case Colon: if (c != ':') return ErrorCode::Malformed; state = Value; break;
                case Value:
                    if (c == '[') {
                        if (field_name == "books") {
                            if (have_books) return ErrorCode::Malformed;
                            have_books = true;
                        }
                        state = ArrayStart;
                    } else if (field_name == "books" || !start_value(c)) return ErrorCode::Malformed;
                    break;
                case ArrayStart:
                    if (c == ']') { state = AfterField; break; }
                    [[fallthrough]];
                case Element:
                    if (!start_value(c)) return ErrorCode::Malformed;
                    break;
                case AfterElement:
                    if (c == ']') state = AfterField;
                    else if (c == ',') state = Element;
                    else return ErrorCode::Malformed;
                    break;
                case AfterField:
                    if (c == '}') state = Done;
                    else if (c == ',') state = Key;
                    else return ErrorCode::Malformed;
                    break;
                case Done: return ErrorCode::Malformed;
            }
        }
        return ErrorCode::None;
    }

    bool finish(std::vector<uint16_t>* order, Error* e) {
        if (state != Done || !token.empty()) return fail(e, ErrorCode::Malformed, "书架响应不完整");
        if (server_error)
            return fail(e, server_error == -2012 ? ErrorCode::SessionExpired : ErrorCode::Permission,
                        server_error == -2012 ? "登录已过期，请重新扫码" : "服务端拒绝访问", 0, server_error);
        if (!have_books)
            return fail(e, ErrorCode::Malformed, "书架响应不完整");
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
        std::sort(updates.begin(), updates.end(), [](const Update& a, const Update& b) { return a.id < b.id; });
        size_t stamp = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i && rows[i-1].id == rows[i].id) return fail(e, ErrorCode::Malformed, "书架编号重复");
            while (stamp < updates.size() && updates[stamp].id < rows[i].id) ++stamp;
            while (stamp < updates.size() && updates[stamp].id == rows[i].id) {
                rows[i].updated = std::max(rows[i].updated, updates[stamp].time);
                ++stamp;
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.updated != b.updated ? a.updated > b.updated : a.slot < b.slot;
        });
        order->reserve(rows.size());
        for (const Row& row : rows) order->push_back(row.slot);
        return true;
    }

private:
    enum State { Start, Key, Colon, Value, ArrayStart, Element, AfterElement, AfterField, Done };
    struct Row { std::string id; uint32_t updated; uint16_t slot; };
    State state = Start;
    ShelfSink& sink;
    std::vector<Row> rows;
    struct Update { std::string id; uint32_t time; };
    std::vector<Update> updates;
    std::string token, field_name;
    bool have_books = false, quoted = false, escaped = false;
    unsigned depth = 0;
    int server_error = 0;
    ErrorCode error = ErrorCode::Malformed;

    static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    bool start_value(char c) {
        return c != ']' && c != '}' && c != ',' && c != ':' && append(c);
    }
    bool append(char c) {
        if (token.size() >= 64 * 1024) { error = ErrorCode::Limit; return false; }
        token.push_back(c);
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') {
                quoted = false;
                if (!depth) return finish_token();
            }
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 32) return false; }
        else if (c == ']' || c == '}') {
            if (!depth) return false;
            if (!--depth) return finish_token();
        }
        return true;
    }
    static std::string display_text(cJSON* node, const char* key) {
        cJSON* value = field(node, key);
        if (!cJSON_IsString(value)) return string(node, key, 512);
        size_t n = strnlen(value->valuestring, 513);
        if (n > 512) {
            n = 512;
            while (n && (static_cast<unsigned char>(value->valuestring[n]) & 0xC0) == 0x80) --n;
        }
        return std::string(value->valuestring, n);
    }
    bool finish_token() {
        Json node = parse(token);
        token.clear();
        if (!node) return false;
        if (state == Key) {
            if (!cJSON_IsString(node.get())) return false;
            field_name = node->valuestring;
            state = Colon;
            return true;
        }
        if (state == Value) {
            if ((field_name == "errcode" || field_name == "errCode") && cJSON_IsNumber(node.get()))
                server_error = node->valueint;
            state = AfterField;
            return true;
        }
        state = AfterElement;
        if (field_name == "books") {
            if (rows.size() >= kMaxShelfBooks) { error = ErrorCode::Limit; return false; }
            Book book;
            book.id = string(node.get(), "bookId", 63);
            if (!safe_id(book.id)) return false;
            book.title = display_text(node.get(), "title");
            book.author = display_text(node.get(), "author");
            cJSON* url = field(node.get(), "cover");
            if (cJSON_IsString(url) && strnlen(url->valuestring, 2049) <= 2048) book.cover = url->valuestring;
            if (book.cover.rfind("http://", 0) == 0) book.cover.replace(0, 7, "https://");
            else if (book.cover.rfind("//", 0) == 0) book.cover = "https:" + book.cover;
            if (book.cover.rfind("https://", 0) != 0 || book.cover.find_first_of("\r\n") != std::string::npos)
                book.cover.clear();
            rows.push_back({book.id, 0, static_cast<uint16_t>(rows.size())});
            if (!sink.append(book)) { error = ErrorCode::Storage; return false; }
        } else if (field_name == "bookProgress") {
            std::string id = string(node.get(), "bookId", 63);
            uint32_t updated = 0;
            if (safe_id(id) && number(field(node.get(), "updateTime"), &updated)) {
                if (updates.size() >= kMaxShelfBooks) { error = ErrorCode::Limit; return false; }
                updates.push_back({std::move(id), updated});
            }
        }
        return true;
    }
};

bool Client::FetchShelf(ShelfSink& sink, std::vector<uint16_t>* order, Error* e) noexcept { try {
    clear(e);
    if (!order) return fail(e, ErrorCode::InvalidArgument, "书架参数无效");
    order->clear();
    if (!LoggedIn()) return fail(e, ErrorCode::NotLoggedIn, "请先扫码登录微信读书");
    ShelfStream stream(sink);
    auto r = Request("GET", "/web/shelf/sync", {}, kHost, e, false, 180000, &stream);
    if (r.failure == ErrorCode::Limit) return fail(e, r.failure, "书架超过容量（1 万本、32 MiB 或单项 64 KiB）");
    if (!response_ok(r, e)) return false;
    return stream.finish(order, e);
} WR_CATCH_BOOL(e) }
HttpResponse Client::Request(const char* method, const std::string& path, const std::string& body,
                             const std::string& referer, Error* e, bool tokens, uint32_t budget_ms, ShelfStream* shelf) {
    HttpResponse r;
    if (!transport_ || path.empty() || path[0] != '/' || path.find_first_of("\r\n") != std::string::npos) {
        r.failure = ErrorCode::InvalidArgument; response_ok(r, e); return r;
    }
    if (cancelled_()) { r.failure = ErrorCode::Cancelled; response_ok(r, e); return r; }
    if (!budget_ms) { r.failure = ErrorCode::Timeout; response_ok(r, e); return r; }
    HttpRequest q; q.method = method; q.url = std::string(kHost) + path; q.body = body; q.referer = referer;
    q.reader_tokens = tokens;
    if(path.rfind("/api/auth/getLoginInfo?",0)==0)q.timeout_ms=30000;
    if(tokens || path.rfind("/web/book/chapter/",0)==0)q.timeout_ms=60000;
    if (shelf) {
        q.timeout_ms = budget_ms;
        q.consume_context = shelf;
        q.consume = [](void* stream, const char* data, size_t size) {
            return static_cast<ShelfStream*>(stream)->feed(data, size);
        };
    }
    q.timeout_ms=std::min(q.timeout_ms,budget_ms);
    q.max_response = path.find("/chapter/") != std::string::npos ? (kMaxChapterBytes + 2) / 3 * 4 + 128 : kMaxChapterBytes;
    if (path == "/web/shelf/sync") q.max_response = kMaxShelfBytes;
    auto add = [&](const char* name, const char* value) { if (!*value) return; if (!q.cookie.empty()) q.cookie += "; "; q.cookie += name; q.cookie += '='; q.cookie += value; };
    add("wr_vid", auth_.vid); add("wr_skey", auth_.skey); add("wr_gid", auth_.gid); add("wr_rt", auth_.rt);
    transport_(q, &r, cancelled_, transport_context_);
    if (r.body.size() > q.max_response || r.set_cookies.size() > 16) r.failure = ErrorCode::Limit;
    if (response_ok(r, e) && !AbsorbCookies(r, e)) r.failure = e ? e->code : ErrorCode::Malformed;
    return r; // Never retry POST or GET implicitly.
}
bool Client::BeginQrLogin(QrSession* qr, Error* e) noexcept { try {
    clear(e); if (!qr) return fail(e, ErrorCode::InvalidArgument, "登录参数无效");
    *qr = {}; Logout();
    auto r = Request("GET", "/api/auth/getLoginUid", {}, kHost, e);
    if (!response_ok(r, e)) return false;
    auto j = parse(r.body); if (!j || !server_ok(j.get(), e)) return j ? false : fail(e, ErrorCode::Malformed, "登录二维码响应无效");
    std::string uid = string(j.get(), "uid", 63); if (uid.empty()) uid = string(field(j.get(), "data"), "uid", 63);
    if (!safe_id(uid)) return fail(e, ErrorCode::Malformed, "登录二维码编号无效");
    qr->uid = std::move(uid); qr->url = std::string(kHost) + "/web/confirm?uid=" + qr->uid; return true;
} WR_CATCH_BOOL(e) }
bool Client::PollQrLogin(const QrSession& qr, const std::string& otp, QrState* state, Error* e) noexcept { try {
    clear(e); if (!state || !safe_id(qr.uid) || (!otp.empty() && (otp.size() != 4 || !std::all_of(otp.begin(), otp.end(), [](char c) { return c >= '0' && c <= '9'; }))))
        return fail(e, ErrorCode::InvalidArgument, "二维码或四位验证码无效");
    *state = QrState::Pending;
    auto r = Request("GET", "/api/auth/getLoginInfo?uid=" + qr.uid + "&otp" + (otp.empty() ? "" : "=" + otp), {}, kHost, e);
    if (!response_ok(r, e)) return false;
    auto j = parse(r.body); if (!j) return fail(e, ErrorCode::Malformed, "扫码登录响应无效");
    if (!server_ok(j.get(), e)) return false;
    cJSON* data = field(j.get(), "data"); if (!cJSON_IsObject(data)) data = j.get();
    if (cJSON_IsTrue(field(data, "succeed"))) {
        if (!auth_.vid[0]) { std::string vid = string(data, "webLoginVid", 63); if (vid.empty()) vid = string(data, "vid", 63); if (safe_id(vid)) std::snprintf(auth_.vid, sizeof(auth_.vid), "%s", vid.c_str()); }
        if (!auth_.skey[0]) { std::string token = string(data, "accessToken", 511); if (safe_token(token, sizeof(auth_.skey))) std::snprintf(auth_.skey, sizeof(auth_.skey), "%s", token.c_str()); wipe(token); }
        if (!LoggedIn()) { Logout(); return fail(e, ErrorCode::Malformed, "扫码完成但登录凭据不完整"); }
        *state = QrState::Complete; return true;
    }
    const std::string logic = string(data, "logicCode", 64);
    if (logic == "NEED_OTP") *state = QrState::NeedOtp;
    else if (logic == "OTP_NOT_MATCH") *state = QrState::OtpMismatch;
    else if (logic == "LOGIN_TIMEOUT" || logic == "OTP_EXPIRED") { *state = QrState::Expired; Logout(); }
    else if (!logic.empty()) return fail(e, ErrorCode::Permission, "扫码登录被拒绝，请重新获取二维码");
    return true;
} WR_CATCH_BOOL(e) }
bool Client::RenewSession(Error* e) noexcept { try {
    clear(e); if (!LoggedIn()) return fail(e, ErrorCode::NotLoggedIn, "请先扫码登录微信读书");
    auto r = Request("POST", "/web/login/renewal", "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}", std::string(kHost) + "/web/", e);
    if (!response_ok(r, e)) return false;
    auto j = parse(r.body); if (!j || !server_ok(j.get(), e)) return j ? false : fail(e, ErrorCode::Malformed, "登录续期响应无效");
    uint32_t succ = 0; number(field(j.get(), "succ"), &succ);
    if (succ != 1) return fail(e, ErrorCode::SessionExpired, "登录续期失败，请重新扫码");
    Disconnect(); return true;
} WR_CATCH_BOOL(e) }
bool Client::FetchChapters(const std::string& id, std::vector<Chapter>* out, Error* e) noexcept { try {
    clear(e); if (!out) return fail(e, ErrorCode::InvalidArgument, "目录参数无效"); out->clear(); if (!Ready(id, e)) return false;
    auto r = Request("POST", "/web/book/chapterInfos", "{\"bookIds\":[\"" + id + "\"]}", kHost, e);
    if (!response_ok(r, e)) return false;
    auto j = parse(r.body);
    if (!j) return fail(e, ErrorCode::Malformed, "目录响应无效");
    if (!server_ok(j.get(), e)) return false;
    cJSON* book; cJSON* list = nullptr;
    cJSON_ArrayForEach(book, field(j.get(), "data")) {
        std::string found = string(book, "bookId", 63); if (found.empty()) found = string(field(book, "book"), "bookId", 63);
        if (found != id) continue;
        list = field(book, "updated"); if (!cJSON_IsArray(list)) list = field(book, "chapterInfos"); break;
    }
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) return fail(e, ErrorCode::Permission, "这本书没有可读取的目录");
    if (cJSON_GetArraySize(list) > static_cast<int>(kMaxChapters)) return fail(e, ErrorCode::Limit, "本书超过 512 章，暂不支持");
    std::vector<Chapter> result; result.reserve(cJSON_GetArraySize(list)); cJSON* item;
    cJSON_ArrayForEach(item, list) {
        Chapter c; c.uid = string(item, "chapterUid", 63); c.title = string(item, "title", 512);
        if (!safe_id(c.uid) || !number(field(item, "chapterIdx"), &c.index)) return fail(e, ErrorCode::Malformed, "章节编号无效");
        number(field(item, "wordCount"), &c.word_count);
        if (std::any_of(result.begin(), result.end(), [&](const Chapter& old) { return old.uid == c.uid; })) return fail(e, ErrorCode::Malformed, "目录包含重复章节");
        result.push_back(std::move(c));
    }
    *out = std::move(result); return true;
} WR_CATCH_BOOL(e) }
bool Client::ReaderTokens(const std::string& id, const std::string& chapter, std::string* referer, Error* e, uint32_t budget_ms) {
    if (!safe_id(chapter)) return fail(e, ErrorCode::InvalidArgument, "章节编号无效");
    *referer = std::string(kHost) + "/web/reader/" + crypto::EncodeId(id) + "k" + crypto::EncodeId(chapter);
    const auto now = static_cast<uint32_t>(std::time(nullptr));
    if (token_book_ == id && !psvts_.empty() && token_acquired_at_ &&
        now >= token_acquired_at_ && now - token_acquired_at_ < 300) return true;
    wipe(token_book_); wipe(psvts_); wipe(pclts_); wipe(reader_token_); token_acquired_at_ = 0;
    RequestBudget budget(std::min<uint32_t>(budget_ms,60000));
    auto r = Request("GET", referer->substr(sizeof(kHost) - 1), {}, *referer, e, true,budget.remaining());
    // The reader-page GET retries once; read/time writes are never replayed.
    if (!response_ok(r,e) && !cancelled_() &&
        (r.failure==ErrorCode::Connection || r.failure==ErrorCode::NameResolution ||
         r.failure==ErrorCode::Timeout || r.failure==ErrorCode::Transport)) {
        clear(e);r=Request("GET",referer->substr(sizeof(kHost)-1),{},*referer,e,true,budget.remaining());
    }
    if (!response_ok(r, e)) return false;
    if (!ExtractReaderField(r.body, "psvts", &psvts_)) return fail(e, ErrorCode::Permission, "阅读令牌缺失，请检查登录和书籍授权");
    ExtractReaderField(r.body, "pclts", &pclts_);
    ExtractReaderField(r.body, "token", &reader_token_);
    token_book_ = id; token_acquired_at_ = static_cast<uint32_t>(std::time(nullptr)); return true;
}
namespace {
bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp; unsigned count; unsigned char c = s[i++];
        if (c < 128) { if (!c) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; count = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; count = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; count = 3; }
        else return false;
        const unsigned original = count;
        if (count > s.size() - i) return false;
        while (count--) { c = s[i++]; if ((c & 0xc0) != 0x80) return false; cp = (cp << 6) | (c & 63); }
        if ((original == 2 && cp < 0x800) || (original == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
    }
    return true;
}
bool shard_ok(const HttpResponse& r, Error* e) {
    if (!response_ok(r, e)) return false;
    if (r.body.size() >= 4 && r.body.compare(0, 4, "PK\003\004") == 0)
        return fail(e, ErrorCode::Unsupported, "暂不支持整包 EPUB 类型的微信书籍");
    if (!r.body.empty() && r.body[0] == '{') {
        auto j = parse(r.body); if (j && !server_ok(j.get(), e)) return false;
        return fail(e, ErrorCode::Permission, "章节内容不可用，请检查购买或会员权限");
    }
    return !r.body.empty() || fail(e, ErrorCode::Malformed, "章节响应为空");
}
}
bool Client::FetchChapter(const std::string& id, const Chapter& chapter, ChapterContent* out, Error* e) noexcept { try {
    clear(e); if (!out) return fail(e, ErrorCode::InvalidArgument, "章节参数无效"); *out = {};
    if (!Ready(id, e) || !clock_ready(e)) return false;
    if (!safe_id(chapter.uid)) return fail(e, ErrorCode::InvalidArgument, "章节编号无效");
    // One chapter has a single budget across token acquisition, shards and retries.
    RequestBudget budget(120000);
    std::string referer; if (!ReaderTokens(id, chapter.uid, &referer, e,budget.remaining())) return false;
    auto fetch = [&](const char* endpoint) {
        auto params = crypto::ContentParams(id, chapter.uid, psvts_, static_cast<uint32_t>(std::time(nullptr)), random_value());
        auto response=Request("POST", endpoint, params_json(params), referer, e,false,budget.remaining());
        // These four chapter endpoints only retrieve content. Retry the failed
        // shard once, retaining earlier shards; never replay read/time writes.
        if(!cancelled_() && (response.failure==ErrorCode::Transport || response.failure==ErrorCode::Timeout ||
            response.failure==ErrorCode::Connection || response.failure==ErrorCode::NameResolution)){
            clear(e);params=crypto::ContentParams(id,chapter.uid,psvts_,static_cast<uint32_t>(std::time(nullptr)),random_value());
            response=Request("POST",endpoint,params_json(params),referer,e,false,budget.remaining());
        }
        return response;
    };
    auto first = fetch("/web/book/chapter/e_0");
    if (!response_ok(first, e)) return false;
    bool plain = false;
    if (!first.body.empty() && first.body[0] == '{') {
        auto j = parse(first.body);
        if (!j || !server_ok(j.get(), e)) return j ? false : fail(e, ErrorCode::Malformed, "章节类型响应无效");
        const std::string got = string(j.get(), "bookId", 63);
        if (got != id) return fail(e, ErrorCode::Permission, "章节不可读取，或返回了其他书籍");
        plain = true;
    } else if (!shard_ok(first, e)) return false;
    std::vector<std::string> shards; shards.reserve(3);
    if (!plain) shards.push_back(std::move(first.body));
    else std::string().swap(first.body);
    const char* paths[2] = {plain ? "/web/book/chapter/t_0" : "/web/book/chapter/e_1",
                            plain ? "/web/book/chapter/t_1" : "/web/book/chapter/e_3"};
    size_t aggregate = shards.empty() ? 0 : shards[0].size();
    for (const char* path : paths) {
        if (cancelled_()) return fail(e, ErrorCode::Cancelled, "请求已取消");
        auto r = fetch(path); if (!shard_ok(r, e)) return false;
        if (r.body.size() > (kMaxChapterBytes + 2) / 3 * 4 + 128 - aggregate)
            return fail(e, ErrorCode::Limit, "章节超过 512 KiB，暂不支持");
        aggregate += r.body.size(); shards.push_back(std::move(r.body));
    }
    ChapterContent result;
    if (!crypto::DecodeShards(&shards, &result.bytes, kMaxChapterBytes)) return fail(e, ErrorCode::Malformed, "正文校验或解码失败");
    if (cancelled_()) return fail(e, ErrorCode::Cancelled, "请求已取消");
    if (!valid_utf8(result.bytes)) return fail(e, ErrorCode::Malformed, "正文不是有效 UTF-8 文本");
    result.xhtml = !plain; *out = std::move(result); return true;
} WR_CATCH_BOOL(e) }
bool Client::FetchProgress(const std::string& id, Progress* out, Error* e) noexcept { try {
    clear(e); if (!out) return fail(e, ErrorCode::InvalidArgument, "进度参数无效"); *out = {};
    if (!Ready(id, e)) return false;
    const std::string path = "/web/book/getProgress?bookId=" + id + "&_=" + std::to_string(static_cast<uint64_t>(std::time(nullptr)) * 1000 + random_value() % 1000);
    auto r = Request("GET", path, {}, kHost, e); if (!response_ok(r, e)) return false;
    auto j = parse(r.body); if (!j) return fail(e, ErrorCode::Malformed, "云端进度响应无效"); if (!server_ok(j.get(), e)) return false;
    if (progress(j.get(), id, out)) return true;
    for (const char* name : {"book", "progress", "bookProgress", "data"}) {
        cJSON* obj = field(j.get(), name);
        if (progress(obj, id, out)) return true;
        if (cJSON_IsArray(obj)) { cJSON* item; cJSON_ArrayForEach(item, obj) {
            if (string(item, "bookId", 63) == id && progress(item, id, out)) return true;
        } }
        if (cJSON_IsObject(obj) && progress(field(obj, "book"), id, out)) return true;
    }
    return fail(e, ErrorCode::Malformed, "云端没有可精确定位的阅读位置");
} WR_CATCH_BOOL(e) }
Submission Client::SendRead(const std::string& id, const Chapter& chapter, uint32_t offset, int percent,
                            int seconds, bool report, Error* e) {
    bool possible = false;
    try {
        if (!Ready(id, e) || !clock_ready(e)) return Submission::Rejected;
        if (!safe_id(chapter.uid) || percent < 0 || percent > 100) { fail(e, ErrorCode::InvalidArgument, "待提交的阅读位置无效"); return Submission::Rejected; }
        std::string referer; if (!ReaderTokens(id, chapter.uid, &referer, e)) return Submission::Rejected;
        const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        const uint64_t ts = static_cast<uint64_t>(now) * 1000 + random_value() % 1000;
        const uint32_t rn = random_value() % 1000;
        Params params = {{"appId", app_id()}, {"b", crypto::EncodeId(id)}, {"c", crypto::EncodeId(chapter.uid)},
            {"ci", std::to_string(chapter.index)}, {"co", std::to_string(offset)}, {"sm", ""},
            {"pr", std::to_string(percent)}, {"ct", std::to_string(now)}, {"ps", psvts_},
            {"pc", pclts_.empty() || pclts_ == "0" ? crypto::EncodeId(std::to_string(now)) : pclts_}};
        // Position-only calls completely omit rt. Reading-time uploads explicitly add it.
        if (seconds > 0) params.emplace_back("rt", std::to_string(seconds));
        if (report) {
            params.emplace_back("ts", std::to_string(ts)); params.emplace_back("rn", std::to_string(rn));
            params.emplace_back("sg", crypto::Sha256Hex(std::to_string(ts) + std::to_string(rn) + (reader_token_.empty() ? kDefaultToken : reader_token_)));
        }
        params.emplace_back("s", crypto::Sign(crypto::SortedQuery(params)));
        const std::string body = params_json(params);
        if (cancelled_()) { fail(e, ErrorCode::Cancelled, "请求已取消"); return Submission::Rejected; }
        possible = true;
        auto r = Request("POST", "/web/book/read", body, referer, e);
        if (!response_ok(r, e)) {
            if (!r.sent || (r.failure == ErrorCode::None && r.status >= 400 && r.status < 500)) return Submission::Rejected;
            fail(e, ErrorCode::Uncertain, "提交结果未知，请勿重复上传时长", r.status); return Submission::Unknown;
        }
        auto j = parse(r.body);
        if (!j) { fail(e, ErrorCode::Uncertain, "未收到明确确认，提交结果未知"); return Submission::Unknown; }
        if (!server_ok(j.get(), e)) return Submission::Rejected;
        uint32_t succ = 0; number(field(j.get(), "succ"), &succ);
        const bool acknowledged = succ == 1 || cJSON_IsTrue(field(j.get(), "succeed")) ||
            (seconds < 0 && field(j.get(), "synckey") != nullptr);
        if (acknowledged) return Submission::Confirmed;
        if (field(j.get(), "succ") || field(j.get(), "succeed")) { fail(e, ErrorCode::Permission, "服务端未接受此次提交"); return Submission::Rejected; }
        fail(e, ErrorCode::Uncertain, "未收到明确确认，提交结果未知"); return Submission::Unknown;
    } catch (const std::bad_alloc&) {
        fail(e, possible ? ErrorCode::Uncertain : ErrorCode::NoMemory, possible ? "内存不足，提交结果未知" : "运行内存不足");
    } catch (...) { fail(e, possible ? ErrorCode::Uncertain : ErrorCode::Malformed, possible ? "提交结果未知" : "同步请求无法生成"); }
    return possible ? Submission::Unknown : Submission::Rejected;
}
Submission Client::WritePosition(const std::string& id, const Chapter& c, uint32_t offset, int percent,
                                 Progress* readback, Error* e) noexcept {
    bool started = false;
    try {
        clear(e); if (!readback) { fail(e, ErrorCode::InvalidArgument, "进度参数无效"); return Submission::Rejected; }
        *readback = {};
        if (!Ready(id, e) || !safe_id(c.uid) || percent < 0 || percent > 100) { if (!e || e->code == ErrorCode::None) fail(e, ErrorCode::InvalidArgument, "待提交的阅读位置无效"); return Submission::Rejected; }
        Progress before;
        if (!FetchProgress(id, &before, e)) return Submission::Rejected;
        if (before.chapter_uid == c.uid && before.has_offset && before.chapter_offset == offset) { *readback = std::move(before); return Submission::Confirmed; }
        // Reuse the recently validated book token. Renewal/logout and the
        // bounded token lifetime invalidate it; uploads themselves do not.
        started = true;
        const Submission enter = SendRead(id, c, offset, percent, -1, false, e);
        if (enter == Submission::Rejected) return enter;
        if (enter == Submission::Confirmed) SendRead(id, c, offset, percent, -1, true, e);
        // Even an uncertain POST can have committed. Reconcile once with a GET, not another POST.
        if (FetchProgress(id, readback, e) && readback->has_offset && readback->chapter_uid == c.uid && readback->chapter_offset == offset) { clear(e); return Submission::Confirmed; }
        fail(e, ErrorCode::Uncertain, "云端位置尚未确认，请重新检查远端位置"); return Submission::Unknown;
    } catch (const std::bad_alloc&) { fail(e, started ? ErrorCode::Uncertain : ErrorCode::NoMemory, started ? "内存不足，位置提交结果未知" : "运行内存不足"); }
    catch (...) { fail(e, started ? ErrorCode::Uncertain : ErrorCode::Malformed, "位置同步未完成"); }
    return started ? Submission::Unknown : Submission::Rejected;
}
Submission Client::SubmitReadingTime(const std::string& id, const Chapter& c, uint32_t offset, int percent,
                                     uint32_t seconds, Error* e) noexcept {
    clear(e);
    if (!seconds || seconds > 300) { fail(e, ErrorCode::InvalidArgument, "每次仅支持提交 1 至 300 秒实际阅读时间"); return Submission::Rejected; }
    // No retry, including renewal: service must explicitly re-authenticate and reconcile uncertainty.
    return SendRead(id, c, offset, percent, static_cast<int>(seconds), true, e);
}
} // namespace folio::weread
