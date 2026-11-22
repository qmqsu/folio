// Home keeps the reading action above the fold and uses the panel's native
// grayscale only where it carries data. Included after the shared UI helpers.
static EpdRect home_recent_rect(unsigned index) {
    return (EpdRect){72,792+(int)index*100,540,96};
}
static EpdRect home_continue_rect(void) {return (EpdRect){48,152,588,288};}

static int root_nav_hit(int x,int y) {
    for(int i=0;i<3;++i) if(hit((EpdRect){48+i*204,1120,180,96},x,y)) return i;
    return -1;
}

static void draw_root_nav(view_t selected) {
    const folio_icon_t icons[]={FOLIO_ICON_HOME,FOLIO_ICON_BOOK,FOLIO_ICON_SETTINGS};
    const view_t views[]={VIEW_HOME,VIEW_LIBRARY,VIEW_SYSTEM};
    rule_at(48,1124,588,RULE);
    for(int i=0;i<3;++i) {
        int center=146+i*196; bool current=selected==views[i];
        icon((EpdRect){center-22,1140,44,44},icons[i],current?INK:MUTED);
        if(current)folio_gfx_fill_round_rect(fb(),(EpdRect){center-12,1194,24,4},2,INK);
    }
}

static void home_duration(uint64_t seconds,char* text,size_t capacity) {
    if(!seconds) snprintf(text,capacity,"0 分钟");
    else if(seconds<60) snprintf(text,capacity,"不足 1 分");
    else if(seconds<3600) snprintf(text,capacity,"%llu 分钟",(unsigned long long)(seconds/60));
    else snprintf(text,capacity,"%llu 小时 %llu 分",
                  (unsigned long long)(seconds/3600),(unsigned long long)(seconds%3600/60));
}

static void home_minutes(uint32_t seconds,char* text,size_t capacity) {
    if(seconds && seconds<60) snprintf(text,capacity,"<1");
    else snprintf(text,capacity,"%lu",(unsigned long)(seconds/60));
}

static void home_title(int x,int baseline,int right,int size,int leading,const char* title) {
    // Keep a full first line, then let label add an ellipsis to the last line.
    // Book titles are bounded to 127 UTF-8 bytes by the history record.
    (void)folio_font_set_size(s_font,size);
    const char* cursor=title; const char* split=title; int used=0;
    while(*cursor) {
        const char* start=cursor; uint32_t cp=folio_utf8_next(&cursor);
        int advance=folio_font_advance(s_font,cp,0);
        if(cp=='\n' || used+advance>right-x) {split=start;break;}
        used+=advance; split=cursor;
    }
    char first[128]; size_t count=(size_t)(split-title);
    if(count>=sizeof(first))count=sizeof(first)-1;
    memcpy(first,title,count); first[count]='\0';
    label(x,baseline,right,size,INK,first);
    if(*split=='\n')++split;
    if(*split)label(x,baseline+leading,right,size,INK,split);
}

static int32_t home_epoch_day(uint32_t utc) {
    int64_t seconds=(int64_t)utc+(int64_t)s_settings.timezone_minutes*60;
    return (int32_t)(seconds>=0?seconds/86400:(seconds-86399)/86400);
}

static void home_day_label(int32_t day,char* text,size_t capacity) {
    int64_t seconds=(int64_t)day*86400;
    time_t timestamp=(time_t)seconds; struct tm date;
    if((int64_t)timestamp==seconds && gmtime_r(&timestamp,&date))
        snprintf(text,capacity,"%d/%d",date.tm_mon+1,date.tm_mday);
    else snprintf(text,capacity,"-");
}

static const char* home_book_format(const folio_recent_book_t* book) {
    const char* ext=strrchr(book->path,'.');
    return ext && !strcasecmp(ext,".pdf") ? "PDF" : ext && strcasecmp(ext,".epub")==0?"EPUB":"TXT";
}

static bool home_book_available(const folio_recent_book_t* book) {
    if(!book || !s_books) return false;
    for(size_t i=0;i<s_book_count;++i)
        if(strcmp(s_books[i].path,book->path)==0) return true;
    return false;
}

static void home_metric(int x,int right,const char* caption,const char* value) {
    label(x,534,right,22,MUTED,caption);
    int size=36;
    while(size>26 && text_width(size,value)>right-x) size-=2;
    label(x,576,right,size,INK,value);
}

static EpdRect home_heat_cell(unsigned index) {
    return (EpdRect){72+(int)(index%14)*39,614+(int)(index/14)*39,32,32};
}

