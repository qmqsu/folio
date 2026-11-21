// Folio native UI: 684 x 1216 portrait, restrained ink, generous touch areas.
// Included by app_main.c so rendering and hit geometry share the same state.
#define INK 0x00
#define MUTED 0x40
#define RULE 0xA0
#define PAPER 0xF0
#define UI_RADIUS 12
#define UI_BORDER 0x60
#define GUTTER 48
#define LIST_TOP 240
#define LIST_ROW 108
#define LIBRARY_PAGE_SIZE 6

static int width(void) { return epd_rotated_display_width(); }
static int height(void) { return epd_rotated_display_height(); }
static size_t list_rows(void) { return 7; }
static uint8_t* fb(void) { return s_hw.framebuffer; }
static folio_reader_options_t reader_options(void) {
    return (folio_reader_options_t){.margin=s_settings.margin, .line_gap=s_settings.line_gap,
        .paragraph_gap=s_settings.paragraph_gap, .justify=s_settings.justify != 0,
        .hide_footer=s_settings.footer_hidden!=0, .underline=s_settings.underline, .underline_shade=s_settings.underline_shade};
}
static bool hit(EpdRect r, int x, int y) {
    return x >= r.x && x < r.x+r.width && y >= r.y && y < r.y+r.height;
}
static EpdRect back_rect(void) { return (EpdRect){32, 8, 96, 96}; }
static EpdRect library_tool(unsigned i) { return (EpdRect){348+(int)i*96,112,96,96}; }
static EpdRect done_rect(void) { return (EpdRect){48, 1096, 588, 96}; }
static EpdRect tool_rect(unsigned i) { return (EpdRect){56+(int)(i%3)*196, 640+(int)(i/3)*144, 180, 128}; }
static EpdRect setting_step(int row, int direction) {
    int top=s_settings_tab ? 336+row*112 : 524+row*112;
    return (EpdRect){direction<0 ? 332 : 540, top, 96, 96};
}
static const uint16_t auto_shutdown_minutes[] = {30,180,360};
#define AUTO_SHUTDOWN_CHOICES (sizeof(auto_shutdown_minutes)/sizeof(auto_shutdown_minutes[0]))
static EpdRect auto_shutdown_toggle(void) { return (EpdRect){48,688,588,96}; }
static EpdRect auto_shutdown_choice(unsigned i) {
    return (EpdRect){48+(int)i*200,856,188,88};
}
static EpdRect jump_step(unsigned i) { return (EpdRect){48+(int)i*150, 600, 138, 104}; }
static int footer_hit(int x, int y) {
    if (hit((EpdRect){32,1080,160,112},x,y)) return 0;
    if (hit((EpdRect){208,1080,268,112},x,y)) return 1;
    if (hit((EpdRect){492,1080,160,112},x,y)) return 2;
    return -1;
}
static int list_hit(int x, int y) {
    if (x<48 || x>=636 || y<LIST_TOP) return -1;
    int row=(y-LIST_TOP)/LIST_ROW;
    return row < (int)list_rows() && (y-LIST_TOP)%LIST_ROW < 96 ? row : -1;
}
static EpdRect library_item(unsigned row) {
    if (s_settings.library_list) return (EpdRect){48,240+(int)row*112,588,96};
    return (EpdRect){48+(int)(row%3)*208,248+(int)(row/3)*376,172,340};
}
static int library_hit(int x,int y) {
    for (unsigned i=0;i<LIBRARY_PAGE_SIZE;++i) if(hit(library_item(i),x,y)) return (int)i;
    return -1;
}
static void rule_at(int x,int y,int length,uint8_t color) {
    epd_fill_rect((EpdRect){x,y,length,1},color,fb());
}
static void panel(EpdRect r) { folio_gfx_round_rect(fb(),r,UI_RADIUS,UI_BORDER,2); }
static void selected_panel(EpdRect r,bool selected) {
    folio_gfx_round_rect(fb(),r,UI_RADIUS,selected?INK:UI_BORDER,selected?3:2);
}
static int text_width(int size,const char* value) {
    (void)folio_font_set_size(s_font,size);
    int sum=0; const char* cursor=value;
    while(cursor && *cursor) { uint32_t cp=folio_utf8_next(&cursor); if(cp=='\n') break; sum+=folio_font_advance(s_font,cp,0); }
    return sum;
}
static void label(int x,int baseline,int right,int size,uint8_t color,const char* value) {
    if (!s_font || !value || right<=x) return;
    (void)folio_font_set_size(s_font,size);
    const char* cursor=value;
    int dots=text_width(size,"...");
    bool truncate=text_width(size,value)>right-x;
    while(*cursor) {
        uint32_t cp=folio_utf8_next(&cursor); if(cp=='\n') break;
        int advance=folio_font_advance(s_font,cp,0);
        if(x+advance+(truncate?dots:0)>right) {
            if(truncate) for(int i=0;i<3;++i) { (void)folio_font_draw_glyph_color(s_font,fb(),x,baseline,'.',color,PAPER); x+=folio_font_advance(s_font,'.',0); }
            break;
        }
        (void)folio_font_draw_glyph_color(s_font,fb(),x,baseline,cp,color,PAPER); x+=advance;
    }
}
static void centered(int center,int baseline,int max_width,int size,uint8_t color,const char* value) {
    int used=text_width(size,value); if(used>max_width) used=max_width;
    label(center-used/2,baseline,center+max_width/2,size,color,value);
}
static void right_label(int right,int baseline,int size,uint8_t color,const char* value) {
    label(right-text_width(size,value),baseline,right,size,color,value);
}
typedef struct {
    uint32_t codepoints[128];
    size_t count;
    int advance,left,top,right,bottom;
    bool ink;
} ui_text_run_t;
static void measure_ui_text(int size,const char* value,int max_width,ui_text_run_t* run) {
    memset(run,0,sizeof(*run));
    if(!s_font || !value || max_width<=0)return;
    (void)folio_font_set_size(s_font,size);
    const char* cursor=value;
    while(*cursor && run->count<125) {
        uint32_t cp=folio_utf8_next(&cursor); if(cp=='\n')break;
        run->codepoints[run->count++]=cp;
        run->advance+=folio_font_advance(s_font,cp,0);
    }
    bool truncated=run->advance>max_width || (run->count==125 && *cursor && *cursor!='\n');
    if(truncated) {
        int dots=3*folio_font_advance(s_font,'.',0);
        while(run->count && run->advance+dots>max_width)
            run->advance-=folio_font_advance(s_font,run->codepoints[--run->count],0);
        if(dots<=max_width) {
            for(unsigned i=0;i<3;++i)run->codepoints[run->count++]='.';
            run->advance+=dots;
        }
    }
    int pen=0;
    for(size_t i=0;i<run->count;++i) {
        int left,top,right,bottom;
        uint32_t cp=run->codepoints[i];
        if(folio_font_glyph_bounds(s_font,cp,&left,&top,&right,&bottom)) {
            left+=pen; right+=pen;
            if(!run->ink) {run->left=left;run->top=top;run->right=right;run->bottom=bottom;run->ink=true;}
            else {
                if(left<run->left)run->left=left;
                if(top<run->top)run->top=top;
                if(right>run->right)run->right=right;
                if(bottom>run->bottom)run->bottom=bottom;
            }
        }
        pen+=folio_font_advance(s_font,cp,0);
    }
}
static int baseline_for_center_y(int size,const char* value,int center_y) {
    ui_text_run_t run; measure_ui_text(size,value,32767,&run);
    return run.ink?center_y-run.top-(run.bottom-run.top)/2:center_y;
}
static void centered_on(EpdRect rect,int size,uint8_t color,uint8_t background,const char* value) {
    ui_text_run_t run; measure_ui_text(size,value,rect.width,&run);
    if(!run.ink || rect.height<=0)return;
    int x=rect.x+(rect.width-(run.right-run.left))/2-run.left;
    int baseline=rect.y+(rect.height-(run.bottom-run.top))/2-run.top;
    for(size_t i=0;i<run.count;++i) {
        uint32_t cp=run.codepoints[i];
        (void)folio_font_draw_glyph_color(s_font,fb(),x,baseline,cp,color,background);
        x+=folio_font_advance(s_font,cp,0);
    }
}
static void centered_in(EpdRect rect,int size,uint8_t color,const char* value) {
    centered_on(rect,size,color,PAPER,value);
}
static void ascii_centered_in(EpdRect rect,int scale,const char* value) {
    if(!value || !*value || scale<1)return;
    int ink_width=(int)(strlen(value)*6u-1u)*scale;
    folio_gfx_text(fb(),rect.x+(rect.width-ink_width)/2,rect.y+(rect.height-7*scale)/2,scale,value,INK);
}
static bool ui_ascii_word(uint32_t cp) {
    return (cp>='a' && cp<='z') || (cp>='A' && cp<='Z') || (cp>='0' && cp<='9') || cp=='-' || cp=='_';
}
static bool ui_closing(uint32_t cp) {
    return folio_is_closing_punctuation(cp) || cp=='.' || cp==',' || cp=='!' || cp=='?' || cp==';' || cp==':' || cp==')' || cp==']';
}
// Share explanatory-text wrapping across native pages. Keep short words and
// following punctuation together; oversized tokens retain scalar fallback.
static const char* ui_token(const char* start,int limit,int* width) {
    const char* first=start;uint32_t cp=folio_utf8_next(&first);
    int initial=folio_font_advance(s_font,cp,0);*width=initial;
    if(cp=='\n')return first;
    const char* end=first;unsigned letters=1;
    if(ui_ascii_word(cp))while(*end && ui_ascii_word((unsigned char)*end) && letters++<32){
        *width+=folio_font_advance(s_font,(unsigned char)*end,0);++end;
    }
    while(*end){
        const char* after=end;uint32_t next=folio_utf8_next(&after);if(!ui_closing(next))break;
        *width+=folio_font_advance(s_font,next,0);end=after;
    }
    if(*width>limit){*width=initial;return first;}
    return end;
}
static int paragraph(int x,int baseline,int right,int size,int leading,unsigned lines,uint8_t color,const char* value) {
    if(!s_font || !value || right<=x)return baseline;
    (void)folio_font_set_size(s_font,size);int left=x;unsigned row=0;const char* p=value;
    while(*p && row<lines){
        if(*p=='\n'){++p;x=left;baseline+=leading;++row;continue;}
        if(x==left && (*p==' ' || *p=='\t')){++p;continue;}
        int width;const char* end=ui_token(p,right-left,&width);
        if(width>right-left)break;
        if(x+width>right){x=left;baseline+=leading;if(++row==lines)break;continue;}
        while(p<end){uint32_t cp=folio_utf8_next(&p);
            (void)folio_font_draw_glyph_color(s_font,fb(),x,baseline,cp,color,PAPER);x+=folio_font_advance(s_font,cp,0);}
    }
    return baseline;
}
static void icon(EpdRect rect,folio_icon_t kind,uint8_t color) { folio_gfx_icon(fb(),rect,kind,color); }
static void icon_label(EpdRect rect,folio_icon_t kind,int icon_size,int size,uint8_t color,const char* title) {
    int gap=16,available=rect.width-icon_size-gap;
    int text=text_width(size,title); if(text>available)text=available;
    int left=rect.x+(rect.width-icon_size-gap-text)/2;
    icon((EpdRect){left,rect.y+(rect.height-icon_size)/2,icon_size,icon_size},kind,color);
    centered_in((EpdRect){left+icon_size+gap,rect.y,text,rect.height},size,color,title);
}
static void action_state(EpdRect rect,const char* title,bool primary,bool enabled) {
    if(enabled)selected_panel(rect,primary);
    else folio_gfx_round_rect(fb(),rect,UI_RADIUS,RULE,2);
    centered_in((EpdRect){rect.x+16,rect.y,rect.width-32,rect.height},32,enabled?INK:RULE,title);
}
static void action(EpdRect rect,const char* title,bool primary) { action_state(rect,title,primary,true); }
static void text_action(EpdRect rect,const char* title) {
    action(rect,title,false);
}
static void ui_clock(char* out,size_t cap);
static const char* ui_period(void) {
    struct tm t;
    if(!FOLIO_CLOCK_12H(&s_settings) || !folio_power_datetime(&t,s_settings.timezone_minutes))return "";
    // Local civil time: dawn [00,06), morning [06,12), afternoon [12,18), evening [18,24).
    if(t.tm_hour<6)return "凌晨";
    if(t.tm_hour<12)return "上午";
    if(t.tm_hour<18)return "下午";
    return "晚上";
}
static void rail(const char* back) {
    char power[48],old[32],clock[24]; folio_power_status(old,sizeof(old),s_settings.timezone_minutes);ui_clock(clock,sizeof(clock));
    const char* battery=strrchr(old,' ');const char* period=ui_period();
    snprintf(power,sizeof(power),"%s%s%s  %s",period,*period?" ":"",clock,battery?battery+1:"--%");
    if(back) { icon((EpdRect){64,40,32,32},FOLIO_ICON_BACK,INK); }
    else label(48,62,300,26,INK,"页间  /  FOLIO");
    right_label(636,62,24,MUTED,power);
}
static void page_header(const char* back,const char* title,const char* subtitle) {
    epd_hl_set_all_white(&s_hw.hl); rail(back);
    label(48,152,636,52,INK,title);
    if(subtitle) label(48,200,636,26,MUTED,subtitle);
}
static void pager(size_t page,size_t count,size_t per_page) {
    size_t pages=(count+per_page-1)/per_page; if(!pages)pages=1;
    panel((EpdRect){48,1096,588,96});
    icon((EpdRect){96,1128,32,32},FOLIO_ICON_CHEVRON_LEFT,page?INK:RULE);
    icon((EpdRect){556,1128,32,32},FOLIO_ICON_CHEVRON_RIGHT,page+1<pages?INK:RULE);
    char value[40]; snprintf(value,sizeof(value),"%u / %u",(unsigned)page+1,(unsigned)pages);
    centered_in((EpdRect){208,1096,268,96},26,MUTED,value);
}
static void present(bool full) {
    static unsigned partial_updates;
    unsigned refresh_every=s_settings.refresh_every ? s_settings.refresh_every : 8;
    if(!full && partial_updates+1>=refresh_every)full=true;
    bool clean=full || s_first_present;
    epd_poweron(); int temperature=(int)epd_ambient_temperature(); enum EpdDrawError error;
    if(s_first_present) error=epd_hl_update_screen_from_white(&s_hw.hl,MODE_GC16,temperature);
    else if(full) error=epd_hl_update_screen_full(&s_hw.hl,MODE_GC16,temperature);
    else error=epd_hl_update_screen(&s_hw.hl,MODE_GL16,temperature);
    epd_poweroff(); s_first_present=error!=EPD_DRAW_SUCCESS;
    partial_updates=clean || s_first_present ? 0 : partial_updates+1;
    if(error!=EPD_DRAW_SUCCESS) ESP_LOGW(TAG,"display update: 0x%x",error);
    s_last_activity=esp_timer_get_time();
}
static void present_region(EpdRect area) {
    if (s_first_present) { present(true); return; }
    epd_poweron();
    enum EpdDrawError error=epd_hl_update_area(&s_hw.hl,MODE_GL16,(int)epd_ambient_temperature(),area);
    epd_poweroff();
    if(error!=EPD_DRAW_SUCCESS){s_first_present=true;ESP_LOGW(TAG,"region update: 0x%x",error);}
    s_last_activity=esp_timer_get_time();
    if(s_font)(void)folio_font_set_size(s_font,s_settings.font_size);
}
static void show_error(const char* message,view_t return_view) {
    snprintf(s_error,sizeof(s_error),"%s",message && *message ? message : "操作未完成，请重试");
    s_error_return=return_view==VIEW_ERROR ? s_error_return : return_view; show_view(VIEW_ERROR,true);
}
static void show_busy(const char* message) {
    if(folio_power_shutdown_pending())return;
    activity_track(false,false);
    page_header(NULL,"请稍候",NULL);
    panel((EpdRect){48,280,588,460});
    icon((EpdRect){310,394,64,64},FOLIO_ICON_BOOK,INK);
    centered(342,540,588,36,INK,message);
    centered(342,602,588,26,MUTED,"正在处理，请稍候"); present(false);
}
static void draw_missing_font(void) {
    epd_hl_set_all_white(&s_hw.hl);
    folio_gfx_text(fb(),48,52,3,"FOLIO",INK); rule_at(48,104,588,RULE);
    panel((EpdRect){48,220,588,504});
    icon((EpdRect){294,286,96,96},FOLIO_ICON_BOOK,INK);
    folio_gfx_text(fb(),72,488,3,"FONT UNAVAILABLE",INK);
    folio_gfx_text(fb(),72,582,2,"SAVED FONT CHOICE IS KEPT",INK);
    folio_gfx_text(fb(),72,640,2,s_card_ready?"RETRY OR ADD A TTF TO /FONTS":"RESTART OR INSERT A FONT CARD",INK);
    if(s_card_ready){panel((EpdRect){48,984,588,96});ascii_centered_in((EpdRect){48,984,588,96},2,"WI-FI TRANSFER");}
    else if(card_needs_format()){panel((EpdRect){48,984,588,96});ascii_centered_in((EpdRect){48,984,588,96},2,"FORMAT SD CARD");}
    selected_panel(done_rect(),true); ascii_centered_in(done_rect(),3,"TRY AGAIN");
}
static void refresh_library(void) {
    if(!s_card_ready){s_book_count=s_shelf_count=0;return;}
    /* Migration re-exports every pending .wrb on this (the main) task and takes
     * the storage lock for the whole book. Never do that while the WeRead worker
     * is downloading or exporting: it would fight for the lock and stack another
     * streaming export on a 16 KiB task. The next refresh after the worker stops
     * picks the pending file up. */
    if(s_card_ready && !s_reader && !folio_weread_busy())folio_weread_cache_migrate();
    s_book_count=folio_library_scan(s_books,FOLIO_LIBRARY_LIMIT);
    for(size_t i=0;i<s_book_count;++i){char old[256];
        if(folio_wr_identity(s_books[i].path,old,sizeof(old)) && strcmp(old,s_books[i].path)){
            (void)folio_catalog_relocate(s_catalog,old,s_books[i].path);
            folio_activity_relocate(s_activity,old,s_books[i].path);
        }}
    if(s_activity)(void)folio_activity_save(s_activity);
    if(s_catalog) {
        (void)folio_catalog_sync(s_catalog,s_books,s_book_count,folio_power_unix_time(),false);
        (void)catalog_save();
    }
    catalog_select();
    size_t pages=(s_shelf_count+LIBRARY_PAGE_SIZE-1)/LIBRARY_PAGE_SIZE;
    if(!pages)pages=1;
    if(s_shelf_page>=pages)s_shelf_page=pages-1;
}
static const char* book_format(const folio_book_t* book) {
    const char* ext=strrchr(book->path,'.'); return ext && !strcasecmp(ext,".pdf") ? "PDF" : ext && strcasecmp(ext,".epub")==0 ? "EPUB" : "TXT";
}
static void cover(EpdRect r,const char* path,const char* title,const char* format,bool current) {
    (void)current; // Reading state is text beside the cover, never an image overlay.
    bool image=folio_cover_draw(path,fb(),r);
    if(!image) {
        epd_fill_rect(r,PAPER,fb());
        if(r.width>=100) {
            // A missing image still has a page-sized placeholder; this is the
            // cover boundary, not another frame around the whole shelf item.
            folio_gfx_outline(fb(),r,RULE);
            int size=r.width>=160?30:22;
            label(r.x+12,r.y+30,r.x+r.width-12,20,MUTED,format);
            paragraph(r.x+12,r.y+r.height/2-12,r.x+r.width-12,size,size+8,2,INK,title);
            label(r.x+12,r.y+r.height-16,r.x+r.width-12,20,MUTED,"暂无封面");
        } else icon((EpdRect){r.x+(r.width-28)/2,r.y+(r.height-32)/2,28,32},FOLIO_ICON_BOOK,MUTED);
    }
}
#include "folio_home.inc"
static void draw_library(void) {
    const char* filters[]={"全部","最近","未读"};
    char count[40]; snprintf(count,sizeof(count),"%s · %u 本%s",filters[s_catalog_options.filter],
        (unsigned)s_shelf_count,*s_search?" · 搜索中":"");
    page_header(NULL,"书架",count);
    bool list=s_settings.library_list!=0;
    panel((EpdRect){348,112,288,96});
    for(unsigned i=0;i<3;++i) {
        EpdRect r=library_tool(i);
        if(i)epd_fill_rect((EpdRect){r.x,r.y+24,1,48},RULE,fb());
        if(i==0) {folio_gfx_circle(fb(),r.x+44,r.y+44,12,INK,2);folio_gfx_line(fb(),r.x+53,r.y+53,r.x+64,r.y+64,INK,2);}
        else icon((EpdRect){r.x+32,r.y+28,32,32},i==1?FOLIO_ICON_GRID:FOLIO_ICON_LIST,INK);
        if((i==1&&!list)||(i==2&&list))folio_gfx_fill_round_rect(fb(),(EpdRect){r.x+36,r.y+76,24,3},1,INK);
    }
    for(unsigned row=0;row<LIBRARY_PAGE_SIZE;++row) {
        size_t i=s_shelf_page*LIBRARY_PAGE_SIZE+row; if(i>=s_shelf_count)break;
        EpdRect r=library_item(row); const folio_book_t* book=&s_books[s_shelf_indexes[i]];
        const folio_catalog_metadata_t* metadata=folio_catalog_metadata(s_catalog,book->path);
        const char* title=metadata && *metadata->title ? metadata->title : book->title;
        bool current=strcmp(book->path,s_current_book)==0;
        char meta[56]; snprintf(meta,sizeof(meta),"%s  ·  %lu KB",book_format(book),(unsigned long)((book->size+1023)/1024));
        if(list) {
            cover((EpdRect){r.x,r.y+12,48,72},book->path,title,book_format(book),current);
            label(r.x+72,baseline_for_center_y(32,title,r.y+32),r.x+r.width-16,32,INK,title);
            label(r.x+72,baseline_for_center_y(22,current?"上次阅读":meta,r.y+72),r.x+r.width-16,22,current?INK:MUTED,current?"上次阅读":meta);
            if(row+1<LIBRARY_PAGE_SIZE && i+1<s_shelf_count)rule_at(r.x+72,r.y+103,r.width-72,RULE);
        } else {
            cover((EpdRect){r.x+6,r.y,160,240},book->path,title,book_format(book),current);
            centered_in((EpdRect){r.x+6,r.y+256,160,40},28,INK,title);
            centered_in((EpdRect){r.x+6,r.y+304,160,30},22,current?INK:MUTED,current?"上次阅读":book_format(book));
        }
    }
    if(!s_shelf_count) {
        icon((EpdRect){302,404,80,80},FOLIO_ICON_BOOK,INK);
        centered(342,574,588,38,INK,s_book_count?"没有符合条件的书":"从第一本书开始");
        centered(342,638,588,26,MUTED,s_book_count?"试试更短的书名、拼音或首字母":"将 TXT、EPUB 或 PDF 放进 TF 卡的 books 目录");
        text_action((EpdRect){174,704,336,96},s_book_count?"修改筛选条件":"重新扫描书架");
    }
    size_t pages=(s_shelf_count+5)/6; if(!pages)pages=1;
    panel((EpdRect){48,1016,588,72});
    icon((EpdRect){80,1036,32,32},FOLIO_ICON_CHEVRON_LEFT,s_shelf_page?INK:RULE);
    icon((EpdRect){572,1036,32,32},FOLIO_ICON_CHEVRON_RIGHT,s_shelf_page+1<pages?INK:RULE);
    char page[32]; snprintf(page,sizeof(page),"%u / %u",(unsigned)s_shelf_page+1,(unsigned)pages);
    centered_in((EpdRect){208,1016,268,72},26,MUTED,page);
    draw_root_nav(VIEW_LIBRARY);
}
static void ui_clock(char* out,size_t cap) {
    struct tm t;
    if(!folio_power_datetime(&t,s_settings.timezone_minutes)){snprintf(out,cap,"--:--");return;}
    if(FOLIO_CLOCK_12H(&s_settings))snprintf(out,cap,"%d:%02d",t.tm_hour%12?t.tm_hour%12:12,t.tm_min);
    else snprintf(out,cap,"%02d:%02d",t.tm_hour,t.tm_min);
}
static void reader_chapter_label(size_t chapter,int size,int max_width,char* out,size_t cap) {
    folio_reader_chapter_label(s_reader,chapter,out,cap);
    const char* raw=folio_reader_chapter_title(s_reader,chapter);
    if(!out[0] || !strcmp(out,raw) || text_width(size,out)<=max_width)return;
    // Keep the section number visible when its parent title is too long.
    char suffix[144],parent[192];
    snprintf(suffix,sizeof(suffix)," · %s",raw);
    size_t length=strlen(out),tail=strlen(suffix);
    if(length<tail || strcmp(out+length-tail,suffix))return;
    snprintf(parent,sizeof(parent),"%.*s",(int)(length-tail),out);
    length=strlen(parent);
    while(length) {
        --length;while(length && ((unsigned char)parent[length]&0xc0)==0x80)--length;
        parent[length]=0;
        snprintf(out,cap,"%s...%s",parent,suffix);
        if(text_width(size,out)<=max_width)return;
    }
    snprintf(out,cap,"%s",raw);
}
static void draw_reader_footer(void) {
    if(s_settings.footer_hidden)return;
    epd_fill_rect((EpdRect){0,1160,684,56},PAPER,fb());
    size_t chapter=folio_reader_current_chapter(s_reader);
    char chapter_label[192];
    reader_chapter_label(chapter,22,240,chapter_label,sizeof(chapter_label));
    const char* heading=chapter==SIZE_MAX?folio_reader_title(s_reader):chapter_label;
    char clock[24],power[32],page[40];ui_clock(clock,sizeof(clock));
    folio_power_status(power,sizeof(power),s_settings.timezone_minutes);
    const char* battery=strrchr(power,' ');battery=battery?battery+1:"--%";
    const char* period=ui_period();
    if(*period){
        int size=22,period_width=text_width(20,period);
        if(text_width(size,clock)+text_width(size,battery)+period_width+14>178)size=20;
        int clock_x=32+period_width+6;
        label(32,1198,32+period_width,20,MUTED,period);
        label(clock_x,1198,clock_x+text_width(size,clock),size,MUTED,clock);
        label(clock_x+text_width(size,clock)+8,1198,210,size,MUTED,battery);
    }else{
        int time_width=text_width(22,clock);
        label(32,1198,132,22,MUTED,clock);label(32+time_width+14,1198,210,22,MUTED,battery);
    }
    centered_in((EpdRect){222,1178,240,30},22,MUTED,heading);
    unsigned mode=FOLIO_READER_FOOTER(&s_settings);
    if(mode==2)snprintf(page,sizeof(page),"%u%%",s_settings.progress_chapter?folio_reader_chapter_percent(s_reader):folio_reader_percent(s_reader));
    else {
        (void)folio_font_set_size(s_font,s_settings.font_size);
        uint32_t n=s_settings.progress_chapter?folio_reader_chapter_page_number(s_reader):folio_reader_page_number(s_reader);
        uint32_t total=folio_reader_page_total(s_reader,s_settings.progress_chapter!=0);
        if(total)snprintf(page,sizeof(page),"%lu/%lu",(unsigned long)n,(unsigned long)total);
        else snprintf(page,sizeof(page),"%lu/—",(unsigned long)n);
    }
    right_label(652,1198,22,MUTED,page);
}
static void draw_reader(void) {
    epd_hl_set_all_white(&s_hw.hl);
    if(folio_reader_page_number(s_reader)!=1)s_reader_cover=false;
    if(s_reader_cover){
        if(folio_cover_draw(s_current_book,fb(),(EpdRect){48,48,588,1120})){
            if(s_reader_notice){folio_gfx_fill_round_rect(fb(),(EpdRect){142,1024,400,80},UI_RADIUS,PAPER);panel((EpdRect){142,1024,400,80});centered_in((EpdRect){154,1032,376,64},28,INK,s_reader_notice);}
            return;
        }
        s_reader_cover=false;
    }
    (void)folio_font_set_size(s_font,s_settings.font_size);folio_reader_draw(s_reader,fb());
    draw_reader_footer();
    if(s_reader_notice){folio_gfx_fill_round_rect(fb(),(EpdRect){142,1024,400,80},UI_RADIUS,PAPER);panel((EpdRect){142,1024,400,80});centered_in((EpdRect){154,1032,376,64},28,INK,s_reader_notice);}
}
static void refresh_reader_pagination(void) {
    if(s_view!=VIEW_READING || !s_reader || s_reader_cover || s_reader_notice)return;
    (void)folio_font_set_size(s_font,s_settings.font_size);
    uint32_t before=folio_reader_page_total(s_reader,s_settings.progress_chapter!=0);
    (void)folio_reader_paginate_step(s_reader,1);
    if(!s_settings.footer_hidden && FOLIO_READER_FOOTER(&s_settings)!=2 &&
        before!=folio_reader_page_total(s_reader,s_settings.progress_chapter!=0)) {
        int64_t activity=s_last_activity;draw_reader_footer();present_region((EpdRect){0,1160,684,56});s_last_activity=activity;
    }
}
static void draw_tools(void) {
    draw_reader(); epd_fill_rect((EpdRect){0,480,684,736},PAPER,fb()); panel((EpdRect){32,488,620,712});
    const folio_catalog_metadata_t* metadata=folio_catalog_metadata(s_catalog,s_current_book);
    label(56,552,420,38,INK,folio_reader_title(s_reader));
    label(56,600,420,26,MUTED,metadata&&metadata->author[0]?metadata->author:"作者未知");
    uint64_t seconds=metadata?metadata->reading_seconds:0;char duration[48];
    if(seconds<3600)snprintf(duration,sizeof(duration),"%u分钟",(unsigned)(seconds/60));
    else snprintf(duration,sizeof(duration),"%llu小时",(unsigned long long)(seconds/3600));
    int duration_size=38;while(duration_size>24 && text_width(duration_size,duration)>184)duration_size-=2;
    right_label(628,552,duration_size,INK,duration);
    right_label(628,600,26,MUTED,"累计阅读");
    const char* names[]={"目录","书签",folio_reader_is_bookmarked(s_reader)?"移除书签":"添加书签","同步进度","排版","字体"};
    const folio_icon_t icons[]={FOLIO_ICON_LIST,FOLIO_ICON_BOOKMARK,FOLIO_ICON_BOOKMARK,FOLIO_ICON_REFRESH,FOLIO_ICON_TYPE,FOLIO_ICON_TYPE};
    for(unsigned i=0;i<6;++i) {
        EpdRect r=tool_rect(i); panel(r);
        ui_text_run_t title; measure_ui_text(30,names[i],r.width-24,&title);
        int title_height=title.ink?title.bottom-title.top:30;
        int top=r.y+(r.height-44-16-title_height)/2;
        icon((EpdRect){r.x+(r.width-44)/2,top,44,44},icons[i],INK);
        centered_in((EpdRect){r.x+12,top+60,r.width-24,title_height},30,INK,names[i]);
    }
    panel((EpdRect){56,944,278,96}); panel((EpdRect){350,944,278,96});
    icon_label((EpdRect){56,944,278,96},FOLIO_ICON_BOOK,32,30,INK,"回到书架");
    icon_label((EpdRect){350,944,278,96},FOLIO_ICON_REFRESH,32,30,INK,"刷新");
    action((EpdRect){56,1064,572,112},"继续阅读",true);
}
static void draw_contents(bool bookmarks) {
    size_t count=bookmarks?folio_reader_bookmark_count(s_reader):folio_reader_chapter_count(s_reader);
    page_header("阅读",bookmarks?"书签":"目录",folio_reader_title(s_reader));
    for(size_t row=0;row<list_rows();++row) {
        size_t i=s_list_page*list_rows()+row; if(i>=count)break;
        int y=LIST_TOP+(int)row*LIST_ROW;
        char chapter_label[192];
        reader_chapter_label(i,34,488,chapter_label,sizeof(chapter_label));
        const char* title=bookmarks?folio_reader_bookmark_title(s_reader,i):chapter_label;
        bool current=!bookmarks && folio_reader_current_chapter(s_reader)==i;
        selected_panel((EpdRect){48,y,588,96},current);
        char ordinal[20]; snprintf(ordinal,sizeof(ordinal),"%02u",(unsigned)i+1);
        int center_y=y+(current?32:48);
        label(64,baseline_for_center_y(24,ordinal,center_y),112,24,MUTED,ordinal);
        label(132,baseline_for_center_y(34,title,center_y),bookmarks?516:620,34,INK,title);
        if(current)label(132,baseline_for_center_y(22,"正在阅读",y+74),620,22,MUTED,"正在阅读");
        if(bookmarks)icon((EpdRect){580,y+34,28,28},FOLIO_ICON_CLOSE,MUTED);
    }
    if(!count) {
        icon((EpdRect){306,398,72,72},bookmarks?FOLIO_ICON_BOOKMARK:FOLIO_ICON_LIST,INK);
        centered(342,568,588,38,INK,bookmarks?"留住想再读的一页":"这本书没有目录");
        centered(342,634,588,26,MUTED,bookmarks?"在阅读菜单中添加当前页书签":"这本书暂时没有章节目录");
    }
    pager(s_list_page,count,list_rows());
}
static void draw_settings(void) {
    page_header(s_settings_return==VIEW_SYSTEM?"设置":"阅读","阅读设置","调整会自动保存");
    const char* tabs[]={"排版","偏好","阅读显示"};
    for(unsigned i=0;i<3;++i)action((EpdRect){48+(int)i*200,216,188,96},tabs[i],s_settings_tab==i);
    if(!s_settings_tab) {
        panel((EpdRect){48,320,588,188});
        label(64,baseline_for_center_y(22,"字体与预览",346),540,22,MUTED,"字体与预览"); icon((EpdRect){592,332,28,28},FOLIO_ICON_CHEVRON_RIGHT,INK);
        paragraph(72,410,612,s_settings.font_size,s_settings.font_size+s_settings.line_gap,2,INK,"慢慢读，读到字里行间。\n书页之间，自有天地。");
        const char* names[]={"字号","行距","段距","页边距"};
        unsigned values[]={s_settings.font_size,s_settings.line_gap,s_settings.paragraph_gap,s_settings.margin};
        const unsigned minimum[]={24,0,0,24}, maximum[]={56,24,32,80}, step[]={2,2,4,8};
        for(int i=0;i<4;++i) {
            int y=524+i*112; panel((EpdRect){48,y,588,96}); label(64,baseline_for_center_y(32,names[i],y+48),306,32,INK,names[i]);
            char value[24]; snprintf(value,sizeof(value),"%u",values[i]); centered_in((EpdRect){436,y,96,96},30,INK,value);
            icon((EpdRect){364,y+32,32,32},FOLIO_ICON_MINUS,values[i]>=minimum[i]+step[i]?INK:RULE);
            icon((EpdRect){572,y+32,32,32},FOLIO_ICON_PLUS,values[i]+step[i]<=maximum[i]?INK:RULE);
        }
        label(64,baseline_for_center_y(32,"对齐",1016),280,32,INK,"对齐");
        action((EpdRect){308,968,156,96},"左对齐",!s_settings.justify);
        action((EpdRect){480,968,156,96},"两端",s_settings.justify);
    } else if(s_settings_tab==1) {
        const char* names[]={"全刷间隔","空闲锁屏"};
        for(int i=0;i<2;++i) {
            int y=336+i*112;panel((EpdRect){48,y,588,96});
            label(72,baseline_for_center_y(30,names[i],y+48),308,30,INK,names[i]);
            unsigned v=i?s_settings.idle_minutes:s_settings.refresh_every;char value[24];
            snprintf(value,sizeof(value),"%u%s",v,i?"分钟":"页");
            centered_in((EpdRect){428,y,112,96},28,INK,value);
            icon((EpdRect){364,y+32,32,32},FOLIO_ICON_MINUS,v>(i?FOLIO_IDLE_MINUTES_MIN:1)?INK:RULE);
            icon((EpdRect){572,y+32,32,32},FOLIO_ICON_PLUS,v<(i?FOLIO_IDLE_MINUTES_MAX:32)?INK:RULE);
        }
        panel((EpdRect){48,560,588,96});label(72,baseline_for_center_y(30,"开机继续阅读",608),420,30,INK,"开机继续阅读");
        centered_in((EpdRect){456,560,180,96},30,INK,s_settings.auto_resume?"开启":"关闭");
        panel(auto_shutdown_toggle());
        label(72,baseline_for_center_y(30,"锁屏后自动关机",736),448,30,INK,"锁屏后自动关机");
        centered_in((EpdRect){456,688,180,96},30,INK,s_settings.auto_shutdown_enabled?"开启":"关闭");
        label(48,828,636,24,MUTED,s_settings.auto_shutdown_enabled?
            "从锁屏开始计时 · 唤醒后取消":"已关闭 · 下方保留上次选择的时长");
        const char* durations[]={"30分钟","3小时","6小时"};
        for(unsigned i=0;i<AUTO_SHUTDOWN_CHOICES;++i)action_state(auto_shutdown_choice(i),durations[i],
            s_settings.auto_shutdown_minutes==auto_shutdown_minutes[i],s_settings.auto_shutdown_enabled);
        if(!s_settings.auto_shutdown_enabled) {
            for(unsigned i=0;i<AUTO_SHUTDOWN_CHOICES;++i)if(s_settings.auto_shutdown_minutes==auto_shutdown_minutes[i])
                folio_gfx_round_rect(fb(),auto_shutdown_choice(i),UI_RADIUS,MUTED,3);
        }
    } else {
        panel((EpdRect){48,336,588,96});
        label(72,baseline_for_center_y(30,"底部状态",384),388,30,INK,"底部状态");
        centered_in((EpdRect){436,336,176,96},30,INK,s_settings.footer_hidden?"关闭":"开启");
        const char* titles[]={"显示方式","计算范围"};
        const char* choices[][2]={{"页码","百分比"},{"整本书","当前章节"}};
        for(unsigned row=0;row<2;++row) {
            int y=448+(int)row*112;
            label(64,baseline_for_center_y(28,titles[row],y+48),268,28,INK,titles[row]);
            unsigned selected=row?s_settings.progress_chapter:(FOLIO_READER_FOOTER(&s_settings)==2);
            for(unsigned i=0;i<2;++i)action((EpdRect){280+(int)i*184,y,172,96},choices[row][i],selected==i);
        }
        label(48,710,636,26,MUTED,"文字下划线");
        const char* lines[]={"关闭","直线","虚线"};
        for(unsigned i=0;i<3;++i)action((EpdRect){48+(int)i*200,736,188,88},lines[i],s_settings.underline==i);
        label(48,870,636,26,MUTED,"下划线深浅");
        const char* shades[]={"浅","中","深"};
        for(unsigned i=0;i<3;++i)action((EpdRect){48+(int)i*200,896,188,88},shades[i],s_settings.underline_shade==i);
        centered_in((EpdRect){48,1004,588,64},26,MUTED,s_settings.footer_hidden?"状态已隐藏 · 点屏幕中间可打开菜单":"页码与百分比均使用所选计算范围");
    }
    action(done_rect(),"完成",true);
}
static void draw_fonts(void) {
    const char* back=s_fonts_return==VIEW_SETTINGS?"排版":s_fonts_return==VIEW_HOME_SETTINGS?"个性化":s_fonts_return==VIEW_SYSTEM?"设置":"阅读";
    const char* desired=s_settings.font_path[0]?s_settings.font_path:FOLIO_FONT_BUILTIN_PATH;
    bool fallback=strcmp(desired,s_active_font)!=0;
    page_header(back,"字体",fallback?"暂用其他字体 · 已保留原选择":"四款内置字体 · 也可使用 TF 卡字体");
    for(size_t row=0;row<list_rows();++row) {
        size_t i=s_list_page*list_rows()+row; if(i>=s_font_count)break;
        const char* name=folio_font_builtin_label(s_fonts[i]);
        if(!name){name=strrchr(s_fonts[i],'/');name=name?name+1:s_fonts[i];}
        int y=LIST_TOP+(int)row*LIST_ROW; bool current=strcmp(s_fonts[i],s_active_font)==0;
        bool remembered=strcmp(s_fonts[i],desired)==0;
        const char* state=current?(fallback?"当前使用 · 临时回退":"当前使用"):
            remembered?"已记住 · 暂未加载":folio_font_is_builtin_path(s_fonts[i])?"设备内置":"TF 卡字体";
        selected_panel((EpdRect){48,y,588,96},current);
        label(64,baseline_for_center_y(32,name,y+32),558,32,INK,name);
        label(64,baseline_for_center_y(22,state,y+72),558,22,MUTED,state);
        if(current)icon((EpdRect){588,y+32,32,32},FOLIO_ICON_CHECK,INK);
    }
    if(!s_font_count) { centered(342,520,588,34,INK,"字体列表暂时无法加载"); centered(342,588,588,26,MUTED,"请返回后重试，原选择已保留"); }
    pager(s_list_page,s_font_count,list_rows());
}
static EpdRect system_row(unsigned i) {return (EpdRect){48,232+(int)i*112,588,96};}
static void draw_system(void) {
    page_header(NULL,"设置","让阅读保持简单");
    const char* names[]={"微信读书","Wi-Fi","阅读设置","个性化","日期与时间","传书与系统"};
    const folio_icon_t icons[]={FOLIO_ICON_BOOK,FOLIO_ICON_WIFI,FOLIO_ICON_TYPE,FOLIO_ICON_GRID,FOLIO_ICON_SETTINGS,FOLIO_ICON_LIST};
    for(unsigned i=0;i<6;++i) {
        EpdRect row=system_row(i);panel(row);icon((EpdRect){72,row.y+32,32,32},icons[i],INK);
        label(128,baseline_for_center_y(32,names[i],row.y+48),564,32,INK,names[i]);
        icon((EpdRect){588,row.y+36,24,24},FOLIO_ICON_CHEVRON_RIGHT,MUTED);
    }
    EpdRect lock={48,936,282,96},off={354,936,282,96};panel(lock);panel(off);
    icon_label(lock,FOLIO_ICON_LOCK,32,30,INK,"锁屏");icon_label(off,FOLIO_ICON_POWER,32,30,INK,"关机");
    draw_root_nav(VIEW_SYSTEM);
}
static void draw_jump(void) {
    page_header("阅读","阅读进度",folio_reader_title(s_reader));
    panel((EpdRect){48,256,588,184});
    char value[32]; snprintf(value,sizeof(value),"%u%%",s_jump_percent); centered_in((EpdRect){72,256,540,184},72,INK,value);
    rule_at(48,480,588,RULE); epd_fill_rect((EpdRect){48,478,(int)(588*s_jump_percent/100),4},INK,fb());
    int handle=48+(int)(588*s_jump_percent/100); folio_gfx_circle(fb(),handle,480,8,INK,3);
    label(48,534,200,24,MUTED,"0%"); centered(342,534,100,24,MUTED,"50%"); right_label(636,534,24,MUTED,"100%");
    const char* steps[]={"-10%","-1%","+1%","+10%"};
    for(unsigned i=0;i<4;++i)action_state(jump_step(i),steps[i],false,i<2?s_jump_percent>0:s_jump_percent<100);
    centered(342,792,588,26,MUTED,"轻点刻度定位，再确认跳转"); action(done_rect(),"读到这里",true);
}
static void draw_confirmation(bool bookmark) {
    page_header("返回",bookmark?"删除书签":"暂别书页",NULL);
    panel((EpdRect){48,280,588,512});
    icon((EpdRect){306,342,72,72},bookmark?FOLIO_ICON_BOOKMARK:FOLIO_ICON_POWER,INK);
    centered(342,518,588,40,INK,bookmark?"删除这条书签？":"现在关机？");
    if(bookmark)paragraph(80,596,604,28,44,3,MUTED,folio_reader_bookmark_title(s_reader,s_pending_bookmark));
    else { centered(342,596,588,28,MUTED,"阅读进度会妥善保存"); centered(342,650,588,26,MUTED,"下次按电源键，继续这段阅读"); }
    text_action((EpdRect){48,984,588,96},"取消"); action(done_rect(),bookmark?"删除书签":"保存并关机",true);
}
static void draw_lock_time(void) {
    label(48,102,400,38,INK,"页间 Folio");
    char status[48];folio_power_status(status,sizeof(status),s_settings.timezone_minutes);
    const char* battery=strrchr(status,' ');battery=battery?battery+1:"--%";
    right_label(628,100,26,MUTED,battery);
    panel((EpdRect){48,144,588,280});
    char clock[24];ui_clock(clock,sizeof(clock));
    centered_in((EpdRect){72,170,540,164},144,INK,clock);
    struct tm local;bool valid=folio_power_datetime(&local,s_settings.timezone_minutes);
    char date[80];const char* weekdays[]={"周日","周一","周二","周三","周四","周五","周六"};
    const char* period=ui_period();
    if(valid)snprintf(date,sizeof(date),"%s%s%d年%d月%d日 · %s",period,*period?" · ":"",local.tm_year+1900,local.tm_mon+1,local.tm_mday,weekdays[local.tm_wday]);
    else snprintf(date,sizeof(date),"时间尚未设置");
    centered_in((EpdRect){72,350,540,50},28,MUTED,date);
    panel((EpdRect){48,456,588,584});
    if(!valid){centered_in((EpdRect){72,650,540,100},28,MUTED,"请在日期与时间中校准");return;}
    char month[48];snprintf(month,sizeof(month),"%d年 %d月",local.tm_year+1900,local.tm_mon+1);
    centered_in((EpdRect){72,480,540,64},34,INK,month);
    const char* weeks[]={"一","二","三","四","五","六","日"};
    for(int i=0;i<7;++i)centered_in((EpdRect){62+i*80,560,80,44},24,MUTED,weeks[i]);
    rule_at(72,612,540,RULE);
    int first=((local.tm_wday-local.tm_mday+1)%7+7+6)%7;
    int days=folio_clock_days(local.tm_year+1900,local.tm_mon+1);
    for(int day=1;day<=days;++day){
        int cell=first+day-1;EpdRect r={76+(cell%7)*80,632+(cell/7)*64,52,52};
        bool today=day==local.tm_mday;char number[12];snprintf(number,sizeof(number),"%d",day);
        if(today)folio_gfx_fill_round_rect(fb(),r,UI_RADIUS,INK);
        centered_on(r,30,today?PAPER:INK,today?INK:PAPER,number);
    }
}
static void draw_off_default(void) {
    label(48,112,636,38,INK,"页间 Folio");
    panel((EpdRect){48,192,588,824});
    icon((EpdRect){286,396,112,112},FOLIO_ICON_BOOK,INK);
    centered_in((EpdRect){72,572,540,88},40,INK,"把这一页");
    centered_in((EpdRect){72,676,540,88},40,INK,"留给下一次");
    rule_at(294,836,96,RULE);
}
static bool draw_selected_wallpaper(EpdRect rect) {
    folio_wallpaper_image_t image = {0};
    if (folio_storage_wallpaper_cache_load(&image.pixels, &image.width, &image.height)) {
        image.stride = (image.width + 1u) / 2u;
        bool drawn = folio_wallpaper_draw_image(&image, fb(), rect);
        free(image.pixels); // The storage cache returns ordinary malloc-compatible memory.
        return drawn;
    }
    if (folio_power_shutdown_pending()) return false;
    // Old small selections remain readable; never rebuild a large image here.
    char path[256];
    return folio_storage_wallpaper_load_legacy(path, sizeof(path)) &&
        folio_cover_draw_image(path, fb(), rect);
}
static void draw_lock(bool shutdown) {
    unsigned style=shutdown?FOLIO_OFF_STYLE(&s_settings):FOLIO_LOCK_STYLE(&s_settings);
    epd_hl_set_all_white(&s_hw.hl);
    const folio_recent_book_t* recent=folio_activity_recent(s_activity,0);
    const char* path=s_reader?s_current_book:recent?recent->path:NULL;
    const char* title=s_reader?folio_reader_title(s_reader):recent?recent->title:"页间 Folio";
    bool body_drawn=false;
    if(style==1){
        (void)folio_font_set_size(s_font,s_settings.font_size);
        if(s_reader){folio_reader_draw(s_reader,fb());body_drawn=true;}
        else if(path){
            folio_book_t book={0};snprintf(book.path,sizeof(book.path),"%s",path);
            snprintf(book.title,sizeof(book.title),"%s",title);
            folio_reader_options_t options=reader_options();
            folio_reader_t* preview=folio_reader_open_with_options(&book,s_font,&options);
            if(preview){folio_reader_draw(preview,fb());folio_reader_close(preview);body_drawn=true;}
        }
    }
    if(body_drawn){}
    else if(style==0 && path && folio_cover_draw(path,fb(),(EpdRect){70,96,544,936})){}
    else if(shutdown && style==2 && draw_selected_wallpaper((EpdRect){0,0,width(),height()}))return;
    else if(!shutdown && style==2)draw_lock_time();
    else draw_off_default();
    EpdRect badge={48,1104,588,80};epd_fill_rect(badge,PAPER,fb());panel(badge);
    icon((EpdRect){72,1128,32,32},shutdown?FOLIO_ICON_POWER:FOLIO_ICON_LOCK,INK);
    label(124,1154,290,28,INK,shutdown?"已关机":"已锁屏");
    right_label(612,1154,24,MUTED,shutdown?"按电源键开机":"按电源键继续阅读");
}
static void refresh_lock_preview(void) {
    if(s_view!=VIEW_LOCK_PREVIEW || s_lock_preview_off || FOLIO_LOCK_STYLE(&s_settings)!=2)return;
    uint32_t minute=folio_power_unix_time()/60;
    if(minute==s_lock_preview_minute)return;
    s_lock_preview_minute=minute;int64_t idle=s_last_activity;
    draw_lock(false);present(false);s_last_activity=idle;
}
static void draw_wallpapers(void) {
    page_header("个性化","关机壁纸","PNG / JPG · 最大 32 MiB");
    for(size_t i=0;i<s_wallpaper_count;++i){
        EpdRect r={48,LIST_TOP+(int)i*LIST_ROW,588,96};
        bool selected=!strcmp(s_wallpaper_path,s_file_entries[i].path);
        selected_panel(r,selected);label(72,baseline_for_center_y(30,s_file_entries[i].name,r.y+34),504,30,INK,s_file_entries[i].name);
        const char* source=!strncmp(s_file_entries[i].path,FOLIO_PICTURE_ROOT "/",strlen(FOLIO_PICTURE_ROOT "/"))?"picture":"wallpapers";
        char detail[64];uint64_t bytes=s_file_entries[i].bytes;
        if(bytes>=1024u*1024u)snprintf(detail,sizeof(detail),"%s · %llu.%u MiB",source,
            (unsigned long long)(bytes/(1024u*1024u)),(unsigned)((bytes%(1024u*1024u))*10u/(1024u*1024u)));
        else snprintf(detail,sizeof(detail),"%s · %llu KiB",source,(unsigned long long)((bytes+1023u)/1024u));
        label(72,baseline_for_center_y(20,detail,r.y+72),504,20,MUTED,detail);
        if(selected)icon((EpdRect){588,r.y+34,28,28},FOLIO_ICON_CHECK,INK);
    }
    if(!s_wallpaper_count){
        icon((EpdRect){306,380,72,72},FOLIO_ICON_GRID,INK);
        centered_in((EpdRect){64,504,556,76},36,INK,"放入一张喜欢的图片");
        centered_in((EpdRect){64,608,556,56},26,MUTED,"支持无线传书 · 上传到 picture");
        centered_in((EpdRect){64,676,556,56},26,MUTED,"PNG / JPG ≤ 32 MiB · 最多 3200 万像素");
        centered_in((EpdRect){64,736,556,48},24,MUTED,"最长边 8192 · 选择时自动适配屏幕");
        centered_in((EpdRect){64,824,556,56},24,MUTED,"未选择图片时使用 Folio 默认画面");
    }
    pager(s_wallpaper_page,s_wallpaper_total,7);
}
#include "folio_manage_ui.inc"
static void show_view(view_t view,bool full) {
    if(view!=VIEW_READING)s_reader_notice=NULL;
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    if(s_view==VIEW_WEREAD && view!=VIEW_WEREAD && !folio_weread_stop()){
        s_wr_leaving=true;s_wr_target=view;return;
    }
    if(s_view==VIEW_TRANSFER && view!=VIEW_TRANSFER && !stop_transfer())return;
    activity_track(false,false);
    if(!s_reader && (view==VIEW_READING || view==VIEW_TOOLS || view==VIEW_TOC || view==VIEW_BOOKMARKS || view==VIEW_JUMP || view==VIEW_BOOKMARK_DELETE)) view=VIEW_LIBRARY;
    if(view==VIEW_OFF_CONFIRM && s_view!=VIEW_OFF_CONFIRM)s_power_return=s_view;
    if((s_view==VIEW_TOC || s_view==VIEW_JUMP || s_view==VIEW_BOOKMARKS) && view==VIEW_READING)s_reader_cover=false;
    s_view=view;
    if(!s_font && view==VIEW_OFF_CONFIRM) {
        epd_hl_set_all_white(&s_hw.hl); folio_gfx_text(fb(),48,156,3,"POWER OFF?",INK);
        folio_gfx_text(fb(),48,268,2,"PRESS POWER TO START AGAIN",INK);
        panel((EpdRect){48,984,588,96}); ascii_centered_in((EpdRect){48,984,588,96},2,"CANCEL");
        selected_panel(done_rect(),true); ascii_centered_in(done_rect(),3,"POWER OFF");
    } else if(!s_font && view==VIEW_TRANSFER)draw_transfer_ascii();
    else if(!s_font && (view==VIEW_FORMAT_CONFIRM || view==VIEW_FORMAT_FINAL))draw_format_ascii(view==VIEW_FORMAT_FINAL);
    else if(!s_font && view==VIEW_ERROR) {
        epd_hl_set_all_white(&s_hw.hl);folio_gfx_text(fb(),48,180,3,"OPERATION FAILED",INK);
        folio_gfx_text(fb(),48,360,2,"RETRY OR RESTART THE DEVICE",INK);
        selected_panel(done_rect(),true);ascii_centered_in(done_rect(),3,"TRY AGAIN");
    }
    else if(!s_font) draw_missing_font();
    else switch(view) {
        case VIEW_HOME:draw_home();break; case VIEW_HOME_SETTINGS:draw_home_settings();break;
        case VIEW_LIBRARY:draw_library();break; case VIEW_READING:draw_reader();break;
        case VIEW_LOCK_PREVIEW:s_lock_preview_minute=folio_power_unix_time()/60;draw_lock(s_lock_preview_off);break;
        case VIEW_WALLPAPERS:draw_wallpapers();break;
        case VIEW_TOOLS:draw_tools();break; case VIEW_TOC:draw_contents(false);break;
        case VIEW_BOOKMARKS:draw_contents(true);break; case VIEW_SETTINGS:draw_settings();break;
        case VIEW_FONTS:draw_fonts();break; case VIEW_SYSTEM:draw_system();break;
        case VIEW_JUMP:draw_jump();break; case VIEW_OFF_CONFIRM:draw_confirmation(false);break;
        case VIEW_BOOKMARK_DELETE:draw_confirmation(true);break;
        case VIEW_CATALOG:draw_catalog();break;case VIEW_DEVICE:draw_device();break;
        case VIEW_TRANSFER:draw_transfer();break;case VIEW_STORAGE:draw_storage();break;
        case VIEW_FILES:draw_files();break;case VIEW_FILE_ACTION:draw_file_action();break;
        case VIEW_DELETE_CONFIRM:draw_delete_confirmation();break;
        case VIEW_FORMAT_CONFIRM:draw_format_confirmation(false);break;
        case VIEW_FORMAT_FINAL:draw_format_confirmation(true);break;
        case VIEW_UPDATE:draw_update();break;
        case VIEW_WEREAD:draw_weread();break;
        case VIEW_WIFI:draw_wifi();break;case VIEW_WIFI_EDIT:draw_wifi_edit();break;
        case VIEW_TIME:draw_time();break;case VIEW_TIME_EDIT:draw_time_edit();break;
        case VIEW_ERROR:
            page_header("返回","没能完成这一步",NULL); panel((EpdRect){48,280,588,624}); icon((EpdRect){310,336,64,64},FOLIO_ICON_BOOK,INK);
            paragraph(72,504,612,32,52,6,INK,s_error); action(done_rect(),"知道了",true);break;
    }
    if(view==VIEW_LOCK_PREVIEW && folio_power_shutdown_pending()){emergency_shutdown();return;}
    present(full); if(s_font)(void)folio_font_set_size(s_font,s_settings.font_size);
    if(view==VIEW_READING) {activity_record_book();activity_track(true,false);}
}
