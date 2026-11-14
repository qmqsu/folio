#include "folio_update.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
#define UPDATE_PATH FOLIO_SD_ROOT "/update/folio.bin"
#define UPDATE_SHA_PATH FOLIO_SD_ROOT "/update/folio.bin.sha256"
#define UPDATE_CHUNK 4096u

_Static_assert(sizeof(esp_image_header_t) == 24, "ESP image header format");
_Static_assert(sizeof(esp_image_segment_header_t) == 8, "ESP segment header format");
_Static_assert(sizeof(esp_app_desc_t) == 256, "ESP application descriptor format");
_Static_assert(offsetof(esp_app_desc_t, version) == 16, "ESP application version offset");
_Static_assert(offsetof(esp_app_desc_t, project_name) == 48, "ESP application project offset");

extern bool folio_io_cancelled(void) __attribute__((weak));
static bool update_cancelled(const folio_update_control_t* control) {
    return (control && control->cancelled && control->cancelled(control->context)) ||
        (folio_io_cancelled && folio_io_cancelled());
}
static void update_error(char* error, size_t capacity, const char* message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
}
static uint32_t update_le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t preview_token(const folio_update_info_t* info) {
    uint64_t token = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < sizeof(info->version); ++i) token = (token ^ (uint8_t)info->version[i]) * UINT64_C(1099511628211);
    for (unsigned i = 0; i < 4; ++i) token = (token ^ (uint8_t)(info->size >> (i * 8))) * UINT64_C(1099511628211);
    for (unsigned i = 0; i < sizeof(info->sha256); ++i) token = (token ^ info->sha256[i]) * UINT64_C(1099511628211);
    return token;
}
static bool valid_version(const char version[32]) {
    const char* end = memchr(version, 0, 32);
    if (!end || end == version) return false;
    for (const char* p = version; p < end; ++p) if ((unsigned char)*p < 33 || (unsigned char)*p > 126) return false;
    return true;
}
static bool partition_overlap(const esp_partition_t* a, const esp_partition_t* b) {
    return (uint64_t)a->address < (uint64_t)b->address + b->size &&
        (uint64_t)b->address < (uint64_t)a->address + a->size;
}
static const esp_partition_t* update_target(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t* metadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!running || !target || !metadata || metadata->size < 8192 || target->type != ESP_PARTITION_TYPE_APP ||
        target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || target->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX ||
        partition_overlap(target, running) || partition_overlap(target, metadata) ||
        (uint64_t)target->address + target->size > UINT32_MAX ||
        target->size < sizeof(esp_image_header_t) + 8 + sizeof(esp_app_desc_t) + 16) return NULL;
    return target;
}
bool folio_update_available(void) { return update_target() != NULL; }
bool folio_update_confirm_boot(bool ready) {
    if (!ready) return false;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return false;
    if (running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN || running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return true;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
    return state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool hash_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool read_manifest(uint8_t digest[32], char* error, size_t capacity) {
    FILE* file = fopen(UPDATE_SHA_PATH, "rb");
    if (!file) { update_error(error, capacity, "未找到 folio.bin.sha256 校验文件。"); return false; }
    uint8_t bytes[FOLIO_UPDATE_SHA_FILE_MAX + 1];
    size_t length = fread(bytes, 1, sizeof(bytes), file);
    bool okay = !ferror(file);
    if (fclose(file) != 0) okay = false;
    if (!okay) { update_error(error, capacity, "固件校验文件读取失败，请检查 TF 卡。"); return false; }
    okay = length >= 64 && length <= FOLIO_UPDATE_SHA_FILE_MAX;
    for (unsigned i = 0; okay && i < 32; ++i) {
        int high = hex_value(bytes[i * 2]), low = hex_value(bytes[i * 2 + 1]);
        okay = high >= 0 && low >= 0;
        if (okay) digest[i] = (uint8_t)((high << 4) | low);
    }
    size_t cursor = 64;
    if (okay && cursor < length && !hash_space(bytes[cursor])) okay = false;
    while (cursor < length && hash_space(bytes[cursor])) ++cursor;
    if (okay && cursor < length) {
        if (bytes[cursor] == '*') ++cursor;
        static const char filename[] = "folio.bin";
        okay = length - cursor >= sizeof(filename) - 1 && !memcmp(bytes + cursor, filename, sizeof(filename) - 1);
        if (okay) cursor += sizeof(filename) - 1;
        while (cursor < length && hash_space(bytes[cursor])) ++cursor;
        if (cursor != length) okay = false;
    }
    if (!okay) update_error(error, capacity, "SHA-256 文件格式无效，应包含一个64位十六进制校验值。");
    return okay;
}
static bool finish_hash(psa_hash_operation_t* hash, uint8_t digest[32]) {
    size_t length = 0;
    return psa_hash_finish(hash, digest, 32, &length) == PSA_SUCCESS && length == 32;
}
typedef struct {
    FILE* file;
    uint32_t size, position, chunks;
    psa_hash_operation_t full_hash, image_hash;
    uint8_t checksum;
    char* error;
    size_t error_cap;
    const folio_update_control_t* control;
} image_reader_t;
static bool image_read(image_reader_t* reader, void* buffer, size_t length, bool image_hash, bool checksum) {
    if (update_cancelled(reader->control)) { update_error(reader->error, reader->error_cap, "固件校验已取消。"); return false; }
    if (length > reader->size - reader->position || fread(buffer, 1, length, reader->file) != length) {
        update_error(reader->error, reader->error_cap, ferror(reader->file) ? "固件读取失败，请检查 TF 卡。" : "固件文件不完整。"); return false;
    }
    reader->position += (uint32_t)length;
    if (psa_hash_update(&reader->full_hash, buffer, length) != PSA_SUCCESS ||
        (image_hash && psa_hash_update(&reader->image_hash, buffer, length) != PSA_SUCCESS)) {
        update_error(reader->error, reader->error_cap, "固件 SHA-256 计算失败。"); return false;
    }
    if (checksum) for (size_t i = 0; i < length; ++i) reader->checksum ^= ((const uint8_t*)buffer)[i];
    if ((++reader->chunks & 15u) == 0) vTaskDelay(1);
    return true;
}
static bool inspect_image(FILE* file, folio_update_info_t* info, const folio_update_control_t* control, char* error, size_t capacity) {
    memset(info, 0, sizeof(*info));
    struct stat status;
    if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 288 ||
        (uint64_t)status.st_size > FOLIO_UPDATE_MAX_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        update_error(error, capacity, "固件大小无效，需为不超过3 MiB的应用固件。"); return false;
    }
    image_reader_t reader = {.file = file, .size = (uint32_t)status.st_size, .checksum = 0xef,
        .full_hash = PSA_HASH_OPERATION_INIT, .image_hash = PSA_HASH_OPERATION_INIT, .error = error, .error_cap = capacity, .control = control};
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&reader.full_hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_setup(&reader.image_hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        (void)psa_hash_abort(&reader.full_hash); (void)psa_hash_abort(&reader.image_hash);
        update_error(error, capacity, "无法初始化固件校验。"); return false;
    }
    uint8_t* buffer = heap_caps_malloc(UPDATE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) buffer = malloc(UPDATE_CHUNK);
    bool okay = buffer != NULL;
    if (!okay) update_error(error, capacity, "固件校验内存不足。");
    unsigned segments = 0, appended_hash = 0;
    if (okay) okay = image_read(&reader, buffer, sizeof(esp_image_header_t), true, false);
    if (okay) {
        segments = buffer[1]; appended_hash = buffer[23];
        okay = buffer[0] == ESP_IMAGE_HEADER_MAGIC && segments > 0 && segments <= ESP_IMAGE_MAX_SEGMENTS &&
            buffer[12] == ESP_CHIP_ID_ESP32S3 && buffer[13] == 0 && appended_hash <= 1;
        if (!okay) update_error(error, capacity, "此文件不是有效的 ESP32-S3 应用固件。");
    }
    for (unsigned segment = 0; okay && segment < segments; ++segment) {
        okay = image_read(&reader, buffer, sizeof(esp_image_segment_header_t), true, false);
        if (!okay) break;
        uint32_t remaining = update_le32(buffer + 4);
        if ((remaining & 3u) || remaining > reader.size - reader.position || (segment == 0 && remaining < sizeof(esp_app_desc_t))) {
            update_error(error, capacity, "固件分段长度无效。"); okay = false; break;
        }
        if (segment == 0) {
            okay = image_read(&reader, buffer, sizeof(esp_app_desc_t), true, true);
            if (!okay) break;
            const char* version = (const char*)buffer + offsetof(esp_app_desc_t, version);
            const char* project = (const char*)buffer + offsetof(esp_app_desc_t, project_name);
            if (update_le32(buffer) != ESP_APP_DESC_MAGIC_WORD || !memchr(project, 0, 32) || strcmp(project, "Folio") || !valid_version(version)) {
                update_error(error, capacity, "固件项目或版本不匹配，需要 Folio 应用固件。"); okay = false; break;
            }
            snprintf(info->version, sizeof(info->version), "%s", version);
            remaining -= sizeof(esp_app_desc_t);
        }
        while (okay && remaining) {
            size_t length = remaining < UPDATE_CHUNK ? remaining : UPDATE_CHUNK;
            okay = image_read(&reader, buffer, length, true, true); remaining -= (uint32_t)length;
        }
    }
    if (okay) {
        size_t padding = 16u - (reader.position & 15u);
        okay = image_read(&reader, buffer, padding, true, false);
        if (okay && buffer[padding - 1] != reader.checksum) {
            update_error(error, capacity, "固件内部校验失败，文件可能损坏。"); okay = false;
        }
    }
    uint8_t internal_digest[32];
    if (okay && !finish_hash(&reader.image_hash, internal_digest)) {
        update_error(error, capacity, "固件内部 SHA-256 计算失败。"); okay = false;
    }
    if (okay && appended_hash) {
        okay = image_read(&reader, buffer, 32, false, false);
        if (okay && memcmp(internal_digest, buffer, 32)) {
            update_error(error, capacity, "固件内部 SHA-256 不匹配。"); okay = false;
        }
    }
    if (okay && (reader.position != reader.size || fgetc(file) != EOF || ferror(file))) {
        update_error(error, capacity, "固件含多余数据或读取不完整，不支持此镜像格式。"); okay = false;
    }
    if (okay && !finish_hash(&reader.full_hash, info->sha256)) {
        update_error(error, capacity, "固件 SHA-256 计算失败。"); okay = false;
    }
    if (okay) { info->size = reader.size; info->token = preview_token(info); }
    else memset(info, 0, sizeof(*info));
    free(buffer); (void)psa_hash_abort(&reader.full_hash); (void)psa_hash_abort(&reader.image_hash);
    return okay;
}
bool folio_update_validate_image(const char* path, char* error, size_t capacity) {
    update_error(error, capacity, "");
    if (!path) { update_error(error, capacity, "固件文件路径无效。"); return false; }
    FILE* file = fopen(path, "rb");
    if (!file) { update_error(error, capacity, "固件文件未能打开。"); return false; }
    folio_update_info_t info;
    bool okay = inspect_image(file, &info, NULL, error, capacity);
    if (fclose(file) != 0) { update_error(error, capacity, "固件文件关闭失败，请检查 TF 卡。"); okay = false; }
    return okay;
}
bool folio_update_inspect(folio_update_info_t* info, char* error, size_t capacity) {
    update_error(error, capacity, "");
    if (!info) { update_error(error, capacity, "固件预览参数无效。"); return false; }
    memset(info, 0, sizeof(*info));
    const esp_partition_t* target = update_target();
    if (!target) { update_error(error, capacity, "当前分区布局不支持 OTA，请先通过 USB 安装完整固件。"); return false; }
    uint8_t expected[32]; if (!read_manifest(expected, error, capacity)) return false;
    FILE* file = fopen(UPDATE_PATH, "rb");
    if (!file) { update_error(error, capacity, "未找到 update/folio.bin 固件文件。"); return false; }
    bool okay = inspect_image(file, info, NULL, error, capacity);
    if (fclose(file) != 0) { update_error(error, capacity, "固件文件关闭失败，请检查 TF 卡。"); okay = false; }
    if (okay && memcmp(expected, info->sha256, sizeof(expected))) { update_error(error, capacity, "固件与 SHA-256 校验文件不匹配。"); okay = false; }
    if (okay && info->size > target->size) { update_error(error, capacity, "固件超过可用 OTA 分区大小。"); okay = false; }
    if (!okay) memset(info, 0, sizeof(*info));
    return okay;
}
bool folio_update_inspect_file(const char* path, const folio_update_info_t* expected,
    folio_update_info_t* info, const folio_update_control_t* control, char* error, size_t capacity) {
    update_error(error, capacity, "");
    if (info) memset(info, 0, sizeof(*info));
    if (!path || !expected || !info || !valid_version(expected->version)) {
        update_error(error, capacity, "在线固件参数无效。"); return false;
    }
    const esp_partition_t* target = update_target();
    if (!target || expected->size > target->size) {
        update_error(error, capacity, "没有足够的独立 OTA 分区。"); return false;
    }
    FILE* file = fopen(path, "rb");
    if (!file) { update_error(error, capacity, "固件文件未能打开。"); return false; }
    bool okay = inspect_image(file, info, control, error, capacity);
    if (fclose(file) != 0) { update_error(error, capacity, "TF 卡读取未能完成。"); okay = false; }
    if (okay && (info->size != expected->size || strcmp(info->version, expected->version) ||
        memcmp(info->sha256, expected->sha256, 32))) {
        update_error(error, capacity, "固件与已签名的版本、大小或 SHA-256 不符。"); okay = false;
    }
    if (!okay) memset(info, 0, sizeof(*info));
    return okay;
}
bool folio_update_install(const folio_update_info_t* preview, char* error, size_t capacity) {
    update_error(error, capacity, "");
    if (!preview || !valid_version(preview->version) || preview->size < 288 || preview->size > FOLIO_UPDATE_MAX_SIZE ||
        preview->token != preview_token(preview)) { update_error(error, capacity, "固件预览已失效，请重新检查。"); return false; }
    const esp_partition_t* target = update_target();
    if (!target || preview->size > target->size) { update_error(error, capacity, "没有足够的独立 OTA 分区。"); return false; }
    uint8_t expected[32]; if (!read_manifest(expected, error, capacity)) return false;
    if (memcmp(expected, preview->sha256, 32)) { update_error(error, capacity, "校验文件已变化，请重新检查固件。"); return false; }
    return folio_update_install_file(UPDATE_PATH, preview, NULL, error, capacity);
}
bool folio_update_install_file(const char* path, const folio_update_info_t* preview,
    const folio_update_control_t* control, char* error, size_t capacity) {
    update_error(error, capacity, "");
    if (!path || !preview || !valid_version(preview->version) || preview->size < 288 || preview->size > FOLIO_UPDATE_MAX_SIZE ||
        preview->token != preview_token(preview)) { update_error(error, capacity, "固件预览已失效，请重新检查。"); return false; }
    const esp_partition_t* target = update_target();
    if (!target || preview->size > target->size) { update_error(error, capacity, "没有足够的独立 OTA 分区。"); return false; }
    FILE* file = fopen(path, "rb");
    if (!file) { update_error(error, capacity, "固件文件未能打开。"); return false; }
    folio_update_info_t actual;
    bool okay = inspect_image(file, &actual, control, error, capacity);
    if (okay && (actual.size != preview->size || strcmp(actual.version, preview->version) || memcmp(actual.sha256, preview->sha256, 32))) {
        update_error(error, capacity, "固件自预览后已变化，请重新检查。"); okay = false;
    }
    uint8_t* buffer = NULL;
    if (okay) {
        buffer = heap_caps_malloc(UPDATE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) buffer = malloc(UPDATE_CHUNK);
        if (!buffer) { update_error(error, capacity, "固件写入内存不足。"); okay = false; }
    }
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (okay && (fseek(file, 0, SEEK_SET) != 0 || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS)) {
        update_error(error, capacity, "固件写入准备失败。"); okay = false;
    }
    esp_ota_handle_t handle = 0; bool begun = false;
    if (okay && update_cancelled(control)) { update_error(error, capacity, "固件安装已取消。"); okay = false; }
    if (okay) {
        begun = esp_ota_begin(target, preview->size, &handle) == ESP_OK;
        if (!begun) { update_error(error, capacity, "无法开始写入 OTA 分区，当前固件保留。"); okay = false; }
    }
    uint32_t copied = 0; unsigned chunks = 0;
    while (okay && copied < preview->size) {
        if (update_cancelled(control)) { update_error(error, capacity, "固件安装已取消，当前固件保留。"); okay = false; break; }
        size_t length = preview->size - copied; if (length > UPDATE_CHUNK) length = UPDATE_CHUNK;
        if (fread(buffer, 1, length, file) != length || psa_hash_update(&hash, buffer, length) != PSA_SUCCESS) {
            update_error(error, capacity, "固件读取或校验失败，当前固件保留。"); okay = false; break;
        }
        if (esp_ota_write(handle, buffer, length) != ESP_OK) {
            update_error(error, capacity, "OTA 写入失败，当前固件保留。"); okay = false; break;
        }
        copied += (uint32_t)length;
        if ((++chunks & 15u) == 0) vTaskDelay(1);
    }
    uint8_t written_digest[32];
    if (okay && (fgetc(file) != EOF || ferror(file) || !finish_hash(&hash, written_digest) || memcmp(written_digest, preview->sha256, 32))) {
        update_error(error, capacity, "写入时固件内容变化或校验失败，当前固件保留。"); okay = false;
    }
    if (fclose(file) != 0) { update_error(error, capacity, "TF 卡读取未能完成，当前固件保留。"); okay = false; }
    free(buffer); (void)psa_hash_abort(&hash);
    if (!okay) { if (begun) (void)esp_ota_abort(handle); return false; }
    // IDF frees this handle on BOTH success and failure of esp_ota_end.
    if (esp_ota_end(handle) != ESP_OK) { update_error(error, capacity, "新固件最终校验失败，当前固件保留。"); return false; }
    if (update_cancelled(control)) { update_error(error, capacity, "安装已取消，启动目标保持不变。"); return false; }
    if (esp_ota_set_boot_partition(target) != ESP_OK) { update_error(error, capacity, "无法选择新固件，当前固件保留。"); return false; }
    return true;
}