static void home_heatmap(int32_t today) {
    // A rolling window ends today: every position is an actual day, including
    // zero-reading days. No future slots or weekday-alignment padding.
    for(unsigned i=0;i<28;++i) {
        int32_t day=today-27+(int32_t)i;
        uint32_t seconds=folio_activity_day_seconds(s_activity,day);
        uint8_t shade=!seconds?PAPER:seconds<15*60?0xB0:seconds<30*60?0x60:INK;
        EpdRect cell=home_heat_cell(i);
        folio_gfx_fill_round_rect(fb(),cell,4,shade);
        if(!seconds)folio_gfx_round_rect(fb(),cell,4,RULE,1);
    }
}

static void home_bars(int32_t today) {
    uint32_t seconds[7],maximum=60;
    for(int i=0;i<7;++i) {
        seconds[i]=folio_activity_day_seconds(s_activity,today-6+i);
        if(seconds[i]>maximum) maximum=seconds[i];
    }
    rule_at(72,664,540,RULE);
    for(int i=0;i<7;++i) {
        int center=108+i*78;
        int bar=(int)((uint64_t)seconds[i]*52/maximum);
        if(seconds[i] && bar<3) bar=3;
        if(bar)folio_gfx_fill_round_rect(fb(),(EpdRect){center-18,664-bar,36,bar},4,i==6?INK:0x60);
        char value[24]; home_minutes(seconds[i],value,sizeof(value));
        centered(center,bar?654-bar:654,72,20,INK,value);
        if(i==6) snprintf(value,sizeof(value),"今天");
        else home_day_label(today-6+i,value,sizeof(value));
        centered(center,694,74,20,i==6?INK:MUTED,value);
    }
}

static void home_continue(void) {
    panel(home_continue_rect());
    const folio_recent_book_t* book=folio_activity_recent(s_activity,0);
    if(book) {
        cover((EpdRect){72,176,168,240},book->path,book->title,home_book_format(book),false);
        label(264,204,612,24,MUTED,"继续阅读");
        home_title(264,262,612,38,48,book->title);
        char progress[64];
        if(!home_book_available(book)) snprintf(progress,sizeof(progress),"文件缺失 · 记录保留");
        else if(book->page) snprintf(progress,sizeof(progress),"第 %lu 页  ·  %lu%%",
                               (unsigned long)book->page,(unsigned long)book->percent);
        else snprintf(progress,sizeof(progress),"从上次停下的地方继续");
        label(264,352,612,24,MUTED,progress);
        rule_at(264,374,348,RULE);
        unsigned percent=book->percent>100?100:book->percent;
        if(percent) epd_fill_rect((EpdRect){264,373,(int)(348*percent/100),3},INK,fb());
        label(264,baseline_for_center_y(30,"接着读",400),564,30,INK,"接着读");
        icon((EpdRect){580,386,28,28},FOLIO_ICON_CHEVRON_RIGHT,INK);
    } else {
        icon((EpdRect){72,184,64,64},FOLIO_ICON_BOOK,INK);
        label(72,300,612,42,INK,"从第一本书开始");
        label(72,346,612,26,MUTED,"在书页之间，找到自己的片刻");
        label(72,baseline_for_center_y(30,"前往书架",400),564,30,INK,"前往书架");
        icon((EpdRect){580,386,28,28},FOLIO_ICON_CHEVRON_RIGHT,INK);
    }
}

static void home_recents(void) {
    size_t count=folio_activity_recent_count(s_activity);
    if(count>FOLIO_RECENT_LIMIT) count=FOLIO_RECENT_LIMIT;
    panel((EpdRect){48,732,588,372});
    label(72,776,612,30,INK,"最近阅读");
    for(unsigned i=0;i<count;++i) {
        const folio_recent_book_t* book=folio_activity_recent(s_activity,i);
        if(!book) break;
        EpdRect card=home_recent_rect(i);
        bool available=home_book_available(book);
        cover((EpdRect){card.x,card.y+6,56,84},book->path,book->title,home_book_format(book),false);
        label(152,baseline_for_center_y(32,book->title,card.y+30),560,32,available?INK:MUTED,book->title);
        char progress[64];
        if(!available)snprintf(progress,sizeof(progress),"文件缺失 · 记录保留");
        else if(book->page)snprintf(progress,sizeof(progress),"第 %lu 页  ·  %lu%%",
                                   (unsigned long)book->page,(unsigned long)book->percent);
        else snprintf(progress,sizeof(progress),"%s",home_book_format(book));
        label(152,baseline_for_center_y(24,progress,card.y+66),560,24,MUTED,progress);
        icon((EpdRect){card.x+card.width-24,card.y+36,24,24},FOLIO_ICON_CHEVRON_RIGHT,available?INK:MUTED);
        if(i+1<count)rule_at(152,card.y+98,460,RULE);
    }
    if(!count) {
        bool unavailable=s_activity_load_failed && !folio_activity_total_seconds(s_activity);
        icon((EpdRect){76,898,36,36},FOLIO_ICON_BOOK,MUTED);
        label(136,920,612,28,INK,unavailable?"阅读记录暂不可用":"读过的书，会留在这里");
        label(136,968,612,24,MUTED,unavailable?"请检查 TF 卡后重新扫描书架":"从书架挑一本喜欢的开始");
    }
}

