#pragma once
// Unofficial Web protocol client. No persistent credentials or background retries.
// Derived in part from danleegle/wxBook (MIT); see LICENSE.wxBook.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace folio::weread {
constexpr size_t kMaxChapterBytes = 512 * 1024;
constexpr size_t kMaxShelfBooks = 10000;
constexpr size_t kMaxShelfBytes = 32 * 1024 * 1024;
constexpr size_t kMaxChapters = 512;

enum class ErrorCode : uint8_t {
    None, InvalidArgument, NotLoggedIn, SessionExpired, Cancelled, Timeout,
    Transport, Http, Permission, Unsupported, Limit, Malformed, NoMemory,
    Clock, Conflict, Uncertain, NameResolution, Connection, Certificate, Storage
};
struct Error {
    ErrorCode code = ErrorCode::None;
    int http_status = 0;
    int server_code = 0;
    char message[160] = {};
};
struct CancelCheck {
    bool (*fn)(void*) = nullptr;
    void* context = nullptr;
    bool operator()() const { return fn && fn(context); }
};
struct Progress {
    std::string book_id, chapter_uid;
    uint32_t chapter_offset = 0; // WeRead source UTF-16 offset, NOT cache byte offset.
    uint32_t update_time = 0;
    uint32_t reading_seconds = 0; // Cloud book total; never a local active delta.
    int percent = 0;
    bool has_offset = false;
    bool has_reading_time = false; // Missing fields are not a zero-second total.
};
struct Book {
    std::string id, title, author, cover; // cover is a URL; never fetched with session Cookie.
};
struct ShelfSink {
    virtual bool append(const Book&) = 0;
protected:
    ~ShelfSink() = default;
};
class ShelfStream;
struct Chapter {
    std::string uid, title;
    uint32_t index = 0, word_count = 0;
};
struct ChapterContent {
    std::string bytes; // Decoded original UTF-8; preserve markup/entities/whitespace.
    bool xhtml = false;
    // XHTML positions require a source UTF-16 mapping. TXT is also source UTF-16;
    // cache must preserve original text/newlines to claim exact synchronization.
};
struct QrSession { std::string uid, url; };
enum class QrState : uint8_t { Pending, Complete, NeedOtp, OtpMismatch, Expired };
enum class Submission : uint8_t {
    Confirmed, // Explicit server acknowledgement; position also requires GET readback.
    Rejected,  // Definitely not sent, or explicit rejection. No automatic retry.
    Unknown    // May have been applied. NEVER blindly retry reading-time submissions.
};

// Public transport seam for deterministic host tests, not a generic HTTP API.
// ESP transport permits exactly https://weread.qq.com and never follows redirects.
struct HttpRequest {
    std::string method, url, body, referer, cookie;
    size_t max_response = kMaxChapterBytes;
    ErrorCode (*consume)(void*, const char*, size_t) = nullptr;
    void* consume_context = nullptr;
    bool reader_tokens = false; // bounded streaming HTML probe, at most 4 MiB scanned
    uint32_t timeout_ms = 15000; // Total deadline; body reads check cancellation at least every 3 s.
    ~HttpRequest();
};
enum class RequestPhase : uint8_t { Setup, Connect, Send, Headers, Body, Complete };
struct HttpResponse {
    HttpResponse() = default;
    HttpResponse(HttpResponse&&) noexcept = default;
    HttpResponse& operator=(HttpResponse&&) noexcept = default;
    HttpResponse(const HttpResponse&) = delete;
    int status = 0;
    ErrorCode failure = ErrorCode::None;
    RequestPhase phase = RequestPhase::Setup;
    size_t received_bytes = 0;
    bool sent = false; // conservatively true once a request may have reached the peer
    std::string body;
    std::vector<std::string> set_cookies;
    ~HttpResponse();
};
using Transport = void (*)(const HttpRequest&, HttpResponse*, CancelCheck, void*);
void EspTransport(const HttpRequest&, HttpResponse*, CancelCheck, void*);
void EspTransportDisconnect() noexcept;
// Internal bounded HTML token probe, shared by the ESP transport and host tests.
bool ExtractReaderField(const std::string& window, const char* key, std::string* value);

// Versioned persistence envelope lives in the device store, not this protocol type.
struct Session {
    char vid[64] = {}, skey[512] = {}, gid[512] = {}, rt[1024] = {};
    ~Session();
    bool Valid() const noexcept;
};

// Single-owner, synchronous, NOT thread-safe. Service serializes all calls.
// All public operations catch allocation failures. Build with C++ exceptions enabled.
class Client {
public:
    explicit Client(Transport transport = EspTransport, void* context = nullptr) noexcept;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void SetCancelCheck(CancelCheck check) noexcept { cancelled_ = check; }
    bool LoggedIn() const noexcept;
    const char* AccountId() const noexcept { return auth_.vid; }
    bool ExportSession(Session* out) const noexcept;
    bool RestoreSession(const Session& saved) noexcept;
    void Logout() noexcept; // local logout + best-effort memory wipe, no network request
    void Disconnect() noexcept; // closes ESP keep-alive and clears reader tokens
    bool BeginQrLogin(QrSession*, Error*) noexcept; // clears previous account first
    bool PollQrLogin(const QrSession&, const std::string& otp, QrState*, Error*) noexcept;
    bool RenewSession(Error*) noexcept;
    bool FetchShelf(ShelfSink&, std::vector<uint16_t>* order, Error*) noexcept;
    bool FetchChapters(const std::string& book_id, std::vector<Chapter>*, Error*) noexcept;
    bool FetchChapter(const std::string& book_id, const Chapter&, ChapterContent*, Error*) noexcept;
    bool FetchProgress(const std::string& book_id, Progress*, Error*) noexcept;
    Submission WritePosition(const std::string& book_id, const Chapter&, uint32_t utf16_offset,
                             int percent, Progress* readback, Error*) noexcept;
    // One explicit upload of caller-measured foreground seconds (1..300), bound to
    // caller-confirmed book/position. This endpoint also sends that position. No auto retry.
    Submission SubmitReadingTime(const std::string& book_id, const Chapter&, uint32_t utf16_offset,
                                 int percent, uint32_t seconds, Error*) noexcept;
private:
    using Auth = Session;
    Auth auth_;
    Transport transport_;
    void* transport_context_;
    CancelCheck cancelled_;
    std::string token_book_, psvts_, pclts_, reader_token_;
    uint32_t token_acquired_at_ = 0;
    HttpResponse Request(const char* method, const std::string& path, const std::string& body,
                         const std::string& referer, Error*, bool reader_tokens = false,
                         uint32_t budget_ms = 60000, ShelfStream* shelf = nullptr);
    bool Ready(const std::string& book_id, Error*);
    bool AbsorbCookies(const HttpResponse&, Error*);
    bool ReaderTokens(const std::string& book_id, const std::string& chapter_uid,
                      std::string* referer, Error*, uint32_t budget_ms = 60000);
    Submission SendRead(const std::string& book_id, const Chapter&, uint32_t offset,
                        int percent, int seconds, bool report, Error*);
};
} // namespace folio::weread
