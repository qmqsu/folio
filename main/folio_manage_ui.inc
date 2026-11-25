// Additional native screens reuse the approved Folio type, border and spacing.
static void draw_transfer_qr(const char* value,EpdRect rect) {
    uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(8)],temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    if(!value || !*value || !qrcodegen_encodeText(value,temporary,code,qrcodegen_Ecc_MEDIUM,1,8,qrcodegen_Mask_AUTO,true))return;
    int size=qrcodegen_getSize(code),scale=rect.width/(size+8);
    if(rect.height/(size+8)<scale)scale=rect.height/(size+8);
    if(scale<1)return;
    int left=rect.x+(rect.width-size*scale)/2,top=rect.y+(rect.height-size*scale)/2;
    epd_fill_rect(rect,PAPER,fb());
    for(int y=0;y<size;++y)for(int x=0;x<size;++x)
        if(qrcodegen_getModule(code,x,y))epd_fill_rect((EpdRect){left+x*scale,top+y*scale,scale,scale},INK,fb());
}
// Shared QWERTY keyboard: staggered 10/9/7 rows, Shift and Backspace,
// then mode, space and clear. Drawing and hit-testing share these rectangles.
static EpdRect catalog_key(unsigned i) {
    if(i<10)return (EpdRect){24+(int)i*64,640,58,92};
    if(i<19)return (EpdRect){56+(int)(i-10)*64,744,58,92};
    if(i<26)return (EpdRect){120+(int)(i-19)*64,848,58,92};
    if(i==26)return (EpdRect){140,952,404,92};
    if(i==27)return (EpdRect){576,848,84,92};
    if(i==28)return (EpdRect){556,952,104,92};
    if(i==29)return (EpdRect){24,952,104,92};
    return (EpdRect){24,848,84,92};
}
static const char* keyboard_letters(unsigned mode) {
    const char* sets[]={"qwertyuiopasdfghjklzxcvbnm","QWERTYUIOPASDFGHJKLZXCVBNM",
        "1234567890-/:;()$&@.,?!'\"_","[]{}#%^*+=\\|~<>`:_-/?!.,\"'"};
    return sets[mode%4];
}
static void draw_keyboard(unsigned mode) {
    const char* letters=keyboard_letters(mode);
    for(unsigned i=0;i<31;++i) {
        char value[2]={0};const char* name=value;
        if(i<26)value[0]=letters[i];
        else name=i==26?"空格":i==27?"退格":i==28?"清空":i==29?(mode<2?"123":"ABC"):
            mode==0?"大写":mode==1?"小写":mode==2?"#+=":"123";
        panel(catalog_key(i));centered_in(catalog_key(i),i<26?30:24,INK,name);
    }
}
static void refresh_keyboard(unsigned mode) {
    EpdRect r={24,640,636,404};epd_fill_rect(r,PAPER,fb());draw_keyboard(mode);present_region(r);
}
static void draw_search_input(const char* query) {
    EpdRect r={48,540,588,76};epd_fill_rect(r,PAPER,fb());panel(r);
    label(68,baseline_for_center_y(30,*query?query:"输入关键词",578),616,30,*query?INK:MUTED,*query?query:"输入关键词");
}
static void draw_catalog(void) {
    page_header("书架","查找书籍","书名、作者、无声调拼音或首字母");
    const char* filters[]={"全部","最近","未读"};
    for(unsigned i=0;i<3;++i)action((EpdRect){48+(int)i*200,252,188,72},filters[i],s_catalog_options.filter==i);
    const char* sorts[]={"最近阅读","书名","作者","加入时间"};
    for(unsigned i=0;i<4;++i)action((EpdRect){48+(int)(i%2)*300,348+(int)(i/2)*84,288,72},sorts[i],s_catalog_options.sort==i);
    draw_search_input(s_search);
    draw_keyboard(s_search_keys);
    action(done_rect(),"显示结果",true);
}
static void draw_device(void) {
    page_header("设置","系统设置","网络、时间、存储与固件");
    const char* names[]={"Wi-Fi","日期与时间","文件管理","存储空间","固件升级","重新扫描书架"};
    for(unsigned i=0;i<6;++i) {
        EpdRect r={48,232+(int)i*112,588,96};panel(r);
        label(72,baseline_for_center_y(32,names[i],r.y+48),564,32,INK,names[i]);
        if(i<5)icon((EpdRect){588,r.y+36,24,24},FOLIO_ICON_CHEVRON_RIGHT,MUTED);
    }
    centered(342,1000,588,24,MUTED,"传书与同步结束后，Wi-Fi 自动关闭");
}
static void storage_size(char* output,size_t capacity,uint64_t bytes) {
    if(bytes>=UINT64_C(1000000000))snprintf(output,capacity,"%llu.%02llu GB",(unsigned long long)(bytes/1000000000),(unsigned long long)(bytes%1000000000/10000000));
    else if(bytes>=1000000)snprintf(output,capacity,"%llu.%llu MB",(unsigned long long)(bytes/1000000),(unsigned long long)(bytes%1000000/100000));
    else snprintf(output,capacity,"%llu KB",(unsigned long long)((bytes+1023)/1024));
}
static void draw_storage(void) {
    page_header("返回","存储空间","TF 卡容量与文件分类");
    char used[32],total[32],value[80];storage_size(used,sizeof(used),s_storage_summary.used_bytes);
    storage_size(total,sizeof(total),s_storage_summary.capacity_bytes);
    snprintf(value,sizeof(value),"已用 %s / %s",used,total);
    panel((EpdRect){48,240,588,172});label(72,304,612,32,INK,value);
    uint64_t amounts[]={s_storage_summary.book_bytes,s_storage_summary.font_bytes,s_storage_summary.cache_bytes,s_storage_summary.other_bytes,s_storage_summary.free_bytes};
    uint64_t logical=amounts[0]+amounts[1]+amounts[2]+amounts[3];
    if(s_storage_summary.used_bytes>logical)amounts[3]+=s_storage_summary.used_bytes-logical;
    const uint8_t colors[]={0x00,0x40,0x70,0xB0,0xF0};
    const char* names[]={"书籍","字体","缓存","其他","可用"};
    uint64_t sum=0;for(unsigned i=0;i<5;++i)sum+=amounts[i];
    uint64_t cumulative=0;int left=72;
    for(unsigned i=0;i<5;++i) {
        cumulative+=amounts[i];int right=72+(sum?(int)(540*cumulative/sum):0);
        if(right>left)epd_fill_rect((EpdRect){left,348,right-left,24},colors[i],fb());
        left=right;
        int y=444+(int)i*84;folio_gfx_fill_round_rect(fb(),(EpdRect){72,y+20,28,28},4,colors[i]);
        if(i==4)folio_gfx_round_rect(fb(),(EpdRect){72,y+20,28,28},4,RULE,1);
        label(124,baseline_for_center_y(30,names[i],y+34),360,30,INK,names[i]);
        storage_size(value,sizeof(value),amounts[i]);right_label(612,baseline_for_center_y(28,value,y+34),28,MUTED,value);
    }
    folio_gfx_outline(fb(),(EpdRect){72,348,540,24},RULE);
    label(48,912,636,22,MUTED,"书籍与字体按文件统计；其他含文件系统占用");
    text_action((EpdRect){48,984,588,96},"浏览文件");action(done_rect(),"格式化 TF 卡",false);
}
static void draw_files(void) {
    const char* relative=s_file_directory+strlen(FOLIO_SD_ROOT);
    page_header("上一级","文件管理",*relative?relative:"TF 卡根目录");
    text_action((EpdRect){448,108,188,64},s_files_show_internal?"收起内部":"显示全部");
    for(size_t i=0;i<s_file_count;++i) {
        int y=240+(int)i*108;folio_storage_entry_t* entry=&s_file_entries[i];
        icon((EpdRect){56,y+28,32,36},entry->directory?FOLIO_ICON_GRID:FOLIO_ICON_BOOK,INK);
        label(112,baseline_for_center_y(30,entry->name,y+30),524,30,INK,entry->name);
        char size[32];storage_size(size,sizeof(size),entry->bytes);
        label(112,baseline_for_center_y(22,entry->directory?"文件夹":size,y+70),524,22,MUTED,entry->directory?"文件夹":size);
        icon((EpdRect){574,y+32,28,28},FOLIO_ICON_SETTINGS,MUTED);
        if(i+1<s_file_count)rule_at(112,y+102,524,RULE);
    }
    if(!s_file_count)centered(342,592,588,32,MUTED,"这个文件夹是空的");
    pager(s_file_page,s_file_total,7);
}
static void draw_file_action(void) {
    page_header("文件","文件详情",s_file_selected.directory?"文件夹":"文件");
    panel((EpdRect){48,256,588,448});
    paragraph(72,320,612,36,52,3,INK,s_file_selected.name);
    paragraph(72,520,612,24,36,3,MUTED,s_file_selected.path+strlen(FOLIO_SD_ROOT));
    if(!s_file_selected.directory){char size[32];storage_size(size,sizeof(size),s_file_selected.bytes);label(72,668,612,28,MUTED,size);}
    if(s_file_selected.directory)text_action((EpdRect){48,856,588,96},"打开文件夹");
    text_action((EpdRect){48,984,588,96},"返回文件列表");action(done_rect(),"删除",false);
}
static void draw_delete_confirmation(void) {
    page_header("取消","确认删除",NULL);panel((EpdRect){48,256,588,608});
    paragraph(72,330,612,36,52,3,INK,s_file_selected.name);
    char size[32],info[80];storage_size(size,sizeof(size),s_delete_preview.bytes);
    snprintf(info,sizeof(info),"%u 个文件 · %s",(unsigned)s_delete_preview.file_count,size);
    label(72,552,612,28,MUTED,info);
    paragraph(72,644,612,28,44,3,INK,"删除后无法撤销。文件夹中的内容也会一起删除。");
    text_action((EpdRect){48,984,588,96},"保留文件");action(done_rect(),"确认删除",true);
}
static void draw_format_confirmation(bool final) {
    page_header("取消",final?"最后确认":"格式化 TF 卡",NULL);
    panel((EpdRect){48,256,588,608});icon((EpdRect){306,316,72,72},FOLIO_ICON_CLOSE,INK);
    centered(342,486,540,36,INK,final?"清空这张 TF 卡？":"卡上的内容将全部删除");
    paragraph(72,580,612,30,48,4,INK,"包括书籍、字体、书签与阅读记录。请先将需要保留的文件复制到电脑。");
    if(final)label(72,820,612,24,MUTED,"格式化过程中请保持供电，不要拔卡");
    text_action((EpdRect){48,984,588,96},"取消，保留数据");action(done_rect(),final?"确认清空并格式化":"我已备份，继续",true);
}
static void draw_format_ascii(bool final) {
    epd_hl_set_all_white(&s_hw.hl);folio_gfx_text(fb(),48,120,3,"FORMAT SD CARD",INK);
    folio_gfx_text(fb(),48,300,3,"ALL FILES WILL BE ERASED",INK);
    folio_gfx_text(fb(),48,400,2,"BOOKS, FONTS AND READING RECORDS",INK);
    folio_gfx_text(fb(),48,480,2,"BACK UP YOUR FILES FIRST",INK);
    folio_gfx_text(fb(),48,620,3,final?"FINAL CONFIRMATION":"CONFIRMATION 1 OF 2",INK);
    panel((EpdRect){48,984,588,96});ascii_centered_in((EpdRect){48,984,588,96},3,"CANCEL");
    selected_panel(done_rect(),true);ascii_centered_in(done_rect(),3,final?"ERASE AND FORMAT":"CONTINUE");
}
static EpdRect transfer_address_rect(void) { return (EpdRect){48,1096,288,96}; }
static EpdRect transfer_done_rect(void) {
    return s_font && s_transfer_status.sta_connected?(EpdRect){348,1096,288,96}:done_rect();
}
static void draw_transfer(void) {
    folio_transfer_get_status(&s_transfer_status);
    if(!s_transfer_status.sta_connected)s_transfer_tab=0;
    page_header("返回","无线传书",s_transfer_status.running?"连接后打开网址 · 传完请返回退出":"Wi-Fi 已关闭");
    panel((EpdRect){48,240,588,320});
    label(72,284,372,22,MUTED,"设备热点");
    label(72,338,372,38,INK,s_transfer_status.ssid);
    label(72,386,372,22,MUTED,"热点密码");
    label(72,434,372,30,INK,s_transfer_status.ap_password);
    label(72,482,372,22,MUTED,"连接后直接打开");
    const char* url=s_transfer_tab && s_transfer_status.sta_connected?s_transfer_status.sta_url:s_transfer_status.url;
    draw_transfer_qr(url,(EpdRect){392,258,220,220});
    centered(502,500,220,20,MUTED,s_transfer_tab?"局域网网址":"热点网址");
    char short_url[48];snprintf(short_url,sizeof(short_url),"%.*s",(int)strcspn(url,"#"),url);
    centered(342,536,540,26,INK,short_url);
    char status[64];snprintf(status,sizeof(status),"已接收 %u 个文件",(unsigned)s_transfer_status.completed);
    right_label(636,608,22,MUTED,status);
    unsigned progress=s_transfer_status.total?(unsigned)((uint64_t)s_transfer_status.received*100/s_transfer_status.total):0;
    if(progress>100)progress=100;
    if(s_transfer_status.busy) {
        if(progress==100)snprintf(status,sizeof(status),"正在校验，请稍候");
        else snprintf(status,sizeof(status),"正在接收  %u%%",progress);
    } else snprintf(status,sizeof(status),"%s",s_transfer_status.completed?"文件已保存，可继续传输":"等待手机或电脑上传");
    label(48,608,400,26,INK,status);
    epd_fill_rect((EpdRect){48,632,588,s_transfer_status.busy?8:2},RULE,fb());
    if(s_transfer_status.busy && progress)epd_fill_rect((EpdRect){48,632,(int)(588*progress/100),8},INK,fb());
    label(48,688,340,28,INK,"最近记录");right_label(636,688,22,MUTED,"最近5条 · 新记录在上");
    panel((EpdRect){48,708,588,368});
    unsigned count=s_transfer_status.recent_count;
    if(count>FOLIO_TRANSFER_RECENT_CAP)count=FOLIO_TRANSFER_RECENT_CAP;
    if(!count) {
        centered(342,890,540,26,MUTED,"文件传入后，记录会显示在这里");
        centered(342,940,540,22,MUTED,"离开传书页面后关闭网络服务");
    }
    for(unsigned i=0;i<count;++i) {
        const folio_transfer_record_t* record=&s_transfer_status.recent[i];int y=720+(int)i*68;
        label(72,y+26,500,28,INK,record->name);
        right_label(612,y+26,22,record->result?MUTED:INK,record->result?"未完成":"已保存");
        char size[32],detail[160];storage_size(size,sizeof(size),record->size);
        snprintf(detail,sizeof(detail),"%s · %s",size,record->message);
        label(72,y+54,612,22,MUTED,detail);
        if(i+1<count)rule_at(72,y+66,540,RULE);
    }
    if(s_transfer_status.sta_connected)
        action(transfer_address_rect(),s_transfer_tab?"显示热点地址":"显示局域网地址",false);
    action(transfer_done_rect(),"完成传书",true);
}
static void draw_transfer_ascii(void) {
    folio_transfer_get_status(&s_transfer_status);epd_hl_set_all_white(&s_hw.hl);
    folio_gfx_text(fb(),48,80,3,"WI-FI TRANSFER",INK);
    folio_gfx_text(fb(),48,180,2,"CONNECT TO",INK);
    folio_gfx_text(fb(),48,230,3,s_transfer_status.ssid,INK);
    folio_gfx_text(fb(),48,320,2,"PASSWORD",INK);
    folio_gfx_text(fb(),48,370,3,s_transfer_status.ap_password,INK);
    draw_transfer_qr(s_transfer_status.url,(EpdRect){178,460,328,328});
    char short_url[48];snprintf(short_url,sizeof(short_url),"%.*s",(int)strcspn(s_transfer_status.url,"#"),s_transfer_status.url);
    folio_gfx_text(fb(),48,834,3,short_url,INK);
    folio_gfx_text(fb(),48,894,2,"OPEN AFTER CONNECTING TO WI-FI",INK);
    char status[48];snprintf(status,sizeof(status),"RECEIVED %u FILES",(unsigned)s_transfer_status.completed);
    folio_gfx_text(fb(),48,950,3,status,INK);
    folio_gfx_text(fb(),48,1004,2,"UPLOAD A CHINESE TTF, THEN FINISH",INK);
    selected_panel(done_rect(),true);ascii_centered_in(done_rect(),3,"FINISH");
}
static void draw_update(void) {
    if(!s_update_local){draw_online_update();return;}
    char current[80];snprintf(current,sizeof(current),"当前版本  %s",esp_app_get_description()->version);
    page_header("返回","固件升级",current);
    action((EpdRect){48,232,282,72},"在线更新",false);
    action((EpdRect){354,232,282,72},"本地 TF 卡",true);
    panel((EpdRect){48,336,588,492});
    if(s_update_ready) {
        label(72,392,612,26,MUTED,"已校验的本地更新");
        label(72,458,612,42,INK,s_update_info.version);
        char size[32];storage_size(size,sizeof(size),s_update_info.size);label(72,516,612,28,MUTED,size);
        label(72,592,612,24,MUTED,"SHA-256");char hex[65];
        for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",s_update_info.sha256[i]);
        char first[33];memcpy(first,hex,32);first[32]='\0';
        label(72,636,612,22,INK,first);label(72,676,612,22,INK,hex+32);
        paragraph(72,752,612,24,36,2,MUTED,"请核对版本并保持供电，安装后设备会重新启动。");
    } else paragraph(72,420,612,30,48,7,INK,s_update_message);
    paragraph(48,884,636,26,42,4,MUTED,"通过无线传书上传 folio.bin 和 folio.bin.sha256。也可将文件复制到卡上：\n/update/");
    action(done_rect(),s_update_ready?"安装并重新启动":"重新检查更新",s_update_ready);
}
