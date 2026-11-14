#!/usr/bin/env python3
"""Compile the real BSP against mock drivers; format calls never touch media."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    'esp_err.h': '''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NOT_FINISHED 4
#define ESP_ERR_NO_MEM 5
#define ESP_ERR_TIMEOUT 6
const char* esp_err_to_name(esp_err_t);
''',
    'esp_log.h': '''#pragma once
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
''',
    'driver/sdmmc_host.h': '''#pragma once
typedef struct {int max_freq_khz;} sdmmc_host_t;
typedef struct {int width,clk,cmd,d0,d1,d2,d3,cd,wp,flags;} sdmmc_slot_config_t;
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){0})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})
#define SDMMC_FREQ_HIGHSPEED 40000
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1
#define GPIO_NUM_38 38
#define GPIO_NUM_42 42
#define GPIO_NUM_44 44
#define GPIO_NUM_NC -1
''',
    'sdmmc_cmd.h': '''#pragma once
#include <stdint.h>
typedef struct { struct {char name[8];} cid; struct {uint64_t capacity; unsigned sector_size;} csd; } sdmmc_card_t;
''',
    'esp_vfs_fat.h': '''#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
typedef struct {bool format_if_mount_failed; unsigned max_files,allocation_unit_size;} esp_vfs_fat_sdmmc_mount_config_t;
esp_err_t esp_vfs_fat_sdmmc_mount(const char*,const sdmmc_host_t*,const sdmmc_slot_config_t*,const esp_vfs_fat_sdmmc_mount_config_t*,sdmmc_card_t**);
esp_err_t esp_vfs_fat_sdcard_unmount(const char*,sdmmc_card_t*);
esp_err_t esp_vfs_fat_sdcard_format(const char*,sdmmc_card_t*);
esp_err_t esp_vfs_fat_info(const char*,uint64_t*,uint64_t*);
''',
    'freertos/FreeRTOS.h': '''#pragma once
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ticks) (ticks)
#define pdPASS 1
''',
    'freertos/task.h': '''#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,void*);
void vTaskDelay(unsigned);
void vTaskDelete(void*);
''',
    'read_pico_board.h': '''#pragma once
#include <stdbool.h>
bool read_pico_sd_present(void);
''',
}


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file(): compiler = shutil.which('clang') or shutil.which('cc')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    with tempfile.TemporaryDirectory(prefix='folio-format-mock-') as temporary:
        temp = Path(temporary)
        for name, content in HEADERS.items():
            path = temp / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content)
        binary = temp / 'format-tests'
        subprocess.run([compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(temp),
                        '-I', str(ROOT / 'components/read_pico/include'), str(ROOT / 'tools/tests/test_sd_format.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__': main()
