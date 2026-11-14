#include "folio_font_pack.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define IMAGE_SIZE UINT32_C(5242880)
#define HEADER_SIZE UINT32_C(4096)
#define ALIGNMENT UINT32_C(4096)
#define METADATA_SIZE UINT32_C(45056)
#define PAYLOAD_START UINT32_C(49152)
#define GUARD_START (IMAGE_SIZE - UINT32_C(65536))
#define RAW_LIMIT UINT32_C(6291456)
#define ENTRY(i) (UINT32_C(64) + (uint32_t)(i) * UINT32_C(128))

static unsigned checks;
static const char *const ids[] = {"hei", "kai", "yuan", "symbols"};

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t get32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = (uint8_t)(value >> (i * 8));
    }
}

static uint32_t reference_crc(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
        }
    }
    return crc ^ UINT32_MAX;
}

static uint32_t zlib_crc(const uint8_t *bytes, size_t length)
{
    CHECK(length <= UINT32_MAX);
    return (uint32_t)crc32(0, bytes, (uInt)length);
}

static void seal_header(uint8_t *image)
{
    put32(image + 44, 0);
    put32(image + 44, zlib_crc(image, HEADER_SIZE));
}

static void set_metadata(uint8_t *image, const uint8_t *bytes, size_t length)
{
    CHECK(length <= METADATA_SIZE);
    memset(image + HEADER_SIZE, 0xff, METADATA_SIZE);
    memcpy(image + HEADER_SIZE, bytes, length);
    put32(image + 36, (uint32_t)length);
    put32(image + 40, zlib_crc(image + HEADER_SIZE, length));
    seal_header(image);
}

static void make_image(uint8_t *image)
{
    static const uint8_t metadata[] = "{\"v\":1,\"name\":\"\xe6\xb1\x89\"}";
    memset(image, 0xff, IMAGE_SIZE);
    memset(image, 0, HEADER_SIZE);
    memcpy(image, "FOLFONT1", 8);
    put32(image + 8, 1);
    put32(image + 12, HEADER_SIZE);
    put32(image + 16, PAYLOAD_START + 7 * ALIGNMENT);
    put32(image + 20, 4);
    put32(image + 24, 128);
    put32(image + 28, 64);
    put32(image + 32, HEADER_SIZE);
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t *entry = image + ENTRY(i);
        uint32_t offset = PAYLOAD_START + i * 2 * ALIGNMENT;
        uint32_t raw_length = 12 + i * 19;
        uint8_t *payload = image + offset;
        memcpy(entry, ids[i], strlen(ids[i]));
        put32(entry + 16, offset);
        put32(entry + 20, raw_length + 5);
        put32(entry + 24, raw_length);
        put32(entry + 36, 1);
        /* One final, uncompressed raw-DEFLATE block. */
        payload[0] = 1;
        payload[1] = (uint8_t)raw_length;
        payload[2] = 0;
        payload[3] = (uint8_t)~raw_length;
        payload[4] = 0xff;
        for (uint32_t j = 0; j < raw_length; ++j) {
            payload[5 + j] = (uint8_t)(i * 41 + j);
        }
        put32(entry + 28, zlib_crc(payload, raw_length + 5));
        put32(entry + 32, zlib_crc(payload + 5, raw_length));
        /* SHA fields are opaque to the runtime parser, not authentication. */
        memset(entry + 40, (int)(i + 1), 64);
    }
    set_metadata(image, metadata, sizeof(metadata) - 1);
}

static bool resource_is_clear(const folio_font_resource_t *resource)
{
    return resource->compressed == NULL && resource->compressed_size == 0 &&
           resource->raw_size == 0 && resource->compressed_crc32 == 0 &&
           resource->raw_crc32 == 0;
}

static void reject(const void *image, size_t length, const char *id)
{
    folio_font_resource_t resource = {
        .compressed = (const uint8_t *)"sentinel",
        .compressed_size = SIZE_MAX,
        .raw_size = SIZE_MAX,
        .compressed_crc32 = UINT32_MAX,
        .raw_crc32 = UINT32_MAX,
    };
    CHECK(!folio_font_pack_find(image, length, id, &resource));
    CHECK(resource_is_clear(&resource));
}

