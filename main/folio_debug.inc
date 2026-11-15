/* UI-task-owned physical USB debug bridge. No worker mutates the UI. */
#if defined(CONFIG_FOLIO_USB_DEBUG) && CONFIG_FOLIO_USB_DEBUG
#include <fcntl.h>
#include <unistd.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "folio_debug_protocol.h"
static char s_debug_line[96];
static size_t s_debug_used;
static bool s_debug_overflow;
static uint8_t* s_debug_frame;
static size_t s_debug_cursor,s_debug_size,s_debug_encoded;
static unsigned s_debug_id;
static uint32_t s_debug_checksum;
static int64_t s_debug_deadline;

static void debug_release(void){free(s_debug_frame);s_debug_frame=NULL;}
static void debug_status(unsigned id) {
    folio_network_status_t net;folio_network_get_status(&net);
    folio_weread_status_t wr;folio_weread_get_status(&wr);
    const char* message=s_view==VIEW_ERROR?s_error:s_view==VIEW_WEREAD?wr.message:"";
    char escaped[800];size_t n=0;
    for(const unsigned char* p=(const unsigned char*)message;*p && n+7<sizeof(escaped);++p){
        if(*p=='"' || *p=='\\'){escaped[n++]='\\';escaped[n++]=(char)*p;}
        else if(*p<32){n+=(size_t)snprintf(escaped+n,sizeof(escaped)-n,"\\u%04x",*p);}
        else escaped[n++]=(char)*p;
    }
    escaped[n]=0;
    char power[48];folio_power_status(power,sizeof(power),s_settings.timezone_minutes);
    const char* battery=strrchr(power,' ');battery=battery?battery+1:"--%";
    // Read-only clock evidence: RTC sampling never changes the runtime clock anchor.
    uint32_t rtc_utc=0;
    int64_t rtc_sample_start=esp_timer_get_time();
    bool rtc_ok=folio_power_rtc_time(&rtc_utc);
    int64_t sampled_at=esp_timer_get_time();
    uint32_t utc=folio_power_unix_time();
    // Public font identities and allocator counters only; no TF paths or secrets.
    static const char* const font_ids[]={"songti","hei","kai","yuan"};
    const char* active=s_font?"external":"none";
    const char* desired=s_settings.font_path[0]?"external":"default";
    if(folio_font_is_builtin_path(s_settings.font_path))desired="unavailable";
    for(unsigned i=0;i<FOLIO_FONT_BUILTIN_COUNT;++i) {
        if(s_font && !strcmp(s_active_font,folio_font_builtin_path(i)))active=font_ids[i];
        if(!strcmp(s_settings.font_path,folio_font_builtin_path(i)))desired=font_ids[i];
    }
    uint32_t caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    printf("@FOLIO %u STATUS {\"view\":%d,\"width\":%d,\"height\":%d,\"card\":%d,\"books\":%u,\"wifi\":%d,\"connecting\":%d,\"weread\":%d,\"busy\":%d,\"completed\":%u,\"total\":%u,\"rst\":%d,\"battery\":\"%s\",\"idle_minutes\":%u,\"auto_shutdown_enabled\":%u,\"auto_shutdown_minutes\":%u,\"utc\":%lu,\"rtc_utc\":%lu,\"rtc_read_ok\":%d,\"monotonic_us\":%lld,\"rtc_read_us\":%lld,\"system_utc\":%lld,\"timezone_minutes\":%d,\"clock12h\":%d,\"font_active_id\":\"%s\",\"font_requested_id\":\"%s\",\"psram_free\":%u,\"psram_largest\":%u,\"psram_min_free\":%u,\"internal_free\":%u,\"message\":\"%s\"}\n",
        id,(int)s_view,width(),height(),s_card_ready,(unsigned)s_book_count,net.connected,net.connecting,
        (int)wr.state,wr.busy,(unsigned)wr.completed,(unsigned)wr.total,(int)esp_reset_reason(),battery,
        (unsigned)s_settings.idle_minutes,(unsigned)s_settings.auto_shutdown_enabled,
        (unsigned)s_settings.auto_shutdown_minutes,(unsigned long)utc,(unsigned long)rtc_utc,
        rtc_ok,(long long)sampled_at,(long long)(sampled_at-rtc_sample_start),(long long)time(NULL),
        (int)s_settings.timezone_minutes,FOLIO_CLOCK_12H(&s_settings)?1:0,active,desired,
        (unsigned)heap_caps_get_free_size(caps),(unsigned)heap_caps_get_largest_free_block(caps),
        (unsigned)heap_caps_get_minimum_free_size(caps),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),escaped);
}
static void debug_execute(const folio_debug_command_t* c) {
    // Observation is not user activity: STATUS/SCREEN must not prevent idle lock.
    if(c->kind==DBG_STATUS){debug_status(c->id);return;}
    if(c->kind==DBG_CANCEL){debug_release();printf("@FOLIO %u OK\n",c->id);return;}
    if(c->kind==DBG_PREVIEW_CHECK){
        if(s_view!=VIEW_UPDATE){printf("@FOLIO %u ERROR preview_requires_update_view\n",c->id);return;}
        if(online_active()){printf("@FOLIO %u ERROR online_busy\n",c->id);return;}
        online_connect(ONLINE_ACTION_CHECK_PREVIEW);
        s_last_activity=esp_timer_get_time();
        printf("@FOLIO %u OK {\"channel\":\"preview\",\"check_only\":true}\n",c->id);return;
    }
    if(c->kind==DBG_PREVIEW_DOWNLOAD || c->kind==DBG_PREVIEW_INSTALL){
        if(s_view!=VIEW_UPDATE){printf("@FOLIO %u ERROR preview_requires_update_view\n",c->id);return;}
        if(online_active()){printf("@FOLIO %u ERROR online_busy\n",c->id);return;}
        folio_online_snapshot_t snapshot;folio_online_update_get_snapshot(&snapshot);
        bool preview=!strcmp(snapshot.channel,"preview");
        if(c->kind==DBG_PREVIEW_DOWNLOAD){
            if(!preview || snapshot.state!=FOLIO_ONLINE_AVAILABLE){
                printf("@FOLIO %u ERROR preview_available_required\n",c->id);return;
            }
            online_connect(ONLINE_ACTION_DOWNLOAD_PREVIEW);
            s_last_activity=esp_timer_get_time();
            printf("@FOLIO %u OK {\"channel\":\"preview\",\"action\":\"download\"}\n",c->id);return;
        }
        if(!preview || snapshot.state!=FOLIO_ONLINE_READY){
            printf("@FOLIO %u ERROR preview_ready_required\n",c->id);return;
        }
        // This explicit command is the second confirmation; online_install repeats every safety check.
        s_online_confirm=true;online_install();
        if(!s_online_operation){printf("@FOLIO %u ERROR preview_install_rejected\n",c->id);return;}
        s_last_activity=esp_timer_get_time();
        printf("@FOLIO %u OK {\"channel\":\"preview\",\"install_started\":true}\n",c->id);return;
    }
    if(c->kind==DBG_SCREEN){
        if(s_debug_frame){printf("@FOLIO %u ERROR capture_busy\n",c->id);return;}
        folio_weread_status_t wr;folio_weread_get_status(&wr);
        if(s_view==VIEW_WIFI_EDIT || s_view==VIEW_TRANSFER ||
            (s_view==VIEW_WEREAD && (wr.state==FOLIO_WR_LOGIN || wr.state==FOLIO_WR_OTP))){
            printf("@FOLIO %u ERROR private_screen\n",c->id);return;
        }
        s_debug_size=(size_t)epd_width()*epd_height()/2;
        s_debug_frame=heap_caps_malloc(s_debug_size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!s_debug_frame){printf("@FOLIO %u ERROR memory\n",c->id);return;}
        memcpy(s_debug_frame,fb(),s_debug_size);s_debug_id=c->id;s_debug_cursor=s_debug_encoded=0;
        s_debug_checksum=folio_debug_hash(s_debug_frame,s_debug_size);s_debug_deadline=esp_timer_get_time()+60000000;
        printf("@FOLIO %u BEGIN %d %d %u\n",c->id,epd_width(),epd_height(),(unsigned)s_debug_size);return;
    }
    if(c->kind==DBG_TAP)handle_tap(c->x,c->y);
    else if(c->kind==DBG_KEY)handle_key(c->x,false);
    s_last_activity=esp_timer_get_time();
    printf("@FOLIO %u OK\n",c->id);
}
static void folio_debug_init(void) {
    /* IDF's nonblocking VFS input queries the driver's RX ring buffer. Without
       installing it, bytes in the hardware FIFO never become readable. */
    usb_serial_jtag_driver_config_t config={.tx_buffer_size=4096,.rx_buffer_size=512};
    if(!usb_serial_jtag_is_driver_installed() && usb_serial_jtag_driver_install(&config)!=ESP_OK){
        ESP_LOGW(TAG,"USB debug driver unavailable");return;
    }
    usb_serial_jtag_vfs_use_driver();
    int flags=fcntl(STDIN_FILENO,F_GETFL,0);
    if(flags<0 || fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)<0)ESP_LOGW(TAG,"USB debug input unavailable");
    else ESP_LOGI(TAG,"USB debug bridge ready (physical USB only)");
}
static void folio_debug_poll(void) {
    char bytes[96];ssize_t n=read(STDIN_FILENO,bytes,sizeof(bytes));
    for(ssize_t i=0;i<n;++i){
        char ch=bytes[i];if(ch=='\r')continue;
        if(ch=='\n'){
            s_debug_line[s_debug_used]=0;folio_debug_command_t command;
            if(!s_debug_overflow && folio_debug_parse(s_debug_line,&command))debug_execute(&command);
            else if(s_debug_used)printf("@FOLIO 0 ERROR invalid_command\n");
            s_debug_used=0;s_debug_overflow=false;
        }else if((unsigned char)ch<32 || s_debug_used+1>=sizeof(s_debug_line))s_debug_overflow=true;
        else s_debug_line[s_debug_used++]=ch;
    }
    if(s_debug_frame && esp_timer_get_time()>s_debug_deadline){debug_release();printf("@FOLIO %u ERROR capture_timeout\n",s_debug_id);}
    for(unsigned batch=0;s_debug_frame && batch<4;++batch){
        uint8_t encoded[256];char hex[513];static const char digits[]="0123456789abcdef";
        size_t size=folio_debug_rle(s_debug_frame,s_debug_size,&s_debug_cursor,encoded,sizeof(encoded));
        for(size_t i=0;i<size;++i){hex[i*2]=digits[encoded[i]>>4];hex[i*2+1]=digits[encoded[i]&15];}hex[size*2]=0;
        printf("@FOLIO %u DATA %u %s\n",s_debug_id,(unsigned)s_debug_encoded,hex);s_debug_encoded+=size;
        if(s_debug_cursor==s_debug_size){printf("@FOLIO %u END %u %08lx\n",s_debug_id,(unsigned)s_debug_encoded,(unsigned long)s_debug_checksum);debug_release();}
    }
    fflush(stdout);
}
#else
static void folio_debug_init(void){}
static void folio_debug_poll(void){}
#endif
