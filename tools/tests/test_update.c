#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"

bool fail_crypto_init, fail_hash_setup, fail_hash_finish;
static bool fail_allocation;
static int fail_close_after = -1;
static void* update_test_malloc(size_t size) { return fail_allocation ? NULL : malloc(size); }
static void* update_test_heap_malloc(size_t size, unsigned flags) { (void)flags; return update_test_malloc(size); }
static int update_test_fclose(FILE* file) {
    int result = fclose(file);
    if (fail_close_after == 0) { errno = EIO; return EOF; }
    if (fail_close_after > 0) --fail_close_after;
    return result;
}
#define malloc update_test_malloc
#define heap_caps_malloc update_test_heap_malloc
#define fclose update_test_fclose
#include "../../main/folio_update.c"
#undef malloc
#undef heap_caps_malloc
#undef fclose

static esp_partition_t running = {0x10000, 0x400000, ESP_PARTITION_TYPE_APP, 0};
static esp_partition_t target = {0x920000, 0x300000, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
static esp_partition_t ota_metadata = {0x910000, 8192, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA};
static bool no_target, no_metadata, no_running, fail_begin, fail_end, fail_boot, fail_state, fail_mark;
static bool cancelled, cancel_after_write, cancel_after_end, mutate_after_begin;
static int fail_write_after = -1;
static unsigned begin_calls, write_calls, abort_calls, end_calls, boot_calls, mark_calls;
static bool live_handle;
static uint8_t ota_bytes[FOLIO_UPDATE_MAX_SIZE];
static size_t ota_size, expected_size;
static uint32_t boot_address = 0x10000;
static esp_ota_img_states_t mock_state = ESP_OTA_IMG_VALID;
static const char* fixture_directory;

bool folio_io_cancelled(void) { return cancelled; }
const esp_partition_t* esp_ota_get_running_partition(void) { return no_running ? NULL : &running; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* from) {
    assert(!from); return no_target ? NULL : &target;
}
const esp_partition_t* esp_partition_find_first(unsigned type, unsigned subtype, const char* label) {
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_OTA && !label);
    return no_metadata ? NULL : &ota_metadata;
}
static void patch_byte(size_t offset, unsigned char value) {
    FILE* file = fopen(UPDATE_PATH, "r+b"); assert(file);
    assert(fseek(file, (long)offset, SEEK_SET) == 0 && fputc(value, file) != EOF && fclose(file) == 0);
}
esp_err_t esp_ota_begin(const esp_partition_t* partition, size_t size, esp_ota_handle_t* handle) {
    ++begin_calls; assert(partition == &target && partition->address != running.address && !live_handle);
    assert(partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && size <= partition->size);
    if (fail_begin) return ESP_FAIL;
    live_handle = true; ota_size = 0; expected_size = size; *handle = 57;
    if (mutate_after_begin) patch_byte(8192, 0xee);
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* bytes, size_t size) {
    assert(handle == 57 && live_handle && ota_size + size <= expected_size); ++write_calls;
    if (fail_write_after == 0) return ESP_FAIL;
    if (fail_write_after > 0) --fail_write_after;
    memcpy(ota_bytes + ota_size, bytes, size); ota_size += size;
    if (cancel_after_write) cancelled = true;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    assert(handle == 57 && live_handle && ota_size == expected_size); ++end_calls; live_handle = false;
    if (cancel_after_end) cancelled = true;
    return fail_end ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
    assert(handle == 57 && live_handle); ++abort_calls; live_handle = false; return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
    assert(partition == &target && !live_handle && end_calls == 1); ++boot_calls;
    if (fail_boot) return ESP_FAIL;
    boot_address = partition->address; return ESP_OK;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition, esp_ota_img_states_t* state) {
    assert(partition == &running); *state = mock_state; return fail_state ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { ++mark_calls; return fail_mark ? ESP_FAIL : ESP_OK; }

static void reset_ota(void) {
    assert(!live_handle);
    no_target = no_metadata = no_running = fail_begin = fail_end = fail_boot = fail_state = fail_mark = false;
    cancelled = cancel_after_write = cancel_after_end = mutate_after_begin = false;
    fail_crypto_init = fail_hash_setup = fail_hash_finish = fail_allocation = false;
    fail_close_after = fail_write_after = -1;
    begin_calls = write_calls = abort_calls = end_calls = boot_calls = mark_calls = 0;
    boot_address = 0x10000; ota_size = 0;
    running = (esp_partition_t){0x10000, 0x400000, ESP_PARTITION_TYPE_APP, 0};
    target = (esp_partition_t){0x920000, 0x300000, ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
    ota_metadata.size = 8192;
}
static void copy_file(const char* source, const char* destination) {
    FILE* input = fopen(source, "rb"), *output = fopen(destination, "wb"); assert(input && output);
    uint8_t buffer[4096]; size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), input)) != 0) assert(fwrite(buffer, 1, size, output) == size);
    assert(!ferror(input) && fclose(input) == 0 && fclose(output) == 0);
}
static void fixture(const char* name) {
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.bin", fixture_directory, name); copy_file(path, UPDATE_PATH);
    snprintf(path, sizeof(path), "%s/%s.sha256", fixture_directory, name); copy_file(path, UPDATE_SHA_PATH);
}
static void put_manifest(const char* text, size_t length) {
    FILE* file = fopen(UPDATE_SHA_PATH, "wb"); assert(file);
    assert(fwrite(text, 1, length, file) == length && fclose(file) == 0);
}
static void digest_hex(const uint8_t digest[32], char output[65]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { output[i * 2] = hex[digest[i] >> 4]; output[i * 2 + 1] = hex[digest[i] & 15]; }
    output[64] = 0;
}
static folio_update_info_t good_preview(void) {
    reset_ota(); fixture("good"); folio_update_info_t info; char error[160];
    assert(folio_update_inspect(&info, error, sizeof(error)) && !*error);
    assert(!strcmp(info.version, "0.6.0-dev") && info.size == 16720 && info.token);
    assert(!begin_calls && !write_calls && !boot_calls && !mark_calls);
    return info;
}
static void assert_image_fails(void) {
    char error[160]; assert(!folio_update_validate_image(UPDATE_PATH, error, sizeof(error)) && *error);
    assert(!begin_calls && !boot_calls);
}
static void test_image_validation(void) {
    folio_update_info_t info = good_preview(); char error[160];
    assert(folio_update_validate_image(UPDATE_PATH, error, sizeof(error)));
    fixture("nohash"); assert(folio_update_inspect(&info, error, sizeof(error)));
    const struct { size_t offset; unsigned char value; } bad[] = {
        {0, 0}, {1, 0}, {1, 17}, {12, 5}, {13, 1}, {23, 2}, {28, 255},
        {32, 0}, {48, 0}, {48, '\n'}, {80, 'X'}, {16720 - 33, 0}, {16720 - 32, 0xff}
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) { fixture("good"); patch_byte(bad[i].offset, bad[i].value); assert_image_fails(); }
    fixture("good"); for (unsigned i = 0; i < 32; ++i) patch_byte(48 + i, 'A'); assert_image_fails();
    fixture("good"); for (unsigned i = 0; i < 32; ++i) patch_byte(80 + i, 'A'); assert_image_fails();
    const off_t cuts[] = {0, 23, 31, 287, 288, 16687, 16719};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        fixture("good"); assert(truncate(UPDATE_PATH, cuts[i]) == 0); assert_image_fails();
    }
    fixture("good"); assert(truncate(UPDATE_PATH, FOLIO_UPDATE_MAX_SIZE + 1) == 0); assert_image_fails();
    fixture("good"); FILE* extra = fopen(UPDATE_PATH, "ab"); assert(extra && fputc(0, extra) == 0 && fclose(extra) == 0); assert_image_fails();
    puts("OTA images: hashed/unhashed synthetic images accepted; chip/project/version/segments/checksum/hash/truncation/oversize/trailing bytes rejected");
}
static void test_manifest(void) {
    folio_update_info_t info = good_preview(), inspected; char error[160], hex[65], text[200]; digest_hex(info.sha256, hex);
    put_manifest(hex, 64); assert(folio_update_inspect(&inspected, error, sizeof(error)));
    snprintf(text, sizeof(text), "%s *folio.bin\r\n", hex); put_manifest(text, strlen(text)); assert(folio_update_inspect(&inspected, error, sizeof(error)));
    for (unsigned i = 0; i < 64; ++i) if (hex[i] >= 'a' && hex[i] <= 'f') hex[i] -= 'a' - 'A';
    snprintf(text, sizeof(text), "%s  folio.bin\n", hex); put_manifest(text, strlen(text)); assert(folio_update_inspect(&inspected, error, sizeof(error)));
    const char* suffixes[] = {"../folio.bin", "other.bin", "folio.bin\nextra", "folio.bin other.bin", "*"};
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        snprintf(text, sizeof(text), "%s  %s", hex, suffixes[i]); put_manifest(text, strlen(text));
        assert(!folio_update_inspect(&inspected, error, sizeof(error)) && *error && !inspected.token);
    }
    memset(text, 'x', sizeof(text)); put_manifest(text, 161); assert(!folio_update_inspect(&inspected, error, sizeof(error)));
    put_manifest(text, 63); assert(!folio_update_inspect(&inspected, error, sizeof(error)));
    put_manifest(text, 64); assert(!folio_update_inspect(&inspected, error, sizeof(error)));
    memset(text, '0', 64); put_manifest(text, 64); assert(!folio_update_inspect(&inspected, error, sizeof(error)));
    assert(remove(UPDATE_SHA_PATH) == 0 && !folio_update_inspect(&inspected, error, sizeof(error)));
    fixture("good"); assert(remove(UPDATE_PATH) == 0 && !folio_update_inspect(&inspected, error, sizeof(error)));
    assert(!begin_calls && !boot_calls);
    puts("OTA manifest: bare/sha256sum/uppercase accepted; missing, wrong, oversized, extra or mismatched digests rejected without flash writes");
}
static void test_install(void) {
    folio_update_info_t info = good_preview(); char error[160];
    assert(folio_update_install(&info, error, sizeof(error)) && !*error && !live_handle);
    assert(begin_calls == 1 && write_calls > 1 && end_calls == 1 && boot_calls == 1 && abort_calls == 0 && boot_address == target.address);
    FILE* file = fopen(UPDATE_PATH, "rb"); assert(file);
    uint8_t* expected = malloc(info.size); assert(expected);
    assert(fread(expected, 1, info.size, file) == info.size && fclose(file) == 0 && !memcmp(expected, ota_bytes, info.size)); free(expected);
    info = good_preview();
    running = target; target.address = 0xc20000; ++target.subtype; boot_address = running.address;
    assert(folio_update_available() && folio_update_install(&info, error, sizeof(error)) && boot_address == 0xc20000);
    // Each failure preserves the old boot target. IDF consumes handles at end.
    for (unsigned failure = 0; failure < 7; ++failure) {
        info = good_preview();
        if (failure == 0) fail_begin = true;
        if (failure == 1) fail_write_after = 1;
        if (failure == 2) fail_end = true;
        if (failure == 3) fail_boot = true;
        if (failure == 4) fail_close_after = 1;
        if (failure == 5) cancel_after_write = true;
        if (failure == 6) cancel_after_end = true;
        assert(!folio_update_install(&info, error, sizeof(error)) && *error && !live_handle && boot_address == 0x10000);
        if (failure == 1 || failure == 4 || failure == 5) assert(abort_calls == 1 && end_calls == 0 && boot_calls == 0);
        if (failure == 0 || failure == 2 || failure == 3 || failure == 6) assert(abort_calls == 0);
        if (failure != 3) assert(boot_calls == 0);
    }
    info = good_preview(); info.version[0] ^= 1;
    assert(!folio_update_install(&info, error, sizeof(error)) && !begin_calls);
    info = good_preview(); patch_byte(8192, 0xee);
    assert(!folio_update_install(&info, error, sizeof(error)) && !begin_calls);
    info = good_preview(); fixture("changed");
    assert(!folio_update_install(&info, error, sizeof(error)) && !begin_calls);
    info = good_preview();
    char changed[1024]; snprintf(changed, sizeof(changed), "%s/changed.bin", fixture_directory); copy_file(changed, UPDATE_PATH);
    assert(!folio_update_install(&info, error, sizeof(error)) && !begin_calls);
    info = good_preview(); mutate_after_begin = true;
    assert(!folio_update_install(&info, error, sizeof(error)) && abort_calls == 1 && !end_calls && !boot_calls && boot_address == 0x10000);
    puts("OTA install: exact bytes written to inactive slot; begin/write/end/boot/close/cancel failures and preview/write TOCTOU all preserve old boot target");
}
static void test_resource_and_partitions(void) {
    folio_update_info_t info = good_preview(); char error[160];
    for (unsigned failure = 0; failure < 5; ++failure) {
        info = good_preview();
        if (failure == 0) fail_allocation = true;
        if (failure == 1) fail_crypto_init = true;
        if (failure == 2) fail_hash_setup = true;
        if (failure == 3) fail_hash_finish = true;
        if (failure == 4) cancelled = true;
        assert(!folio_update_install(&info, error, sizeof(error)) && *error && !begin_calls && !boot_calls);
    }
    reset_ota(); assert(folio_update_available());
    no_target = true; assert(!folio_update_available()); no_target = false;
    no_metadata = true; assert(!folio_update_available()); no_metadata = false;
    no_running = true; assert(!folio_update_available()); no_running = false;
    ota_metadata.size = 4096; assert(!folio_update_available()); ota_metadata.size = 8192;
    target.address = running.address; assert(!folio_update_available()); target.address = 0x920000;
    target.address = running.address + 4096; assert(!folio_update_available()); target.address = 0x920000;
    target.address = ota_metadata.address; assert(!folio_update_available()); target.address = 0x920000;
    target.subtype = 0; assert(!folio_update_available()); target.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    target.type = ESP_PARTITION_TYPE_DATA; assert(!folio_update_available()); target.type = ESP_PARTITION_TYPE_APP;
    target.size = 4096; fixture("good"); assert(!folio_update_inspect(&info, error, sizeof(error)) && !begin_calls);
    reset_ota(); assert(!folio_update_confirm_boot(false) && !mark_calls);
    assert(folio_update_confirm_boot(true) && !mark_calls); // Factory needs no OTA confirmation.
    running.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_MIN; mock_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(!folio_update_confirm_boot(false) && !mark_calls);
    assert(folio_update_confirm_boot(true) && mark_calls == 1);
    fail_mark = true; assert(!folio_update_confirm_boot(true)); fail_mark = false;
    mock_state = ESP_OTA_IMG_VALID; unsigned marks = mark_calls; assert(folio_update_confirm_boot(true) && mark_calls == marks);
    mock_state = ESP_OTA_IMG_ABORTED; assert(!folio_update_confirm_boot(true) && mark_calls == marks);
    fail_state = true; assert(!folio_update_confirm_boot(true));
    puts("OTA limits: allocation/crypto/cancellation failures, absent or unsafe slots, and ready-only pending-boot confirmation passed");
}
static void test_maximum_and_mutations(void) {
    reset_ota(); fixture("maximum"); char error[160]; folio_update_info_t info;
    assert(folio_update_inspect(&info, error, sizeof(error)) && info.size == FOLIO_UPDATE_MAX_SIZE);
    assert(folio_update_install(&info, error, sizeof(error)) && ota_size == FOLIO_UPDATE_MAX_SIZE && boot_calls == 1);
    reset_ota(); fixture("good");
    uint8_t original[16720]; FILE* file = fopen(UPDATE_PATH, "rb"); assert(file);
    assert(fread(original, 1, sizeof(original), file) == sizeof(original) && fclose(file) == 0);
    uint32_t random = 0x3256;
    for (unsigned i = 0; i < 100; ++i) {
        fixture("good"); random = random * 1664525u + 1013904223u;
        size_t offset = random % sizeof(original);
        patch_byte(offset, original[offset] ^ (uint8_t)(1u << (random % 8)));
        assert_image_fails();
    }
    assert(!begin_calls && !boot_calls);
    fixture("good"); assert(folio_update_validate_image(UPDATE_PATH, NULL, 0));
    assert(!folio_update_validate_image(NULL, NULL, 0) && !folio_update_inspect(NULL, NULL, 0) && !folio_update_install(NULL, NULL, 0));
    puts("OTA boundaries: exact 3MiB image streamed, 100 single-bit mutations rejected, nullable diagnostics safe");
}
int main(int argc, char** argv) {
    assert(argc >= 2); fixture_directory = argv[1];
    assert(mkdir(FOLIO_SD_ROOT, 0700) == 0 && mkdir(FOLIO_SD_ROOT "/update", 0700) == 0);
    test_image_validation(); test_manifest(); test_install(); test_resource_and_partitions(); test_maximum_and_mutations();
    reset_ota();
    if (argc > 2) {
        char error[160];
        if (!folio_update_validate_image(argv[2], error, sizeof(error))) { fprintf(stderr, "%s\n", error); abort(); }
        printf("Actual Folio build image validated: %s\n", argv[2]);
    }
    if (argc > 3) {
        char error[160]; assert(!folio_update_validate_image(argv[3], error, sizeof(error)) && *error);
        printf("Actual merged/foreign image rejected: %s\n", argv[3]);
    }
    puts("OTA host tests passed; no device operations or automatic reboot executed");
    return 0;
}
