#include "folio_storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fault_after=-1,race_after=-1,checks;
/* A fresh process is used for each crash boundary and each recovery. */
bool folio_storage_cancelled(void) {
    int current=checks++;
    if(fault_after>=0 && current==fault_after)_Exit(75);
    /* The exact generated test root is asserted in main before this hook. */
    if(race_after>=0 && current==race_after)assert(mkdir(FOLIO_PICTURE_ROOT,0700)==0);
    return false;
}
static bool exists(const char* path) {struct stat st;return lstat(path,&st)==0;}
static void expect_selection(const char* relative) {
    char selected[FOLIO_STORAGE_PATH_CAP],expected[FOLIO_STORAGE_PATH_CAP];
    bool okay=folio_storage_wallpaper_load(selected,sizeof(selected));
    if(!relative){assert(!okay && !selected[0]);return;}
    snprintf(expected,sizeof(expected),FOLIO_SD_ROOT "/%s",relative);
    if(!okay || strcmp(selected,expected))fprintf(stderr,"selection: expected %s, got %s (ok=%d)\n",expected,selected,okay);
    assert(okay && !strcmp(selected,expected));
}
int main(int argc,char** argv) {
    assert(argc>=2 && strstr(FOLIO_SD_ROOT,"folio-storage-") && strcmp(FOLIO_SD_ROOT,"/sdcard"));
    char error[256]={0};const char* mode=argv[1];
    if(!strcmp(mode,"crash")) {
        assert(argc==3);fault_after=atoi(argv[2]);
        bool okay=folio_storage_prepare_wallpapers(error,sizeof(error));
        if(!okay)fprintf(stderr,"prepare: %s\n",error);
        assert(okay);printf("crash checks reached: %d\n",checks);return 0;
    }
    if(!strcmp(mode,"race-directory")) {
        assert(argc==3);race_after=atoi(argv[2]);
        assert(!folio_storage_prepare_wallpapers(error,sizeof(error)) && strstr(error,"未迁移"));
        assert(exists(FOLIO_PICTURE_ROOT) && exists(FOLIO_LEGACY_WALLPAPER_ROOT));
        expect_selection("wallpapers/selected.png");
    } else if(!strcmp(mode,"canonical-fallback")) {
        expect_selection("picture/z.png");
    } else if(!strcmp(mode,"empty")) {
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        assert(exists(FOLIO_PICTURE_ROOT) && !exists(FOLIO_LEGACY_WALLPAPER_ROOT));
        expect_selection(NULL);
    } else if(!strcmp(mode,"legacy") || !strcmp(mode,"legacy-large") || !strcmp(mode,"recover")) {
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        assert(exists(FOLIO_PICTURE_ROOT) && !exists(FOLIO_LEGACY_WALLPAPER_ROOT));
        expect_selection("picture/selected.png");
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        expect_selection("picture/selected.png");
        if(!strcmp(mode,"legacy-large")) {
            char path[FOLIO_STORAGE_PATH_CAP];uint8_t* pixels=NULL;unsigned width=0,height=0;
            assert(!folio_storage_wallpaper_load_legacy(path,sizeof(path)) && !path[0]);
            assert(!folio_storage_wallpaper_cache_load(&pixels,&width,&height) && !pixels && !width && !height);
        }
    } else if(!strcmp(mode,"legacy-unselected")) {
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        assert(!exists(FOLIO_LEGACY_WALLPAPER_ROOT));
        expect_selection("picture/原名 图.png");
    } else if(!strcmp(mode,"canonical")) {
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        expect_selection("picture/原名 图.png");
    } else if(!strcmp(mode,"coexist")) {
        assert(!folio_storage_prepare_wallpapers(error,sizeof(error)) && strstr(error,"未迁移") && strstr(error,"保留"));
        expect_selection("wallpapers/selected.png");
        folio_storage_entry_t entries[8];size_t count,total;
        assert(folio_storage_wallpapers(0,entries,8,&count,&total,error,sizeof(error)) && count==4 && total==4);
        unsigned legacy=0,canonical=0;
        for(size_t i=0;i<count;++i)if(!strcmp(entries[i].name,"selected.png")) {
            if(!strcmp(entries[i].path,FOLIO_PICTURE_ROOT "/selected.png"))++canonical;
            if(!strcmp(entries[i].path,FOLIO_LEGACY_WALLPAPER_ROOT "/selected.png"))++legacy;
        }
        assert(legacy==1 && canonical==1);
        for(size_t i=0;i<count;++i) {
            folio_storage_entry_t one;size_t n,t;
            assert(folio_storage_wallpapers(i,&one,1,&n,&t,error,sizeof(error)) && n==1 && t==4);
            assert(!strcmp(one.path,entries[i].path) && !strcmp(one.name,entries[i].name));
        }
        assert(folio_storage_wallpaper_save(FOLIO_PICTURE_ROOT "/selected.png",error,sizeof(error)));
        expect_selection("picture/selected.png");
        assert(folio_storage_wallpaper_save(FOLIO_LEGACY_WALLPAPER_ROOT "/selected.png",error,sizeof(error)));
        expect_selection("wallpapers/selected.png");
        assert(!folio_storage_prepare_wallpapers(error,sizeof(error)) && strstr(error,"未迁移"));
        expect_selection("wallpapers/selected.png");
    } else if(!strcmp(mode,"ambiguous")) {
        assert(folio_storage_prepare_wallpapers(error,sizeof(error)));
        expect_selection(NULL); /* v1 basename is not proof of a rename. */
    } else if(!strcmp(mode,"unsafe")) {
        assert(!folio_storage_prepare_wallpapers(error,sizeof(error)) && error[0]);
    } else if(!strcmp(mode,"unsafe-save")) {
        assert(!folio_storage_wallpaper_save(FOLIO_PICTURE_ROOT "/selected.png",error,sizeof(error)));
    } else if(!strcmp(mode,"save-legacy")) {
        assert(folio_storage_wallpaper_save(FOLIO_LEGACY_WALLPAPER_ROOT "/selected.png",error,sizeof(error)));
        expect_selection("wallpapers/selected.png");
    } else if(!strcmp(mode,"pending-legacy")) {
        assert(!folio_storage_prepare_wallpapers(error,sizeof(error)) && strstr(error,"未迁移"));
        expect_selection("wallpapers/selected.png");
    } else if(!strcmp(mode,"pending-conflict")) {
        assert(!folio_storage_prepare_wallpapers(error,sizeof(error)) && strstr(error,"未迁移"));
        expect_selection("picture/selected.png");
    } else if(!strcmp(mode,"changed-pending")) {
        expect_selection(NULL);
    } else if(!strcmp(mode,"names")) {
        folio_storage_entry_t entries[16];size_t count,total;
        assert(folio_storage_list_view(FOLIO_SD_ROOT,0,false,entries,16,&count,&total,error,sizeof(error)));
        assert(count==6 && total==6);
        const char* names[]={"books","fonts","picture","wallpapers","原名 文件夹","自定义目录"};
        for(size_t j=0;j<6;++j) {
            bool found=false;
            for(size_t i=0;i<count;++i)if(!strcmp(names[j],entries[i].name))found=true;
            assert(found);
        }
    } else assert(!"unknown migration test mode");
    puts(mode);return 0;
}