static folio_font_resource_t accept(const uint8_t *image, unsigned index)
{
    folio_font_resource_t resource;
    const uint8_t *entry = image + ENTRY(index);
    CHECK(folio_font_pack_find(image, IMAGE_SIZE, ids[index], &resource));
    CHECK(resource.compressed == image + get32(entry + 16));
    CHECK(resource.compressed_size == get32(entry + 20));
    CHECK(resource.raw_size == get32(entry + 24));
    CHECK(resource.compressed_crc32 == get32(entry + 28));
    CHECK(resource.raw_crc32 == get32(entry + 32));
    CHECK(resource.compressed_crc32 == zlib_crc(resource.compressed,
                                               resource.compressed_size));
    return resource;
}

static void verify_inflate(const folio_font_resource_t *resource)
{
    uint8_t *raw = malloc(resource->raw_size);
    CHECK(raw != NULL);
    z_stream stream = {0};
    CHECK(inflateInit2(&stream, -MAX_WBITS) == Z_OK);
    stream.next_in = (Bytef *)resource->compressed;
    stream.avail_in = (uInt)resource->compressed_size;
    stream.next_out = raw;
    stream.avail_out = (uInt)resource->raw_size;
    CHECK(inflate(&stream, Z_FINISH) == Z_STREAM_END);
    CHECK(stream.total_in == resource->compressed_size);
    CHECK(stream.total_out == resource->raw_size);
    CHECK(inflateEnd(&stream) == Z_OK);
    CHECK(zlib_crc(raw, resource->raw_size) == resource->raw_crc32);
    CHECK(folio_font_pack_crc32(raw, resource->raw_size) == resource->raw_crc32);
    free(raw);
}

static void test_crc(void)
{
    CHECK(folio_font_pack_crc32(NULL, 0) == 0);
    CHECK(folio_font_pack_crc32(NULL, SIZE_MAX) == 0);
    CHECK(folio_font_pack_crc32("", 0) == 0);
    CHECK(folio_font_pack_crc32("123456789", 9) == UINT32_C(0xcbf43926));
    uint8_t bytes[1031];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)i;
    }
    /* Every table index occurs in the one-byte vectors. */
    for (unsigned i = 0; i < 256; ++i) {
        CHECK(folio_font_pack_crc32(bytes + i, 1) == reference_crc(bytes + i, 1));
        CHECK(folio_font_pack_crc32(bytes + i, 1) == zlib_crc(bytes + i, 1));
    }
    for (size_t length = 0; length <= 1024; ++length) {
        CHECK(folio_font_pack_crc32(bytes + 1, length) ==
              reference_crc(bytes + 1, length));
        CHECK(folio_font_pack_crc32(bytes + 1, length) == zlib_crc(bytes + 1, length));
    }
}

static void test_inputs(uint8_t *image)
{
    make_image(image);
    for (unsigned i = 0; i < 4; ++i) {
        folio_font_resource_t resource = accept(image, i);
        verify_inflate(&resource);
    }
    reject(NULL, IMAGE_SIZE, "hei");
    reject(NULL, 0, NULL);
    reject(image, IMAGE_SIZE, NULL);
    reject(image, IMAGE_SIZE, "");
    reject(image, IMAGE_SIZE, "song");
    reject(image, IMAGE_SIZE, "Hei");
    reject(image, IMAGE_SIZE, "heiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii");
    reject(image, IMAGE_SIZE, "symbols-extra");
    CHECK(!folio_font_pack_find(NULL, SIZE_MAX, NULL, NULL));
    CHECK(!folio_font_pack_find(image, IMAGE_SIZE, "hei", NULL));

    uint8_t one_byte = 0;
    /* ASan catches reads before the complete-image length check. */
    for (size_t length = 0; length <= HEADER_SIZE; ++length) {
        reject(&one_byte, length, "hei");
    }
    const size_t bad_lengths[] = {
        PAYLOAD_START - 1, PAYLOAD_START, GUARD_START, IMAGE_SIZE - 1,
        IMAGE_SIZE + 1, UINT32_MAX, SIZE_MAX
    };
    for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(*bad_lengths); ++i) {
        reject(&one_byte, bad_lengths[i], "hei");
    }
}

