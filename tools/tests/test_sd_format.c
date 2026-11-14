/* Include production BSP with driver calls mocked: never mounts real media. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "read_pico_board.h"

static bool mock_present = true, mock_remount_failure;
static int mounts, formats, directories;
static esp_err_t mount_status, format_status, info_status;
static sdmmc_card_t* live_card;
static size_t copy_string(char* to, const char* from, size_t cap) {
    size_t size = strlen(from); if (cap) { size_t n = size < cap - 1 ? size : cap - 1; memcpy(to, from, n); to[n] = 0; } return size;
}
static int fake_mkdir(const char* path, mode_t mode) {
    (void)mode; assert(!strncmp(path, "/sdcard/", 8)); ++directories; return 0;
}
#define mkdir fake_mkdir
#define strlcpy copy_string
#include "../../components/read_pico/read_pico_sd.c"
#undef mkdir
#undef strlcpy

bool read_pico_sd_present(void) { return mock_present; }
const char* esp_err_to_name(esp_err_t status) { (void)status; return "mock"; }
BaseType_t xTaskCreate(void (*task)(void*), const char* name, unsigned stack, void* arg, unsigned priority, void* handle) {
    (void)name; (void)stack; (void)priority; (void)handle; task(arg); return pdPASS;
}
void vTaskDelay(unsigned ticks) { (void)ticks; }
void vTaskDelete(void* task) { (void)task; }
static sdmmc_card_t* new_card(void) {
    sdmmc_card_t* value = calloc(1, sizeof(*value)); assert(value);
    strcpy(value->cid.name, "TEST"); value->csd.capacity = 1000; value->csd.sector_size = 512; return value;
}
esp_err_t esp_vfs_fat_sdmmc_mount(const char* path, const sdmmc_host_t* host,
                                 const sdmmc_slot_config_t* slot, const esp_vfs_fat_sdmmc_mount_config_t* config, sdmmc_card_t** output) {
    assert(!strcmp(path, "/sdcard") && host->max_freq_khz && slot->width == 1 && config->max_files == 8);
    ++mounts;
    if (mount_status == ESP_OK) *output = live_card = new_card();
    return mount_status;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char* path, sdmmc_card_t* value) {
    (void)path; assert(value == live_card); free(value); live_card = NULL; return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_format(const char* path, sdmmc_card_t* value) {
    assert(!strcmp(path, "/sdcard") && value == live_card && value); ++formats;
    if (mock_remount_failure) { free(live_card); live_card = NULL; info_status = ESP_ERR_INVALID_STATE; }
    return format_status;
}
esp_err_t esp_vfs_fat_info(const char* path, uint64_t* total, uint64_t* available) {
    assert(!strcmp(path, "/sdcard"));
    if (info_status == ESP_OK) { assert(live_card); *total = 500000; *available = 490000; }
    return info_status;
}
static void reset(bool mounted) {
    free(live_card); live_card = NULL; card = NULL;
    mock_present = true; mock_remount_failure = false; mounts = formats = directories = 0;
    mount_status = format_status = info_status = ESP_OK;
    probe_state = 2; media_invalidated = false;
    memset(&cached_info, 0, sizeof(cached_info)); cached_info.present = true;
    if (mounted) { live_card = card = new_card(); cached_info.mounted = true; }
}
int main(void) {
    reset(true); assert(read_pico_sd_format() == ESP_OK);
    assert(formats == 1 && mounts == 0 && directories == 3 && cached_info.mounted);
    reset(false); assert(read_pico_sd_format() == ESP_OK);
    assert(formats == 1 && mounts == 1 && directories == 3 && cached_info.mounted);
    reset(false); mount_status = ESP_FAIL;
    assert(read_pico_sd_format() == ESP_FAIL && formats == 0 && directories == 0 && !cached_info.mounted);
    reset(true); format_status = ESP_FAIL;
    assert(read_pico_sd_format() == ESP_FAIL && formats == 1 && !cached_info.mounted);
    reset(true); mock_remount_failure = true;
    assert(read_pico_sd_format() == ESP_ERR_INVALID_STATE && !card && !cached_info.mounted && directories == 0);
    reset(true); probe_state = 1;
    assert(read_pico_sd_format() == ESP_ERR_NOT_FINISHED && !formats);
    reset(true); media_invalidated = true;
    assert(read_pico_sd_format() == ESP_ERR_INVALID_STATE && !formats);
    reset(false); mock_present = false;
    assert(read_pico_sd_format() == ESP_ERR_NOT_FOUND && !formats && !mounts);
    reset(true); info_status = ESP_FAIL;
    assert(read_pico_sd_format() == ESP_FAIL && !cached_info.mounted && directories == 0);
    reset(false);
    puts("SD format: 9 mocked driver states passed; no physical media operations");
    return 0;
}
