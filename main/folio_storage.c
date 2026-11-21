#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include "folio_storage.h"
#include "folio_wallpaper_limits.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t storage_mutex;
static portMUX_TYPE storage_guard = portMUX_INITIALIZER_UNLOCKED;
bool folio_storage_lock(uint32_t timeout_ms) {
    portENTER_CRITICAL(&storage_guard);
    SemaphoreHandle_t mutex = storage_mutex;
    portEXIT_CRITICAL(&storage_guard);
    if (!mutex) {
        SemaphoreHandle_t candidate = xSemaphoreCreateRecursiveMutex();
        if (!candidate) return false;
        portENTER_CRITICAL(&storage_guard);
        if (!storage_mutex) { storage_mutex = candidate; candidate = NULL; }
        mutex = storage_mutex;
        portEXIT_CRITICAL(&storage_guard);
        if (candidate) vSemaphoreDelete(candidate);
    }
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !ticks) ticks = 1;
    return xSemaphoreTakeRecursive(mutex, ticks) == pdTRUE;
}
void folio_storage_unlock(void) { if (storage_mutex) (void)xSemaphoreGiveRecursive(storage_mutex); }
static void storage_yield(void) { vTaskDelay(1); }
static void* storage_alloc(size_t size) {
    void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : malloc(size);
}
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <time.h>
static pthread_mutex_t storage_mutex;
static pthread_once_t storage_once = PTHREAD_ONCE_INIT;
static void initialize_mutex(void) {
    pthread_mutexattr_t attributes;
    (void)pthread_mutexattr_init(&attributes);
    (void)pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&storage_mutex, &attributes);
    (void)pthread_mutexattr_destroy(&attributes);
}
static uint64_t milliseconds(void) {
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
bool folio_storage_lock(uint32_t timeout_ms) {
    (void)pthread_once(&storage_once, initialize_mutex);
    uint64_t started = milliseconds();
    do {
        int status = pthread_mutex_trylock(&storage_mutex);
        if (!status) return true;
        if (status != EBUSY || !timeout_ms) return false;
        struct timespec delay = {0, 1000000};
        (void)nanosleep(&delay, NULL);
    } while (timeout_ms == UINT32_MAX || milliseconds() - started < timeout_ms);
    return false;
}
void folio_storage_unlock(void) { (void)pthread_mutex_unlock(&storage_mutex); }
static void storage_yield(void) {}
static void* storage_alloc(size_t size) { return malloc(size); }
#endif

#define STORAGE_WAIT_MS 500
__attribute__((weak, noinline)) bool folio_storage_cancelled(void) { return false; }
__attribute__((weak, noinline)) bool folio_io_cancelled(void) { return false; }
typedef struct {
    uint64_t device, inode, bytes;
    int64_t modified, changed;
    uint32_t modified_ns, changed_ns, mode;
} identity_t;
typedef struct {
    char path[FOLIO_STORAGE_PATH_CAP];
    identity_t identity;
    bool directory;
} record_t;
typedef struct { record_t* records; size_t count, capacity; } inventory_t;
typedef struct { uint64_t first, second; } digest_t;
static folio_storage_preview_t pending_preview;
static digest_t pending_digest;
static uint64_t next_token;
static bool preview_valid;

static bool fail(char* error, size_t cap, const char* message) {
    if (error && cap) snprintf(error, cap, "%s", message);
    return false;
}
static void clear_error(char* error, size_t cap) { if (error && cap) error[0] = '\0'; }
static bool cancelled(char* error, size_t cap) {
    if (folio_storage_cancelled() || folio_io_cancelled()) {
        fail(error, cap, "存储操作已取消"); return true;
    }
    return false;
}
static bool begin(char* error, size_t cap) {
    clear_error(error, cap);
    return folio_storage_lock(STORAGE_WAIT_MS) || fail(error, cap, "TF 卡正忙，请稍后重试。");
}

bool folio_storage_normalize(const char* path, char* output, size_t output_cap,
                             char* error, size_t error_cap) {
    clear_error(error, error_cap);
    char normalized[FOLIO_STORAGE_PATH_CAP];
    if (!path || !output || !output_cap || path[0] != '/')
        return fail(error, error_cap, "TF 卡路径无效。");
    size_t used = 0, position = 0;
    while (path[position]) {
        if (path[position] == '/') { ++position; continue; }
        size_t start = position;
        while (path[position] && path[position] != '/') {
            unsigned char c = (unsigned char)path[position++];
            if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == '"')
                return fail(error, error_cap, "TF 卡路径包含不允许的字符。");
            if (position >= FOLIO_STORAGE_PATH_CAP * 2)
                return fail(error, error_cap, "TF 卡路径过长。");
        }
        size_t size = position - start;
        if (size == 1 && path[start] == '.') continue;
        if (size == 2 && path[start] == '.' && path[start + 1] == '.')
            return fail(error, error_cap, "路径中不能包含上级目录跳转。");
        /* FatFs strips trailing spaces/dots. Do not accept aliases such as
         * ".. " that could acquire traversal semantics after normalization. */
        if (path[position - 1] == ' ' || path[position - 1] == '.')
            return fail(error, error_cap, "文件名不能以空格或句点结尾。");
        if (used + size + 1 >= sizeof(normalized)) return fail(error, error_cap, "TF 卡路径过长。");
        normalized[used++] = '/';
        memcpy(normalized + used, path + start, size); used += size;
    }
    normalized[used] = '\0';
    size_t root_size = strlen(FOLIO_SD_ROOT);
    if (!root_size || strncmp(normalized, FOLIO_SD_ROOT, root_size) ||
        (normalized[root_size] && normalized[root_size] != '/'))
        return fail(error, error_cap, "只能操作 TF 卡内的文件。");
    unsigned depth = 0;
    for (size_t i = root_size; i < used; ++i) if (normalized[i] == '/') ++depth;
    if (depth > FOLIO_STORAGE_DEPTH_LIMIT) return fail(error, error_cap, "文件夹嵌套超过 24 层。");
    if (used >= output_cap) return fail(error, error_cap, "路径缓冲区不足。");
    memcpy(output, normalized, used + 1);
    return true;
}

static bool regular_or_directory(const struct stat* info) {
    return S_ISREG(info->st_mode) || S_ISDIR(info->st_mode);
}
static identity_t identity(const struct stat* info) {
    identity_t result = { .device = (uint64_t)info->st_dev, .inode = (uint64_t)info->st_ino,
        .bytes = (uint64_t)info->st_size, .modified = (int64_t)info->st_mtime,
        .changed = (int64_t)info->st_ctime, .mode = (uint32_t)info->st_mode };
#if defined(__APPLE__)
    result.modified_ns = (uint32_t)info->st_mtimespec.tv_nsec;
    result.changed_ns = (uint32_t)info->st_ctimespec.tv_nsec;
#elif !defined(ESP_PLATFORM)
    result.modified_ns = (uint32_t)info->st_mtim.tv_nsec;
    result.changed_ns = (uint32_t)info->st_ctim.tv_nsec;
#endif
    return result;
}
static bool same_identity(identity_t a, identity_t b, bool directory_after_children) {
    if (a.device != b.device || a.inode != b.inode || a.mode != b.mode) return false;
    return directory_after_children || (a.bytes == b.bytes && a.modified == b.modified &&
        a.changed == b.changed && a.modified_ns == b.modified_ns && a.changed_ns == b.changed_ns);
}

#ifndef ESP_PLATFORM
/* Each host path component is opened independently without following links.
 * FAT has no symlinks; the ESP branch below validates every directory too. */