static void test_header_mutations(uint8_t *image)
{
    make_image(image);
    /* All header bytes, including SHA and unused bytes, are CRC-covered. */
    for (size_t i = 0; i < HEADER_SIZE; ++i) {
        image[i] ^= 1;
        reject(image, IMAGE_SIZE, "hei");
        image[i] ^= 1;
    }

    static const struct {
        uint32_t offset;
        uint32_t value;
    } cases[] = {
        {8, 0}, {8, 2}, {8, UINT32_MAX},
        {12, 0}, {12, 4095}, {12, 8192}, {12, UINT32_MAX},
        {16, 0}, {16, PAYLOAD_START}, {16, PAYLOAD_START + 6 * ALIGNMENT},
        {16, PAYLOAD_START + 8 * ALIGNMENT}, {16, PAYLOAD_START + 7 * ALIGNMENT + 1},
        {16, GUARD_START + ALIGNMENT}, {16, UINT32_MAX},
        {20, 0}, {20, 3}, {20, 5}, {20, UINT32_MAX},
        {24, 0}, {24, 127}, {24, 256}, {24, UINT32_MAX},
        {28, 0}, {28, 65}, {28, UINT32_MAX},
        {32, 0}, {32, 8192}, {32, UINT32_MAX},
        {36, 0}, {36, METADATA_SIZE + 1}, {36, UINT32_MAX},
        {40, 0}, {44, 0},
        {ENTRY(3) + 16, 0}, {ENTRY(3) + 16, PAYLOAD_START - ALIGNMENT},
        {ENTRY(3) + 16, PAYLOAD_START + 6 * ALIGNMENT + 1},
        {ENTRY(3) + 16, PAYLOAD_START + 7 * ALIGNMENT},
        {ENTRY(3) + 16, GUARD_START}, {ENTRY(3) + 16, UINT32_C(0xfffff000)},
        {ENTRY(3) + 16, UINT32_MAX},
        {ENTRY(3) + 20, 0}, {ENTRY(3) + 20, ALIGNMENT + 1},
        {ENTRY(3) + 20, UINT32_MAX},
        {ENTRY(3) + 24, 0}, {ENTRY(3) + 24, 1}, {ENTRY(3) + 24, 11},
        {ENTRY(3) + 24, RAW_LIMIT + 1}, {ENTRY(3) + 24, UINT32_MAX},
        {ENTRY(3) + 36, 0}, {ENTRY(3) + 36, 2}, {ENTRY(3) + 36, UINT32_MAX},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        make_image(image);
        put32(image + cases[i].offset, cases[i].value);
        /* Repair CRCs so semantic guards, not incidental corruption, reject. */
        seal_header(image);
        if (cases[i].offset == 44) {
            put32(image + 44, cases[i].value);
        }
        reject(image, IMAGE_SIZE, "hei");
    }

    const uint32_t reserved[] = {
        48, 63, ENTRY(0) + 104, ENTRY(0) + 127,
        ENTRY(1) + 104, ENTRY(1) + 127, ENTRY(2) + 104, ENTRY(2) + 127,
        ENTRY(3) + 104, ENTRY(3) + 127, ENTRY(4), HEADER_SIZE - 1
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(*reserved); ++i) {
        make_image(image);
        image[reserved[i]] = 1;
        seal_header(image);
        reject(image, IMAGE_SIZE, "hei");
    }

    make_image(image);
    image[0] ^= 0x80;
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");
}

static void test_entries(uint8_t *image)
{
    for (unsigned i = 0; i < 4; ++i) {
        make_image(image);
        memset(image + ENTRY(i), 'x', 16); /* Unterminated on-disk ID. */
        seal_header(image);
        reject(image, IMAGE_SIZE, "hei");
        make_image(image);
        image[ENTRY(i) + 15] = 1; /* Nonzero NUL padding. */
        seal_header(image);
        reject(image, IMAGE_SIZE, "hei");
        make_image(image);
        memset(image + ENTRY(i), 0, 16);
        seal_header(image);
        reject(image, IMAGE_SIZE, "hei");
    }
    make_image(image);
    memcpy(image + ENTRY(3), image + ENTRY(0), 16); /* Duplicate ID. */
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");
    make_image(image);
    memset(image + ENTRY(3), 0, 16);
    memcpy(image + ENTRY(3), "unknown", 7);
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");
    make_image(image);
    memcpy(image + ENTRY(0), "kai", 3);
    memcpy(image + ENTRY(1), "hei", 3);
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");

    /* Exact overlap and partial overlap, including an unselected entry. */
    for (unsigned i = 1; i < 4; ++i) {
        make_image(image);
        put32(image + ENTRY(i) + 16, PAYLOAD_START);
        seal_header(image);
        reject(image, IMAGE_SIZE, "hei");
    }
    make_image(image);
    put32(image + ENTRY(0) + 20, 2 * ALIGNMENT + 1);
    seal_header(image);
    reject(image, IMAGE_SIZE, "symbols");

    /* Offset + length would wrap a naive uint32_t range check. */
    make_image(image);
    put32(image + ENTRY(3) + 16, UINT32_C(0xfffff000));
    put32(image + ENTRY(3) + 20, UINT32_C(8192));
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");

    /* Adjacent nonoverlapping ranges are legal. */
    make_image(image);
    put32(image + ENTRY(0) + 20, 2 * ALIGNMENT);
    put32(image + ENTRY(0) + 28, zlib_crc(image + PAYLOAD_START, 2 * ALIGNMENT));
    seal_header(image);
    (void)accept(image, 0);

    /* Physical payload order need not match the mandatory ID table order. */
    make_image(image);
    uint8_t descriptor[88];
    memcpy(descriptor, image + ENTRY(0) + 16, sizeof(descriptor));
    memcpy(image + ENTRY(0) + 16, image + ENTRY(3) + 16, sizeof(descriptor));
    memcpy(image + ENTRY(3) + 16, descriptor, sizeof(descriptor));
    seal_header(image);
    for (unsigned i = 0; i < 4; ++i) {
        folio_font_resource_t resource = accept(image, i);
        verify_inflate(&resource);
    }

    /* Inclusive raw-size limits are structural; inflate is the loader's job. */
    make_image(image);
    put32(image + ENTRY(0) + 24, RAW_LIMIT);
    seal_header(image);
    CHECK(accept(image, 0).raw_size == RAW_LIMIT);
    put32(image + ENTRY(0) + 24, 12);
    seal_header(image);
    CHECK(accept(image, 0).raw_size == 12);
}

