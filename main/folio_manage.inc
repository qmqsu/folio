static void apply_catalog(void) {
    s_shelf_page=0;catalog_select();
    s_settings.reserved[0]=(uint8_t)((s_settings.reserved[0]&0xf0u)|s_catalog_options.filter|(s_catalog_options.sort<<2));
    if(!folio_settings_save(&s_settings))show_error("筛选已应用，但未能保存到设备。",VIEW_LIBRARY);
    else show_view(VIEW_LIBRARY,true);
}
static void enter_transfer(void) {
    if(!s_card_ready) {show_error("请先插入可用的 TF 卡。",VIEW_DEVICE);return;}
    if(!save_reader() || !activity_flush() || !catalog_save()) {
        show_error("阅读记录未能保存，请检查 TF 卡。",VIEW_DEVICE);return;
    }
    close_reader();folio_cover_clear_cache();
    char error[192];
    folio_network_status_t network;folio_network_get_status(&network);
    if(network.running && !stop_transfer())return;
    if(!folio_transfer_start(NULL,error,sizeof(error))) {
        if(!stop_transfer())return;
        show_error(error,VIEW_DEVICE);return;
    }
    s_transfer_tab=0;show_view(VIEW_TRANSFER,true);
}
static void refresh_files(void) {
    char error[192];
    if(!folio_storage_list_view(s_file_directory,s_file_page*7,s_files_show_internal,s_file_entries,7,&s_file_count,&s_file_total,error,sizeof(error))) {
        show_error(error,VIEW_DEVICE);return;
    }
    if(s_file_page && !s_file_count) {s_file_page=0;refresh_files();return;}
    for(size_t i=0;i<s_file_count;++i){
        folio_storage_entry_t* entry=&s_file_entries[i];const char* ext=strrchr(entry->path,'.');
        if(!entry->directory && ext && !strcasecmp(ext,".wrb")){
            char title[128],author[128],message[128];
            if(folio_weread_cache_metadata(entry->path,title,sizeof(title),author,sizeof(author),message,sizeof(message)))snprintf(entry->name,sizeof(entry->name),"%s",title);
        }
    }
    show_view(VIEW_FILES,true);
}
static void enter_storage(void) {
    read_pico_sd_info_t info={0};char error[192];
    if(read_pico_sd_get_info(&info)!=ESP_OK || !info.mounted) {show_error("TF 卡未就绪，请重新插入并扫描。",VIEW_DEVICE);return;}
    show_busy("正在统计 TF 卡空间");
    if(!folio_storage_space(&info.capacity_bytes,&info.free_bytes,error,sizeof(error)) ||
        !folio_storage_scan(info.capacity_bytes,info.free_bytes,&s_storage_summary,error,sizeof(error)))show_error(error,VIEW_DEVICE);
    else show_view(VIEW_STORAGE,true);
}
static void enter_file_directory(const char* path) {
    snprintf(s_file_directory,sizeof(s_file_directory),"%s",path);s_file_page=0;refresh_files();
}
static void files_move(int direction) {
    if(direction<0 && s_file_page)--s_file_page;
    else if(direction>0 && (s_file_page+1)*7<s_file_total)++s_file_page;
    else return;
    refresh_files();
}
static bool prepare_card_change(void) {
    if(!stop_transfer())return false;
    if(folio_main_io_cancelled()){emergency_shutdown();return false;}
    if(!save_reader())return false;
    activity_record_book();
    if(!activity_flush() || !catalog_save()) {show_error("记录未能保存，已取消文件操作。",VIEW_DEVICE);return false;}
    if(!folio_storage_lock(2000)) {show_error("TF 卡正忙，请稍后重试。",VIEW_DEVICE);return false;}
    close_reader();activity_close();catalog_close();folio_cover_clear_cache();
    folio_font_close(s_font);s_font=NULL;s_active_font[0]='\0';
    return true;
}
static bool deleting_path(const char* path) {
    size_t n=strlen(s_delete_preview.path);
    return !strncmp(path,s_delete_preview.path,n)&&(path[n]==0||path[n]=='/');
}
static void delete_selected(void) {
    char error[192];
    if(!stop_transfer() || !save_reader())return;
    if(!activity_flush() || !catalog_save()){show_error("记录未能保存，已取消删除。",VIEW_FILE_ACTION);return;}
    if(folio_main_io_cancelled()){emergency_shutdown();return;}
    bool font_changed=!folio_font_is_builtin_path(s_active_font) && deleting_path(s_active_font);
    bool choice_deleted=s_settings.font_path[0] && !folio_font_is_builtin_path(s_settings.font_path) &&
        deleting_path(s_settings.font_path);
    bool metadata_changed=deleting_path(FOLIO_SD_ROOT "/.folio") || !strncmp(s_delete_preview.path,FOLIO_SD_ROOT "/.folio/",strlen(FOLIO_SD_ROOT "/.folio/"));
    if(!folio_storage_lock(2000)){show_error("TF 卡正忙，请稍后重试。",VIEW_FILE_ACTION);return;}
    close_reader();
    if(font_changed){
        folio_font_t* fallback=folio_font_open_builtin(s_settings.font_size);
        if(!fallback){folio_storage_unlock();show_error("内置字体加载失败，已取消删除。",VIEW_FILE_ACTION);return;}
        folio_font_close(s_font);s_font=fallback;snprintf(s_active_font,sizeof(s_active_font),"%s",FOLIO_FONT_BUILTIN_PATH);
    }
    if(metadata_changed){activity_close();catalog_close();}
    bool okay=folio_storage_commit_delete(&s_delete_preview,error,sizeof(error));
    folio_storage_unlock();
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    if(metadata_changed){
        s_activity=folio_activity_open();s_catalog=folio_catalog_open();
        s_activity_save_failed=false;
        s_activity_load_failed=!s_activity || folio_activity_error(s_activity)[0]!=0;
        if(!s_catalog || *folio_catalog_error(s_catalog) || s_activity_load_failed){show_error("文件操作后记录读取失败，请重新扫描 TF 卡。",VIEW_DEVICE);return;}
    }
    folio_cover_clear_cache();
    if(font_changed)scan_fonts();
    if(!okay){show_error(error,VIEW_DEVICE);return;}
    // Removing an external recovery face must not erase a remembered native ID.
    // Only a confirmed deletion of the desired external path changes preference.
    if(choice_deleted){
        folio_settings_t candidate=s_settings;candidate.font_path[0]='\0';
        if(!folio_settings_save(&candidate)){
            show_error("文件已删除，但字体选择未能保存。原选择仍保留，请到字体页面重新选择。",VIEW_DEVICE);return;
        }
        s_settings=candidate;
    }
    refresh_files();
}
static void format_card(void) {
    if(!s_format_armed)return;
    s_format_armed=false;
    show_busy("正在格式化 TF 卡，请勿断电");
    if(!prepare_card_change())return;
    if(folio_main_io_cancelled()){folio_storage_unlock();emergency_shutdown();return;}
    esp_err_t error=read_pico_sd_format();
    folio_storage_unlock();
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    load_card(false,false);
    if(error!=ESP_OK)show_error("格式化未完成，请重新插入 TF 卡后检查。",VIEW_DEVICE);
}
static void inspect_local_update(void) {
    s_update_ready=false;
    if(!folio_update_available())snprintf(s_update_message,sizeof(s_update_message),"当前分区不支持无线升级。首次安装此版本需要使用 USB 刷写完整发布包。");
    else {show_busy("正在校验更新文件");s_update_ready=folio_update_inspect(&s_update_info,s_update_message,sizeof(s_update_message));}
    show_view(VIEW_UPDATE,true);
}
static void enter_update(void) {
    s_download_face=0;
    s_update_local=false;s_online_confirm=false;s_online_wifi_setup=false;
    show_view(VIEW_UPDATE,true);
}
static void install_update(void) {
    if(!s_update_ready){inspect_local_update();return;}
    show_busy("正在安装固件，请保持供电");
    if(!prepare_card_change())return;
    char error[192];bool okay=folio_update_install(&s_update_info,error,sizeof(error));
    folio_storage_unlock();s_update_ready=false;
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    if(!okay){load_card(false,false);show_error(error,VIEW_DEVICE);return;}
    (void)read_pico_sd_sync();esp_restart();
}
static bool manage_back(void) {
    switch(s_view) {
        case VIEW_CATALOG:apply_catalog();return true;
        case VIEW_DEVICE:show_view(VIEW_SYSTEM,true);return true;
        case VIEW_TRANSFER:
            if(!stop_transfer())return true;
            folio_cover_clear_cache();
            if(s_wr_network_setup){s_wr_network_setup=false;enter_weread();return true;}
            if(!s_font)load_card(false,false);
            else {refresh_library();show_view(VIEW_SYSTEM,true);}return true;
        case VIEW_STORAGE:show_view(VIEW_DEVICE,true);return true;
        case VIEW_FILES: {
            if(strcmp(s_file_directory,FOLIO_SD_ROOT)==0)show_view(VIEW_DEVICE,true);
            else {char* slash=strrchr(s_file_directory,'/');if(slash)*slash='\0';s_file_page=0;refresh_files();}
            return true;
        }
        case VIEW_FILE_ACTION:refresh_files();return true;
        case VIEW_DELETE_CONFIRM:folio_storage_cancel_delete();show_view(VIEW_FILE_ACTION,true);return true;
        case VIEW_FORMAT_CONFIRM:case VIEW_FORMAT_FINAL:s_format_armed=false;show_view(VIEW_STORAGE,true);return true;
        case VIEW_UPDATE:
            if(s_download_face){if(online_leave(VIEW_FONTS))show_view(VIEW_FONTS,true);return true;}
            if(s_online_confirm){s_online_confirm=false;show_view(VIEW_UPDATE,true);return true;}
            if(online_leave(VIEW_DEVICE))show_view(VIEW_DEVICE,true);
            return true;
        default:return false;
    }
}
static bool manage_tap(int x,int y) {
    if(s_view==VIEW_CATALOG) {
        if(hit(done_rect(),x,y)) {apply_catalog();return true;}
        for(unsigned i=0;i<3;++i)if(hit((EpdRect){48+(int)i*200,252,188,72},x,y)) {
            s_catalog_options.filter=(folio_catalog_filter_t)i;show_view(s_view,false);return true;
        }
        for(unsigned i=0;i<4;++i)if(hit((EpdRect){48+(int)(i%2)*300,348+(int)(i/2)*84,288,72},x,y)) {
            s_catalog_options.sort=(folio_catalog_sort_t)i;show_view(s_view,false);return true;
        }
        for(unsigned i=0;i<31;++i)if(hit(catalog_key(i),x,y)) {
            size_t n=strlen(s_search);const char* letters=keyboard_letters(s_search_keys);
            if(i<26 && i<strlen(letters) && n+1<sizeof(s_search)){s_search[n]=letters[i];s_search[n+1]='\0';}
            else if(i==26 && n+1<sizeof(s_search)){s_search[n]=' ';s_search[n+1]='\0';}
            else if(i==27 && n) {do{--n;}while(n && ((unsigned char)s_search[n]&0xC0)==0x80);s_search[n]='\0';}
            else if(i==28)s_search[0]='\0';
            else if(i==29)s_search_keys=s_search_keys<2?2:0;
            else if(i==30)s_search_keys^=1;
            if(i>=29)refresh_keyboard(s_search_keys);
            else {draw_search_input();present_region((EpdRect){48,540,588,76});}
            return true;
        }
        return true;
    }
    if(s_view==VIEW_DEVICE) {
        if(x<48 || x>=636 || y<232 || y>=904 || (y-232)%112>=96)return true;
        switch((y-232)/112) {
            case 0:enter_wifi();break;case 1:enter_time();break;
            case 2:enter_file_directory(FOLIO_SD_ROOT);break;case 3:enter_storage();break;
            case 4:enter_update();break;
            case 5:if(rescan_library())show_view(VIEW_LIBRARY,true);break;
        }return true;
    }
    if(s_view==VIEW_TRANSFER) {
        // Hit-test the same snapshot that drew the buttons; a network event must
        // not turn the visible Finish button into an address switch.
        if(s_font && s_transfer_status.sta_connected && hit(transfer_address_rect(),x,y)) {
            s_transfer_tab=!s_transfer_tab;show_view(VIEW_TRANSFER,false);
        } else if(hit(transfer_done_rect(),x,y)) manage_back();
        return true;
    }
    if(s_view==VIEW_STORAGE) {
        if(hit((EpdRect){48,984,588,96},x,y))enter_file_directory(FOLIO_SD_ROOT);
        else if(hit(done_rect(),x,y)){s_format_armed=false;show_view(VIEW_FORMAT_CONFIRM,true);}
        return true;
    }
    if(s_view==VIEW_FILES) {
        if(hit((EpdRect){448,108,188,64},x,y)){s_files_show_internal=!s_files_show_internal;s_file_page=0;refresh_files();return true;}
        int footer=footer_hit(x,y);if(footer==0 || footer==2){files_move(footer==0?-1:1);return true;}
        int row=list_hit(x,y);if(row>=0 && (size_t)row<s_file_count) {
            s_file_selected=s_file_entries[row];
            if(s_file_selected.directory && x<540)enter_file_directory(s_file_selected.path);
            else show_view(VIEW_FILE_ACTION,true);
        }return true;
    }
    if(s_view==VIEW_FILE_ACTION) {
        if(hit((EpdRect){48,984,588,96},x,y))refresh_files();
        else if(s_file_selected.directory && hit((EpdRect){48,856,588,96},x,y))enter_file_directory(s_file_selected.path);
        else if(hit(done_rect(),x,y)) {
            char error[192];
            if(!save_reader() || !activity_flush() || !catalog_save())show_error("记录未能保存，已取消删除。",VIEW_FILE_ACTION);
            else if(!folio_storage_preview_delete(s_file_selected.path,&s_delete_preview,error,sizeof(error)))show_error(error,VIEW_FILE_ACTION);
            else show_view(VIEW_DELETE_CONFIRM,true);
        }return true;
    }
    if(s_view==VIEW_DELETE_CONFIRM) {
        if(hit((EpdRect){48,984,588,96},x,y))manage_back();
        else if(hit(done_rect(),x,y))delete_selected();
        return true;
    }
    if(s_view==VIEW_FORMAT_CONFIRM || s_view==VIEW_FORMAT_FINAL) {
        if(hit((EpdRect){48,984,588,96},x,y))manage_back();
        else if(hit(done_rect(),x,y)) {
            if(s_view==VIEW_FORMAT_CONFIRM){s_format_armed=true;show_view(VIEW_FORMAT_FINAL,true);}
            else format_card();
        }return true;
    }
    if(s_view==VIEW_UPDATE){online_tap(x,y);return true;}
    return false;
}
static void refresh_transfer_status(void) {
    static int64_t next;
    if(s_view!=VIEW_TRANSFER || esp_timer_get_time()<next)return;
    next=esp_timer_get_time()+2000000;
    folio_transfer_status_t status;folio_transfer_get_status(&status);
    bool changed=status.running!=s_transfer_status.running || status.busy!=s_transfer_status.busy ||
        status.completed!=s_transfer_status.completed || status.sta_connected!=s_transfer_status.sta_connected ||
        strcmp(status.message,s_transfer_status.message)!=0 || status.recent_count!=s_transfer_status.recent_count ||
        memcmp(status.recent,s_transfer_status.recent,sizeof(status.recent))!=0;
    unsigned old=s_transfer_status.total?(unsigned)((uint64_t)s_transfer_status.received*10/s_transfer_status.total):0;
    unsigned current=status.total?(unsigned)((uint64_t)status.received*10/status.total):0;
    if(changed || old!=current) {
        int64_t last=s_last_activity;bool progress=status.received!=s_transfer_status.received;
        show_view(VIEW_TRANSFER,false);s_last_activity=progress?esp_timer_get_time():last;
    }
}