static int safe_parent(const char* path, char* leaf) {
    int directory = open(FOLIO_SD_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) return -1;
    const char* remaining = path + strlen(FOLIO_SD_ROOT);
    if (*remaining == '/') ++remaining;
    while (true) {
        const char* slash = strchr(remaining, '/');
        size_t length = slash ? (size_t)(slash - remaining) : strlen(remaining);
        memcpy(leaf, remaining, length); leaf[length] = '\0';
        if (!slash) return directory;
        int child = openat(directory, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        int saved = errno; (void)close(directory); errno = saved;
        if (child < 0) return -1;
        directory = child; remaining = slash + 1;
    }
}
static bool safe_stat(const char* path, struct stat* info) {
    char leaf[FOLIO_STORAGE_PATH_CAP];
    int parent = safe_parent(path, leaf);
    if (parent < 0) return false;
    int status = leaf[0] ? fstatat(parent, leaf, info, AT_SYMLINK_NOFOLLOW) : fstat(parent, info);
    int saved = errno; (void)close(parent); errno = saved;
    if (status != 0) return false;
    if (!regular_or_directory(info)) { errno = EINVAL; return false; }
    return true;
}
static DIR* safe_directory(const char* path) {
    char leaf[FOLIO_STORAGE_PATH_CAP];
    int parent = safe_parent(path, leaf);
    if (parent < 0) return NULL;
    int descriptor = leaf[0] ? openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : dup(parent);
    int saved = errno; (void)close(parent); errno = saved;
    if (descriptor < 0) return NULL;
    DIR* directory = fdopendir(descriptor);
    if (!directory) (void)close(descriptor);
    return directory;
}
static FILE* safe_file(const char* path) {
    char leaf[FOLIO_STORAGE_PATH_CAP];
    int parent = safe_parent(path, leaf);
    if (parent < 0) return NULL;
    int descriptor = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    int saved = errno; (void)close(parent); errno = saved;
    if (descriptor < 0) return NULL;
    struct stat info;
    if (fstat(descriptor, &info) || !S_ISREG(info.st_mode)) { (void)close(descriptor); return NULL; }
    FILE* file = fdopen(descriptor, "rb");
    if (!file) (void)close(descriptor);
    return file;
}
static bool safe_remove(const record_t* record) {
    char leaf[FOLIO_STORAGE_PATH_CAP];
    int parent = safe_parent(record->path, leaf);
    if (parent < 0 || !leaf[0]) { if (parent >= 0) (void)close(parent); return false; }
    struct stat info;
    bool okay = fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW) == 0 && regular_or_directory(&info) &&
        same_identity(record->identity, identity(&info), record->directory);
    if (okay) okay = unlinkat(parent, leaf, record->directory ? AT_REMOVEDIR : 0) == 0;
    (void)close(parent);
    return okay;
}
#else
static bool safe_stat(const char* path, struct stat* info) {
    char prefix[FOLIO_STORAGE_PATH_CAP];
    size_t length = strlen(path), root = strlen(FOLIO_SD_ROOT);
    for (size_t i = root; i <= length; ++i) {
        if (i != length && path[i] != '/') continue;
        memcpy(prefix, path, i); prefix[i] = '\0';
        if (stat(prefix, info) || !regular_or_directory(info)) return false;
        if (i != length && !S_ISDIR(info->st_mode)) return false;
    }
    return true;
}
static DIR* safe_directory(const char* path) {
    struct stat info;
    return safe_stat(path, &info) && S_ISDIR(info.st_mode) ? opendir(path) : NULL;
}
static FILE* safe_file(const char* path) {
    struct stat info;
    return safe_stat(path, &info) && S_ISREG(info.st_mode) ? fopen(path, "rb") : NULL;
}
static bool safe_remove(const record_t* record) {
    struct stat info;
    if (!safe_stat(record->path, &info) || !same_identity(record->identity, identity(&info), record->directory))
        return false;
    return (record->directory ? rmdir(record->path) : unlink(record->path)) == 0;
}
#endif

bool folio_storage_validate_parent(const char* path, char* error, size_t error_cap) {
    if (!begin(error, error_cap)) return false;
    bool okay = false;
    char normalized[FOLIO_STORAGE_PATH_CAP], parent[FOLIO_STORAGE_PATH_CAP];
    struct stat info;
    if (!folio_storage_normalize(path, normalized, sizeof(normalized), error, error_cap)) goto done;
    if (!strcmp(normalized, FOLIO_SD_ROOT)) { fail(error, error_cap, "不能将 TF 卡根目录作为文件写入。"); goto done; }
    strcpy(parent, normalized); *strrchr(parent, '/') = '\0';
    if (!safe_stat(parent, &info) || !S_ISDIR(info.st_mode)) {
        fail(error, error_cap, "文件夹不存在，或不是安全目录。"); goto done;
    }
    if (!safe_stat(normalized, &info) && errno != ENOENT) {
        fail(error, error_cap, "目标不安全，或无法读取。"); goto done;
    }
    okay = true;
done:
    folio_storage_unlock(); return okay;
}

bool folio_storage_space(uint64_t* capacity_bytes, uint64_t* free_bytes, char* error, size_t error_cap) {
    if (capacity_bytes) *capacity_bytes = 0;
    if (free_bytes) *free_bytes = 0;
    if (!capacity_bytes || !free_bytes) return fail(error, error_cap, "存储容量参数无效。");
    if (!begin(error, error_cap)) return false;
    bool okay = false;
    struct stat info;
    if (cancelled(error, error_cap)) goto done;
    if (!safe_stat(FOLIO_SD_ROOT, &info) || !S_ISDIR(info.st_mode)) {
        fail(error, error_cap, "TF 卡不可用。"); goto done;
    }
#ifdef ESP_PLATFORM
    if (esp_vfs_fat_info(FOLIO_SD_ROOT, capacity_bytes, free_bytes) != ESP_OK) {
        fail(error, error_cap, "无法读取 TF 卡可用空间。"); goto done;
    }
#else
    struct statvfs volume;
    if (statvfs(FOLIO_SD_ROOT, &volume) || !volume.f_frsize ||
        (uint64_t)volume.f_blocks > UINT64_MAX / volume.f_frsize ||
        (uint64_t)volume.f_bavail > UINT64_MAX / volume.f_frsize) {
        fail(error, error_cap, "无法读取 TF 卡可用空间。"); goto done;
    }
    *capacity_bytes = (uint64_t)volume.f_blocks * volume.f_frsize;
    *free_bytes = (uint64_t)volume.f_bavail * volume.f_frsize;
#endif
    if (*free_bytes > *capacity_bytes) { fail(error, error_cap, "TF 卡可用空间数据无效。"); goto done; }
    okay = true;
done:
    if (!okay) *capacity_bytes = *free_bytes = 0;
    folio_storage_unlock(); return okay;
}

