#include "folio_font_pack.h"

#include <string.h>

#define PACK_IMAGE_SIZE UINT32_C(5242880)
#define PACK_HEADER_SIZE UINT32_C(4096)
#define PACK_ALIGNMENT UINT32_C(4096)
#define PACK_ENTRY_COUNT UINT32_C(4)
#define PACK_ENTRY_SIZE UINT32_C(128)
#define PACK_ENTRIES_OFFSET UINT32_C(64)
#define PACK_METADATA_OFFSET UINT32_C(4096)
#define PACK_PAYLOAD_OFFSET UINT32_C(49152)
#define PACK_GUARD_SIZE UINT32_C(65536)
#define PACK_PAYLOAD_LIMIT (PACK_IMAGE_SIZE - PACK_GUARD_SIZE)
#define PACK_RAW_LIMIT UINT32_C(6291456)

static const char pack_ids[PACK_ENTRY_COUNT][16] = {
    "hei", "kai", "yuan", "symbols"
};

/* Reflected IEEE polynomial 0xedb88320. Immutable for concurrent callers. */
static const uint32_t crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    while (length-- != 0) {
        crc = crc32_table[(crc ^ *bytes++) & UINT32_C(255)] ^ (crc >> 8);
    }
    return crc;
}

uint32_t folio_font_pack_crc32(const void *bytes, size_t length)
{
    if (bytes == NULL) {
        return 0;
    }
    return crc32_update(UINT32_MAX, bytes, length) ^ UINT32_MAX;
}

/* Only called on fixed fields after the complete image length is checked. */
static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool bytes_are(const uint8_t *bytes, size_t length, uint8_t value)
{
    while (length-- != 0) {
        if (*bytes++ != value) {
            return false;
        }
    }
    return true;
}

