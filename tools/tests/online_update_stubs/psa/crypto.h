#pragma once
#include "../../update_stubs/psa/crypto.h"
#include <stdlib.h>
#include <assert.h>
#ifdef __APPLE__
#include <Security/Security.h>
#else
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#endif
// Real host crypto adapter, NOT an accept-all verifier. PSA call shape is shared
// with production; ESP-IDF compilation additionally checks the real PSA API.
typedef uintptr_t psa_key_id_t;
typedef struct { unsigned bits, type, usage, alg; } psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_ECC_FAMILY_SECP_R1 1u
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(f) (0x4000u | (f))
#define PSA_KEY_USAGE_VERIFY_HASH 0x01u
#define PSA_ALG_ECDSA(a) (0x06000600u | ((a) & 0xff))
static inline void psa_set_key_type(psa_key_attributes_t* a, unsigned x) { a->type = x; }
static inline void psa_set_key_bits(psa_key_attributes_t* a, unsigned x) { a->bits = x; }
static inline void psa_set_key_usage_flags(psa_key_attributes_t* a, unsigned x) { a->usage = x; }
static inline void psa_set_key_algorithm(psa_key_attributes_t* a, unsigned x) { a->alg = x; }
static inline void psa_reset_key_attributes(psa_key_attributes_t* a) { memset(a, 0, sizeof(*a)); }
static inline psa_status_t psa_import_key(const psa_key_attributes_t* a, const uint8_t* p, size_t n, psa_key_id_t* id) {
    if (a->type != PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1) || a->bits != 256 ||
        a->usage != PSA_KEY_USAGE_VERIFY_HASH || a->alg != PSA_ALG_ECDSA(PSA_ALG_SHA_256) || n != 65 || *p != 4) return -1;
#ifdef __APPLE__
    int bits = 256;
    CFNumberRef size = CFNumberCreate(NULL, kCFNumberIntType, &bits);
    const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits};
    const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic, size};
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef data = CFDataCreate(NULL, p, (CFIndex)n);
    SecKeyRef key = SecKeyCreateWithData(data, attributes, NULL);
    CFRelease(data); CFRelease(attributes); CFRelease(size);
    *id = (uintptr_t)key;
#else
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    const unsigned char* cursor = p;
    if (!key || !o2i_ECPublicKey(&key, &cursor, (long)n)) { if (key) EC_KEY_free(key); return -1; }
    *id = (uintptr_t)key;
#endif
    return *id ? PSA_SUCCESS : -1;
}
static inline psa_status_t psa_verify_hash(psa_key_id_t id, unsigned alg, const uint8_t* hash, size_t hn, const uint8_t* signature, size_t sn) {
    if (!id || alg != PSA_ALG_ECDSA(PSA_ALG_SHA_256) || hn != 32 || sn != 64) return -1;
#ifdef __APPLE__
    uint8_t der[72]; size_t used = 2; der[0] = 0x30;
    for (unsigned half = 0; half < 2; ++half) {
        const uint8_t* p = signature + half * 32; size_t n = 32;
        while (n > 1 && !*p) { ++p; --n; }
        der[used++] = 2; der[used++] = (uint8_t)(n + ((*p & 0x80) ? 1 : 0));
        if (*p & 0x80) der[used++] = 0;
        memcpy(der + used, p, n); used += n;
    }
    der[1] = (uint8_t)(used - 2);
    CFDataRef digest = CFDataCreate(NULL, hash, (CFIndex)hn), sig = CFDataCreate(NULL, der, (CFIndex)used);
    bool good = SecKeyVerifySignature((SecKeyRef)id, kSecKeyAlgorithmECDSASignatureDigestX962SHA256, digest, sig, NULL);
    CFRelease(digest); CFRelease(sig); return good ? 0 : -1;
#else
    ECDSA_SIG* sig = ECDSA_SIG_new();
    ECDSA_SIG_set0(sig, BN_bin2bn(signature, 32, NULL), BN_bin2bn(signature + 32, 32, NULL));
    int result = ECDSA_do_verify(hash, (int)hn, sig, (EC_KEY*)id); ECDSA_SIG_free(sig); return result == 1 ? 0 : -1;
#endif
}
static inline psa_status_t psa_destroy_key(psa_key_id_t id) {
#ifdef __APPLE__
    if (id) CFRelease((SecKeyRef)id);
#else
    if (id) EC_KEY_free((EC_KEY*)id);
#endif
    return PSA_SUCCESS;
}