static bool append_record(inventory_t* inventory, const char* path, const struct stat* info,
                          char* error, size_t error_cap) {
    if (inventory->count >= FOLIO_STORAGE_ENTRY_LIMIT) return fail(error, error_cap, "文件及文件夹数量超过 4096 项。");
    if (inventory->count == inventory->capacity) {
        size_t capacity = inventory->capacity ? inventory->capacity * 2 : 16;
        record_t* records = storage_alloc(capacity * sizeof(*records));
        if (!records) return fail(error, error_cap, "内存不足，无法扫描存储空间。");
        if (inventory->count) memcpy(records, inventory->records, inventory->count * sizeof(*records));
        free(inventory->records); inventory->records = records; inventory->capacity = capacity;
    }
    record_t* record = &inventory->records[inventory->count++];
    strcpy(record->path, path); record->identity = identity(info); record->directory = S_ISDIR(info->st_mode);
    return true;
}
static unsigned path_depth(const char* path) {
    unsigned depth = 0;
    for (const char* p = path + strlen(FOLIO_SD_ROOT); *p; ++p) if (*p == '/') ++depth;
    return depth;
}
static bool collect(const char* target, bool recursive, inventory_t* inventory, char* error, size_t error_cap) {
    if (cancelled(error, error_cap)) return false;
    struct stat info;
    if (!safe_stat(target, &info) || info.st_size < 0) return fail(error, error_cap, "无法读取目标，或该文件类型不安全。");
    if (!append_record(inventory, target, &info, error, error_cap)) return false;
    for (size_t position = 0; position < inventory->count; ++position) {
        if (!inventory->records[position].directory) continue;
        if (position && !recursive) break;
        /* append_record can move the array, so preserve the directory locally. */
        record_t current = inventory->records[position];
        if (path_depth(current.path) > FOLIO_STORAGE_DEPTH_LIMIT)
            return fail(error, error_cap, "文件夹嵌套超过 24 层。");
        DIR* directory = safe_directory(current.path);
        if (!directory) return fail(error, error_cap, "无法打开文件夹。");
        bool okay = true;
        while (okay) {
            if (cancelled(error, error_cap)) { okay = false; break; }
            errno = 0;
            struct dirent* item = readdir(directory);
            if (!item) { if (errno) okay = fail(error, error_cap, "无法读取文件夹。"); break; }
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
            char child[FOLIO_STORAGE_PATH_CAP], normalized[FOLIO_STORAGE_PATH_CAP];
            int n = snprintf(child, sizeof(child), "%s/%s", current.path, item->d_name);
            if (n < 0 || (size_t)n >= sizeof(child)) { okay = fail(error, error_cap, "TF 卡路径过长。"); break; }
            if (!folio_storage_normalize(child, normalized, sizeof(normalized), error, error_cap) || strcmp(child, normalized)) {
                okay = fail(error, error_cap, "文件或文件夹名称不安全。"); break;
            }
            if (path_depth(child) > FOLIO_STORAGE_DEPTH_LIMIT || !safe_stat(child, &info) ||
                info.st_size < 0 || (uint64_t)info.st_dev != current.identity.device) {
                okay = fail(error, error_cap, "文件不安全、发生变化，或无法读取。"); break;
            }
            okay = append_record(inventory, child, &info, error, error_cap);
            if (!(inventory->count % 64)) storage_yield();
        }
        if (closedir(directory)) okay = fail(error, error_cap, "关闭文件夹时发生读写错误。");
        if (!okay) return false;
        if (!safe_stat(current.path, &info) || !same_identity(current.identity, identity(&info), false))
            return fail(error, error_cap, "扫描期间文件夹发生变化，请重试。");
        storage_yield();
    }
    return true;
}
static bool suffix(const char* path, const char* extension) {
    size_t path_size = strlen(path), extension_size = strlen(extension);
    if (path_size < extension_size) return false;
    path += path_size - extension_size;
    while (*extension) if (tolower((unsigned char)*path++) != *extension++) return false;
    return true;
}
static folio_storage_category_t category(const char* path) {
    const char* relative = path + strlen(FOLIO_SD_ROOT);
    const char* caches[] = {"/.folio/cache","/.folio/weread"};
    for(unsigned i=0;i<sizeof(caches)/sizeof(caches[0]);++i) {
        size_t n=strlen(caches[i]);
        if(!strncmp(relative,caches[i],n) && (!relative[n] || relative[n]=='/'))return FOLIO_STORAGE_CACHE;
    }
    // Publication pointers are books and derived indexes are caches. Time
    // ledgers (.wrt.*), settings and history remain non-rebuildable "other".
    if(!strcmp(relative,"/.folio/catalog.a") || !strcmp(relative,"/.folio/catalog.b"))return FOLIO_STORAGE_CACHE;
    if (suffix(path, ".pdf") || suffix(path, ".epub") || suffix(path, ".txt") || suffix(path,".wrb")) return FOLIO_STORAGE_BOOK;
    if (suffix(path, ".ttf") || suffix(path, ".otf") || suffix(path, ".ttc")) return FOLIO_STORAGE_FONT;
    return FOLIO_STORAGE_OTHER;
}
static bool add_bytes(uint64_t* total, uint64_t value, char* error, size_t error_cap) {
    if (UINT64_MAX - *total < value) return fail(error, error_cap, "文件大小总计超出支持范围。");
    *total += value; return true;
}

bool folio_storage_scan(uint64_t capacity_bytes, uint64_t free_bytes,
                        folio_storage_summary_t* result, char* error, size_t error_cap) {
    if (result) memset(result, 0, sizeof(*result));
    if (!result) return fail(error, error_cap, "存储统计参数无效。");
    if (!begin(error, error_cap)) return false;
    bool okay = false;
    inventory_t inventory = {0};
    folio_storage_summary_t summary = { .capacity_bytes = capacity_bytes, .free_bytes = free_bytes };
    if (free_bytes > capacity_bytes) { fail(error, error_cap, "TF 卡容量数据无效。"); goto done; }
    summary.used_bytes = capacity_bytes - free_bytes;
    if (!collect(FOLIO_SD_ROOT, true, &inventory, error, error_cap)) goto done;
    for (size_t i = 1; i < inventory.count; ++i) {
        record_t* record = &inventory.records[i];
        if (record->directory) { ++summary.directory_count; continue; }
        ++summary.file_count;
        uint64_t* bytes;
        switch (category(record->path)) {
            case FOLIO_STORAGE_BOOK: bytes = &summary.book_bytes; break;
            case FOLIO_STORAGE_FONT: bytes = &summary.font_bytes; break;
            case FOLIO_STORAGE_CACHE: bytes = &summary.cache_bytes; break;
            default: bytes = &summary.other_bytes; break;
        }
        if (!add_bytes(bytes, record->identity.bytes, error, error_cap)) goto done;
    }
    *result = summary; okay = true;
done:
    free(inventory.records); folio_storage_unlock(); return okay;
}
static int directory_order(const void* a, const void* b) {
    const record_t* first = a; const record_t* second = b;
    if (first->directory != second->directory) return first->directory ? -1 : 1;
    return strcmp(first->path, second->path);
}
static bool visible_record(const record_t* record,const inventory_t* inventory) {
    const char* name=strrchr(record->path,'/')+1;
    if(name[0]=='.')return false;
    if(record->directory)return true;
    const char* ext=strrchr(record->path,'.');
    if(!ext || (strcasecmp(ext,".jpg") && strcasecmp(ext,".jpeg") && strcasecmp(ext,".png")))return true;
    size_t stem=(size_t)(ext-record->path);
    for(size_t i=1;i<inventory->count;++i){
        const char* path=inventory->records[i].path;
        if(!inventory->records[i].directory && strlen(path)>stem && !strncmp(path,record->path,stem) &&
            (!strcasecmp(path+stem,".pdf") || !strcasecmp(path+stem,".txt") || !strcasecmp(path+stem,".epub") || !strcasecmp(path+stem,".wrb")))return false;
    }
    return true;
}
static bool storage_list_filtered(const char* path, size_t offset, bool show_internal,bool wallpapers,
                        folio_storage_entry_t* entries, size_t entry_capacity,
                        size_t* count, size_t* total, char* error, size_t error_cap) {
    if (count) *count = 0;
    if (total) *total = 0;
    if (!count || !total || (!entries && entry_capacity)) return fail(error, error_cap, "文件列表参数无效。");
    if (!begin(error, error_cap)) return false;
    bool okay = false;
    char normalized[FOLIO_STORAGE_PATH_CAP]; inventory_t inventory = {0};
    if (!folio_storage_normalize(path, normalized, sizeof(normalized), error, error_cap) ||
        !collect(normalized, false, &inventory, error, error_cap)) goto done;
    if (!inventory.records[0].directory) { fail(error, error_cap, "所选目标不是文件夹。"); goto done; }
    size_t available = inventory.count - 1;
    if(wallpapers) {
        size_t kept=0;
        for(size_t i=1;i<inventory.count;++i) {
            record_t* r=&inventory.records[i];const char* name=strrchr(r->path,'/')+1;
            if(!r->directory && name[0]!='.' && r->identity.bytes && r->identity.bytes<=FOLIO_WALLPAPER_INPUT_MAX &&
               (suffix(name,".png") || suffix(name,".jpg") || suffix(name,".jpeg")))inventory.records[++kept]=*r;
        }
        available=kept;
    } else if(!show_internal){
        size_t kept=0;
        /* Mark first: filtering must not overwrite companions used by later checks. */
        for(size_t i=1;i<inventory.count;++i)if(!visible_record(&inventory.records[i],&inventory))inventory.records[i].path[0]=0;
        for(size_t i=1;i<inventory.count;++i)if(inventory.records[i].path[0])inventory.records[++kept]=inventory.records[i];
        available=kept;
    }
    if (available) qsort(inventory.records + 1, available, sizeof(record_t), directory_order);
    size_t written = 0;
    for (size_t i = offset; i < available && written < entry_capacity; ++i) {
        const record_t* record = &inventory.records[i + 1];
        folio_storage_entry_t* entry = &entries[written++];
        memset(entry, 0, sizeof(*entry)); strcpy(entry->path, record->path);
        strcpy(entry->name, strrchr(record->path, '/') + 1);
        entry->directory = record->directory; entry->bytes = record->directory ? 0 : record->identity.bytes;
        entry->modified = record->identity.modified; entry->category = category(record->path);
    }
    *count = written; *total = available; okay = true;
done:
    free(inventory.records); folio_storage_unlock(); return okay;
}

