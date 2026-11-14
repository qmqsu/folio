#include "folio_storage.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char error[256];
static int cancel_after = -1, cancel_checks;
bool folio_storage_cancelled(void) { return cancel_after >= 0 && cancel_checks++ >= cancel_after; }
static void write_file(const char* path, const char* value) {
    FILE* file = fopen(path, "wb"); assert(file);
    size_t size = strlen(value); assert(fwrite(value, 1, size, file) == size); assert(fclose(file) == 0);
}
static bool exists(const char* path) { struct stat info; return lstat(path, &info) == 0; }
static bool preview(const char* path, folio_storage_preview_t* result) {
    return folio_storage_preview_delete(path, result, error, sizeof(error));
}
static bool commit(const folio_storage_preview_t* result) { return folio_storage_commit_delete(result, error, sizeof(error)); }
static void* locked_other_thread(void* argument) {
    (void)argument;
    assert(!folio_storage_lock(30));
    folio_storage_summary_t summary;
    uint64_t capacity, available;
    assert(!folio_storage_space(&capacity, &available, error, sizeof(error)) && !capacity && !available);
    assert(!folio_storage_scan(1000, 500, &summary, error, sizeof(error)));
    assert(strstr(error, "正忙"));
    return NULL;
}
static void test_weread_categories(void) {
    const char* paths[]={FOLIO_SD_ROOT "/books/cloud.wrb",FOLIO_SD_ROOT "/books/cloud.wrb.wrt.a",
        FOLIO_SD_ROOT "/books/cloud.wrb.wrt.b",FOLIO_SD_ROOT "/.folio/weread/chapter.txt",
        FOLIO_SD_ROOT "/.folio/wereadish/trap.txt",FOLIO_SD_ROOT "/.folio/catalog.a"};
    const char* values[]={"WRB","time-a","time-b","chapter","text","index"};
    assert(mkdir(FOLIO_SD_ROOT "/.folio/weread",0700)==0);
    assert(mkdir(FOLIO_SD_ROOT "/.folio/wereadish",0700)==0);
    for(unsigned i=0;i<6;++i)write_file(paths[i],values[i]);
    folio_storage_summary_t s;assert(folio_storage_scan(100000,60000,&s,error,sizeof(error)));
    assert(s.book_bytes==11+3+4 && s.cache_bytes==5+7+5 && s.other_bytes==6+6+6 && s.font_bytes==7);
    folio_storage_entry_t entries[8];size_t count,total;
    assert(folio_storage_list(FOLIO_SD_ROOT "/books",0,entries,8,&count,&total,error,sizeof(error)));
    unsigned seen=0;
    for(size_t i=0;i<count;++i) {
        if(!strcmp(entries[i].name,"cloud.wrb")){assert(entries[i].category==FOLIO_STORAGE_BOOK);++seen;}
        if(strstr(entries[i].name,".wrt.")){assert(entries[i].category==FOLIO_STORAGE_OTHER);++seen;}
    }
    assert(seen==3);
    for(unsigned i=0;i<6;++i)assert(unlink(paths[i])==0);
    assert(rmdir(FOLIO_SD_ROOT "/.folio/weread")==0 && rmdir(FOLIO_SD_ROOT "/.folio/wereadish")==0);
}
static void test_wallpapers(const char* outside) {
    char selected[256];size_t count,total;folio_storage_entry_t entries[4];
    assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
    assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
    assert(!folio_storage_wallpaper_load(selected,sizeof(selected)) && !selected[0]);
    write_file(FOLIO_SD_ROOT "/picture/a.png","image-a");
    write_file(FOLIO_SD_ROOT "/picture/b.JPG","image-b");
    write_file(FOLIO_SD_ROOT "/picture/notes.txt","not-an-image");
    write_file(FOLIO_SD_ROOT "/picture/.hidden.png","hidden");
    assert(mkdir(FOLIO_SD_ROOT "/picture/folder.png",0700)==0);
    assert(folio_storage_wallpapers(0,entries,4,&count,&total,error,sizeof(error)) && count==2 && total==2);
    assert(!strcmp(entries[0].name,"a.png") && !strcmp(entries[1].name,"b.JPG"));
    assert(folio_storage_wallpapers(1,entries,1,&count,&total,error,sizeof(error)) && count==1 && total==2);
    assert(!strcmp(entries[0].name,"b.JPG"));
    assert(folio_storage_wallpaper_load(selected,sizeof(selected)) && !strcmp(selected,FOLIO_SD_ROOT "/picture/a.png"));
    assert(!folio_storage_wallpaper_save(FOLIO_SD_ROOT "/picture/../books/a.png",error,sizeof(error)));
    assert(!folio_storage_wallpaper_save(FOLIO_SD_ROOT "/picture/notes.txt",error,sizeof(error)));
    assert(folio_storage_wallpaper_save(FOLIO_SD_ROOT "/picture/a.png",error,sizeof(error)));
    assert(folio_storage_wallpaper_save(FOLIO_SD_ROOT "/picture/b.JPG",error,sizeof(error)));
    assert(folio_storage_wallpaper_load(selected,sizeof(selected)) && !strcmp(selected,FOLIO_SD_ROOT "/picture/b.JPG"));
    write_file(FOLIO_SD_ROOT "/.folio/wallpaper.b","partial");
    assert(folio_storage_wallpaper_load(selected,sizeof(selected)) && !strcmp(selected,FOLIO_SD_ROOT "/picture/a.png"));
    assert(unlink(FOLIO_SD_ROOT "/picture/a.png")==0);
    assert(folio_storage_wallpaper_load(selected,sizeof(selected)) && !strcmp(selected,FOLIO_SD_ROOT "/picture/b.JPG"));
    assert(!folio_storage_wallpaper_load(selected,2) && !selected[0]);
    assert(unlink(FOLIO_SD_ROOT "/picture/b.JPG")==0);
    assert(unlink(FOLIO_SD_ROOT "/picture/notes.txt")==0);
    assert(unlink(FOLIO_SD_ROOT "/picture/.hidden.png")==0);
    assert(rmdir(FOLIO_SD_ROOT "/picture/folder.png")==0);
    assert(unlink(FOLIO_SD_ROOT "/.folio/wallpaper.a")==0 && unlink(FOLIO_SD_ROOT "/.folio/wallpaper.b")==0);
    assert(rmdir(FOLIO_SD_ROOT "/picture")==0);
    write_file(FOLIO_SD_ROOT "/picture","conflict");
    assert(!folio_storage_prepare_wallpapers(error,sizeof(error)));
    assert(unlink(FOLIO_SD_ROOT "/picture")==0);
    assert(symlink(outside,FOLIO_SD_ROOT "/picture")==0);
    assert(!folio_storage_prepare_wallpapers(error,sizeof(error)));
    assert(!folio_storage_wallpaper_load(selected,sizeof(selected)));
    assert(unlink(FOLIO_SD_ROOT "/picture")==0);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(strcmp(FOLIO_SD_ROOT, "/sdcard") && strstr(FOLIO_SD_ROOT, "folio-storage-"));
    char path[256];
    assert(folio_storage_normalize(FOLIO_SD_ROOT "//books/./a.txt/", path, sizeof(path), error, sizeof(error)));
    assert(!strcmp(path, FOLIO_SD_ROOT "/books/a.txt"));
    const char* invalid[] = {"/etc/passwd", "books/a.txt", FOLIO_SD_ROOT "evil/file", FOLIO_SD_ROOT "/books/../font",
        FOLIO_SD_ROOT "/books/../../file", FOLIO_SD_ROOT "/back\\slash", FOLIO_SD_ROOT "/newline\n", FOLIO_SD_ROOT "/colon:x",
        FOLIO_SD_ROOT "/books/.. /outside", FOLIO_SD_ROOT "/books/../.", FOLIO_SD_ROOT "/books/file.", FOLIO_SD_ROOT "/books/*"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert(!folio_storage_normalize(invalid[i], path, sizeof(path), error, sizeof(error)) && error[0]);
    assert(!folio_storage_normalize(FOLIO_SD_ROOT, path, 2, error, sizeof(error)));
    uint64_t capacity, available;
    assert(folio_storage_space(&capacity, &available, error, sizeof(error)) && capacity > 0 && available <= capacity);
    folio_storage_summary_t summary;
    assert(folio_storage_scan(100000, 60000, &summary, error, sizeof(error)));
    assert(summary.capacity_bytes == 100000 && summary.used_bytes == 40000 && summary.free_bytes == 60000);
    assert(summary.book_bytes == 11 && summary.font_bytes == 7 && summary.cache_bytes == 5 && summary.other_bytes == 6);
    assert(summary.file_count == 6 && summary.directory_count == 5);
    test_weread_categories();
    test_wallpapers(argv[1]);
    assert(!folio_storage_scan(100, 101, &summary, error, sizeof(error)) && !summary.capacity_bytes);
    folio_storage_entry_t entries[2]; size_t count, total;
    assert(folio_storage_list(FOLIO_SD_ROOT "/books", 0, entries, 2, &count, &total, error, sizeof(error)));
    assert(count == 2 && total == 3 && entries[0].directory && !strcmp(entries[0].name, "sub"));
    assert(!strcmp(entries[1].name, "a.txt") && entries[1].bytes == 4);
    assert(folio_storage_list(FOLIO_SD_ROOT "/books", 2, entries, 2, &count, &total, error, sizeof(error)));
    assert(count == 1 && !strcmp(entries[0].name, "b.EPUB"));
    assert(folio_storage_list(FOLIO_SD_ROOT "/books", SIZE_MAX, NULL, 0, &count, &total, error, sizeof(error)) && !count && total == 3);
    write_file(FOLIO_SD_ROOT "/books/.hidden","cache");write_file(FOLIO_SD_ROOT "/books/a.jpg","cover");write_file(FOLIO_SD_ROOT "/books/art.png","image");
    assert(folio_storage_list_view(FOLIO_SD_ROOT "/books",0,false,entries,2,&count,&total,error,sizeof(error)) && count==2 && total==4);
    assert(entries[0].directory && !strcmp(entries[1].name,"a.txt"));
    assert(folio_storage_list_view(FOLIO_SD_ROOT "/books",2,false,entries,2,&count,&total,error,sizeof(error)) && count==2 && total==4 && !strcmp(entries[0].name,"art.png"));
    assert(folio_storage_list_view(FOLIO_SD_ROOT "/books",0,true,entries,2,&count,&total,error,sizeof(error)) && total==6);
    assert(unlink(FOLIO_SD_ROOT "/books/.hidden")==0 && unlink(FOLIO_SD_ROOT "/books/a.jpg")==0 && unlink(FOLIO_SD_ROOT "/books/art.png")==0);
    assert(!folio_storage_list(FOLIO_SD_ROOT "/books/a.txt", 0, entries, 2, &count, &total, error, sizeof(error)) && !count && !total);
    assert(folio_storage_validate_parent(FOLIO_SD_ROOT "/books/new.txt", error, sizeof(error)));
    assert(!folio_storage_validate_parent(FOLIO_SD_ROOT "/missing/new.txt", error, sizeof(error)));
    assert(!folio_storage_validate_parent(FOLIO_SD_ROOT, error, sizeof(error)));
    folio_storage_preview_t first, second;
    assert(!preview(FOLIO_SD_ROOT, &first) && !first.token);
    assert(preview(FOLIO_SD_ROOT "/books/a.txt", &first) && first.bytes == 4 && first.file_count == 1 && !first.directory_count);
    assert(preview(FOLIO_SD_ROOT "/books/b.EPUB", &second));
    assert(!commit(&first) && exists(FOLIO_SD_ROOT "/books/a.txt"));
    folio_storage_cancel_delete(); assert(!commit(&second));
    assert(preview(FOLIO_SD_ROOT "/books/a.txt", &first));
    write_file(FOLIO_SD_ROOT "/books/a.txt", "CCCC");
    assert(!commit(&first) && exists(FOLIO_SD_ROOT "/books/a.txt") && strstr(error, "变化"));
    assert(!commit(&first)); /* One-use even after changed-target failure. */
    assert(preview(FOLIO_SD_ROOT "/books", &first) && first.directory && first.file_count == 3 && first.directory_count == 2 && first.bytes == 14);
    write_file(FOLIO_SD_ROOT "/books/sub/new.txt", "new");
    assert(!commit(&first) && exists(FOLIO_SD_ROOT "/books/sub/new.txt") && exists(FOLIO_SD_ROOT "/books/a.txt"));
    assert(unlink(FOLIO_SD_ROOT "/books/sub/new.txt") == 0);
    /* A replacement file with the same name, size and mtime is a changed target. */
    assert(preview(FOLIO_SD_ROOT "/books/a.txt", &first));
    struct stat before; assert(stat(FOLIO_SD_ROOT "/books/a.txt", &before) == 0);
    write_file(FOLIO_SD_ROOT "/books/replacement", "CCCC");
    assert(rename(FOLIO_SD_ROOT "/books/replacement", FOLIO_SD_ROOT "/books/a.txt") == 0);
    struct timespec times[2] = {{before.st_atime, 0}, {before.st_mtime, 0}};
    assert(utimensat(AT_FDCWD, FOLIO_SD_ROOT "/books/a.txt", times, 0) == 0);
    assert(!commit(&first));
    /* Links anywhere in the path fail; outside files must never be touched. */
    assert(symlink(argv[1], FOLIO_SD_ROOT "/link") == 0);
    assert(!folio_storage_list(FOLIO_SD_ROOT "/link", 0, entries, 2, &count, &total, error, sizeof(error)));
    assert(!folio_storage_validate_parent(FOLIO_SD_ROOT "/link/new.txt", error, sizeof(error)));
    assert(!preview(FOLIO_SD_ROOT "/link/sentinel.txt", &first));
    assert(!folio_storage_scan(100000, 60000, &summary, error, sizeof(error)));
    assert(unlink(FOLIO_SD_ROOT "/link") == 0);
    assert(symlink(argv[1], FOLIO_SD_ROOT "/books/a-link") == 0);
    assert(!folio_storage_validate_parent(FOLIO_SD_ROOT "/books/a-link", error, sizeof(error)));
    assert(!preview(FOLIO_SD_ROOT "/books", &first));
    assert(unlink(FOLIO_SD_ROOT "/books/a-link") == 0);
    assert(mkfifo(FOLIO_SD_ROOT "/pipe", 0600) == 0);
    assert(!preview(FOLIO_SD_ROOT "/pipe", &first));
    assert(unlink(FOLIO_SD_ROOT "/pipe") == 0);
    if (geteuid() != 0) {
        assert(chmod(FOLIO_SD_ROOT "/books/a.txt", 0000) == 0);
        assert(!preview(FOLIO_SD_ROOT "/books/a.txt", &first));
        assert(chmod(FOLIO_SD_ROOT "/books/a.txt", 0600) == 0);
    }
    assert(folio_storage_lock(0) && folio_storage_lock(0));
    pthread_t thread; assert(pthread_create(&thread, NULL, locked_other_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(folio_storage_scan(100000, 60000, &summary, error, sizeof(error))); /* Recursive same-task call. */
    folio_storage_unlock(); folio_storage_unlock();
    assert(preview(FOLIO_SD_ROOT "/books/a.txt", &first) && commit(&first));
    assert(!exists(FOLIO_SD_ROOT "/books/a.txt") && !commit(&first));
    assert(preview(FOLIO_SD_ROOT "/books", &first) && commit(&first));
    assert(!exists(FOLIO_SD_ROOT "/books") && exists(FOLIO_SD_ROOT "/fonts/main.ttf"));
    /* Removing a file after preview refuses the entire commit before any write. */
    assert(preview(FOLIO_SD_ROOT "/.folio", &first));
    assert(unlink(FOLIO_SD_ROOT "/.folio/cache/chapter.txt") == 0);
    assert(!commit(&first) && exists(FOLIO_SD_ROOT "/.folio/state"));
    cancel_after = 0; cancel_checks = 0;
    assert(!folio_storage_scan(100000, 60000, &summary, error, sizeof(error)) && strstr(error, "取消"));
    assert(!preview(FOLIO_SD_ROOT "/fonts", &first) && !first.token && strstr(error, "取消"));
    cancel_after = -1;
    assert(mkdir(FOLIO_SD_ROOT "/cancel", 0700) == 0);
    write_file(FOLIO_SD_ROOT "/cancel/a", "a"); write_file(FOLIO_SD_ROOT "/cancel/b", "b"); write_file(FOLIO_SD_ROOT "/cancel/c", "c");
    cancel_after = 100000; cancel_checks = 0;
    assert(preview(FOLIO_SD_ROOT "/cancel", &first));
    int preview_checks = cancel_checks;
    cancel_checks = 0; cancel_after = preview_checks + 1;
    assert(!commit(&first) && strstr(error, "取消"));
    assert(exists(FOLIO_SD_ROOT "/cancel") && exists(FOLIO_SD_ROOT "/cancel/a") && !exists(FOLIO_SD_ROOT "/cancel/c"));
    cancel_after = -1;
    /* Directory limit is explicit and the failure cannot issue a confirmation. */
    assert(mkdir(FOLIO_SD_ROOT "/many", 0700) == 0);
    for (unsigned i = 0; i < FOLIO_STORAGE_ENTRY_LIMIT; ++i) {
        snprintf(path, sizeof(path), FOLIO_SD_ROOT "/many/%04u", i); write_file(path, "");
    }
    assert(!preview(FOLIO_SD_ROOT "/many", &first) && !first.token && strstr(error, "4096"));
    puts("storage: path confinement, totals, browsing, mutex, preview/change/deletion and limits passed");
    return 0;
}
