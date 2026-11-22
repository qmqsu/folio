// Native Wi-Fi and clock settings. All PMU/UI work stays on the main task.
static folio_network_ap_t s_wifi_items[16];
static size_t s_wifi_count,s_wifi_page;
static bool s_wifi_scanning,s_wifi_connecting,s_wifi_manual,s_wifi_saved;
static unsigned s_wifi_keys;
static char s_wifi_name[33],s_wifi_password[64],s_setup_message[160];
static int64_t s_setup_deadline;
static bool s_clock_waiting,s_clock_ntp_started;
static struct tm s_clock_edit;
static unsigned s_clock_field;
static uint32_t s_time_clock_minute;
static view_t s_wifi_return=VIEW_SYSTEM;
static EpdRect wifi_row(unsigned i){return (EpdRect){48,336+(int)i*108,588,96};}
static EpdRect clock_field(unsigned i){return (EpdRect){48+(int)(i%3)*200,352+(int)(i/3)*152,188,128};}
static void setup_save(view_t view) {
    if(!folio_settings_save(&s_settings))show_error("设置未能保存，请重试。",view);else show_view(view,false);
}
static void draw_wifi(void) {
    page_header("返回","Wi-Fi","2.4 GHz · 需要联网时自动连接已保存网络");
    folio_network_status_t state;folio_network_get_status(&state);
    panel((EpdRect){48,232,588,88});
    wr_paragraph(72,266,612,24,32,2,INK,s_setup_message);
    for(unsigned i=0;i<6;++i){size_t index=s_wifi_page*6+i;if(index>=s_wifi_count)break;
        EpdRect row=wifi_row(i);panel(row);label(72,row.y+42,556,30,INK,s_wifi_items[index].ssid);
        label(72,row.y+76,556,22,MUTED,folio_network_saved_password(s_wifi_items[index].ssid,NULL,0)?"已保存密码":s_wifi_items[index].secured?"需要密码":"开放网络");
        icon((EpdRect){588,row.y+36,24,24},FOLIO_ICON_CHEVRON_RIGHT,INK);
    }
    if(!s_wifi_count)centered(342,576,588,30,MUTED,s_wifi_scanning?"正在扫描附近网络":"暂未发现网络，请重新扫描");
    action_state((EpdRect){48,984,282,96},"重新扫描",false,!s_wifi_scanning&&!s_wifi_connecting);
    action_state((EpdRect){354,984,282,96},"手动输入",false,!s_wifi_scanning&&!s_wifi_connecting);
    pager(s_wifi_page,s_wifi_count,6);
}
static void draw_wifi_input(void) {
    epd_fill_rect((EpdRect){48,264,588,144},PAPER,fb());panel((EpdRect){48,264,588,144});
    if(s_wifi_manual)label(72,344,612,32,INK,*s_wifi_name?s_wifi_name:"输入网络名称");
    else wr_paragraph(72,304,612,24,36,3,INK,*s_wifi_password?s_wifi_password:"输入密码");
}
static void draw_wifi_edit(void) {
    page_header("返回",s_wifi_manual?"网络名称":"网络密码",s_wifi_manual?"输入名称后继续设置密码":s_wifi_name);
    draw_wifi_input();
    wr_paragraph(48,468,636,26,40,3,MUTED,s_setup_message);
    draw_keyboard(s_wifi_keys);
    if(!s_wifi_manual && s_wifi_saved){action((EpdRect){48,1096,282,96},"忘记网络",false);action((EpdRect){354,1096,282,96},"连接",true);}
    else action(done_rect(),s_wifi_manual?"下一步":"连接",true);
}
static void start_wifi_scan(void) {
    s_wifi_count=s_wifi_page=0;s_wifi_connecting=false;
    s_wifi_scanning=folio_network_scan_start(s_setup_message,sizeof(s_setup_message));
    if(s_wifi_scanning){snprintf(s_setup_message,sizeof(s_setup_message),"正在扫描附近网络");s_setup_deadline=esp_timer_get_time()+20000000;}
    show_view(VIEW_WIFI,true);
}
static const char* wifi_connected_message(void) {
    return s_wifi_return==VIEW_WEREAD?"已连接，返回后继续微信读书。":"已连接。下次需要联网时会自动连接。";
}
static void enter_wifi_for(view_t target) {
    s_wifi_return=target;
    folio_network_status_t state;folio_network_get_status(&state);
    if(state.connected){s_wifi_scanning=s_wifi_connecting=false;snprintf(s_setup_message,sizeof(s_setup_message),"%s",wifi_connected_message());show_view(VIEW_WIFI,true);return;}
    if(!stop_transfer()){snprintf(s_setup_message,sizeof(s_setup_message),"网络正在停止，请稍后重新扫描。");show_view(VIEW_WIFI,true);return;}
    s_view=VIEW_WIFI;start_wifi_scan();
}
static void enter_wifi(void) {enter_wifi_for(VIEW_DEVICE);}
static void enter_wifi_from_weread(void) {enter_wifi_for(VIEW_WEREAD);}
static void enter_time(void) {
    snprintf(s_setup_message,sizeof(s_setup_message),"手动设置或通过 Wi-Fi 同步时间。\n长时间关机后建议联网校时。");show_view(VIEW_TIME,true);
}
static void draw_time_card(void) {
    EpdRect card={48,240,588,208};epd_fill_rect(card,PAPER,fb());panel(card);
    char clock[24],date[72];struct tm t;ui_clock(clock,sizeof(clock));const char* period=ui_period();
    if(folio_power_datetime(&t,s_settings.timezone_minutes))snprintf(date,sizeof(date),"%s%s%04d 年 %02d 月 %02d 日",period,*period?" · ":"",t.tm_year+1900,t.tm_mon+1,t.tm_mday);else snprintf(date,sizeof(date),"日期尚未设置");
    centered_in((EpdRect){72,264,540,88},48,INK,clock);centered_in((EpdRect){72,364,540,56},28,MUTED,date);
    s_time_clock_minute=folio_power_unix_time()/60;
}
static void draw_time(void) {
    page_header("返回","日期与时间","时间保存至设备时钟");
    draw_time_card();
    action_state((EpdRect){48,472,588,96},"手动设置日期与时间",false,!s_clock_waiting);
    action_state((EpdRect){48,592,588,96},s_clock_waiting?"正在通过 Wi-Fi 同步":"通过 Wi-Fi 同步时间",false,!s_clock_waiting);
    label(48,756,636,28,MUTED,"时间格式");
    action((EpdRect){48,784,282,96},"24 小时",!FOLIO_CLOCK_12H(&s_settings));
    action((EpdRect){354,784,282,96},"12 小时",FOLIO_CLOCK_12H(&s_settings));
    char zone[48];int tz=s_settings.timezone_minutes;
    snprintf(zone,sizeof(zone),"时区 UTC%c%02d:%02d",tz<0?'-':'+',abs(tz)/60,abs(tz)%60);
    centered_in((EpdRect){152,912,380,80},28,INK,zone);
    icon((EpdRect){80,936,32,32},FOLIO_ICON_MINUS,INK);icon((EpdRect){572,936,32,32},FOLIO_ICON_PLUS,INK);
    wr_paragraph(48,1052,636,24,36,3,MUTED,s_setup_message);
}
static void draw_clock_fields(void) {
    epd_fill_rect((EpdRect){48,352,588,280},PAPER,fb());
    const char* labels[]={"年","月","日","时","分"};int values[]={s_clock_edit.tm_year+1900,s_clock_edit.tm_mon+1,s_clock_edit.tm_mday,s_clock_edit.tm_hour,s_clock_edit.tm_min};
    for(unsigned i=0;i<5;++i){EpdRect field=clock_field(i);selected_panel(field,s_clock_field==i);centered_in((EpdRect){field.x,field.y+12,field.width,36},24,MUTED,labels[i]);char text[16];snprintf(text,sizeof(text),i?"%02d":"%04d",values[i]);centered_in((EpdRect){field.x,field.y+56,field.width,56},38,INK,text);}
}
static void draw_time_edit(void) {
    page_header("返回","手动设置时间","选择数值，再用减号或加号调整（24 小时）");
    draw_clock_fields();
    EpdRect minus={48,752,282,96},plus={354,752,282,96};panel(minus);panel(plus);icon_label(minus,FOLIO_ICON_MINUS,32,30,INK,"减少");icon_label(plus,FOLIO_ICON_PLUS,32,30,INK,"增加");
    wr_paragraph(48,924,636,26,40,3,MUTED,s_setup_message);action(done_rect(),"保存时间",true);
}
static void change_clock(int delta) {
    int* fields[]={&s_clock_edit.tm_year,&s_clock_edit.tm_mon,&s_clock_edit.tm_mday,&s_clock_edit.tm_hour,&s_clock_edit.tm_min};
    const int min[]={124,0,1,0,0},max[]={199,11,folio_clock_days(s_clock_edit.tm_year+1900,s_clock_edit.tm_mon+1),23,59};
    int n=*fields[s_clock_field]+delta;if(n<min[s_clock_field])n=max[s_clock_field];if(n>max[s_clock_field])n=min[s_clock_field];*fields[s_clock_field]=n;
    int days=folio_clock_days(s_clock_edit.tm_year+1900,s_clock_edit.tm_mon+1);if(s_clock_edit.tm_mday>days)s_clock_edit.tm_mday=days;
}
static bool setup_back(void) {
    if(s_view==VIEW_WIFI_EDIT){memset(s_wifi_password,0,sizeof(s_wifi_password));show_view(VIEW_WIFI,true);return true;}
    if(s_view==VIEW_TIME_EDIT){show_view(VIEW_TIME,true);return true;}
    if(s_view!=VIEW_WIFI && s_view!=VIEW_TIME)return false;
    folio_network_status_t state;folio_network_get_status(&state);
    bool keep_connection=s_view==VIEW_WIFI && state.connected;
    if(!keep_connection && !folio_network_stop()){snprintf(s_setup_message,sizeof(s_setup_message),"正在关闭网络，请稍后再返回。");show_view(s_view,false);return true;}
    s_clock_waiting=s_clock_ntp_started=s_wifi_scanning=s_wifi_connecting=false;memset(s_wifi_password,0,sizeof(s_wifi_password));
    if(s_view==VIEW_WIFI && s_wifi_return==VIEW_WEREAD)enter_weread();else show_view(VIEW_DEVICE,true);return true;
}
static bool setup_tap(int x,int y) {
    if(s_view==VIEW_WIFI) {
        if(s_wifi_scanning||s_wifi_connecting)return true;
        if(hit((EpdRect){48,984,282,96},x,y)){start_wifi_scan();return true;}
        if(hit((EpdRect){354,984,282,96},x,y)){s_wifi_manual=true;s_wifi_name[0]=s_wifi_password[0]=0;s_wifi_keys=0;s_setup_message[0]=0;show_view(VIEW_WIFI_EDIT,true);return true;}
        int page=footer_hit(x,y);
        if(page==0&&s_wifi_page){--s_wifi_page;show_view(VIEW_WIFI,false);return true;}
        if(page==2&&(s_wifi_page+1)*6<s_wifi_count){++s_wifi_page;show_view(VIEW_WIFI,false);return true;}
        for(unsigned i=0;i<6;++i){size_t n=s_wifi_page*6+i;if(n<s_wifi_count && hit(wifi_row(i),x,y)) {
            snprintf(s_wifi_name,sizeof(s_wifi_name),"%s",s_wifi_items[n].ssid);s_wifi_saved=folio_network_saved_password(s_wifi_name,s_wifi_password,sizeof(s_wifi_password));s_wifi_manual=false;s_wifi_keys=0;
            snprintf(s_setup_message,sizeof(s_setup_message),s_wifi_items[n].secured?"密码直接显示；用大小写、123 和 #+= 切换键盘。":"开放网络可留空密码直接连接。");show_view(VIEW_WIFI_EDIT,true);return true;}}
        return true;
    }
    if(s_view==VIEW_WIFI_EDIT) {
        char* field=s_wifi_manual?s_wifi_name:s_wifi_password;size_t cap=s_wifi_manual?sizeof(s_wifi_name):sizeof(s_wifi_password);
        if(!s_wifi_manual && s_wifi_saved && hit((EpdRect){48,1096,282,96},x,y)){
            if(folio_network_forget(s_wifi_name,s_setup_message,sizeof(s_setup_message))){s_wifi_saved=false;memset(s_wifi_password,0,sizeof(s_wifi_password));snprintf(s_setup_message,sizeof(s_setup_message),"已忘记此网络，其他网络不受影响。");}
            show_view(VIEW_WIFI_EDIT,false);return true;
        }
        if(hit(!s_wifi_manual&&s_wifi_saved?(EpdRect){354,1096,282,96}:done_rect(),x,y)) {
            if(s_wifi_manual){if(!s_wifi_name[0])return true;s_wifi_manual=false;s_wifi_saved=folio_network_saved_password(s_wifi_name,s_wifi_password,sizeof(s_wifi_password));s_setup_message[0]=0;show_view(VIEW_WIFI_EDIT,true);return true;}
            if(!folio_network_configure(s_wifi_name,s_wifi_password,s_setup_message,sizeof(s_setup_message)) || !folio_network_connect(s_setup_message,sizeof(s_setup_message))){show_view(VIEW_WIFI_EDIT,true);return true;}
            memset(s_wifi_password,0,sizeof(s_wifi_password));s_wifi_connecting=true;s_setup_deadline=esp_timer_get_time()+35000000;
            snprintf(s_setup_message,sizeof(s_setup_message),"正在连接所选网络");show_view(VIEW_WIFI,true);return true;
        }
        for(unsigned i=0;i<31;++i)if(hit(catalog_key(i),x,y)) {size_t n=strlen(field);const char* chars=keyboard_letters(s_wifi_keys);
            if(i<26 && i<strlen(chars) && n+1<cap){field[n]=chars[i];field[n+1]=0;}
            else if(i==26&&n+1<cap){field[n]=' ';field[n+1]=0;}
            else if(i==27&&n)field[n-1]=0;else if(i==28)memset(field,0,cap);
            else if(i==29)s_wifi_keys=s_wifi_keys<2?2:0;else if(i==30)s_wifi_keys^=1;
            if(i>=29)refresh_keyboard(s_wifi_keys);
            else {draw_wifi_input();present_region((EpdRect){48,264,588,144});}
            break;}
        return true;
    }
    if(s_view==VIEW_TIME) {
        if(hit((EpdRect){48,472,588,96},x,y)&&!s_clock_waiting){if(!folio_power_datetime(&s_clock_edit,s_settings.timezone_minutes))s_clock_edit=(struct tm){.tm_year=126,.tm_mon=0,.tm_mday=1};s_clock_field=0;s_setup_message[0]=0;show_view(VIEW_TIME_EDIT,true);}
        else if(hit((EpdRect){48,592,588,96},x,y)&&!s_clock_waiting){
            if(folio_network_connect(s_setup_message,sizeof(s_setup_message))){s_clock_waiting=true;s_clock_ntp_started=false;s_setup_deadline=esp_timer_get_time()+35000000;snprintf(s_setup_message,sizeof(s_setup_message),"正在连接校时网络，请稍候。");}show_view(VIEW_TIME,false);
        } else if(hit((EpdRect){48,784,588,96},x,y)){if(x<342)s_settings.reserved[0]&=(uint8_t)~0x10u;else s_settings.reserved[0]|=0x10u;setup_save(VIEW_TIME);}
        else if(hit((EpdRect){48,904,96,96},x,y)||hit((EpdRect){540,904,96,96},x,y)){int tz=s_settings.timezone_minutes+(x<342?-30:30);if(tz>=-720&&tz<=840){s_settings.timezone_minutes=tz;setup_save(VIEW_TIME);}}
        return true;
    }
    if(s_view==VIEW_TIME_EDIT) {
        for(unsigned i=0;i<5;++i)if(hit(clock_field(i),x,y)){s_clock_field=i;draw_clock_fields();present_region((EpdRect){48,352,588,280});return true;}
        if(hit((EpdRect){48,752,588,96},x,y)){change_clock(x<342?-1:1);draw_clock_fields();present_region((EpdRect){48,352,588,280});}
        else if(hit(done_rect(),x,y)){uint32_t utc;
            if(!folio_clock_to_utc(&s_clock_edit,s_settings.timezone_minutes,&utc)||!folio_power_set_time(utc)){snprintf(s_setup_message,sizeof(s_setup_message),"时钟保存失败，请检查日期后重试。");show_view(VIEW_TIME_EDIT,true);}
            else {snprintf(s_setup_message,sizeof(s_setup_message),"时间已保存至设备。");show_view(VIEW_TIME,true);}}
        return true;
    }
    return false;
}
static void setup_time_received(bool saved) {
    if(!s_clock_waiting)return;
    ESP_LOGI(TAG,"Network clock saved to PMU: %d",saved);
    s_clock_waiting=s_clock_ntp_started=false;folio_network_cancel_time();
    snprintf(s_setup_message,sizeof(s_setup_message),saved?"已通过 Wi-Fi 同步并保存时间。":"已获取网络时间，但设备时钟保存失败，请重试。");
    (void)folio_network_stop();if(s_view==VIEW_TIME)show_view(VIEW_TIME,false);
}
static void refresh_setup(void) {
    // A visible clock is live, but a draft in TIME_EDIT belongs to the user.
    if(s_view==VIEW_TIME && s_font && folio_power_unix_time()/60!=s_time_clock_minute){
        int64_t idle=s_last_activity;
        EpdRect status={328,8,324,96};epd_fill_rect(status,PAPER,fb());rail("返回");
        present_region(status);draw_time_card();present_region((EpdRect){48,240,588,208});
        s_last_activity=idle;
    }
    if(s_view==VIEW_WIFI && s_wifi_scanning) {
        if(folio_network_scan_result(s_wifi_items,16,&s_wifi_count)){s_wifi_scanning=false;snprintf(s_setup_message,sizeof(s_setup_message),"选择网络后输入密码");show_view(VIEW_WIFI,false);}
        else if(esp_timer_get_time()>s_setup_deadline){s_wifi_scanning=false;snprintf(s_setup_message,sizeof(s_setup_message),"扫描超时，请重新扫描。");show_view(VIEW_WIFI,false);}
    }
    if(s_view==VIEW_WIFI && s_wifi_connecting){folio_network_status_t state;folio_network_get_status(&state);
        if(state.connected||!state.connecting||esp_timer_get_time()>s_setup_deadline){s_wifi_connecting=false;snprintf(s_setup_message,sizeof(s_setup_message),"%s",state.connected?wifi_connected_message():!state.connecting&&state.message[0]?state.message:"连接超时，请检查网络后重试。");show_view(VIEW_WIFI,false);}}
    if(!s_clock_waiting){s_clock_ntp_started=false;return;}
    folio_network_status_t state;folio_network_get_status(&state);
    if(!state.connected && !state.connecting){
        s_clock_waiting=s_clock_ntp_started=false;folio_network_cancel_time();
        snprintf(s_setup_message,sizeof(s_setup_message),"%s",state.message[0]?state.message:"Wi-Fi 连接中断，请重新连接后校时。");
        (void)folio_network_stop();if(s_view==VIEW_TIME)show_view(VIEW_TIME,false);return;
    }
    if(esp_timer_get_time()>s_setup_deadline){
        snprintf(s_setup_message,sizeof(s_setup_message),"%s",s_clock_ntp_started?"时间服务器未响应，请检查网络能否访问互联网。":"Wi-Fi 连接超时，尚未开始校时。");
        s_clock_waiting=s_clock_ntp_started=false;folio_network_cancel_time();(void)folio_network_stop();if(s_view==VIEW_TIME)show_view(VIEW_TIME,false);return;
    }
    if(!s_clock_ntp_started && state.connected){
        s_clock_ntp_started=folio_network_sync_time(s_setup_message,sizeof(s_setup_message));
        if(s_clock_ntp_started){s_setup_deadline=esp_timer_get_time()+65000000;snprintf(s_setup_message,sizeof(s_setup_message),"Wi-Fi 已连接，正在获取网络时间。");}
        else{s_clock_waiting=false;(void)folio_network_stop();}
        if(s_view==VIEW_TIME)show_view(VIEW_TIME,false);
    }
}
