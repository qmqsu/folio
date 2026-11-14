// Adapted from danleegle/wxBook (MIT). See LICENSE.wxBook.
#include "folio_weread_crypto.h"

#ifdef FOLIO_WEREAD_HOST_TEST
#include <CommonCrypto/CommonDigest.h>
#else
#include <psa/crypto.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace folio::weread::crypto {
namespace {

std::string Hex(const unsigned char* data, size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (size_t i = 0; i < length; ++i) {
        result[i * 2] = kHex[data[i] >> 4];
        result[i * 2 + 1] = kHex[data[i] & 15];
    }
    return result;
}

std::string Md5(const void* data, size_t length) {
    unsigned char digest[16] = {};
    #ifdef FOLIO_WEREAD_HOST_TEST
    CC_MD5(data, static_cast<CC_LONG>(length), digest);
#else
    size_t written = 0;
    if (psa_hash_compute(PSA_ALG_MD5, static_cast<const uint8_t*>(data), length, digest, sizeof(digest), &written) != PSA_SUCCESS || written != sizeof(digest)) throw std::runtime_error("hash");
#endif
    return Hex(digest, sizeof(digest));
}

std::string Md5(const std::string& input) { return Md5(input.data(), input.size()); }

std::string UrlEncode(const std::string& input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output.push_back(static_cast<char>(c));
        } else {
            output.push_back('%');
            output.push_back(kHex[c >> 4]);
            output.push_back(kHex[c & 15]);
        }
    }
    return output;
}

std::vector<int> SwapPositions(const std::string& encrypted) {
    std::vector<int> positions;
    const int length = static_cast<int>(encrypted.size());
    if (length < 4) return positions;
    if (length < 11) return {0, 2};
    const int tail = std::min(4, (length + 9) / 10);
    std::string digits;
    for (int i = length - 1; i >= length - tail; --i) {
        const uint8_t value = static_cast<uint8_t>(encrypted[i]);
        uint32_t expanded = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((value >> bit) & 1U) expanded += 1U << (bit * 2);
        }
        digits += std::to_string(expanded);
    }
    const int modulus = length - tail - 2;
    if (modulus <= 0) return positions;
    const int width = static_cast<int>(std::to_string(modulus).size());
    for (int i = 0; positions.size() < 10 && i + width < static_cast<int>(digits.size()); i += width) {
        positions.push_back(std::stoi(digits.substr(i, width)) % modulus);
        positions.push_back(std::stoi(digits.substr(i + 1, width)) % modulus);
    }
    return positions;
}

}  // namespace

std::string EncodeId(const std::string& value) {
    const std::string digest = Md5(value);
    const bool numeric = !value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    std::vector<std::string> chunks;
    if (numeric) {
        for (size_t i = 0; i < value.size(); i += 9) {
            const auto number = std::stoull(value.substr(i, 9));
            char buffer[24] = {};
            std::snprintf(buffer, sizeof(buffer), "%llx", number);
            chunks.emplace_back(buffer);
        }
    } else {
        std::string bytes;
        char buffer[4] = {};
        for (unsigned char c : value) {
            std::snprintf(buffer, sizeof(buffer), "%x", c);
            bytes += buffer;
        }
        chunks.push_back(std::move(bytes));
    }

    std::string result = digest.substr(0, 3) + (numeric ? "3" : "4") + "2" + digest.substr(30);
    for (size_t i = 0; i < chunks.size(); ++i) {
        static constexpr char digits[] = "0123456789abcdef";
        const size_t length = chunks[i].size();
        if (length > 255) throw std::length_error("encoded id");
        result += digits[length >> 4];
        result += digits[length & 15];
        result += chunks[i];
        if (i + 1 < chunks.size()) result += 'g';
    }
    if (result.size() < 20) result += digest.substr(0, 20 - result.size());
    return result + Md5(result).substr(0, 3);
}

std::string Sign(const std::string& query) {
    int64_t left = 0x15051505;
    int64_t right = left;
    int i = static_cast<int>(query.size());
    while (i > 1) {
        left = (left ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 1])) <<
                ((query.size() - i + 1) % 30))) & 0x7fffffff;
        right = (right ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 2])) <<
                 ((i - 1) % 30))) & 0x7fffffff;
        i -= 2;
    }
    char result[24] = {};
    std::snprintf(result, sizeof(result), "%llx", static_cast<unsigned long long>(left + right));
    return result;
}