static void draw_home(void) {
    epd_hl_set_all_white(&s_hw.hl);
    label(48,baseline_for_center_y(52,"页间 Folio",80),388,52,INK,"页间 Folio");
    struct tm local; uint32_t utc=folio_power_unix_time();
    s_home_clock_minute=utc/60;
    bool dated=utc!=0 && folio_power_datetime(&local,s_settings.timezone_minutes);
    char date[72],clock[24];
    if(dated) {
        const char* weekday[]={"日","一","二","三","四","五","六"};
        snprintf(date,sizeof(date),"%d.%02d.%02d · 周%s",
                 local.tm_year+1900,local.tm_mon+1,local.tm_mday,weekday[local.tm_wday]);
        ui_clock(clock,sizeof(clock));
    } else {
        snprintf(date,sizeof(date),"日期尚未校准");
        snprintf(clock,sizeof(clock),"--:--");
    }
    const char* period=dated?ui_period():"";
    if(*period)right_label(636-text_width(48,clock)-12,88,24,MUTED,period);
    right_label(636,88,48,INK,clock); right_label(636,126,22,MUTED,date);
    char power[48],battery_label[64];
    folio_power_status(power,sizeof(power),s_settings.timezone_minutes);
    const char* battery=strrchr(power,' ');battery=battery?battery+1:"--%";
    snprintf(s_home_battery,sizeof(s_home_battery),"%s",battery);
    s_home_status_checked=esp_timer_get_time();
    snprintf(battery_label,sizeof(battery_label),"电量 %s",battery);
    label(48,126,300,24,MUTED,battery_label);
    home_continue();
    panel((EpdRect){48,460,588,252});
    label(72,500,352,30,INK,"阅读时长");
    right_label(612,500,22,MUTED,s_settings.home_chart?"近7天 · 分钟":"近28天");
    bool records=s_activity && (!s_activity_load_failed || folio_activity_total_seconds(s_activity)>0 ||
                               folio_activity_recent_count(s_activity)>0);
    char today[64],total[64];
    if(records && dated) home_duration(folio_activity_day_seconds(s_activity,home_epoch_day(utc)),today,sizeof(today));
    else snprintf(today,sizeof(today),"%s",records?"未校准":"暂不可用");
    if(records) home_duration(folio_activity_total_seconds(s_activity),total,sizeof(total));
    else snprintf(total,sizeof(total),"暂不可用");
    const char* total_caption=s_activity_save_failed?"累计（尚未保存）":
        s_activity_load_failed && records?"累计（已恢复）":
        folio_activity_undated_seconds(s_activity)?"累计（含未分日）":"累计阅读";
    home_metric(72,326,"今日阅读",today); home_metric(356,612,total_caption,total);
    if(!records) {
        centered(342,634,540,28,INK,"阅读记录暂不可用");
        centered(342,678,540,24,MUTED,"请检查 TF 卡后重新扫描书架");
    } else if(!dated) {
        centered(342,624,540,28,INK,"时间尚未校准");
        centered(342,656,540,24,MUTED,"已记录的阅读时长会计入累计");
        centered(342,690,540,24,MUTED,"校准日期后，可按天查看阅读统计");
    } else if(s_settings.home_chart) home_bars(home_epoch_day(utc));
    else home_heatmap(home_epoch_day(utc));
    home_recents(); draw_root_nav(VIEW_HOME);
}

static void draw_home_settings(void) {
    page_header("设置","显示与电源","首页统计、锁屏与关机画面");
    settings_tabs();
    label(48,384,636,26,MUTED,"首页阅读统计");
    const char* charts[]={"热力图","条形图"};
    for(unsigned i=0;i<2;++i)action((EpdRect){48+(int)i*300,400,288,96},charts[i],s_settings.home_chart==i);
    label(48,560,636,26,MUTED,"锁屏显示");label(48,736,636,26,MUTED,"关机显示");
    const char* lock_styles[]={"书籍封面","阅读正文","时间显示"};
    const char* off_styles[]={"书籍封面","阅读正文","图片壁纸"};
    for(unsigned i=0;i<3;++i){
        action((EpdRect){48+(int)i*200,576,188,96},lock_styles[i],FOLIO_LOCK_STYLE(&s_settings)==i);
        action((EpdRect){48+(int)i*200,752,188,96},off_styles[i],FOLIO_OFF_STYLE(&s_settings)==i);
    }
    action((EpdRect){48,864,588,96},"选择关机壁纸",false);
    action((EpdRect){48,976,282,96},"预览锁屏",false);action((EpdRect){354,976,282,96},"预览关机",false);
    centered_in((EpdRect){48,1088,588,64},24,MUTED,"锁屏时间按分钟更新 · 关机保留静态画面");
}

