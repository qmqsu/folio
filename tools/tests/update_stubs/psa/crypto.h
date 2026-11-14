#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX host_sha_context;
#define host_sha_init(c) CC_SHA256_Init(c)
#define host_sha_update(c,p,n) CC_SHA256_Update(c,p,(CC_LONG)(n))
#define host_sha_finish(out,c) CC_SHA256_Final(out,c)
#else
#include <openssl/sha.h>
typedef SHA256_CTX host_sha_context;
#define host_sha_init(c) SHA256_Init(c)
#define host_sha_update(c,p,n) SHA256_Update(c,p,n)
#define host_sha_finish(out,c) SHA256_Final(out,c)
#endif
typedef int psa_status_t;
typedef struct { host_sha_context context; bool active; } psa_hash_operation_t;
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 0x02000009u
#define PSA_HASH_OPERATION_INIT {0}
extern bool fail_crypto_init, fail_hash_setup, fail_hash_finish;
static inline psa_status_t psa_crypto_init(void) { return fail_crypto_init ? -1 : 0; }
static inline psa_status_t psa_hash_setup(psa_hash_operation_t* operation, unsigned algorithm) {
    if (fail_hash_setup || algorithm != PSA_ALG_SHA_256) return -1;
    operation->active = true; return host_sha_init(&operation->context) == 1 ? 0 : -1;
}
static inline psa_status_t psa_hash_update(psa_hash_operation_t* operation, const uint8_t* bytes, size_t size) {
    return operation->active && host_sha_update(&operation->context, bytes, size) == 1 ? 0 : -1;
}
static inline psa_status_t psa_hash_finish(psa_hash_operation_t* operation, uint8_t* out, size_t capacity, size_t* size) {
    if (!operation->active || fail_hash_finish || capacity < 32) return -1;
    operation->active = false; *size = 32; return host_sha_finish(out, &operation->context) == 1 ? 0 : -1;
}
static inline psa_status_t psa_hash_abort(psa_hash_operation_t* operation) { memset(operation, 0, sizeof(*operation)); return 0; }
