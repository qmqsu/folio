// Native e-paper flow: static QR, explicit sync direction, no animation/poll redraw.
static EpdRect wr_left(int y){return (EpdRect){48,y,280,96};}
static EpdRect wr_right(int y){return (EpdRect){356,y,280,96};}
static EpdRect wr_account(unsigned i){return (EpdRect){48+(int)i*200,984,188,96};}
static EpdRect wr_row(unsigned i){return (EpdRect){48,240+(int)i*104,588,96};}
static EpdRect wr_digit(unsigned i){return (EpdRect){48+(int)(i%3)*200,440+(int)(i/3)*112,188,96};}
static bool wr_can_relogin(void) {
    return !s_wr_status.busy && !s_wr_leaving &&
        (s_wr_status.login_required || s_wr_status.state==FOLIO_WR_SHELF);
}
static int wr_paragraph(int x,int baseline,int right,int size,int leading,unsigned lines,uint8_t color,const char* value) {
    return paragraph(x,baseline,right,size,leading,lines,color,value);
}
static void wr_message(int y,unsigned lines,const char* value){wr_paragraph(72,y,612,26,42,lines,MUTED,value);}
static void draw_weread(void) {
    folio_weread_get_status(&s_wr_status);
    page_header("返回",s_wr_status.state==FOLIO_WR_SYNC?"同步进度":s_wr_status.title[0]?s_wr_status.title:"微信读书",
        s_wr_status.state==FOLIO_WR_SYNC?s_wr_status.title:"设备直连 · 登录与离线书籍自动保存");
    if(s_wr_confirm){
        const char* title=s_wr_confirm==1?"开启时长记录？":s_wr_confirm==2?"提交新增时长？":s_wr_confirm==3?"退出登录？":s_wr_confirm==5?"重新登录？":"上传进度与时长？";
        const char* message=s_wr_confirm==1?"仅从开启后记录有效阅读。时长按书籍与账号保存，上传仍需你手动确认。":
            s_wr_confirm==2?"本次最多提交 5 分钟，并采用本机阅读位置。结果未知的时长会隔离，不会自动重试。":
            s_wr_confirm==3?"设备保存的登录信息将清除。已下载书籍和本地阅读记录仍保留。":
            s_wr_confirm==5?"重新获取二维码并扫码登录。已下载书籍和本地阅读记录保留；时长只会提交给原账号。":"采用本机位置，并提交已开启记录的新增时长。云端位置变化时停止；结果未知的时长不重复提交。";
        panel((EpdRect){48,280,588,592});centered_in((EpdRect){72,320,540,96},38,INK,title);
        wr_message(484,6,message);action((EpdRect){48,984,588,96},"取消",false);action(done_rect(),"确认",true);return;
    }
    if(s_wr_leaving){
        panel((EpdRect){48,280,588,480});centered_in((EpdRect){72,332,540,96},34,INK,"正在关闭连接");
        wr_message(508,4,"等待请求退出并保存已完成的章节，请稍候。");return;
    }
    if(s_wr_status.state==FOLIO_WR_LOGIN){
        panel((EpdRect){48,248,588,568});draw_transfer_qr(s_wr_status.qr_url,(EpdRect){142,280,400,400});
        centered_in((EpdRect){72,704,540,80},30,INK,"微信扫码 · 手机确认");
        wr_message(876,3,"登录将保存至设备，关机重启后保留。会话失效时可能需要重新扫码。");
        action(done_rect(),"重新获取二维码",false);return;
    }
    if(s_wr_status.state==FOLIO_WR_OTP){
        wr_message(264,2,s_wr_status.message);panel((EpdRect){48,328,588,88});
        centered_in((EpdRect){64,328,556,88},38,INK,s_wr_otp[0]?s_wr_otp:"输入4位验证码");
        const char* keys[]={"1","2","3","4","5","6","7","8","9","清空","0","退格"};
        for(unsigned i=0;i<12;++i)action(wr_digit(i),keys[i],false);
        action_state(done_rect(),"提交验证码",true,!s_wr_status.busy && strlen(s_wr_otp)==4);return;
    }
    if(s_wr_status.state==FOLIO_WR_DOWNLOAD_ERROR && !s_wr_status.busy){
        panel((EpdRect){48,264,588,592});
        centered_in((EpdRect){72,308,540,80},36,INK,"下载未完成");
        wr_paragraph(84,468,600,30,46,6,INK,s_wr_status.message);
        wr_message(800,1,"已保存的章节仍可离线阅读。");
        action(wr_left(1096),"返回目录",false);action(wr_right(1096),"重试下载",true);return;
    }
    if(s_wr_status.busy || s_wr_status.state==FOLIO_WR_WORK || s_wr_status.state==FOLIO_WR_CONNECTING){
        panel((EpdRect){48,280,588,520});icon((EpdRect){310,332,64,64},FOLIO_ICON_BOOK,INK);
        wr_paragraph(84,500,600,32,48,3,INK,s_wr_status.message);
        if(s_wr_status.total){char progress[64];snprintf(progress,sizeof(progress),"%u / %u 章",(unsigned)s_wr_status.completed,(unsigned)s_wr_status.total);
            centered_in((EpdRect){72,652,540,80},30,MUTED,progress);
            folio_gfx_fill_round_rect(fb(),(EpdRect){84,752,516,8},4,RULE);
            int filled=(int)((uint64_t)516*s_wr_status.completed/s_wr_status.total);if(filled>516)filled=516;
            if(filled)folio_gfx_fill_round_rect(fb(),(EpdRect){84,752,filled,8},4,INK);
        }
        action(done_rect(),"取消并返回",false);return;
    }
    if(s_wr_status.state==FOLIO_WR_SHELF || s_wr_status.state==FOLIO_WR_BOOK){
        for(size_t i=0;i<s_wr_status.item_count;++i){
            EpdRect r=wr_row((unsigned)i);
            label(64,baseline_for_center_y(32,s_wr_status.items[i].title,r.y+30),588,32,INK,s_wr_status.items[i].title);
            label(64,baseline_for_center_y(24,s_wr_status.items[i].subtitle,r.y+72),588,24,MUTED,s_wr_status.items[i].subtitle);
            icon((EpdRect){604,r.y+36,24,24},s_wr_status.items[i].cached?FOLIO_ICON_CHECK:FOLIO_ICON_CHEVRON_RIGHT,INK);
            if(i+1<s_wr_status.item_count)epd_fill_rect((EpdRect){64,r.y+103,556,1},RULE,fb());
        }
        if(!s_wr_status.item_count){panel((EpdRect){48,280,588,480});wr_message(388,5,s_wr_status.message);}
        else wr_message(914,2,s_wr_status.message);
        if(s_wr_status.state==FOLIO_WR_SHELF){if(wr_can_relogin())action(wr_account(0),"重新登录",false);action(wr_account(1),"退出登录",false);action_state(wr_account(2),"刷新书架",true,!s_wr_status.busy);}
        else {
            action_state((EpdRect){48,984,188,96},"离线阅读",true,s_wr_status.can_read && !s_wr_status.busy);
            action_state((EpdRect){248,984,188,96},"同步进度",false,s_wr_status.can_read && !s_wr_status.busy);
            action_state((EpdRect){448,984,188,96},"下载整书",false,!s_wr_status.busy);
        }
        pager(s_wr_status.page,s_wr_status.count,FOLIO_WR_PAGE_SIZE);return;
    }
    if(s_wr_status.state==FOLIO_WR_SYNC){
        panel((EpdRect){48,248,588,216});label(72,296,612,28,MUTED,"本机位置");
        wr_paragraph(72,354,612,30,42,2,INK,s_wr_status.local_position);
        char duration[100];
        const folio_catalog_metadata_t* metadata=folio_catalog_metadata(s_catalog,s_wr_status.local_path);
        snprintf(duration,sizeof(duration),"本机累计 %llu 分钟",(unsigned long long)(metadata?metadata->reading_seconds/60:0));
        label(72,438,612,24,MUTED,duration);
        panel((EpdRect){48,488,588,216});label(72,536,612,28,MUTED,"微信读书");
        wr_paragraph(72,590,612,30,38,2,INK,s_wr_status.cloud_position);
        if(s_wr_status.cloud_time_available)snprintf(duration,sizeof(duration),"云端累计 %llu 分钟",(unsigned long long)(s_wr_status.cloud_total_seconds/60));
        else if(s_wr_status.cloud_time_saved)snprintf(duration,sizeof(duration),"上次同步时长 %llu 分钟",(unsigned long long)(s_wr_status.saved_cloud_seconds/60));
        else snprintf(duration,sizeof(duration),"云端暂未返回本书时长");
        label(72,678,612,24,MUTED,duration);
        wr_paragraph(72,746,612,24,32,2,MUTED,s_wr_status.message);
        action_state(wr_left(804),"上传到微信读书",false,s_wr_status.sync_local && !s_wr_status.busy);
        action_state(wr_right(804),"下载到本机",false,s_wr_status.sync_cloud && !s_wr_status.busy);
        char time_label[80];snprintf(time_label,sizeof(time_label),"时长记录：%s",s_wr_status.time_enabled?"已开启":"关闭");
        action_state((EpdRect){48,912,588,96},time_label,false,!s_wr_status.busy);
        char amount[100];snprintf(amount,sizeof(amount),"待提交 %u 秒 · 结果未知 %u 秒",(unsigned)s_wr_status.pending_seconds,(unsigned)s_wr_status.uncertain_seconds);
        centered_in((EpdRect){48,1016,588,56},24,MUTED,amount);
        action_state(done_rect(),"刷新云端数据",false,!s_wr_status.busy);return;
    }
    panel((EpdRect){48,264,588,544});
    centered_in((EpdRect){72,300,540,96},36,INK,s_wr_status.login_required?"登录已过期":s_wr_status.state==FOLIO_WR_ERROR?"操作未完成":"开始微信读书");
    wr_message(468,6,s_wr_status.message[0]?s_wr_status.message:"需要联网时自动连接已保存的 Wi-Fi，再用手机扫码登录。登录会保留，关机重启后无需重复扫码。");
    if(s_wr_status.login_required)wr_message(876,3,"已下载书籍和阅读记录保留，重新扫码后继续。");
    else wr_message(876,3,"重试当前操作。需要更换 Wi-Fi 时，返回设置即可。");
    action(done_rect(),wr_can_relogin()?"重新登录":"重试",true);
}
static void enter_weread(void) {
    if(!s_card_ready || !s_font){show_error("请先准备可用的 TF 卡与字体。",VIEW_DEVICE);return;}
    folio_transfer_status_t transfer;folio_transfer_get_status(&transfer);
    if(transfer.running && !stop_transfer()) {s_wr_leaving=true;s_wr_target=VIEW_WEREAD;return;}
    if(!save_reader() || !activity_flush() || !catalog_save())return;
    close_reader();folio_cover_clear_cache();s_wr_confirm=0;s_wr_otp[0]=0;s_wr_leaving=false;
    // Reuse the connected native STA; start_station checks AP ownership and TLS time.
    s_view=VIEW_WEREAD;
    char error[192];
    if(s_wr_reader_path[0])(void)folio_weread_start_sync(s_wr_reader_path,error,sizeof(error));
    else (void)folio_weread_start(error,sizeof(error));
    show_view(VIEW_WEREAD,true);
}
static void enter_reader_sync(void) {
    char id[64];
    if(!s_reader || !folio_wr_path_id(s_current_book,id,sizeof(id))){
        show_error("这本书未关联微信读书。请从微信读书下载后使用同步进度。",VIEW_TOOLS);return;
    }
    if(!save_reader())return;
    activity_record_book();
    if(!activity_flush() || !catalog_save() || !folio_weread_flush()){
        show_error("阅读记录尚未保存，请检查 TF 卡后重试同步。",VIEW_TOOLS);return;
    }
    snprintf(s_wr_reader_path,sizeof(s_wr_reader_path),"%s",s_current_book);
    s_wr_reader_position=folio_reader_position(s_reader);s_wr_reader_cover=s_reader_cover;
    enter_weread();
}
static void weread_finish_open(void) {
    char path[256];snprintf(path,sizeof(path),"%s",s_wr_open_path);uint32_t position=s_wr_open_position;
    bool reader_return=s_wr_reader_path[0]!=0;
    bool restore_cover=s_wr_reader_path[0] && !strcmp(path,s_wr_reader_path) && position==s_wr_reader_position && s_wr_reader_cover;
    s_wr_reader_path[0]=0;s_wr_open_pending=s_wr_leaving=false;
    char relocated[256];if(folio_weread_cache_public_path(path,relocated,sizeof(relocated)))snprintf(path,sizeof(path),"%s",relocated);
    bool known=false;
    for(size_t i=0;i<s_book_count;++i)if(!strcmp(path,s_books[i].path)){known=true;break;}
    // Sync only changes the position ledger, so the existing shelf is current.
    // Downloads still rescan to pick up new/replaced EPUBs and covers.
    if(!reader_return || !known)refresh_library();
    for(size_t i=0;i<s_book_count;++i)if(!strcmp(path,s_books[i].path)){
        open_book_absolute(i,true);
        if(s_reader && s_view!=VIEW_ERROR && position!=UINT32_MAX){
            if(!folio_reader_seek(s_reader,position) || !folio_reader_save(s_reader)){show_error(folio_reader_error(s_reader),VIEW_READING);return;}
            s_reader_cover=restore_cover;activity_record_book();
            show_view(VIEW_READING,true);activity_flush();
        }
        return;
    }
    show_error("本地书架未找到缓存。请检查 TF 卡和书库容量，再重新下载。",VIEW_DEVICE);
}
static void weread_open(const char* path,uint32_t position) {
    snprintf(s_wr_open_path,sizeof(s_wr_open_path),"%s",path);s_wr_open_position=position;s_wr_open_pending=true;
    if(folio_weread_stop())weread_finish_open();
    else {s_wr_leaving=true;s_wr_target=VIEW_READING;show_view(VIEW_WEREAD,false);}
}
static void weread_back(void) {
    if(s_wr_leaving)return;
    if(s_wr_confirm){s_wr_confirm=0;show_view(VIEW_WEREAD,false);return;}
    folio_weread_get_status(&s_wr_status);
    if(s_wr_reader_path[0]){weread_open(s_wr_reader_path,s_wr_reader_position);return;}
    if(!s_wr_status.busy && s_wr_status.running && s_wr_status.state==FOLIO_WR_OTP){
        memset(s_wr_otp,0,sizeof(s_wr_otp));folio_weread_command(FOLIO_WR_NEW_QR,0,NULL);show_view(VIEW_WEREAD,true);return;
    }
    if(!s_wr_status.busy && s_wr_status.running && s_wr_status.state==FOLIO_WR_DOWNLOAD_ERROR){folio_weread_command(FOLIO_WR_RETURN_TOC,0,NULL);return;}
    if(!s_wr_status.busy && s_wr_status.running && s_wr_status.state==FOLIO_WR_SYNC){folio_weread_command(FOLIO_WR_SELECT_BOOK,s_wr_book_index,NULL);return;}
    if(!s_wr_status.busy && s_wr_status.running && s_wr_status.state==FOLIO_WR_BOOK){folio_weread_command(FOLIO_WR_RETURN_SHELF,0,NULL);return;}
    s_wr_leaving=true;s_wr_target=VIEW_SYSTEM;
    if(folio_weread_stop()){s_wr_leaving=false;show_view(VIEW_SYSTEM,true);}else show_view(VIEW_WEREAD,false);
}
static void weread_page(int direction) {
    folio_weread_get_status(&s_wr_status);
    if(s_wr_status.busy || s_wr_leaving || s_wr_confirm)return;
    if(s_wr_status.state!=FOLIO_WR_BOOK && s_wr_status.state!=FOLIO_WR_SHELF)return;
    if(direction<0 && s_wr_status.page)folio_weread_command(FOLIO_WR_PAGE,s_wr_status.page-1,NULL);
    if(direction>0 && (s_wr_status.page+1)*FOLIO_WR_PAGE_SIZE<s_wr_status.count)folio_weread_command(FOLIO_WR_PAGE,s_wr_status.page+1,NULL);
}
static void weread_tap(int x,int y) {
    if(s_wr_leaving)return;
    folio_weread_get_status(&s_wr_status);
    if(s_wr_confirm){
        if(hit((EpdRect){48,984,588,96},x,y)){s_wr_confirm=0;show_view(VIEW_WEREAD,false);}
        else if(hit(done_rect(),x,y)){
            unsigned confirmation=s_wr_confirm;s_wr_confirm=0;
            if(confirmation==5 && !s_wr_status.running){
                enter_weread();folio_weread_get_status(&s_wr_status);
                if(!s_wr_status.running)return;
            }
            folio_weread_command(confirmation==1?FOLIO_WR_ENABLE_TIME:confirmation==2?FOLIO_WR_UPLOAD_TIME:
                confirmation==3?FOLIO_WR_LOGOUT:confirmation==5?FOLIO_WR_RELOGIN:FOLIO_WR_UPLOAD_SYNC,0,NULL);
        }return;
    }
    if(s_wr_status.busy || s_wr_status.state==FOLIO_WR_WORK || s_wr_status.state==FOLIO_WR_CONNECTING){
        if(hit(done_rect(),x,y))weread_back();
        return;
    }
    if(s_wr_status.state==FOLIO_WR_DOWNLOAD_ERROR){
        if(hit(wr_left(1096),x,y))folio_weread_command(FOLIO_WR_RETURN_TOC,0,NULL);
        else if(hit(wr_right(1096),x,y))folio_weread_command(FOLIO_WR_RETRY_DOWNLOAD,0,NULL);
        return;
    }
    if(s_wr_status.state==FOLIO_WR_LOGIN){if(hit(done_rect(),x,y))folio_weread_command(FOLIO_WR_NEW_QR,0,NULL);return;}
    if(s_wr_status.state==FOLIO_WR_OTP){
        for(unsigned i=0;i<12;++i)if(hit(wr_digit(i),x,y)){
            size_t n=strlen(s_wr_otp);
            if(i<9 && n<4){s_wr_otp[n]=(char)('1'+i);s_wr_otp[n+1]=0;}
            else if(i==9)s_wr_otp[0]=0;
            else if(i==10 && n<4){s_wr_otp[n]='0';s_wr_otp[n+1]=0;}
            else if(i==11 && n)s_wr_otp[n-1]=0;
            epd_fill_rect((EpdRect){48,328,588,88},PAPER,fb());panel((EpdRect){48,328,588,88});
            centered_in((EpdRect){64,328,556,88},38,INK,s_wr_otp[0]?s_wr_otp:"输入4位验证码");
            present_region((EpdRect){48,328,588,88});
            if((n==4)!=(strlen(s_wr_otp)==4)){
                epd_fill_rect(done_rect(),PAPER,fb());
                action_state(done_rect(),"提交验证码",true,strlen(s_wr_otp)==4);
                present_region(done_rect());
            }
            return;
        }
        if(hit(done_rect(),x,y) && strlen(s_wr_otp)==4){folio_weread_command(FOLIO_WR_SUBMIT_OTP,0,s_wr_otp);memset(s_wr_otp,0,sizeof(s_wr_otp));show_view(VIEW_WEREAD,true);}return;
    }
    if(s_wr_status.state==FOLIO_WR_SHELF || s_wr_status.state==FOLIO_WR_BOOK){
        if(y>=1096){int footer=footer_hit(x,y);if(footer==0 || footer==2)weread_page(footer==0?-1:1);return;}
        for(size_t i=0;i<s_wr_status.item_count;++i)if(hit(wr_row((unsigned)i),x,y)){
            size_t index=s_wr_status.page*FOLIO_WR_PAGE_SIZE+i;
            if(s_wr_status.state==FOLIO_WR_SHELF){s_wr_book_index=index;folio_weread_command(FOLIO_WR_SELECT_BOOK,index,NULL);}
            else if(folio_weread_command(FOLIO_WR_DOWNLOAD_CHAPTER,index,NULL))show_view(VIEW_WEREAD,false);
            return;
        }
        if(s_wr_status.state==FOLIO_WR_SHELF){
            if(wr_can_relogin() && hit(wr_account(0),x,y)){s_wr_confirm=5;show_view(VIEW_WEREAD,false);}
            else if(hit(wr_account(1),x,y)){s_wr_confirm=3;show_view(VIEW_WEREAD,false);}
            else if(hit(wr_account(2),x,y))folio_weread_command(FOLIO_WR_REFRESH,0,NULL);
        }else {
            if(hit((EpdRect){48,984,188,96},x,y) && s_wr_status.can_read)weread_open(s_wr_status.local_path,UINT32_MAX);
            else if(hit((EpdRect){248,984,188,96},x,y) && s_wr_status.can_read)folio_weread_command(FOLIO_WR_CHECK_SYNC,0,NULL);
            else if(hit((EpdRect){448,984,188,96},x,y) && folio_weread_command(FOLIO_WR_DOWNLOAD_ALL,0,NULL))show_view(VIEW_WEREAD,false);
        }return;
    }
    if(s_wr_status.state==FOLIO_WR_SYNC){
        if(hit(wr_left(804),x,y) && s_wr_status.sync_local){s_wr_confirm=4;show_view(VIEW_WEREAD,false);}
        else if(hit(wr_right(804),x,y) && s_wr_status.sync_cloud)folio_weread_command(FOLIO_WR_USE_CLOUD,0,NULL);
        else if(hit((EpdRect){48,912,588,96},x,y)){
            if(s_wr_status.time_enabled)folio_weread_command(FOLIO_WR_DISABLE_TIME,0,NULL);
            else {s_wr_confirm=1;show_view(VIEW_WEREAD,false);}
        }else if(hit(done_rect(),x,y))folio_weread_command(FOLIO_WR_CHECK_SYNC,0,NULL);
        return;
    }
    if(hit(done_rect(),x,y)){
        if(wr_can_relogin()){s_wr_confirm=5;show_view(VIEW_WEREAD,false);return;}
        folio_network_status_t network;folio_network_get_status(&network);
        if(s_wr_status.running && network.connected)folio_weread_command(FOLIO_WR_RETRY,0,NULL);
        else if(s_wr_status.running) {
            if(!folio_weread_stop()){s_wr_leaving=true;s_wr_target=VIEW_WEREAD;show_view(VIEW_WEREAD,false);}
            else enter_weread();
        } else enter_weread();
    }
}
static void refresh_weread(void) {
    static int64_t next;
    if(s_view!=VIEW_WEREAD && !s_wr_leaving)return;
    if(esp_timer_get_time()<next)return;
    next=esp_timer_get_time()+500000;
    if(!s_wr_open_pending && folio_weread_take_open(s_wr_open_path,sizeof(s_wr_open_path),&s_wr_open_position)){
        s_wr_reader_cover=false;s_wr_open_pending=true;s_wr_leaving=true;s_wr_target=VIEW_READING;
        int64_t last=s_last_activity;show_view(VIEW_WEREAD,false);s_last_activity=last;
    }
    if(s_wr_leaving){
        if(!folio_weread_stop())return;
        s_wr_leaving=false;
        if(s_wr_open_pending){weread_finish_open();return;}
        if(s_wr_target==VIEW_WIFI){enter_wifi_from_weread();return;}
        if(s_wr_target==VIEW_WEREAD){enter_weread();return;}
        refresh_library();show_view(s_wr_target,true);return;
    }
    folio_weread_status_t current;folio_weread_get_status(&current);
    if(current.revision!=s_wr_status.revision){
        static int64_t last_draw;
        // Every chapter bumps the revision, and every redraw is a panel refresh
        // that costs more than the chapter itself. While a job runs, refresh the
        // progress a few times per second and always on a state change.
        const int64_t now=esp_timer_get_time();
        if(current.busy&&s_wr_status.busy&&current.state==s_wr_status.state&&now-last_draw<1200000){
            s_last_activity=now;return;
        }
        last_draw=now;
        int64_t last=s_last_activity;
        // A running job must never be interrupted by the idle lock: the export
        // stage keeps `completed` fixed while the worker is still busy.
        bool progressed=current.busy || current.completed!=s_wr_status.completed;
        show_view(VIEW_WEREAD,false);s_last_activity=progressed?esp_timer_get_time():last;
    }
}