static void test_metadata(uint8_t *image)
{
    make_image(image);
    image[HEADER_SIZE + 2] ^= 1;
    reject(image, IMAGE_SIZE, "hei");
    make_image(image);
    image[HEADER_SIZE + get32(image + 36)] = 0;
    reject(image, IMAGE_SIZE, "hei");
    make_image(image);
    image[PAYLOAD_START - 1] = 0;
    reject(image, IMAGE_SIZE, "hei");

    static const struct {
        uint8_t bytes[4];
        size_t length;
    } invalid_utf8[] = {
        {{0x80}, 1}, {{0xc0, 0x80}, 2}, {{0xc1, 0xbf}, 2},
        {{0xc2}, 1}, {{0xc2, 0x20}, 2}, {{0xe0, 0x80, 0x80}, 3},
        {{0xe1, 0x80}, 2}, {{0xed, 0xa0, 0x80}, 3},
        {{0xf0, 0x80, 0x80, 0x80}, 4}, {{0xf1, 0x80, 0x80}, 3},
        {{0xf4, 0x90, 0x80, 0x80}, 4}, {{0xf5, 0x80, 0x80, 0x80}, 4},
        {{0xff}, 1}
    };
    for (size_t i = 0; i < sizeof(invalid_utf8) / sizeof(*invalid_utf8); ++i) {
        make_image(image);
        set_metadata(image, invalid_utf8[i].bytes, invalid_utf8[i].length);
        reject(image, IMAGE_SIZE, "hei");
    }
    static const uint8_t valid_utf8[] = {
        '"', 0xc2, 0x80, 0xdf, 0xbf, 0xe0, 0xa0, 0x80,
        0xed, 0x9f, 0xbf, 0xee, 0x80, 0x80, 0xef, 0xbf, 0xbf,
        0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf, '"'
    };
    make_image(image);
    set_metadata(image, valid_utf8, sizeof(valid_utf8));
    (void)accept(image, 0);
    set_metadata(image, (const uint8_t *)"0", 1);
    (void)accept(image, 0);

    /* Maximum metadata consumes the entire reserved metadata extent. */
    memset(image + HEADER_SIZE, 'a', METADATA_SIZE);
    image[HEADER_SIZE] = '"';
    image[PAYLOAD_START - 1] = '"';
    put32(image + 36, METADATA_SIZE);
    put32(image + 40, zlib_crc(image + HEADER_SIZE, METADATA_SIZE));
    seal_header(image);
    (void)accept(image, 0);
}

