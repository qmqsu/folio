#include "folio_remote_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psa/crypto.h"
#ifndef FOLIO_RELEASE_KEY_HEADER
#define FOLIO_RELEASE_KEY_HEADER "folio_release_key.h"
#endif
#include FOLIO_RELEASE_KEY_HEADER
_Static_assert(sizeof(folio_release_public_key) == 65, "P256 SEC1 public key size");

// This deliberately accepts only the frozen schema, not arbitrary JSON trees.
// No recursive descent into unrecognized objects and no floating-point parser.
typedef struct { const uint8_t* p; const uint8_t* end; } json_t;
static void message(char* out, size_t cap, const char* text) {
    if (out && cap) snprintf(out, cap, "%s", text);
}
static void space(json_t* j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) ++j->p;
}
static bool take(json_t* j, uint8_t c) {
    space(j); if (j->p == j->end || *j->p != c) return false;
    ++j->p; return true;
}
static int hex(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool utf8(const uint8_t* s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint32_t c = s[i++], minimum = 0; unsigned more = 0;
        if (c < 0x80) { if (!c) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { more = 1; c &= 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { more = 2; c &= 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { more = 3; c &= 7; minimum = 0x10000; }
        else return false;
        if (n - i < more) return false;
        while (more--) { if ((s[i] & 0xc0) != 0x80) return false; c = (c << 6) | (s[i++] & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
static bool unicode4(json_t* j, uint32_t* value) {
    if ((size_t)(j->end - j->p) < 4) return false;
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) { int h = hex(*j->p++); if (h < 0) return false; *value = (*value << 4) | (unsigned)h; }
    return true;
}
static bool string(json_t* j, char* out, size_t cap) {
    if (!cap || !take(j, '"')) return false;
    size_t n = 0;
    while (j->p < j->end) {
        uint32_t c = *j->p++;
        if (c == '"') { out[n] = 0; return utf8((const uint8_t*)out, n); }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (j->p == j->end) return false;
            c = *j->p++;
            if (c == 'b') c = '\b'; else if (c == 'f') c = '\f';
            else if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c == 'u') {
                if (!unicode4(j, &c) || !c) return false;
                if (c >= 0xd800 && c <= 0xdbff) {
                    uint32_t low;
                    if (j->end - j->p < 2 || *j->p++ != '\\' || *j->p++ != 'u' ||
                        !unicode4(j, &low) || low < 0xdc00 || low > 0xdfff) return false;
                    c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff) return false;
                uint8_t b[4]; size_t count;
                if (c < 0x80) { b[0] = (uint8_t)c; count = 1; }
                else if (c < 0x800) { b[0] = 0xc0 | (c >> 6); b[1] = 0x80 | (c & 63); count = 2; }
                else if (c < 0x10000) { b[0] = 0xe0 | (c >> 12); b[1] = 0x80 | ((c >> 6) & 63); b[2] = 0x80 | (c & 63); count = 3; }
                else { b[0] = 0xf0 | (c >> 18); b[1] = 0x80 | ((c >> 12) & 63); b[2] = 0x80 | ((c >> 6) & 63); b[3] = 0x80 | (c & 63); count = 4; }
                if (count >= cap - n) return false;
                memcpy(out + n, b, count); n += count; continue;
            } else if (c != '"' && c != '\\' && c != '/') return false;
        }
        if (n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    return false;
}
static bool integer(json_t* j, uint32_t* out) {
    space(j); if (j->p == j->end || *j->p < '0' || *j->p > '9') return false;
    bool zero = *j->p == '0'; uint32_t value = *j->p++ - '0';
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        unsigned digit = *j->p++ - '0';
        if (zero || value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value; return true;
}
// Return 1 for another field, 0 for object end, -1 for syntax errors.
static int field(json_t* j, bool* first, char key[24]) {
    space(j);
    if (j->p < j->end && *j->p == '}') { ++j->p; return 0; }
    if (!*first && !take(j, ',')) return -1;
    *first = false;
    return string(j, key, 24) && take(j, ':') ? 1 : -1;
}
static bool unique(unsigned* mask, unsigned bit) {
    if (*mask & bit) return false;
    *mask |= bit; return true;
}
static int base64_digit(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static bool base64(const char* text, uint8_t* out, size_t cap, size_t* length) {
    size_t n = strlen(text), used = 0;
    if (!n || (n & 3)) return false;
    for (size_t i = 0; i < n; i += 4) {
        int a = base64_digit(text[i]), b = base64_digit(text[i + 1]);
        int c = base64_digit(text[i + 2]), d = base64_digit(text[i + 3]);
        bool pad2 = text[i + 2] == '=', pad1 = text[i + 3] == '=';
        if (a < 0 || b < 0 || (!pad2 && c < 0) || (!pad1 && d < 0) ||
            (pad2 && !pad1) || ((pad1 || pad2) && i + 4 != n) ||
            (pad2 && (b & 15)) || (!pad2 && pad1 && (c & 3))) return false;
        size_t bytes = pad2 ? 1 : pad1 ? 2 : 3;
        if (bytes > cap - used) return false;
        out[used++] = (uint8_t)((a << 2) | (b >> 4));
        if (bytes > 1) out[used++] = (uint8_t)((b << 4) | (c >> 2));
        if (bytes > 2) out[used++] = (uint8_t)((c << 6) | d);
    }
    *length = used; return true;
}
const char* folio_remote_manifest_url(const char* channel) {
    if (channel && !strcmp(channel, "stable")) return FOLIO_REMOTE_ORIGIN "channels/stable.json";
    if (channel && !strcmp(channel, "preview")) return FOLIO_REMOTE_ORIGIN "channels/preview.json";
    return NULL;
}
static bool image(json_t* j, folio_remote_manifest_t* out) {
    if (!take(j, '{')) return false;
    bool first = true; unsigned mask = 0; char key[24], digest[65]; int next;
    while ((next = field(j, &first, key)) > 0) {
        if (!strcmp(key, "url")) { if (!unique(&mask, 1) || !string(j, out->url, sizeof(out->url))) return false; }
        else if (!strcmp(key, "size")) { if (!unique(&mask, 2) || !integer(j, &out->image.size)) return false; }
        else if (!strcmp(key, "sha256")) { if (!unique(&mask, 4) || !string(j, digest, sizeof(digest))) return false; }
        else return false;
    }
    if (next || mask != 7 || out->image.size < 288 || out->image.size > FOLIO_UPDATE_MAX_SIZE || strlen(digest) != 64) return false;
    for (unsigned i = 0; i < 64; ++i) if (!(digest[i] >= '0' && digest[i] <= '9') && !(digest[i] >= 'a' && digest[i] <= 'f')) return false;
    for (unsigned i = 0; i < 32; ++i) out->image.sha256[i] = (uint8_t)((hex(digest[i * 2]) << 4) | hex(digest[i * 2 + 1]));
    return true;
}
static bool literal(json_t* j, const char* expected) {
    char value[48]; return string(j, value, sizeof(value)) && !strcmp(value, expected);
}
static bool payload(const uint8_t* bytes, size_t length, folio_remote_manifest_t* out) {
    json_t j = {bytes, bytes + length};
    if (!take(&j, '{')) return false;
    bool first = true; unsigned mask = 0; int next; char key[24]; uint32_t schema;
    while ((next = field(&j, &first, key)) > 0) {
        if (!strcmp(key, "schema")) { if (!unique(&mask, 1) || !integer(&j, &schema) || schema != 1) return false; }
        else if (!strcmp(key, "kind")) { if (!unique(&mask, 2) || !literal(&j, "ota")) return false; }
        else if (!strcmp(key, "project")) { if (!unique(&mask, 4) || !literal(&j, "Folio")) return false; }
        else if (!strcmp(key, "chip")) { if (!unique(&mask, 8) || !literal(&j, "esp32s3")) return false; }
        else if (!strcmp(key, "hardware")) { if (!unique(&mask, 16) || !literal(&j, FOLIO_REMOTE_HARDWARE)) return false; }
        else if (!strcmp(key, "layout")) { if (!unique(&mask, 32) || !literal(&j, FOLIO_REMOTE_LAYOUT)) return false; }
        else if (!strcmp(key, "channel")) { if (!unique(&mask, 64) || !string(&j, out->channel, sizeof(out->channel))) return false; }
        else if (!strcmp(key, "version")) { if (!unique(&mask, 128) || !string(&j, out->image.version, sizeof(out->image.version))) return false; }
        else if (!strcmp(key, "sequence")) { if (!unique(&mask, 256) || !integer(&j, &out->sequence)) return false; }
        else if (!strcmp(key, "image")) { if (!unique(&mask, 512) || !image(&j, out)) return false; }
        else if (!strcmp(key, "notes")) { if (!unique(&mask, 1024) || !string(&j, out->notes, sizeof(out->notes))) return false; }
        else return false;
    }
    space(&j);
    if (next || (mask & 1023) != 1023 || j.p != j.end) return false;
    const char* version = out->image.version;
    if (!*version || *version == '.' || *version == '-' || *version == '_') return false;
    for (const char* p = version; *p; ++p) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_')) return false;
    char expected[192];
    int n = snprintf(expected, sizeof(expected), FOLIO_REMOTE_ORIGIN "firmware/%s/folio.bin", version);
    return n > 0 && (size_t)n < sizeof(expected) && !strcmp(expected, out->url);
}
static bool verify_signature(const uint8_t* bytes, size_t length, const uint8_t signature[64]) {
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    uint8_t digest[32]; size_t digest_len = 0;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&hash, bytes, length) != PSA_SUCCESS ||
        psa_hash_finish(&hash, digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != 32) {
        (void)psa_hash_abort(&hash); return false;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t key = 0;
    psa_status_t result = psa_import_key(&attributes, folio_release_public_key, sizeof(folio_release_public_key), &key);
    psa_reset_key_attributes(&attributes);
    if (result == PSA_SUCCESS) result = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, sizeof(digest), signature, 64);
    if (key) (void)psa_destroy_key(key);
    (void)psa_hash_abort(&hash);
    return result == PSA_SUCCESS;
}
bool folio_remote_manifest_verify(const uint8_t* envelope, size_t length, const char* channel,
    uint32_t current_sequence, folio_remote_manifest_t* out, char* error, size_t cap) {
    if (out) memset(out, 0, sizeof(*out));
    message(error, cap, "发布清单格式无效。");
    if (!out || !envelope || !length || length > FOLIO_REMOTE_MANIFEST_MAX || !folio_remote_manifest_url(channel)) return false;
    typedef struct { char encoded[FOLIO_REMOTE_MANIFEST_MAX + 1]; uint8_t decoded[3072]; } scratch_t;
    scratch_t* scratch = malloc(sizeof(*scratch));
    if (!scratch) { message(error, cap, "验证发布清单内存不足。"); return false; }
    char key_id[32] = {0}, signature_text[89] = {0}, key[24];
    json_t j = {envelope, envelope + length}; bool okay = take(&j, '{'), first = true;
    unsigned mask = 0; int next = -1;
    while (okay && (next = field(&j, &first, key)) > 0) {
        if (!strcmp(key, "key_id")) okay = unique(&mask, 1) && string(&j, key_id, sizeof(key_id));
        else if (!strcmp(key, "payload")) okay = unique(&mask, 2) && string(&j, scratch->encoded, sizeof(scratch->encoded));
        else if (!strcmp(key, "signature")) okay = unique(&mask, 4) && string(&j, signature_text, sizeof(signature_text));
        else okay = false;
    }
    space(&j); okay = okay && !next && mask == 7 && j.p == j.end;
    if (okay && strcmp(key_id, FOLIO_RELEASE_KEY_ID)) { message(error, cap, "发布签名密钥身份未知。"); okay = false; }
    uint8_t signature[64]; size_t payload_len = 0, signature_len = 0;
    if (okay) okay = base64(scratch->encoded, scratch->decoded, sizeof(scratch->decoded), &payload_len) &&
        base64(signature_text, signature, sizeof(signature), &signature_len) && signature_len == 64;
    if (okay && !verify_signature(scratch->decoded, payload_len, signature)) {
        message(error, cap, "发布者签名验证失败，更新已拒绝。"); okay = false;
    }
    if (okay && !payload(scratch->decoded, payload_len, out)) {
        message(error, cap, "签名清单的格式、硬件、布局或固件路径不匹配。"); okay = false;
    }
    if (okay && strcmp(channel, out->channel)) { message(error, cap, "发布通道不匹配。"); okay = false; }
    if (okay && out->sequence <= current_sequence) { message(error, cap, "发布序号不高于当前固件，已拒绝旧版本。"); okay = false; }
    free(scratch);
    if (!okay) memset(out, 0, sizeof(*out)); else message(error, cap, "");
    return okay;
}