static bool metadata_is_utf8(const uint8_t *bytes, size_t length)
{
    size_t position = 0;
    while (position < length) {
        uint32_t codepoint = bytes[position++];
        uint32_t minimum;
        unsigned continuation;
        if (codepoint < 0x80) {
            continue;
        }
        if (codepoint >= 0xc2 && codepoint <= 0xdf) {
            codepoint &= 0x1f;
            minimum = 0x80;
            continuation = 1;
        } else if (codepoint >= 0xe0 && codepoint <= 0xef) {
            codepoint &= 0x0f;
            minimum = 0x800;
            continuation = 2;
        } else if (codepoint >= 0xf0 && codepoint <= 0xf4) {
            codepoint &= 0x07;
            minimum = 0x10000;
            continuation = 3;
        } else {
            return false;
        }
        if (continuation > length - position) {
            return false;
        }
        while (continuation-- != 0) {
            uint8_t next = bytes[position++];
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static uint32_t header_crc32(const uint8_t *bytes)
{
    static const uint8_t zero[4] = {0};
    uint32_t crc = crc32_update(UINT32_MAX, bytes, 44);
    crc = crc32_update(crc, zero, sizeof(zero));
    return crc32_update(crc, bytes + 48, PACK_HEADER_SIZE - 48) ^ UINT32_MAX;
}

bool folio_font_pack_find(const void *bytes, size_t length, const char *id,
                          folio_font_resource_t *out)
{
    if (out == NULL) {
        return false;
    }
    *out = (folio_font_resource_t){0};
    if (bytes == NULL || id == NULL || length != PACK_IMAGE_SIZE) {
        return false;
    }

    uint32_t selected = PACK_ENTRY_COUNT;
    for (uint32_t i = 0; i < PACK_ENTRY_COUNT; ++i) {
        if (strcmp(id, pack_ids[i]) == 0) {
            selected = i;
            break;
        }
    }
    if (selected == PACK_ENTRY_COUNT) {
        return false;
    }

    const uint8_t *image = bytes;
    if (memcmp(image, "FOLFONT1", 8) != 0 ||
        read_le32(image + 8) != 1 ||
        read_le32(image + 12) != PACK_HEADER_SIZE ||
        read_le32(image + 20) != PACK_ENTRY_COUNT ||
        read_le32(image + 24) != PACK_ENTRY_SIZE ||
        read_le32(image + 28) != PACK_ENTRIES_OFFSET ||
        read_le32(image + 32) != PACK_METADATA_OFFSET ||
        !bytes_are(image + 48, 16, 0) ||
        !bytes_are(image + PACK_ENTRIES_OFFSET + PACK_ENTRY_COUNT * PACK_ENTRY_SIZE,
                   PACK_HEADER_SIZE - PACK_ENTRIES_OFFSET -
                       PACK_ENTRY_COUNT * PACK_ENTRY_SIZE, 0) ||
        header_crc32(image) != read_le32(image + 44)) {
        return false;
    }

    uint32_t total_used = read_le32(image + 16);
    uint32_t metadata_length = read_le32(image + 36);
    if (total_used < PACK_PAYLOAD_OFFSET || total_used > PACK_PAYLOAD_LIMIT ||
        total_used % PACK_ALIGNMENT != 0 || metadata_length == 0 ||
        metadata_length > PACK_PAYLOAD_OFFSET - PACK_METADATA_OFFSET) {
        return false;
    }
    if (folio_font_pack_crc32(image + PACK_METADATA_OFFSET, metadata_length) !=
            read_le32(image + 40) ||
        !metadata_is_utf8(image + PACK_METADATA_OFFSET, metadata_length) ||
        !bytes_are(image + PACK_METADATA_OFFSET + metadata_length,
                   PACK_PAYLOAD_OFFSET - PACK_METADATA_OFFSET - metadata_length,
                   0xff) ||
        !bytes_are(image + PACK_PAYLOAD_LIMIT, PACK_GUARD_SIZE, 0xff)) {
        return false;
    }

    uint32_t starts[PACK_ENTRY_COUNT];
    uint32_t ends[PACK_ENTRY_COUNT];
    uint32_t last_end = 0;
    const uint8_t *selected_entry = NULL;
    for (uint32_t i = 0; i < PACK_ENTRY_COUNT; ++i) {
        const uint8_t *entry = image + PACK_ENTRIES_OFFSET + i * PACK_ENTRY_SIZE;
        uint32_t offset = read_le32(entry + 16);
        uint32_t compressed_size = read_le32(entry + 20);
        uint32_t raw_size = read_le32(entry + 24);
        if (memcmp(entry, pack_ids[i], sizeof(pack_ids[i])) != 0 ||
            read_le32(entry + 36) != 1 ||
            !bytes_are(entry + 104, 24, 0) ||
            offset < PACK_PAYLOAD_OFFSET || offset > total_used ||
            offset % PACK_ALIGNMENT != 0 || compressed_size == 0 ||
            compressed_size > total_used - offset ||
            raw_size < 12 || raw_size > PACK_RAW_LIMIT) {
            return false;
        }
        /* Subtraction above proves this addition cannot overflow. */
        uint32_t end = offset + compressed_size;
        for (uint32_t j = 0; j < i; ++j) {
            if (offset < ends[j] && starts[j] < end) {
                return false;
            }
        }
        starts[i] = offset;
        ends[i] = end;
        if (end > last_end) {
            last_end = end;
        }
        if (i == selected) {
            selected_entry = entry;
        }
    }
    /* last_end <= total_used <= PACK_PAYLOAD_LIMIT, so rounding is bounded. */
    if ((last_end + PACK_ALIGNMENT - 1) / PACK_ALIGNMENT * PACK_ALIGNMENT !=
        total_used) {
        return false;
    }

    const uint8_t *compressed = image + starts[selected];
    uint32_t compressed_size = ends[selected] - starts[selected];
    uint32_t compressed_crc = read_le32(selected_entry + 28);
    if (folio_font_pack_crc32(compressed, compressed_size) != compressed_crc) {
        return false;
    }
    *out = (folio_font_resource_t){
        .compressed = compressed,
        .compressed_size = compressed_size,
        .raw_size = read_le32(selected_entry + 24),
        .compressed_crc32 = compressed_crc,
        .raw_crc32 = read_le32(selected_entry + 32),
    };
    return true;
}