static void test_payload_and_guard(uint8_t *image)
{
    make_image(image);
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t offset = get32(image + ENTRY(i) + 16);
        image[offset] ^= 1;
        reject(image, IMAGE_SIZE, ids[i]);
        /* An unselected compressed CRC is intentionally deferred. */
        (void)accept(image, (i + 1) % 4);
        image[offset] ^= 1;
    }
    put32(image + ENTRY(0) + 28, get32(image + ENTRY(0) + 28) ^ 1);
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");

    make_image(image);
    image[ENTRY(0) + 40] ^= 1;
    image[ENTRY(0) + 72] ^= 1;
    put32(image + ENTRY(0) + 32, UINT32_C(0x12345678));
    seal_header(image);
    CHECK(accept(image, 0).raw_crc32 == UINT32_C(0x12345678));

    const uint32_t guard_bytes[] = {GUARD_START, GUARD_START + 32768, IMAGE_SIZE - 1};
    for (size_t i = 0; i < sizeof(guard_bytes) / sizeof(*guard_bytes); ++i) {
        make_image(image);
        image[guard_bytes[i]] = 0;
        reject(image, IMAGE_SIZE, "hei");
    }

    /* No full-image/payload-gap scan is required at runtime. */
    make_image(image);
    image[PAYLOAD_START + ALIGNMENT] = 0;
    image[get32(image + 16)] = 0;
    image[GUARD_START - 1] = 0;
    (void)accept(image, 0);

    /* Last payload can end exactly at the start of the protected guard. */
    make_image(image);
    memset(image + GUARD_START - ALIGNMENT, 0x5a, ALIGNMENT);
    put32(image + ENTRY(3) + 16, GUARD_START - ALIGNMENT);
    put32(image + ENTRY(3) + 20, ALIGNMENT);
    put32(image + ENTRY(3) + 28,
          zlib_crc(image + GUARD_START - ALIGNMENT, ALIGNMENT));
    put32(image + 16, GUARD_START);
    seal_header(image);
    (void)accept(image, 3);
    put32(image + ENTRY(3) + 20, ALIGNMENT + 1);
    seal_header(image);
    reject(image, IMAGE_SIZE, "hei");
}

static void test_semantic_sweep(uint8_t *image)
{
    make_image(image);
    uint32_t random = UINT32_C(0x6b3f2189);
    for (unsigned i = 0; i < 2048; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        uint32_t entry = (random >> 8) % 4;
        uint32_t field = 16 + 4 * ((random >> 16) % 6);
        uint8_t *position = image + ENTRY(entry) + field;
        uint32_t saved = get32(position);
        put32(position, random);
        seal_header(image);
        folio_font_resource_t resource = {
            .compressed = image, .compressed_size = 1, .raw_size = 1,
            .compressed_crc32 = 1, .raw_crc32 = 1
        };
        unsigned selected = i % 4;
        if (folio_font_pack_find(image, IMAGE_SIZE, ids[selected], &resource)) {
            CHECK(resource.compressed >= image + PAYLOAD_START);
            CHECK(resource.compressed < image + GUARD_START);
            CHECK(resource.compressed_size <=
                  (size_t)(image + GUARD_START - resource.compressed));
            CHECK(resource.raw_size >= 12 && resource.raw_size <= RAW_LIMIT);
            CHECK(zlib_crc(resource.compressed, resource.compressed_size) ==
                  resource.compressed_crc32);
        } else {
            CHECK(resource_is_clear(&resource));
        }
        put32(position, saved);
        seal_header(image);
    }
}

static void test_real_pack(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror(path);
        exit(1);
    }
    uint8_t *image = malloc((size_t)IMAGE_SIZE + 1);
    CHECK(image != NULL);
    size_t length = fread(image, 1, (size_t)IMAGE_SIZE + 1, file);
    CHECK(!ferror(file));
    CHECK(fclose(file) == 0);
    CHECK(length == IMAGE_SIZE);
    for (unsigned i = 0; i < 4; ++i) {
        folio_font_resource_t resource = accept(image, i);
        verify_inflate(&resource);
        printf("fixture %s: compressed=%zu raw=%zu compressed_crc=%08" PRIx32
               " raw_crc=%08" PRIx32 " CRC/inflate OK\n", ids[i],
               resource.compressed_size, resource.raw_size,
               resource.compressed_crc32, resource.raw_crc32);
    }
    free(image);
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [folio_fonts.bin]\n", argv[0]);
        return 2;
    }
    /* Every parser test also exercises an intentionally unaligned image. */
    uint8_t *allocation = malloc((size_t)IMAGE_SIZE + 1);
    CHECK(allocation != NULL);
    uint8_t *image = allocation + 1;
    test_crc();
    test_inputs(image);
    test_header_mutations(image);
    test_entries(image);
    test_metadata(image);
    test_payload_and_guard(image);
    test_semantic_sweep(image);
    free(allocation);
    if (argc == 2) {
        test_real_pack(argv[1]);
    }
    printf("font pack tests passed: %u checks (ASan/UBSan host suite)\n", checks);
    return 0;
}