bool folio_storage_list_view(const char* path,size_t offset,bool show_internal,
                        folio_storage_entry_t* entries,size_t capacity,
                        size_t* count,size_t* total,char* error,size_t cap) {
    return storage_list_filtered(path,offset,show_internal,false,entries,capacity,count,total,error,cap);
}

bool folio_storage_list(const char* path,size_t offset,folio_storage_entry_t* entries,size_t capacity,
                        size_t* count,size_t* total,char* error,size_t cap) {
    return folio_storage_list_view(path,offset,true,entries,capacity,count,total,error,cap);
}

/* Only fixed, application-owned root names reach this helper. */
static bool ensure_root_directory(const char* name,char* error,size_t cap) {
    char path[FOLIO_STORAGE_PATH_CAP];struct stat info;
    int n=snprintf(path,sizeof(path),FOLIO_SD_ROOT "/%s",name);
    if(n<0 || (size_t)n>=sizeof(path) || !safe_stat(FOLIO_SD_ROOT,&info) || !S_ISDIR(info.st_mode))
        return fail(error,cap,"TF 卡根目录不安全或未就绪。");
    if(safe_stat(path,&info))return S_ISDIR(info.st_mode) || fail(error,cap,"文件夹被同名文件占用。");
    if(errno!=ENOENT)return fail(error,cap,"无法读取安全文件夹。");
#ifndef ESP_PLATFORM
    int root=open(FOLIO_SD_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(root<0)return fail(error,cap,"TF 卡未就绪。");
    bool okay=mkdirat(root,name,0777)==0 || errno==EEXIST;close(root);
#else
    bool okay=mkdir(path,0777)==0 || errno==EEXIST;
#endif
    return (okay && safe_stat(path,&info) && S_ISDIR(info.st_mode)) || fail(error,cap,"无法创建文件夹。");
}

/* -1 is unsafe/unreadable, 0 is absent, 1 is a real directory. */
static int wallpaper_directory_state(const char* path) {
    struct stat info;
    if(!safe_stat(path,&info))return errno==ENOENT?0:-1;
    return S_ISDIR(info.st_mode)?1:-1;
}
static bool wallpaper_exists(const char* path) {
    struct stat st;
    return safe_stat(path,&st) && S_ISREG(st.st_mode) && st.st_size>0 && (uint64_t)st.st_size<=FOLIO_WALLPAPER_INPUT_MAX;
}
static bool wallpaper_path(const char* root,const char* name,char* path,size_t cap) {
    if(!name || !*name || name[0]=='.' || strchr(name,'/') || strchr(name,'\\') ||
       (!suffix(name,".png") && !suffix(name,".jpg") && !suffix(name,".jpeg")))return false;
    char full[FOLIO_STORAGE_PATH_CAP],normalized[FOLIO_STORAGE_PATH_CAP];
    int n=snprintf(full,sizeof(full),"%s/%s",root,name);
    if(n<0 || (size_t)n>=sizeof(full) || (size_t)n>=cap ||
       !folio_storage_normalize(full,normalized,sizeof(normalized),NULL,0) || strcmp(full,normalized))return false;
    if(path)memcpy(path,full,(size_t)n+1);
    return true;
}
static int wallpaper_order(const void* a,const void* b) {
    const record_t* first=a;const record_t* second=b;
    int order=strcmp(strrchr(first->path,'/')+1,strrchr(second->path,'/')+1);
    return order?order:strcmp(first->path,second->path);
}
bool folio_storage_wallpapers(size_t offset,folio_storage_entry_t* entries,size_t capacity,
                             size_t* count,size_t* total,char* error,size_t cap) {
    if(count)*count=0;
    if(total)*total=0;
    if(!count || !total || (!entries && capacity))return fail(error,cap,"文件列表参数无效。");
    if(!begin(error,cap))return false;
    bool okay=false;inventory_t candidates={0},source={0};
    const char* roots[]={FOLIO_PICTURE_ROOT,FOLIO_LEGACY_WALLPAPER_ROOT};
    if(wallpaper_directory_state(FOLIO_SD_ROOT)!=1){fail(error,cap,"TF 卡未就绪。");goto done;}
    for(unsigned d=0;d<2;++d) {
        int state=wallpaper_directory_state(roots[d]);
        if(state<0){fail(error,cap,"图片目录不安全，未读取。");goto done;}
        if(!state)continue;
        if(!collect(roots[d],false,&source,error,cap))goto done;
        for(size_t i=1;i<source.count;++i) {
            record_t* r=&source.records[i];const char* name=strrchr(r->path,'/')+1;
            if(r->directory || name[0]=='.' || !r->identity.bytes || r->identity.bytes>FOLIO_WALLPAPER_INPUT_MAX ||
               (!suffix(name,".png") && !suffix(name,".jpg") && !suffix(name,".jpeg")))continue;
            struct stat info;
            if(!safe_stat(r->path,&info) || !same_identity(r->identity,identity(&info),false) ||
               !append_record(&candidates,r->path,&info,error,cap))goto done;
        }
        free(source.records);source=(inventory_t){0};
    }
    if(candidates.count)qsort(candidates.records,candidates.count,sizeof(record_t),wallpaper_order);
    size_t written=0;
    for(size_t i=offset;i<candidates.count && written<capacity;++i) {
        const record_t* r=&candidates.records[i];folio_storage_entry_t* e=&entries[written++];
        memset(e,0,sizeof(*e));strcpy(e->path,r->path);strcpy(e->name,strrchr(r->path,'/')+1);
        e->bytes=r->identity.bytes;e->modified=r->identity.modified;e->category=FOLIO_STORAGE_OTHER;
    }
    *count=written;*total=candidates.count;okay=true;
done:
    free(source.records);free(candidates.records);folio_storage_unlock();return okay;
}

#define WALLPAPER_V1_MAGIC 0x31505746u
#define WALLPAPER_V2_MAGIC 0x32505746u
enum { WALLPAPER_PICTURE=1,WALLPAPER_LEGACY=2,WALLPAPER_MIGRATING=3 };
typedef struct {
    uint32_t magic,generation;
    char name[FOLIO_STORAGE_PATH_CAP];
    uint32_t checksum;
} wallpaper_v1_t;
typedef struct {
    uint32_t magic,generation;
    char name[FOLIO_STORAGE_PATH_CAP];
    /* Fixed 32-bit fields keep the on-card layout equal on ESP and hosts. */
    uint32_t source,bytes,hash[4],checksum;
} wallpaper_record_t;
static uint32_t wallpaper_checksum(const void* data,size_t size) {
    const uint8_t* p=data;uint32_t sum=2166136261u;
    for(size_t i=0;i<size;++i)sum=(sum^p[i])*16777619u;
    return sum;
}
static bool wallpaper_record_read_checked(unsigned slot,wallpaper_record_t* record,bool* io_error) {
    if(io_error)*io_error=false;
    char path[FOLIO_STORAGE_PATH_CAP];snprintf(path,sizeof(path),FOLIO_SD_ROOT "/.folio/wallpaper.%c",'a'+slot);
    errno=0;FILE* file=safe_file(path);
    if(!file){if(io_error)*io_error=errno!=ENOENT;return false;}
    uint8_t bytes[sizeof(*record)+1];size_t got=fread(bytes,1,sizeof(bytes),file);
    bool okay=!ferror(file) && (got==sizeof(bytes) || feof(file));if(fclose(file))okay=false;
    if(!okay){if(io_error)*io_error=true;return false;}
    memset(record,0,sizeof(*record));
    if(got==sizeof(wallpaper_v1_t)) {
        wallpaper_v1_t old;memcpy(&old,bytes,sizeof(old));
        if(old.magic!=WALLPAPER_V1_MAGIC || old.checksum!=wallpaper_checksum(&old,offsetof(wallpaper_v1_t,checksum)))return false;
        record->magic=old.magic;record->generation=old.generation;memcpy(record->name,old.name,sizeof(old.name));
        record->source=WALLPAPER_LEGACY;
    } else if(got==sizeof(*record)) {
        memcpy(record,bytes,sizeof(*record));
        if(record->magic!=WALLPAPER_V2_MAGIC || record->source<WALLPAPER_PICTURE || record->source>WALLPAPER_MIGRATING ||
           record->checksum!=wallpaper_checksum(record,offsetof(wallpaper_record_t,checksum)))return false;
        if(record->source==WALLPAPER_MIGRATING && (!record->bytes || record->bytes>FOLIO_WALLPAPER_INPUT_MAX))return false;
    } else return false;
    return memchr(record->name,0,sizeof(record->name)) && wallpaper_path(
        record->source==WALLPAPER_PICTURE?FOLIO_PICTURE_ROOT:FOLIO_LEGACY_WALLPAPER_ROOT,
        record->name,NULL,FOLIO_STORAGE_PATH_CAP);
}
static bool wallpaper_record_read(unsigned slot,wallpaper_record_t* record) {
    return wallpaper_record_read_checked(slot,record,NULL);
}
static unsigned wallpaper_newest(const wallpaper_record_t records[2],const bool valid[2]) {
    return valid[1] && (!valid[0] || (int32_t)(records[1].generation-records[0].generation)>0)?1:0;
}
/* Digest only the selected file, never a book. Migration may resolve a new
 * directory only when all original bytes match, not merely the basename. */
static bool wallpaper_fingerprint_bounded(const char* path,wallpaper_record_t* record,uint32_t limit) {
    struct stat before,after;
    if(!wallpaper_exists(path) || !safe_stat(path,&before) || (uint64_t)before.st_size>limit)return false;
    FILE* file=safe_file(path);if(!file)return false;
    uint64_t first=UINT64_C(14695981039346656037),second=UINT64_C(0x243f6a8885a308d3);
    uint8_t buffer[4096];uint32_t total=0;struct stat opened;
    bool okay=fstat(fileno(file),&opened)==0 && S_ISREG(opened.st_mode) && opened.st_size==before.st_size;
#ifndef ESP_PLATFORM
    if(okay)okay=same_identity(identity(&before),identity(&opened),false);
#endif
    while(okay) {
        if(cancelled(NULL,0)){okay=false;break;}
        size_t got=fread(buffer,1,sizeof(buffer),file);
        if(!got){if(ferror(file))okay=false;break;}
        if(got>limit-total){okay=false;break;}
        total+=(uint32_t)got;
        for(size_t i=0;i<got;++i) {
            first=(first^buffer[i])*UINT64_C(1099511628211);
            second=(second^buffer[i])*UINT64_C(0x9e3779b185ebca87);
            second=(second<<27)|(second>>37);
        }
        storage_yield();
    }
    if(fclose(file))okay=false;
    if(!okay || total!=(uint64_t)before.st_size || !safe_stat(path,&after) ||
       !same_identity(identity(&before),identity(&after),false))return false;
    record->bytes=total;
    record->hash[0]=(uint32_t)first;record->hash[1]=(uint32_t)(first>>32);
    record->hash[2]=(uint32_t)second;record->hash[3]=(uint32_t)(second>>32);
    return true;
}
static bool wallpaper_fingerprint(const char* path,wallpaper_record_t* record) {
    return wallpaper_fingerprint_bounded(path,record,FOLIO_WALLPAPER_INPUT_MAX);
}
static bool wallpaper_matches(const char* path,const wallpaper_record_t* record) {
    wallpaper_record_t actual={0};struct stat info;
    /* A small legacy record must never make shutdown hash a replaced large file. */
    return safe_stat(path,&info) && info.st_size>0 && (uint64_t)info.st_size==record->bytes &&
        wallpaper_fingerprint_bounded(path,&actual,record->bytes) && actual.bytes==record->bytes &&
        !memcmp(actual.hash,record->hash,sizeof(actual.hash));
}
static bool wallpaper_record_plan(wallpaper_record_t* record,unsigned* slot,char* error,size_t cap) {
    wallpaper_record_t records[2];bool io[2];
    bool valid[2]={wallpaper_record_read_checked(0,&records[0],&io[0]),wallpaper_record_read_checked(1,&records[1],&io[1])};
    /* Missing/corrupt inactive slots are recoverable; unreadable slots are not
     * proof that they are inactive. Never overwrite a possibly active cache. */
    if(io[0] || io[1])return fail(error,cap,"无法读取壁纸选择记录，原选择保留。");
    unsigned newest=wallpaper_newest(records,valid);*slot=valid[newest]?1-newest:0;
    record->magic=WALLPAPER_V2_MAGIC;record->generation=valid[newest]?records[newest].generation+1:1;
    record->checksum=wallpaper_checksum(record,offsetof(wallpaper_record_t,checksum));
    return true;
}
static FILE* wallpaper_safe_write(const char* path) {
#ifndef ESP_PLATFORM
    char leaf[FOLIO_STORAGE_PATH_CAP];int parent=safe_parent(path,leaf);
    /* Never truncate symlinks, special files or hard links to user data. */
    int fd=parent<0?-1:openat(parent,leaf,O_WRONLY|O_CREAT|O_NOFOLLOW|O_NONBLOCK,0600);
    if(parent>=0)close(parent);
    struct stat info;
    if(fd>=0 && (fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_nlink!=1 || ftruncate(fd,0))) {close(fd);fd=-1;}
    FILE* file=fd<0?NULL:fdopen(fd,"wb");if(fd>=0 && !file)close(fd);
    return file;
#else
    return fopen(path,"wb");
#endif
}
static bool wallpaper_sync_parent(const char* path) {
#ifndef ESP_PLATFORM
    char leaf[FOLIO_STORAGE_PATH_CAP];int parent=safe_parent(path,leaf);
    bool okay=parent>=0 && fsync(parent)==0;
    if(parent>=0)close(parent);
    return okay;
#else
    (void)path;return true;
#endif
}
static bool wallpaper_record_commit(const wallpaper_record_t* record,unsigned slot,char* error,size_t cap) {
    char target[FOLIO_STORAGE_PATH_CAP];snprintf(target,sizeof(target),FOLIO_SD_ROOT "/.folio/wallpaper.%c",'a'+slot);
    if(cancelled(error,cap) || !folio_storage_validate_parent(target,error,cap))return false;
    FILE* file=wallpaper_safe_write(target);
    if(!file)return fail(error,cap,"无法安全保存壁纸选择，原选择保留。");
    /* Once record bytes may have reached the card, a false result is uncertain.
     * Never promise the old choice survived an fsync/close/readback failure. */
    bool okay=fwrite(record,1,sizeof(*record),file)==sizeof(*record) && fflush(file)==0 && fsync(fileno(file))==0;
    if(fclose(file))okay=false;
    wallpaper_record_t verify;
    okay=okay && wallpaper_record_read(slot,&verify) && !memcmp(record,&verify,sizeof(*record));
    if(okay)okay=wallpaper_sync_parent(target);
    return okay || fail(error,cap,"壁纸选择提交结果不确定，请重新打开壁纸列表核对。");
}
static bool wallpaper_record_write(wallpaper_record_t* record,char* error,size_t cap) {
    if(cancelled(error,cap) || !ensure_root_directory(".folio",error,cap))return false;
    unsigned slot;
    if(!wallpaper_record_plan(record,&slot,error,cap))return false;
    return wallpaper_record_commit(record,slot,error,cap);
}
static bool wallpaper_resolve(const wallpaper_record_t* record,char* path,size_t cap) {
    const char* root=record->source==WALLPAPER_PICTURE?FOLIO_PICTURE_ROOT:FOLIO_LEGACY_WALLPAPER_ROOT;
    if(record->source==WALLPAPER_MIGRATING) {
        if(wallpaper_path(root,record->name,path,cap) && wallpaper_matches(path,record))return true;
        /* A recreated legacy directory must not hide the migrated original.
         * Content identity, never directory absence, authorizes this alias. */
        return wallpaper_path(FOLIO_PICTURE_ROOT,record->name,path,cap) && wallpaper_matches(path,record);
    }
    return wallpaper_path(root,record->name,path,cap) && wallpaper_exists(path);
}
#define WALLPAPER_LEGACY_INPUT_MAX (2u*1024u*1024u)
static bool wallpaper_load(char* path,size_t cap,bool legacy_only) {
    if(!path || !cap)return false;
    path[0]=0;
    if(!begin(NULL,0))return false;
    wallpaper_record_t records[2];bool io[2];
    bool valid[2]={wallpaper_record_read_checked(0,&records[0],&io[0]),wallpaper_record_read_checked(1,&records[1],&io[1])};
    unsigned slot=wallpaper_newest(records,valid);
    bool okay=false;
    if(io[0] || io[1])goto done;
    if(legacy_only && valid[slot] && records[slot].source==WALLPAPER_MIGRATING &&
       records[slot].bytes>WALLPAPER_LEGACY_INPUT_MAX)goto done;
    okay=valid[slot] && wallpaper_resolve(&records[slot],path,cap);
    /* An unresolved legacy choice must not select a different picture by name.
     * Keep its durable record and let the caller show the static default. */
    if(!okay && (!valid[slot] || records[slot].source==WALLPAPER_PICTURE)) {
        folio_storage_entry_t entry;size_t count,total;
        bool listed=valid[slot]
            ?storage_list_filtered(FOLIO_PICTURE_ROOT,0,false,true,&entry,1,&count,&total,NULL,0)
            :folio_storage_wallpapers(0,&entry,1,&count,&total,NULL,0);
        okay=listed && count && strlen(entry.path)<cap;
        if(okay)strcpy(path,entry.path);
    }
    if(okay && legacy_only) {
        struct stat info;
        okay=safe_stat(path,&info) && info.st_size>0 && (uint64_t)info.st_size<=WALLPAPER_LEGACY_INPUT_MAX;
    }
done:
    if(!okay)path[0]=0;
    folio_storage_unlock();return okay;
}
bool folio_storage_wallpaper_load(char* path,size_t cap) {return wallpaper_load(path,cap,false);}
bool folio_storage_wallpaper_load_legacy(char* path,size_t cap) {return wallpaper_load(path,cap,true);}
bool folio_storage_wallpaper_save(const char* path,char* error,size_t cap) {
    if(!begin(error,cap))return false;
    bool okay=false;char checked[FOLIO_STORAGE_PATH_CAP];wallpaper_record_t record={0};
    const char* name=path?strrchr(path,'/'):NULL;
    if(name && wallpaper_path(FOLIO_PICTURE_ROOT,name+1,checked,sizeof(checked)) && !strcmp(path,checked))record.source=WALLPAPER_PICTURE;
    else if(name && wallpaper_path(FOLIO_LEGACY_WALLPAPER_ROOT,name+1,checked,sizeof(checked)) && !strcmp(path,checked))record.source=WALLPAPER_LEGACY;
    if(!record.source || !wallpaper_exists(checked)) {
        fail(error,cap,"请选择 picture（或保留的 wallpapers）目录中不超过 32 MiB 的 PNG 或 JPEG 图片。");goto done;
    }
    strcpy(record.name,name+1);okay=wallpaper_record_write(&record,error,cap);
done:
    folio_storage_unlock();return okay;
}
/* Prepared wallpaper snapshots have their own slots. The existing selection
 * record layout stays unchanged; its slot/generation is the publication point.
 * Source size/mtime invalidation is not proof of current byte identity. */
#define WALLPAPER_CACHE_MAGIC 0x31435746u
#define WALLPAPER_CACHE_VERSION 1u
#define WALLPAPER_CACHE_LAYOUT 1u /* Row-major gray4, left pixel in high nibble. */
typedef struct {
    uint32_t magic,version,layout,slot,generation,source,width,height,stride,bytes;
    char name[FOLIO_STORAGE_PATH_CAP];
    uint32_t source_bytes,source_modified[2],source_modified_ns;
    uint32_t pixels_crc,header_crc;
} wallpaper_cache_t;
_Static_assert(sizeof(wallpaper_cache_t)==320,"Stable wallpaper cache header layout");
static uint32_t wallpaper_crc32(uint32_t crc,const void* data,size_t size) {
    const uint8_t* bytes=data;crc=~crc;
    while(size--) {
        crc^=*bytes++;
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    return ~crc;
}
static bool wallpaper_cache_dimensions(unsigned width,unsigned height) {
    return width && height && width<=FOLIO_WALLPAPER_TARGET_MAX_W && height<=FOLIO_WALLPAPER_TARGET_MAX_H;
}
static void* wallpaper_cache_alloc(size_t size) {
#ifdef ESP_PLATFORM
    /* A full-screen snapshot must not consume the small internal DMA heap. */
    return heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}
static void wallpaper_cache_yield(size_t processed) {
    /* Check cancellation every 4 KiB, but spend at most one RTOS tick per 64 KiB. */
    if(!(processed&65535u))storage_yield();
}
static void wallpaper_cache_path(unsigned slot,char path[FOLIO_STORAGE_PATH_CAP]) {
    snprintf(path,FOLIO_STORAGE_PATH_CAP,FOLIO_SD_ROOT "/.folio/wallpaper-cache.%c",'a'+slot);
}
static bool wallpaper_cache_source(const wallpaper_cache_t* header,identity_t* found) {
    char path[FOLIO_STORAGE_PATH_CAP];struct stat info;
    const char* root=header->source==WALLPAPER_PICTURE?FOLIO_PICTURE_ROOT:FOLIO_LEGACY_WALLPAPER_ROOT;
    if(!wallpaper_path(root,header->name,path,sizeof(path)) || !safe_stat(path,&info) ||
       !S_ISREG(info.st_mode) || info.st_size<=0 || (uint64_t)info.st_size>FOLIO_WALLPAPER_INPUT_MAX)return false;
    identity_t actual=identity(&info);
    uint64_t modified=(uint64_t)header->source_modified[0]|((uint64_t)header->source_modified[1]<<32);
    if(actual.bytes!=header->source_bytes || (uint64_t)actual.modified!=modified ||
       actual.modified_ns!=header->source_modified_ns)return false;
    if(found)*found=actual;
    return true;
}
static bool wallpaper_cache_read(unsigned slot,const wallpaper_record_t* record,const wallpaper_cache_t* expected,
                                 uint8_t** output,unsigned* width,unsigned* height) {
    if(record->magic!=WALLPAPER_V2_MAGIC || record->source==WALLPAPER_MIGRATING)return false;
    char path[FOLIO_STORAGE_PATH_CAP];wallpaper_cache_path(slot,path);
    FILE* file=safe_file(path);if(!file)return false;
    wallpaper_cache_t header;struct stat info;identity_t source_before,source_after;
    uint8_t* pixels=NULL;uint8_t buffer[4096];uint32_t crc=0;size_t position=0;
    bool okay=fstat(fileno(file),&info)==0 && S_ISREG(info.st_mode);
#ifndef ESP_PLATFORM
    okay=okay && info.st_nlink==1;
#endif
    okay=okay && fread(&header,1,sizeof(header),file)==sizeof(header) &&
        header.magic==WALLPAPER_CACHE_MAGIC && header.version==WALLPAPER_CACHE_VERSION &&
        header.layout==WALLPAPER_CACHE_LAYOUT && header.slot==slot && header.generation==record->generation &&
        header.source==record->source && !memcmp(header.name,record->name,sizeof(header.name)) &&
        wallpaper_cache_dimensions(header.width,header.height) && header.stride==(header.width+1u)/2u &&
        header.bytes==header.stride*header.height &&
        (uint64_t)info.st_size==sizeof(header)+(uint64_t)header.bytes &&
        header.header_crc==wallpaper_crc32(0,&header,offsetof(wallpaper_cache_t,header_crc)) &&
        (!expected || !memcmp(expected,&header,sizeof(header))) && wallpaper_cache_source(&header,&source_before);
    if(okay && output) {pixels=wallpaper_cache_alloc(header.bytes);okay=pixels!=NULL;}
    while(okay && position<header.bytes) {
        if(cancelled(NULL,0)){okay=false;break;}
        size_t count=header.bytes-position<sizeof(buffer)?header.bytes-position:sizeof(buffer);
        uint8_t* block=pixels?pixels+position:buffer;
        if(fread(block,1,count,file)!=count){okay=false;break;}
        crc=wallpaper_crc32(crc,block,count);position+=count;wallpaper_cache_yield(position);
    }
    okay=okay && crc==header.pixels_crc && fgetc(file)==EOF && !ferror(file) &&
        wallpaper_cache_source(&header,&source_after) && same_identity(source_before,source_after,false);
    if(fclose(file))okay=false;
    if(okay && output) {*output=pixels;*width=header.width;*height=header.height;}
    else free(pixels);
    return okay;
}
bool folio_storage_wallpaper_cache_load(uint8_t** pixels,unsigned* width,unsigned* height) {
    if(pixels)*pixels=NULL;
    if(width)*width=0;
    if(height)*height=0;
    if(!pixels || !width || !height || !begin(NULL,0))return false;
    wallpaper_record_t records[2];bool io[2];
    bool valid[2]={wallpaper_record_read_checked(0,&records[0],&io[0]),wallpaper_record_read_checked(1,&records[1],&io[1])};
    unsigned slot=wallpaper_newest(records,valid);
    bool okay=!io[0] && !io[1] && !cancelled(NULL,0) && valid[slot] &&
        wallpaper_cache_read(slot,&records[slot],NULL,pixels,width,height);
    folio_storage_unlock();return okay;
}
bool folio_storage_wallpaper_save_prepared(const char* path,const uint8_t* pixels,unsigned width,unsigned height,
                                           char* error,size_t cap) {
    if(!begin(error,cap))return false;
    bool okay=false;char checked[FOLIO_STORAGE_PATH_CAP];wallpaper_record_t record={0};struct stat info,after;
    const char* name=path?strrchr(path,'/'):NULL;
    if(cancelled(error,cap))goto done;
    if(!pixels || !wallpaper_cache_dimensions(width,height)) {
        fail(error,cap,"壁纸缓存尺寸无效，原选择保留。");goto done;
    }
    if(name && wallpaper_path(FOLIO_PICTURE_ROOT,name+1,checked,sizeof(checked)) && !strcmp(path,checked))record.source=WALLPAPER_PICTURE;
    else if(name && wallpaper_path(FOLIO_LEGACY_WALLPAPER_ROOT,name+1,checked,sizeof(checked)) && !strcmp(path,checked))record.source=WALLPAPER_LEGACY;
    if(!record.source || !wallpaper_exists(checked) || !safe_stat(checked,&info)) {
        fail(error,cap,"请选择 picture（或保留的 wallpapers）目录中不超过 32 MiB 的 PNG 或 JPEG 图片。");goto done;
    }
    strcpy(record.name,name+1);
    if(!ensure_root_directory(".folio",error,cap))goto done;
    unsigned slot;
    if(!wallpaper_record_plan(&record,&slot,error,cap))goto done;
    identity_t source=identity(&info);
    wallpaper_cache_t header={.magic=WALLPAPER_CACHE_MAGIC,.version=WALLPAPER_CACHE_VERSION,.layout=WALLPAPER_CACHE_LAYOUT,
        .slot=slot,.generation=record.generation,.source=record.source,.width=width,.height=height,.stride=(width+1u)/2u,
        .bytes=((width+1u)/2u)*height,.source_bytes=(uint32_t)source.bytes,
        .source_modified={(uint32_t)source.modified,(uint32_t)((uint64_t)source.modified>>32)},.source_modified_ns=source.modified_ns};
    memcpy(header.name,record.name,sizeof(header.name));
    for(size_t pos=0;pos<header.bytes;) {
        if(cancelled(error,cap))goto done;
        size_t count=header.bytes-pos<4096?header.bytes-pos:4096;
        header.pixels_crc=wallpaper_crc32(header.pixels_crc,pixels+pos,count);pos+=count;wallpaper_cache_yield(pos);
    }
    header.header_crc=wallpaper_crc32(0,&header,offsetof(wallpaper_cache_t,header_crc));
    char target[FOLIO_STORAGE_PATH_CAP];wallpaper_cache_path(slot,target);
    if(cancelled(error,cap) || !folio_storage_validate_parent(target,error,cap))goto done;
    FILE* file=wallpaper_safe_write(target);
    if(!file){fail(error,cap,"无法安全写入壁纸缓存，原选择保留。");goto done;}
    bool written=fwrite(&header,1,sizeof(header),file)==sizeof(header);
    for(size_t pos=0;written && pos<header.bytes;) {
        if(cancelled(error,cap)){written=false;break;}
        size_t count=header.bytes-pos<4096?header.bytes-pos:4096;
        written=fwrite(pixels+pos,1,count,file)==count;pos+=count;wallpaper_cache_yield(pos);
    }
    written=written && fflush(file)==0 && fsync(fileno(file))==0;
    if(fclose(file))written=false;
    if(written)written=wallpaper_sync_parent(target) && wallpaper_cache_read(slot,&record,&header,NULL,NULL,NULL);
    if(!written){fail(error,cap,"壁纸缓存未完成或操作已取消，原选择保留。");goto done;}
    if(cancelled(error,cap))goto done;
    if(!safe_stat(checked,&after) || !same_identity(source,identity(&after),false)) {
        fail(error,cap,"图片在准备期间发生变化，原选择保留，请重新选择。");goto done;
    }
    /* Only now publish the already synced, reread snapshot. */
    okay=wallpaper_record_commit(&record,slot,error,cap);
done:
    folio_storage_unlock();return okay;
}

/* Both paths are fixed direct children of the validated root. Host rename is
 * explicitly no-replace; ESP-IDF's FAT VFS calls f_rename, which rejects FR_EXIST.
 * All firmware writers serialize on the recursive storage mutex. */
static bool wallpaper_rename(const identity_t* expected) {
#ifndef ESP_PLATFORM
    int root=open(FOLIO_SD_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(root<0)return false;
    struct stat info;bool okay=fstatat(root,"wallpapers",&info,AT_SYMLINK_NOFOLLOW)==0 &&
        S_ISDIR(info.st_mode) && same_identity(*expected,identity(&info),false);
    if(okay) {
#if defined(__APPLE__)
        okay=renameatx_np(root,"wallpapers",root,FOLIO_PICTURE_DIRECTORY,RENAME_EXCL)==0;
#elif defined(__linux__) && defined(SYS_renameat2)
        okay=syscall(SYS_renameat2,root,"wallpapers",root,FOLIO_PICTURE_DIRECTORY,1u)==0; /* RENAME_NOREPLACE */
#else
        okay=false; /* Never substitute an overwriting POSIX rename. */
#endif
    }
    /* Sync namespace changes where the host filesystem supports it. */
    if(okay)(void)fsync(root);
    close(root);return okay;
#else
    struct stat info;
    return safe_stat(FOLIO_LEGACY_WALLPAPER_ROOT,&info) && S_ISDIR(info.st_mode) &&
        same_identity(*expected,identity(&info),false) && wallpaper_directory_state(FOLIO_PICTURE_ROOT)==0 &&
        rename(FOLIO_LEGACY_WALLPAPER_ROOT,FOLIO_PICTURE_ROOT)==0;
#endif
}
bool folio_storage_prepare_wallpapers(char* error,size_t cap) {
    if(!begin(error,cap))return false;
    bool okay=false;inventory_t inventory={0};
    if(cancelled(error,cap))goto done;
    if(wallpaper_directory_state(FOLIO_SD_ROOT)!=1){fail(error,cap,"TF 卡根目录不安全或未就绪。");goto done;}
    int picture=wallpaper_directory_state(FOLIO_PICTURE_ROOT),legacy=wallpaper_directory_state(FOLIO_LEGACY_WALLPAPER_ROOT);
    if(picture<0 || legacy<0){fail(error,cap,"picture 或 wallpapers 被文件/链接占用，未迁移，原内容保留。");goto done;}
    if(picture && legacy){fail(error,cap,"picture 与 wallpapers 同时存在，未迁移；两个目录和原选择均保留。");goto done;}
    if(!legacy){okay=ensure_root_directory(FOLIO_PICTURE_DIRECTORY,error,cap);goto done;}
    /* Reject unsafe descendants before the only namespace mutation. No file
     * contents are rewritten, merged or removed, including non-image files. */
    if(!collect(FOLIO_LEGACY_WALLPAPER_ROOT,true,&inventory,error,cap))goto done;
    wallpaper_record_t records[2];bool valid[2]={wallpaper_record_read(0,&records[0]),wallpaper_record_read(1,&records[1])};
    unsigned slot=wallpaper_newest(records,valid);
    if(valid[slot] && records[slot].source==WALLPAPER_PICTURE) {
        char conflicting[FOLIO_STORAGE_PATH_CAP];
        if(wallpaper_path(FOLIO_LEGACY_WALLPAPER_ROOT,records[slot].name,conflicting,sizeof(conflicting)) && wallpaper_exists(conflicting)) {
            fail(error,cap,"picture 原选择缺失且旧目录有同名图片，未迁移，原内容保留。");goto done;
        }
    }
    if(valid[slot] && records[slot].source!=WALLPAPER_PICTURE) {
        wallpaper_record_t pending=records[slot];char selected[FOLIO_STORAGE_PATH_CAP];
        if(wallpaper_path(FOLIO_LEGACY_WALLPAPER_ROOT,pending.name,selected,sizeof(selected)) && wallpaper_exists(selected)) {
            if(pending.source==WALLPAPER_MIGRATING) {
                if(!wallpaper_matches(selected,&pending)){fail(error,cap,"原选中图片已变化，未迁移，原目录保留。");goto done;}
            } else {
                if(!wallpaper_fingerprint(selected,&pending)){fail(error,cap,"无法验证原选中图片，未迁移。");goto done;}
                pending.source=WALLPAPER_MIGRATING;
                if(!wallpaper_record_write(&pending,error,cap))goto done;
            }
        }
    }
    if(cancelled(error,cap))goto done;
    /* Revalidate every inventoried path after journaling; reject links/changes. */
    for(size_t i=0;i<inventory.count;++i) {
        struct stat info;record_t* r=&inventory.records[i];
        if(!safe_stat(r->path,&info) || !same_identity(r->identity,identity(&info),false)) {
            fail(error,cap,"迁移前目录发生变化，原内容保留，请重试。");goto done;
        }
    }
    if(!wallpaper_rename(&inventory.records[0].identity)){fail(error,cap,"wallpapers 未迁移，未覆盖任何文件，请重试。");goto done;}
    okay=true;
done:
    free(inventory.records);folio_storage_unlock();return okay;
}

static int path_order(const void* a, const void* b) { return strcmp(((const record_t*)a)->path, ((const record_t*)b)->path); }
static void hash_bytes(digest_t* digest, const void* input, size_t size) {
    const uint8_t* bytes = input;
    while (size--) {
        digest->first = (digest->first ^ *bytes) * UINT64_C(1099511628211);
        digest->second ^= *bytes++;
        digest->second *= UINT64_C(0x9e3779b185ebca87);
        digest->second = (digest->second << 27) | (digest->second >> 37);
    }
}
static void hash_number(digest_t* digest, uint64_t number) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i) { bytes[i] = (uint8_t)number; number >>= 8; }
    hash_bytes(digest, bytes, sizeof(bytes));
}
static bool opened_file_matches(FILE* file, const record_t* record) {
    struct stat info;
    if (fstat(fileno(file), &info)) return false;
#ifdef ESP_PLATFORM
    /* IDF FAT's fstat intentionally zeroes all timestamps. Validate the open
     * file length/type and separately verify path metadata before and after. */
    return S_ISREG(info.st_mode) && info.st_size >= 0 && (uint64_t)info.st_size == record->identity.bytes;
#else
    return same_identity(record->identity, identity(&info), false);
#endif
}
static bool fingerprint(inventory_t* inventory, digest_t* digest, folio_storage_preview_t* preview,
                         char* error, size_t error_cap) {
    qsort(inventory->records, inventory->count, sizeof(record_t), path_order);
    *digest = (digest_t){UINT64_C(14695981039346656037), UINT64_C(0x243f6a8885a308d3)};
    uint8_t buffer[4096];
    preview->bytes = 0; preview->file_count = 0; preview->directory_count = 0;
    for (size_t i = 0; i < inventory->count; ++i) {
        if (cancelled(error, error_cap)) return false;
        const record_t* record = &inventory->records[i];
        const identity_t* id = &record->identity;
        hash_bytes(digest, record->path, strlen(record->path) + 1);
        hash_number(digest, id->device); hash_number(digest, id->inode); hash_number(digest, id->bytes);
        hash_number(digest, (uint64_t)id->modified); hash_number(digest, (uint64_t)id->changed);
        hash_number(digest, id->modified_ns); hash_number(digest, id->changed_ns); hash_number(digest, id->mode);
        struct stat info;
        if (!safe_stat(record->path, &info) || !same_identity(*id, identity(&info), false))
            return fail(error, error_cap, "预览期间目标发生变化，请重新预览。");
        if (record->directory) { ++preview->directory_count; continue; }
        ++preview->file_count;
        if (!add_bytes(&preview->bytes, id->bytes, error, error_cap)) return false;
        FILE* file = safe_file(record->path);
        if (!file) return fail(error, error_cap, "无法读取文件，已取消删除预览。");
        bool okay = opened_file_matches(file, record);
        uint64_t read_bytes = 0;
        while (okay) {
            if (cancelled(error, error_cap)) { okay = false; break; }
            size_t got = fread(buffer, 1, sizeof(buffer), file);
            if (!got) { if (ferror(file)) okay = false; break; }
            read_bytes += got;
            if (read_bytes > id->bytes) { okay = false; break; }
            hash_bytes(digest, buffer, got);
            if (!(read_bytes % (64u * 1024u))) storage_yield();
        }
        if (read_bytes != id->bytes || !opened_file_matches(file, record)) okay = false;
        if (fclose(file)) okay = false;
        if (!okay) {
            if (error && error_cap && error[0]) return false;
            return fail(error, error_cap, "预览期间文件发生变化，或读取失败。");
        }
    }
    /* Catch a directory mutation while the file payloads were being read. */
    for (size_t i = 0; i < inventory->count; ++i) {
        const record_t* record = &inventory->records[i]; struct stat info;
        if (!safe_stat(record->path, &info) || !same_identity(record->identity, identity(&info), false))
            return fail(error, error_cap, "预览期间目标发生变化，请重新预览。");
    }
    return true;
}
bool folio_storage_preview_delete(const char* path, folio_storage_preview_t* result,
                                  char* error, size_t error_cap) {
    if (result) memset(result, 0, sizeof(*result));
    if (!result) return fail(error, error_cap, "删除预览参数无效。");
    if (!begin(error, error_cap)) return false;
    preview_valid = false;
    inventory_t inventory = {0}; bool okay = false;
    folio_storage_preview_t preview = {0}; digest_t digest;
    if (!folio_storage_normalize(path, preview.path, sizeof(preview.path), error, error_cap)) goto done;
    if (!strcmp(preview.path, FOLIO_SD_ROOT)) { fail(error, error_cap, "不能删除 TF 卡根目录。"); goto done; }
    if (!collect(preview.path, true, &inventory, error, error_cap)) goto done;
    preview.directory = inventory.records[0].directory;
    if (!fingerprint(&inventory, &digest, &preview, error, error_cap)) goto done;
    if (++next_token == 0) ++next_token;
    preview.token = next_token; pending_preview = preview; pending_digest = digest; preview_valid = true;
    *result = preview; okay = true;
done:
    free(inventory.records); folio_storage_unlock(); return okay;
}
bool folio_storage_commit_delete(const folio_storage_preview_t* preview, char* error, size_t error_cap) {
    if (!begin(error, error_cap)) return false;
    inventory_t inventory = {0}; bool okay = false;
    if (!preview || !preview_valid || !preview->token || preview->token != pending_preview.token ||
        strncmp(preview->path, pending_preview.path, sizeof(preview->path))) {
        fail(error, error_cap, "删除确认已失效，请重新预览。"); goto done;
    }
    preview_valid = false; /* Every attempted commit consumes the confirmation. */
    folio_storage_preview_t current = {0}; digest_t digest;
    if (!collect(pending_preview.path, true, &inventory, error, error_cap) ||
        !fingerprint(&inventory, &digest, &current, error, error_cap)) goto done;
    if (digest.first != pending_digest.first || digest.second != pending_digest.second ||
        current.bytes != pending_preview.bytes || current.file_count != pending_preview.file_count ||
        current.directory_count != pending_preview.directory_count) {
        fail(error, error_cap, "目标已发生变化，请重新预览后删除。"); goto done;
    }
    /* Reverse lexical order guarantees children precede their parent. Only the
     * validated inventory is removed: new entries make rmdir fail, never erased. */
    for (size_t i = inventory.count; i > 0; --i) {
        if (cancelled(error, error_cap)) goto done;
        if (!safe_remove(&inventory.records[i - 1])) {
            fail(error, error_cap, "删除已停止：TF 卡或文件发生变化，或读写失败。请刷新文件列表。"); goto done;
        }
        storage_yield();
    }
    okay = true;
done:
    free(inventory.records); folio_storage_unlock(); return okay;
}
void folio_storage_cancel_delete(void) {
    if (!folio_storage_lock(STORAGE_WAIT_MS)) return;
    preview_valid = false;
    folio_storage_unlock();
}