std::string SortedQuery(const std::vector<std::pair<std::string, std::string>>& params) {
    auto sorted = params;
    std::sort(sorted.begin(), sorted.end());
    std::string query;
    for (const auto& [key, value] : sorted) {
        if (key == "s") continue;
        if (!query.empty()) query += '&';
        query += key + "=" + UrlEncode(value);
    }
    return query;
}

std::vector<std::pair<std::string, std::string>> ContentParams(
        const std::string& book_id, const std::string& chapter_uid,
        const std::string& psvts, uint32_t timestamp, uint32_t entropy) {
    if (EncodeId(std::to_string(timestamp)) == psvts) ++timestamp;
    const uint32_t random = entropy % 10000;
    std::vector<std::pair<std::string, std::string>> params = {
        {"b", EncodeId(book_id)}, {"c", EncodeId(chapter_uid)},
        {"r", std::to_string(random * random)}, {"ct", std::to_string(timestamp)},
        {"ps", psvts}, {"pc", EncodeId(std::to_string(timestamp))}, {"sc", "1"},
        {"prevChapter", "false"}, {"st", "0"},
    };
    params.emplace_back("s", Sign(SortedQuery(params)));
    return params;
}

// Unlike the original, every shard must validate; partial corrupted text is never returned.
bool DecodeShards(std::vector<std::string>* shards, std::string* output, size_t limit) {
    if (!shards || !output || shards->empty()) return false;
    output->clear();
    const size_t encoded_limit = (limit + 2) / 3 * 4 + 16;
    size_t total = 0;
    for (const auto& shard : *shards) {
        if (shard.size() < 32 || shard.size() - 32 > encoded_limit - total) return false;
        std::string digest = Md5(shard.data() + 32, shard.size() - 32);
        for (size_t i = 0; i < 32; ++i)
            if (std::tolower(static_cast<unsigned char>(shard[i])) != digest[i]) return false;
        total += shard.size() - 32;
    }
    if (total < 2) return false;
    std::string encoded;
    encoded.reserve(total);
    for (auto& shard : *shards) { encoded.append(shard, 32, std::string::npos); std::string().swap(shard); }
    // Protocol prefix precedes permuted Base64.
    encoded.erase(0, 1);
    const auto positions = SwapPositions(encoded);
    for (int i = static_cast<int>(positions.size()) - 1; i > 0; i -= 2) {
        for (int delta = 1; delta >= 0; --delta) {
            size_t a = static_cast<size_t>(positions[i] + delta);
            size_t b = static_cast<size_t>(positions[i - 1] + delta);
            if (a >= encoded.size() || b >= encoded.size()) return false;
            std::swap(encoded[a], encoded[b]);
        }
    }
    // Decode in one pass, rejecting unexpected characters instead of silently dropping them.
    output->reserve(std::min(limit, encoded.size() / 4 * 3 + 3));
    uint32_t acc = 0; unsigned bits = 0; bool padding = false; unsigned pads = 0;
    size_t digits = 0;
    for (unsigned char c : encoded) {
        if (c == '=') { padding = true; if (++pads > 2) return false; continue; }
        if (padding) return false;
        unsigned value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+' || c == '-') value = 62;
        else if (c == '/' || c == '_') value = 63;
        else return false;
        ++digits; acc = (acc << 6) | value; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output->size() >= limit) return false;
            output->push_back(static_cast<char>((acc >> bits) & 255));
        }
    }
    if (digits % 4 == 1 || (pads && (digits + pads) % 4) ||
        (bits && (acc & ((1u << bits) - 1u)))) { output->clear(); return false; }
    return !output->empty();
}

std::string Sha256Hex(const std::string& input) {
    unsigned char digest[32] = {};
    #ifdef FOLIO_WEREAD_HOST_TEST
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), digest);
#else
    size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest, sizeof(digest), &written) != PSA_SUCCESS || written != sizeof(digest)) throw std::runtime_error("hash");
#endif
    return Hex(digest, sizeof(digest));
}

}  // namespace folio::weread::crypto
