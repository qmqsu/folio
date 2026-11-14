#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int psa_status_t;
typedef uint32_t psa_algorithm_t;
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 0x02000009u
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(psa_algorithm_t,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
