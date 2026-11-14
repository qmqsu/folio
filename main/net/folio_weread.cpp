#include "folio_weread.h"
#include "folio_weread_time.h"
#include "../book/folio_weread_paths.h"
#include "weread/folio_weread_client.h"
#include "weread/folio_weread_session.h"
extern "C" {
#include "folio_network.h"
#include "folio_storage.h"
#include "folio_weread_cache.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "psa/crypto.h"
}
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#include "esp_heap_caps.h"
#endif
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace folio::weread;
/* Account-bound navigation metadata. These caches are hints only; readable
 * content retains its independent checksums and is verified when opened. */
#define SHELF_MAGIC 0x31464c53u
#define SHELF_FILE FOLIO_SD_ROOT "/.folio/weread/shelf.bin"
#define SHELF_LIMIT 128
struct StorageLock {
    bool held=folio_storage_lock(2000);
    ~StorageLock(){if(held)folio_storage_unlock();}
};
typedef struct { uint32_t magic, version, count, checksum; uint8_t owner[32]; } shelf_header_t;
typedef struct { char id[64], title[160], author[96], cover[192]; } shelf_entry_t;
static_assert(sizeof(shelf_entry_t)==512,"shelf entry must stay compact");
template<size_t N> void copy(char (&out)[N],const char* input);
static bool owner_digest(uint8_t out[32]);
static uint32_t shelf_checksum(uint32_t value,const void* bytes,size_t n) {
    const uint8_t* p=static_cast<const uint8_t*>(bytes);
    while(n--)value=(value^*p++)*16777619u;
    return value;
}
static bool shelf_load(std::vector<Book>* out) {
    StorageLock lock;uint8_t owner[32];if(!lock.held || !owner_digest(owner))return false;
    FILE* f=fopen(SHELF_FILE,"rb");if(!f)return false;
    shelf_header_t h{};bool okay=fread(&h,sizeof(h),1,f)==1 && h.magic==SHELF_MAGIC &&
        h.version==2 && h.count<=SHELF_LIMIT && !memcmp(h.owner,owner,sizeof(owner));
    uint32_t expected=h.checksum;h.checksum=0;
    uint32_t sum=shelf_checksum(2166136261u,&h,sizeof(h));
    std::vector<Book> cached;
    for(uint32_t i=0;okay && i<h.count;++i){
        shelf_entry_t e{};if(fread(&e,sizeof(e),1,f)!=1){okay=false;break;}
        sum=shelf_checksum(sum,&e,sizeof(e));
        if(!e.id[0] || !memchr(e.id,0,sizeof(e.id)) || !memchr(e.title,0,sizeof(e.title)) ||
           !memchr(e.author,0,sizeof(e.author)) || !memchr(e.cover,0,sizeof(e.cover))){okay=false;break;}
        Book book;book.id=e.id;book.title=e.title;book.author=e.author;book.cover=e.cover;
        cached.push_back(std::move(book));
    }
    okay=okay && sum==expected && fgetc(f)==EOF && !ferror(f);
    fclose(f);if(!okay)return false;
    *out=std::move(cached);return true;
}
static void shelf_save(const std::vector<Book>& list) {
    StorageLock lock;shelf_header_t h{SHELF_MAGIC,2,0,0,{}};
    if(!lock.held || !owner_digest(h.owner) || list.size()>SHELF_LIMIT)return;
    mkdir(FOLIO_SD_ROOT "/.folio",0777);mkdir(FOLIO_SD_ROOT "/.folio/weread",0777);
    FILE* f=fopen(SHELF_FILE ".tmp","wb");if(!f)return;
    h.count=(uint32_t)list.size();uint32_t sum=shelf_checksum(2166136261u,&h,sizeof(h));
    bool okay=fwrite(&h,sizeof(h),1,f)==1;
    for(uint32_t i=0;okay && i<h.count;++i){
        shelf_entry_t e{};copy(e.id,list[i].id.c_str());copy(e.title,list[i].title.c_str());
        copy(e.author,list[i].author.c_str());copy(e.cover,list[i].cover.c_str());
        sum=shelf_checksum(sum,&e,sizeof(e));okay=fwrite(&e,sizeof(e),1,f)==1;
    }
    h.checksum=sum;
    okay=okay && fseek(f,0,SEEK_SET)==0 && fwrite(&h,sizeof(h),1,f)==1 && fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)!=0)okay=false;
    // FAT cannot replace an existing destination. A missing hint is harmless.
    if(okay && (unlink(SHELF_FILE)==0 || errno==ENOENT))okay=rename(SHELF_FILE ".tmp",SHELF_FILE)==0;
    if(!okay)unlink(SHELF_FILE ".tmp");
}
struct Command { folio_weread_action_t action; size_t index; char value[16]; };
struct StageTimer {
    const char* name;
    int64_t started=esp_timer_get_time();
    explicit StageTimer(const char* stage):name(stage){}
    ~StageTimer(){
#ifdef ESP_PLATFORM
        ESP_LOGI("folio_wr_stage","%s ms=%llu",name,(unsigned long long)((esp_timer_get_time()-started)/1000));
#endif
    }
};
static portMUX_TYPE status_mux=portMUX_INITIALIZER_UNLOCKED;
static folio_weread_status_t status;
static std::atomic<bool> stopping{false}, worker_alive{false};
static std::atomic<TaskHandle_t> worker_task{nullptr};
static QueueHandle_t commands;
static Client client;
static QrSession qr;
static int64_t qr_started;
static std::vector<Book> books;
static std::vector<Chapter> chapters;
static size_t selected=SIZE_MAX;
static size_t download_first;
static bool download_all;
static Progress cloud;
static folio_weread_position_t local_anchor{},cloud_anchor{};
static uint32_t cloud_byte;
static bool cloud_position_ready;
static char book_path[256],open_path[256],requested_sync_path[256];
static bool login_required, shelf_cached;
static std::string chapters_book, chapters_owner;
static int64_t chapters_fetched;
enum class RetryTask { Shelf, Book, Sync, Login };
static RetryTask retry_task=RetryTask::Shelf;
static size_t retry_book=SIZE_MAX;
static bool retry_sync_only;
static std::atomic<bool> relogin_requested{false};
static uint32_t open_byte;
// UI-thread-only active-reading record. The worker starts only after flushing it.
static char record_path[256];
static folio_weread_time_t record;
static bool record_loaded,record_dirty,record_error;

template<size_t N> void copy(char (&out)[N],const char* input) {
    if(!input)input="";
    size_t n=strlen(input);if(n>=N){n=N-1;while(n && ((unsigned char)input[n]&0xc0)==0x80)--n;}
    memcpy(out,input,n);out[n]=0;
}
static void publish(folio_weread_state_t state,const char* message,bool busy=false) {
    portENTER_CRITICAL(&status_mux);status.state=state;status.busy=busy;
    status.logged_in=client.LoggedIn() && !login_required;status.login_required=login_required;copy(status.message,message);++status.revision;portEXIT_CRITICAL(&status_mux);
}
static void fail(const char* message){publish(FOLIO_WR_ERROR,message && *message?message:"操作未完成，请重试。");}
static void fail(const Error& error){
#ifdef ESP_PLATFORM
    // Codes only: never print request URLs, cookies, OTPs or account details.
    ESP_LOGW("folio_weread","Request failed: kind=%d http=%d server=%d",(int)error.code,error.http_status,error.server_code);
#endif
    if(error.code==ErrorCode::SessionExpired || error.code==ErrorCode::NotLoggedIn){
        login_required=true;
        portENTER_CRITICAL(&status_mux);status.sync_local=status.sync_cloud=false;status.qr_url[0]=0;portEXIT_CRITICAL(&status_mux);
        fail("登录已过期，请点重新登录；离线书籍和阅读记录仍保留。");
    }else fail(error.message);
}
static void work(const char* message) {
    portENTER_CRITICAL(&status_mux);status.completed=status.total=0;portEXIT_CRITICAL(&status_mux);
    publish(FOLIO_WR_WORK,message,true);
}
static bool cancelled(void*){return stopping.load() || relogin_requested.load();}
static bool owner_digest(uint8_t out[32]) {
    if(!client.LoggedIn())return false;
    if(psa_crypto_init()!=PSA_SUCCESS)return false;
    size_t size=0;const char* account=client.AccountId();
    return psa_hash_compute(PSA_ALG_SHA_256,(const uint8_t*)account,strlen(account),out,32,&size)==PSA_SUCCESS && size==32;
}
static bool load_time(folio_weread_time_t* out,bool require_owner) {
    StorageLock lock;if(!lock.held || !folio_weread_time_load(book_path,out))return false;
    if(require_owner){uint8_t digest[32];if(!owner_digest(digest) || !folio_weread_time_bind(out,digest))return false;}
    return true;
}
static void time_status() {
    folio_weread_time_t t{};bool okay=book_path[0] && load_time(&t,false);
    uint8_t owner[32];bool eligible=okay && owner_digest(owner) && folio_weread_time_bind(&t,owner);
    portENTER_CRITICAL(&status_mux);
    status.time_enabled=eligible && t.enabled;
    status.cloud_time_saved=eligible && t.cloud_valid;status.saved_cloud_seconds=eligible?t.cloud_seconds:0;
    status.pending_seconds=eligible?(uint32_t)std::min<uint64_t>(t.pending,UINT32_MAX):0;
    status.uncertain_seconds=eligible?(uint32_t)std::min<uint64_t>(t.uncertain,UINT32_MAX):0;
    ++status.revision;portEXIT_CRITICAL(&status_mux);
}
static void shelf_page(size_t page) {
    size_t pages=(books.size()+FOLIO_WR_PAGE_SIZE-1)/FOLIO_WR_PAGE_SIZE;
    if(page>=pages)page=pages?pages-1:0;
    folio_weread_item_t items[FOLIO_WR_PAGE_SIZE]{};size_t count=0;
    for(size_t i=page*FOLIO_WR_PAGE_SIZE;i<books.size() && count<FOLIO_WR_PAGE_SIZE;++i,++count){
        copy(items[count].title,books[i].title.c_str());copy(items[count].subtitle,books[i].author.c_str());
    }
    portENTER_CRITICAL(&status_mux);status.page=page;status.count=books.size();status.item_count=count;
    memcpy(status.items,items,sizeof(items));status.can_read=false;status.local_path[0]=0;
    copy(status.title,"微信读书");portEXIT_CRITICAL(&status_mux);
    publish(FOLIO_WR_SHELF,shelf_cached?"已显示上次书架，点刷新获取最新内容。":books.empty()?"书架为空，可先在微信读书添加书籍。":"选择书籍，下载后可离线阅读。");
}
static void book_page(size_t page,const char* message=nullptr) {
    StageTimer timing("book-page");
    if(selected>=books.size())return;
    size_t pages=(chapters.size()+FOLIO_WR_PAGE_SIZE-1)/FOLIO_WR_PAGE_SIZE;
    if(page>=pages)page=pages?pages-1:0;
    folio_weread_item_t items[FOLIO_WR_PAGE_SIZE]{};size_t count=0;
    const char* uids[FOLIO_WR_PAGE_SIZE]{};bool present[FOLIO_WR_PAGE_SIZE]{};
    for(size_t i=page*FOLIO_WR_PAGE_SIZE;i<chapters.size() && count<FOLIO_WR_PAGE_SIZE;++i)uids[count++]=chapters[i].uid.c_str();
    bool readable=book_path[0] && folio_weread_cache_presence(book_path,uids,count,present);
    count=0;
    for(size_t i=page*FOLIO_WR_PAGE_SIZE;i<chapters.size() && count<FOLIO_WR_PAGE_SIZE;++i,++count){
        copy(items[count].title,chapters[i].title.c_str());
        items[count].cached=present[count];
        copy(items[count].subtitle,items[count].cached?"已下载 · 点击重新下载":"点击下载本章");
    }
    portENTER_CRITICAL(&status_mux);status.page=page;status.count=chapters.size();status.item_count=count;
    memcpy(status.items,items,sizeof(items));copy(status.title,books[selected].title.c_str());
    status.can_read=readable;copy(status.local_path,readable?book_path:"");portEXIT_CRITICAL(&status_mux);
    time_status();publish(FOLIO_WR_BOOK,message?message:"已下载章节可从本地书架打开。");
}
static bool session_loaded;
static bool persist_login(){
    Session saved;if(!client.ExportSession(&saved))return true;
    if(SaveSession(saved))return true;
    fail("登录状态未能保存，请重试；当前登录仍可使用。");return false;
}
static bool restore_login(){
    if(session_loaded)return true;
    Session saved;SessionLoad result=LoadSession(&saved);
    if(result==SessionLoad::Failed){fail("无法读取已保存的登录，请重试。");return false;}
    if(result==SessionLoad::Invalid){
        if(!EraseSession()){fail("登录记录无法清除，请重试。");return false;}
        session_loaded=true;fail("保存的登录记录无效，请重新扫码。");return false;
    }
    if(result==SessionLoad::Loaded&&!client.RestoreSession(saved)){fail("登录记录无效，请重新扫码。");return false;}
    session_loaded=true;return true;
}
static bool transient_read_error(ErrorCode code){
    return code==ErrorCode::Transport||code==ErrorCode::Timeout||code==ErrorCode::NameResolution||code==ErrorCode::Connection;
}
static bool connected() {
    folio_network_status_t net;folio_network_get_status(&net);
    if(!net.connected && net.connecting){
        work("正在连接 Wi-Fi");
        int64_t deadline=esp_timer_get_time()+INT64_C(30000000);
        while(!cancelled(nullptr) && net.connecting && !net.connected && esp_timer_get_time()<deadline){
            vTaskDelay(pdMS_TO_TICKS(100));folio_network_get_status(&net);
        }
    }
    if(cancelled(nullptr))return false;
    if(!net.connected){fail("Wi-Fi 已断开。返回重新连接，或进入网络设置。");return false;}
    if(!net.clock_valid){fail("请先在日期与时间页面校准时间。");return false;}
    return true;
}
static void begin_login() {
    retry_task=RetryTask::Login;
    if(!connected())return;
    if(!EraseSession()){fail("登录记录无法清除，请重试。");return;}
    session_loaded=true;login_required=false;shelf_cached=false;client.Logout();books.clear();chapters.clear();chapters_book.clear();chapters_owner.clear();selected=SIZE_MAX;book_path[0]=0;
    {StorageLock lock;if(lock.held)unlink(SHELF_FILE);}
    portENTER_CRITICAL(&status_mux);status.item_count=status.count=0;status.can_read=status.sync_local=status.sync_cloud=false;
    status.local_path[0]=open_path[0]=0;status.cloud_time_available=false;portEXIT_CRITICAL(&status_mux);
    work("正在获取登录二维码");Error error;
    bool okay=client.BeginQrLogin(&qr,&error);
    if(!okay && !stopping && transient_read_error(error.code)){
        vTaskDelay(pdMS_TO_TICKS(300));folio_network_status_t net;folio_network_get_status(&net);
        if(net.connected&&!stopping)okay=client.BeginQrLogin(&qr,&error);
    }
    if(!okay){fail(error);return;}
    qr_started=esp_timer_get_time();
    portENTER_CRITICAL(&status_mux);copy(status.qr_url,qr.url.c_str());copy(status.title,"扫码登录");portEXIT_CRITICAL(&status_mux);
    publish(FOLIO_WR_LOGIN,"请用微信扫码，并在手机上确认登录。");
}
static void fetch_shelf() {
    StageTimer timing("shelf");
    retry_task=RetryTask::Shelf;
    if(!connected())return;
    // No hidden background request: busy accurately describes ownership.
    work("正在同步微信读书书架");
    Error error;
    std::vector<Book> refreshed;
    bool fetched=client.FetchShelf(&refreshed,&error);
    // One bounded retry for a transient read failure; do not repeat business
    // rejections or write/OTP submissions.
    static const uint16_t kShelfPauseMs[]={300};
    for(unsigned attempt=0;!fetched && !cancelled(nullptr) && attempt<sizeof(kShelfPauseMs)/sizeof(kShelfPauseMs[0]) &&
        transient_read_error(error.code);++attempt) {
        work("正在重试获取书架");vTaskDelay(pdMS_TO_TICKS(kShelfPauseMs[attempt]));
        folio_network_status_t network;folio_network_get_status(&network);
        if(!network.connected)break;
        refreshed.clear();
        fetched=client.FetchShelf(&refreshed,&error);
    }
    // One renewal/retry is restricted to this read-only operation. Write/time
    // requests never enter this path and are never automatically repeated.
    if(!fetched && error.code==ErrorCode::SessionExpired){
        Error expired=error;
        if(client.RenewSession(&error))fetched=client.FetchShelf(&refreshed,&error);
        else error=expired; // Keep the confirmed expiry even if renewal hit a network error.
    }
    if(!fetched){
        if(error.code==ErrorCode::SessionExpired || error.code==ErrorCode::NotLoggedIn)fail(error);
        else if(!books.empty()&&transient_read_error(error.code)){shelf_cached=true;shelf_page(0);publish(FOLIO_WR_SHELF,"刷新失败，仍显示上次书架；可稍后重试。");}
        else fail(error);
        return;
    }
    login_required=false;
    if(!persist_login())return;
    books.swap(refreshed);shelf_cached=false;selected=SIZE_MAX;book_path[0]=0;shelf_save(books);shelf_page(0);
}
static void open_requested_sync();
static void poll_login(const char* otp) {
    if(stopping)return;
    if(esp_timer_get_time()-qr_started>INT64_C(180000000)){fail("二维码已过期，请重新获取后扫码。");return;}
    QrState state;Error error;
    if(!client.PollQrLogin(qr,otp?otp:"",&state,&error)) {
        // No phone confirmation yet: retain this QR and continue read-only
        // polling until its 180-second deadline. OTP submissions are not retried.
        if((!otp || !*otp) && error.code==ErrorCode::Timeout) {
            folio_network_status_t network;folio_network_get_status(&network);
            if(network.connected)return;
        }
        fail(error);return;
    }
    if(state==QrState::Complete){if(!persist_login())return;portENTER_CRITICAL(&status_mux);status.qr_url[0]=0;portEXIT_CRITICAL(&status_mux);if(requested_sync_path[0])open_requested_sync();else fetch_shelf();}
    else if(state==QrState::NeedOtp || state==QrState::OtpMismatch)publish(FOLIO_WR_OTP,state==QrState::OtpMismatch?"验证码不匹配，请重新输入。":"请按手机提示输入登录验证码。");
    else if(state==QrState::Expired)fail("二维码已过期，请重新获取后扫码。");
    else if(state==QrState::Pending) {
        portENTER_CRITICAL(&status_mux);bool busy=status.busy;portEXIT_CRITICAL(&status_mux);
        if(busy)publish(FOLIO_WR_LOGIN,"请继续在手机上确认登录。");
    }
}
static bool fetch_cover(const Book& book,bool* changed=nullptr);
static void choose_book(size_t index,bool sync_only=false) {
    StageTimer timing("choose-book");
    retry_task=RetryTask::Book;retry_book=index;retry_sync_only=sync_only;
    if(index>=books.size() || !connected())return;
    selected=index;book_path[0]=0;cloud={};local_anchor={};cloud_anchor={};cloud_position_ready=false;
    work("正在获取章节目录");Error error;
    // The TOC is a read-only lookup, so a dropped link is retried with the same
    // growing pause as the shelf instead of failing the whole selection.
    static const uint16_t kTocPauseMs[]={300};
    bool reused=chapters_book==books[index].id && chapters_owner==client.AccountId() && !chapters.empty() &&
        esp_timer_get_time()>=chapters_fetched && esp_timer_get_time()-chapters_fetched<INT64_C(300000000);
    bool toc_okay=reused || client.FetchChapters(books[index].id,&chapters,&error);
    for(unsigned attempt=0;!toc_okay && !cancelled(nullptr) && attempt<sizeof(kTocPauseMs)/sizeof(kTocPauseMs[0]) &&
        transient_read_error(error.code);++attempt){
        work("正在重试获取章节目录");vTaskDelay(pdMS_TO_TICKS(kTocPauseMs[attempt]));
        folio_network_status_t network;folio_network_get_status(&network);
        if(!network.connected)break;
        chapters.clear();
        toc_okay=client.FetchChapters(books[index].id,&chapters,&error);
    }
    if(!toc_okay){chapters_book.clear();fail(error);return;}
    if(!reused){chapters_book=books[index].id;chapters_owner=client.AccountId();chapters_fetched=esp_timer_get_time();}
    login_required=false;
    // IDs are validated; display titles are sanitized only by the EPUB exporter.
    // All book data stays under books / .folio.
    if(!folio_weread_cache_path(books[index].id.c_str(),book_path,sizeof(book_path))){fail("书籍标识不受支持。");return;}
    // Showing the directory must not hash every body or wait for a cover CDN.
    // Cover refresh and export belong to an explicit download operation.
    char exported[256];if(folio_weread_cache_public_path(book_path,exported,sizeof(exported)))copy(book_path,exported);
    if(sync_only){
        portENTER_CRITICAL(&status_mux);copy(status.title,books[index].title.c_str());copy(status.local_path,book_path);
        status.can_read=true;status.page=status.item_count=status.count=0;portEXIT_CRITICAL(&status_mux);
        publish(FOLIO_WR_BOOK,"正在读取同步信息",true);
    }else book_page(0);
}
struct WriterCloser {void operator()(folio_weread_cache_writer_t* w)const{folio_weread_cache_close(w);}};
static bool cover_host(const std::string& url) {
    if(url.compare(0,8,"https://"))return false;
    size_t end=url.find('/',8);std::string host=url.substr(8,end==std::string::npos?end:end-8);
    if(host.empty() || host.size()>253 || host.find_first_of("@:#?\\")!=std::string::npos)return false;
    // WeRead serves covers from its own Tencent COS bucket as well as the QQ image
    // hosts; the allowlist has to cover those or every cover is skipped.
    const char* domains[]={"qpic.cn","qq.com","qlogo.cn","myqcloud.com","gtimg.cn","weixin.qq.com"};
    for(const char* domain:domains){size_t n=strlen(domain);if(host==domain || (host.size()>n && host[host.size()-n-1]=='.' && host.compare(host.size()-n,n,domain)==0))return true;}
    return false;
}
static bool fetch_cover(const Book& book,bool* changed) {
    if(changed)*changed=false;
    // Separate unauthenticated client: never send WeRead cookies to image CDN.
#ifdef ESP_PLATFORM
    if(!book_path[0] || !cover_host(book.cover) || stopping){
        ESP_LOGW("folio_weread","Cover skipped: path=%u allowed=%d stop=%d url=%s",(unsigned)(book_path[0]!=0),
            (int)cover_host(book.cover),(int)stopping.load(),book.cover.c_str());
        return false;
    }
#else
    if(!book_path[0] || !cover_host(book.cover) || stopping)return false;
#endif
    StorageLock lock;if(!lock.held)return false;
    char temporary[288],target[288],other[288],error[80];
    for(const char* suffix:{".png",".jpg"}){
        char filename[24];snprintf(filename,sizeof(filename),"cover%s",suffix);
        if(!folio_wr_aux(book_path,filename,target,sizeof(target)))return false;
        if(folio_storage_validate_parent(target,error,sizeof(error))){
            FILE* cached=fopen(target,"rb");
            if(cached){unsigned char signature[8]{};size_t n=fread(signature,1,8,cached);fclose(cached);
                if(n==8 && (!memcmp(signature,"\x89PNG\r\n\x1a\n",8) || (signature[0]==0xff && signature[1]==0xd8)))return true;}
        }
    }
    if(!folio_wr_aux(book_path,"cover.tmp",temporary,sizeof(temporary)))return false;
    if(!folio_storage_validate_parent(temporary,error,sizeof(error)))return false;
    esp_http_client_config_t config{};config.url=book.cover.c_str();config.timeout_ms=5000;
    config.crt_bundle_attach=esp_crt_bundle_attach;config.disable_auto_redirect=true;
    config.buffer_size=2048;config.buffer_size_tx=1024;
    esp_http_client_handle_t http=esp_http_client_init(&config);if(!http)return false;
    int64_t deadline=esp_timer_get_time()+INT64_C(12000000);
    FILE* file=nullptr;bool okay=esp_http_client_open(http,0)==ESP_OK && !cancelled(nullptr) && esp_timer_get_time()<deadline;
    if(okay)okay=esp_http_client_set_timeout_ms(http,2000)==ESP_OK;
    int64_t length=-1;
    while(okay && !cancelled(nullptr) && esp_timer_get_time()<deadline){
        length=esp_http_client_fetch_headers(http);
        if(length==-ESP_ERR_HTTP_EAGAIN)continue;
        break;
    }
    okay=okay && esp_http_client_get_status_code(http)==200 && length>=0 && length<=2*1024*1024;
    if(okay){file=fopen(temporary,"wb");okay=file!=nullptr;}
    unsigned char buffer[2048],signature[8]{};size_t total=0;
    while(okay && !cancelled(nullptr) && esp_timer_get_time()<deadline){
        int n=esp_http_client_read(http,(char*)buffer,sizeof(buffer));
        if(n==-ESP_ERR_HTTP_EAGAIN)continue;
        if(n<0){okay=false;break;}if(!n){okay=esp_http_client_is_complete_data_received(http);break;}
        for(int i=0;i<n && total+(size_t)i<sizeof(signature);++i)signature[total+i]=buffer[i];
        total+=(size_t)n;if(total>2*1024*1024 || fwrite(buffer,1,(size_t)n,file)!=(size_t)n){okay=false;break;}
    }
    okay=okay && !cancelled(nullptr) && esp_timer_get_time()<deadline && total>8 && (length<=0 || (uint64_t)length==total);
    bool png=!memcmp(signature,"\x89PNG\r\n\x1a\n",8),jpeg=signature[0]==0xff && signature[1]==0xd8;
    okay=okay && (png || jpeg);
    if(file){if(fflush(file) || fsync(fileno(file)))okay=false;if(fclose(file))okay=false;}
    esp_http_client_close(http);esp_http_client_cleanup(http);
    if(!folio_wr_aux(book_path,png?"cover.png":"cover.jpg",target,sizeof(target)))return false;
    if(okay && folio_storage_validate_parent(target,error,sizeof(error)) && (unlink(target)==0 || errno==ENOENT) && rename(temporary,target)==0){
        if(changed)*changed=true;
        (void)folio_wr_aux(book_path,png?"cover.jpg":"cover.png",other,sizeof(other));
        if(folio_storage_validate_parent(other,error,sizeof(error)))unlink(other);
    }else {okay=false;unlink(temporary);}
#ifdef ESP_PLATFORM
    ESP_LOGI("folio_weread","Cover fetch: saved=%u bytes=%u",okay,(unsigned)total);
#endif
    return okay;
}
static bool fetch_chapter(size_t index,ChapterContent* content,Error* error) {
    bool okay=client.FetchChapter(books[selected].id,chapters[index],content,error);
    // The client already retries individual read-only shards. Repeating the
    // whole chapter multiplies deadlines and redownloads successful shards.
    if(!okay && error->code==ErrorCode::NoMemory && !cancelled(nullptr)){
        content->bytes.clear();client.Disconnect();vTaskDelay(pdMS_TO_TICKS(300));
        okay=client.FetchChapter(books[selected].id,chapters[index],content,error);
    }
    return okay;
}
static void download(size_t first,bool all) {
    StageTimer timing("download");
    if(selected>=books.size() || !connected() || chapters.empty())return;
    if(!all && first>=chapters.size())return;
    download_first=first;download_all=all;
    publish(FOLIO_WR_WORK,"正在准备下载，可返回取消",true);
    std::vector<folio_weread_chapter_t> toc(chapters.size());
    for(size_t i=0;i<chapters.size();++i){copy(toc[i].uid,chapters[i].uid.c_str());copy(toc[i].title,chapters[i].title.c_str());}
    char error_text[192]{};
    std::unique_ptr<folio_weread_cache_writer_t,WriterCloser> writer(folio_weread_cache_begin(
        books[selected].id.c_str(),books[selected].title.c_str(),books[selected].author.c_str(),toc.data(),toc.size(),error_text,sizeof(error_text)));
    if(!writer){publish(FOLIO_WR_DOWNLOAD_ERROR,error_text);return;}
    folio_weread_time_t old_time{};folio_weread_position_t previous{};
    bool restore=load_time(&old_time,false) && old_time.book_revision && old_time.book_revision==folio_weread_cache_revision(book_path) &&
        folio_weread_cache_local_to_cloud(book_path,old_time.position,&previous,error_text,sizeof(error_text));
    size_t end=all?chapters.size():first+1;if(all)first=0;
    portENTER_CRITICAL(&status_mux);status.completed=0;status.total=(uint32_t)(end-first);portEXIT_CRITICAL(&status_mux);
    publish(FOLIO_WR_WORK,all?"正在下载整书，可返回取消":"正在下载选中的章节",true);
    bool okay=true;Error error;
    unsigned failed=0;size_t failed_first=0;char first_error[128]{};
    // Give up on a book only when this many chapters stayed unavailable: the
    // rest of the download is still worth keeping and exporting.
    static constexpr unsigned kFailedLimit=16;
    for(size_t i=first;i<end && !cancelled(nullptr);++i){
        // Explicitly selecting a chapter refreshes it. Whole-book downloads
        // resume verified completed chapters after an interrupted operation.
        if(!all || !folio_weread_cache_has_chapter(writer.get(),i)){
            char stage[128];snprintf(stage,sizeof(stage),"正在下载第 %u 章，连接较慢时会等待并重试",(unsigned)i+1);
            publish(FOLIO_WR_WORK,stage,true);
            ChapterContent content;
            if(!fetch_chapter(i,&content,&error)){
#ifdef ESP_PLATFORM
                ESP_LOGW("folio_weread","Chapter download failed: index=%u kind=%d http=%d",(unsigned)i,(int)error.code,error.http_status);
#endif
                if(cancelled(nullptr))return;
                if(error.code==ErrorCode::SessionExpired || error.code==ErrorCode::NotLoggedIn){fail(error);return;}
                // One unavailable chapter must not discard the rest of the book:
                // remember the gap, keep going, and report it after the export.
                if(!failed){failed_first=i;copy(first_error,error.message);}
                if(++failed>=kFailedLimit){snprintf(error_text,sizeof(error_text),"第 %u 章：%s",(unsigned)i+1,error.message);okay=false;break;}
                portENTER_CRITICAL(&status_mux);status.completed=(uint32_t)(i-first+1);++status.revision;portEXIT_CRITICAL(&status_mux);
                continue;
            }
            if(!folio_weread_cache_write_chapter(writer.get(),i,(const uint8_t*)content.bytes.data(),content.bytes.size(),
                content.xhtml?FOLIO_WEREAD_XHTML:FOLIO_WEREAD_TEXT,error_text,sizeof(error_text))){okay=false;break;}
        }
        portENTER_CRITICAL(&status_mux);status.completed=(uint32_t)(i-first+1);++status.revision;portEXIT_CRITICAL(&status_mux);
    }
    if(cancelled(nullptr))return;
    char published[256];char publish_error[192];
    bool committed;
    {StageTimer phase("publish");committed=folio_weread_cache_publish(writer.get(),published,sizeof(published),publish_error,sizeof(publish_error));}
    bool cover_saved=false;
    if(committed){
        copy(book_path,published);
        uint32_t new_position=0;
        if(restore && folio_weread_cache_cloud_to_local(book_path,&previous,&new_position,publish_error,sizeof(publish_error))){
            StorageLock lock;if(lock.held){old_time.position=new_position;old_time.book_revision=folio_weread_cache_revision(book_path);folio_weread_time_save(book_path,&old_time);}
        }
        if(okay){publish(FOLIO_WR_WORK,"正文已保存，正在获取封面",true);cover_saved=fetch_cover(books[selected]);}
    }
    if(!okay){
        if(committed && folio_weread_cache_export(book_path,published,sizeof(published),publish_error,sizeof(publish_error)))copy(book_path,published);
        publish(FOLIO_WR_DOWNLOAD_ERROR,error_text);return;
    }
    if(!committed){publish(FOLIO_WR_DOWNLOAD_ERROR,publish_error);return;}
    publish(FOLIO_WR_WORK,"正在生成 EPUB",true);
    {StageTimer phase("export");
        if(!folio_weread_cache_export(book_path,published,sizeof(published),publish_error,sizeof(publish_error))){publish(FOLIO_WR_DOWNLOAD_ERROR,publish_error);return;}
    }
    copy(book_path,published);
    if(failed){
        char gap[192];
        snprintf(gap,sizeof(gap),"已保存 %u / %u 章。第 %u 章起有 %u 章暂未取得（%s），可稍后重试下载。",
            (unsigned)((end-first)-failed),(unsigned)(end-first),(unsigned)failed_first+1,failed,first_error);
        book_page(first/FOLIO_WR_PAGE_SIZE,gap);
        return;
    }
    book_page(first/FOLIO_WR_PAGE_SIZE,cover_saved?"下载已保存。现在可以离线阅读。":"正文已保存，封面暂未取得，重试下载可补取。");
}
static const Chapter* find_chapter(const char* uid) {
    for(const auto& chapter:chapters)if(chapter.uid==uid)return &chapter;
    return nullptr;
}
static bool verify_source(const folio_weread_position_t& anchor,Error* error) {
    const Chapter* chapter=find_chapter(anchor.chapter_uid);
    if(!chapter)return false;
    ChapterContent raw;if(!client.FetchChapter(books[selected].id,*chapter,&raw,error))return false;
    return folio_weread_cache_matches_raw(&anchor,(const uint8_t*)raw.bytes.data(),raw.bytes.size(),
        raw.xhtml?FOLIO_WEREAD_XHTML:FOLIO_WEREAD_TEXT);
}
static bool same_progress(const Progress& a,const Progress& b) {
    return a.has_offset==b.has_offset && a.chapter_uid==b.chapter_uid &&
        a.chapter_offset==b.chapter_offset && a.update_time==b.update_time;
}
static bool complete_cache() {
    /* Counting must not materialize the book: cache_prepare() parses and copies
     * every chapter, which made 进度同步 take seconds on a large book. */
    return folio_weread_cache_chapter_count(book_path)==chapters.size();
}
static void check_sync() {
    retry_task=RetryTask::Sync;
    if(selected>=books.size() || !connected())return;
    work("正在核对云端位置与正文版本");
    portENTER_CRITICAL(&status_mux);status.sync_local=status.sync_cloud=false;portEXIT_CRITICAL(&status_mux);
    Error error;char message[192]{};folio_weread_time_t t{};
    // Reading the cloud position is a GET; retry a dropped link before failing.
    bool cloud_okay=client.FetchProgress(books[selected].id,&cloud,&error);
    for(unsigned attempt=0;!cloud_okay && !cancelled(nullptr) && attempt<2 && transient_read_error(error.code);++attempt){
        vTaskDelay(pdMS_TO_TICKS(300*(attempt+1)));
        folio_network_status_t network;folio_network_get_status(&network);
        if(!network.connected)break;
        cloud={};
        cloud_okay=client.FetchProgress(books[selected].id,&cloud,&error);
    }
    if(!cloud_okay){fail(error);return;}
    login_required=false;
    bool local=load_time(&t,true) && t.book_revision && t.book_revision==folio_weread_cache_revision(book_path) &&
        folio_weread_cache_local_to_cloud(book_path,t.position,&local_anchor,message,sizeof(message));
    bool complete=local && complete_cache();
    local=local && complete;

    cloud_anchor={};copy(cloud_anchor.book_id,books[selected].id.c_str());copy(cloud_anchor.chapter_uid,cloud.chapter_uid.c_str());
    cloud_anchor.offset_utf16=cloud.chapter_offset;
    bool remote=cloud.has_offset && folio_weread_cache_cloud_to_local(book_path,&cloud_anchor,&cloud_byte,message,sizeof(message));
    if(remote){remote=folio_weread_cache_local_to_cloud(book_path,cloud_byte,&cloud_anchor,message,sizeof(message));}
    char local_text[128],cloud_text[128];
    const Chapter* lc=local?find_chapter(local_anchor.chapter_uid):nullptr;
    const Chapter* cc=find_chapter(cloud.chapter_uid.c_str());
    if(local)snprintf(local_text,sizeof(local_text),"%s · %u%%",lc?lc->title.c_str():"已验证位置",(unsigned)t.percent);
    else copy(local_text,"请先打开本地书籍阅读，或重新下载正文。");
    if(cloud.has_offset)snprintf(cloud_text,sizeof(cloud_text),"%s · %d%%",cc?cc->title.c_str():"云端章节",cloud.percent);
    else copy(cloud_text,"云端暂时没有可核对的阅读位置。");
    cloud_position_ready=remote;
    portENTER_CRITICAL(&status_mux);status.sync_local=local;status.sync_cloud=remote || cloud.has_reading_time;
    status.cloud_time_available=cloud.has_reading_time;status.cloud_total_seconds=cloud.reading_seconds;
    copy(status.local_position,local_text);copy(status.cloud_position,cloud_text);portEXIT_CRITICAL(&status_mux);
    time_status();publish(FOLIO_WR_SYNC,local && remote?"请选择同步方向，确认时将核对正文版本。":!complete?
        "上传本机位置与时长需完整下载并阅读。采用云端位置需下载对应章节。":
        "仅开放可精确定位的位置，确认时核对正文版本。");
}
static void open_requested_sync() {
    char id[64],title[160],author[96],error[192];
    if(!folio_weread_cache_book_id(requested_sync_path,id,sizeof(id)) ||
       !folio_weread_cache_metadata(requested_sync_path,title,sizeof(title),author,sizeof(author),error,sizeof(error))){
        fail("这本书没有可同步的微信读书记录，请重新下载。");return;
    }
    Book book;book.id=id;book.title=title;book.author=author;
    books.clear();books.push_back(std::move(book));
    choose_book(0,true);
    if(status.state==FOLIO_WR_BOOK && !stopping)check_sync();
}
static bool fresh_preview() {
    Progress latest;Error error;
    if(!client.FetchProgress(books[selected].id,&latest,&error)){fail(error);return false;}
    if(!same_progress(latest,cloud)){fail("云端进度已发生变化，请重新核对后同步。");return false;}
    cloud=std::move(latest);return true;
}
static void use_cloud() {
    bool valid;portENTER_CRITICAL(&status_mux);valid=status.sync_cloud;portEXIT_CRITICAL(&status_mux);
    if(!valid || selected>=books.size() || !connected())return;
    work("正在确认云端阅读位置");Error error;char message[192];uint32_t position;
    if(!fresh_preview())return;
    if(cloud_position_ready){
        if(!verify_source(cloud_anchor,&error)){if(error.code!=ErrorCode::None)fail(error);else fail("正文已变化，请重新下载并核对。");return;}
        if(!folio_weread_cache_cloud_to_local(book_path,&cloud_anchor,&position,message,sizeof(message))){fail("正文已变化或无法定位，请重新下载并核对。");return;}
    }
    if(cloud.has_reading_time){
        folio_weread_time_t t{};
        if(!load_time(&t,true)){fail("时长记录属于其他账号或已损坏，未覆盖本地记录。");return;}
        // Timestamp belongs to the server position, not the local download time.
        t.cloud_valid=true;t.cloud_seconds=cloud.reading_seconds;t.cloud_updated_at=cloud.update_time;
        StorageLock lock;if(!lock.held || !folio_weread_time_save(book_path,&t)){fail("云端时长未能保存，请检查 TF 卡。");return;}
    }
    if(!cloud_position_ready){
        time_status();publish(FOLIO_WR_SYNC,"已下载云端累计时长；阅读位置无法精确定位，已保留本机位置。");return;
    }
    portENTER_CRITICAL(&status_mux);copy(open_path,book_path);open_byte=position;portEXIT_CRITICAL(&status_mux);
    publish(FOLIO_WR_BOOK,cloud.has_reading_time?"已下载云端位置和累计时长，即将继续阅读。":"已下载云端位置；服务器未提供累计时长。");
}
static void upload(bool time) {
    bool valid;portENTER_CRITICAL(&status_mux);valid=status.sync_local;portEXIT_CRITICAL(&status_mux);
    if(!valid || selected>=books.size() || !connected())return;
    work(time?"正在提交本次新增阅读时长":"正在上传本机位置并读回核对");
    if(!fresh_preview())return;
    folio_weread_time_t t;Error error;
    if(!load_time(&t,true)){fail("记录损坏或属于其他账号，已停止上传。");return;}
    if(t.book_revision!=folio_weread_cache_revision(book_path)){fail("正文版本已变化，请重新下载并核对。");return;}
    if(!verify_source(local_anchor,&error)){if(error.code!=ErrorCode::None)fail(error);else fail("正文版本已变化，请重新下载并核对。");return;}
    const Chapter* chapter=find_chapter(local_anchor.chapter_uid);if(!chapter){fail("未找到对应章节。");return;}
    Submission result;
    if(time){
        StorageLock lock;
        if(!lock.held || !folio_weread_time_begin(book_path,&t)){fail("没有可提交的新增时长，或记录未能安全保存。");return;}
    }
    if(time)result=client.SubmitReadingTime(books[selected].id,*chapter,local_anchor.offset_utf16,(int)t.percent,t.inflight,&error);
    else {Progress readback;result=client.WritePosition(books[selected].id,*chapter,local_anchor.offset_utf16,(int)t.percent,&readback,&error);}
    if(time){
        StorageLock lock;
        folio_wr_time_result_t outcome=result==Submission::Confirmed?FOLIO_WR_TIME_ACCEPTED:
            result==Submission::Rejected?FOLIO_WR_TIME_REJECTED:FOLIO_WR_TIME_UNKNOWN;
        if(!lock.held || !folio_weread_time_finish(book_path,&t,outcome)){
            fail("请求结果未能写回 TF 卡，本段时长已隔离，请勿重复上传。");return;
        }
    }
    if(result==Submission::Confirmed){
        book_page(0,time?"请求已接受。实际计入时长以微信读书统计为准。":"云端位置已读回核对成功。");
    }else if(result==Submission::Unknown){
        book_page(0,time?"结果未知，该段时长已隔离，不会再次上传。":"位置提交结果未知，请重新查询后再决定。");
    }else {if(error.code==ErrorCode::SessionExpired || error.code==ErrorCode::NotLoggedIn)fail(error);else book_page(0,error.message);}
}
// One explicit direction can submit several acknowledged chunks. Never retry a
// write whose result is uncertain, and bound the operation so the UI stays usable.
static void upload_sync() {
    folio_weread_time_t t{};
    if(!load_time(&t,true)){fail("记录损坏或属于其他账号，已停止上传。");return;}
    if(!t.enabled || !t.pending){upload(false);return;}
    bool valid;portENTER_CRITICAL(&status_mux);valid=status.sync_local;portEXIT_CRITICAL(&status_mux);
    if(!valid || selected>=books.size() || !connected())return;
    work("正在上传阅读进度和新增时长");
    if(!fresh_preview())return;
    Error error;
    if(t.book_revision!=folio_weread_cache_revision(book_path) || !verify_source(local_anchor,&error)){
        if(error.code!=ErrorCode::None)fail(error);else fail("正文版本已变化，请重新核对。");return;
    }
    const Chapter* chapter=find_chapter(local_anchor.chapter_uid);
    if(!chapter){fail("未找到对应章节。");return;}
    int64_t deadline=esp_timer_get_time()+INT64_C(60000000);
    uint64_t submitted=0;
    while(t.pending && !cancelled(nullptr) && esp_timer_get_time()<deadline){
        {
            StorageLock lock;
            if(!lock.held || !folio_weread_time_begin(book_path,&t)){fail("新增时长未能安全保存，已停止上传。");return;}
        }
        uint32_t seconds=t.inflight;
        Submission result=client.SubmitReadingTime(books[selected].id,*chapter,local_anchor.offset_utf16,(int)t.percent,seconds,&error);
        {
            StorageLock lock;
            folio_wr_time_result_t outcome=result==Submission::Confirmed?FOLIO_WR_TIME_ACCEPTED:
                result==Submission::Rejected?FOLIO_WR_TIME_REJECTED:FOLIO_WR_TIME_UNKNOWN;
            if(!lock.held || !folio_weread_time_finish(book_path,&t,outcome)){
                fail("请求结果未能写回 TF 卡，本段时长已隔离，请勿重复上传。");return;
            }
        }
        if(result!=Submission::Confirmed){
            time_status();
            if(error.code==ErrorCode::SessionExpired || error.code==ErrorCode::NotLoggedIn)fail(error);
            else publish(FOLIO_WR_SYNC,result==Submission::Unknown?
                "本段结果未知，已隔离且不会重发。其余新增时长仍保留。":error.message);
            return;
        }
        submitted+=seconds;
        Progress readback;
        if(!client.FetchProgress(books[selected].id,&readback,&error)){
            time_status();fail(error);return;
        }
        if(!readback.has_offset || readback.chapter_uid!=local_anchor.chapter_uid || readback.chapter_offset!=local_anchor.offset_utf16){
            time_status();fail("时长请求已接受，但云端位置有变化；已停止后续提交，请重新核对。");return;
        }
        cloud=std::move(readback);
        portENTER_CRITICAL(&status_mux);status.cloud_time_available=cloud.has_reading_time;status.cloud_total_seconds=cloud.reading_seconds;portEXIT_CRITICAL(&status_mux);
    }
    char result[192];
    snprintf(result,sizeof(result),"进度已核对；已提交 %u 秒，剩余 %u 秒。云端实际计入时长以微信读书为准。",
        (unsigned)std::min<uint64_t>(submitted,UINT32_MAX),(unsigned)std::min<uint64_t>(t.pending,UINT32_MAX));
    time_status();publish(FOLIO_WR_SYNC,result);
}
static void enable_time(bool enabled) {
    folio_weread_time_t t;
    if(!load_time(&t,true)){fail("记录损坏或属于其他账号，不能改变时长同步设置。");return;}
    StorageLock lock;t.enabled=enabled;
    if(!lock.held || !folio_weread_time_save(book_path,&t)){fail("时长设置未保存，请检查 TF 卡。");return;}
    time_status();publish(FOLIO_WR_SYNC,enabled?"已开启：仅记录今后的有效阅读，提交仍需手动确认。":"已关闭新增时长记录；已有待提交记录仍保留。");
}
static void execute(const Command& command) {
    if(login_required && command.action!=FOLIO_WR_RELOGIN && command.action!=FOLIO_WR_NEW_QR &&
       command.action!=FOLIO_WR_LOGOUT && command.action!=FOLIO_WR_CANCEL){
        fail("登录已过期，请点重新登录；离线书籍和阅读记录仍保留。");return;
    }
    switch(command.action){
        case FOLIO_WR_RETRY:
            // Never resubmit writes, OTPs or login confirmations after an
            // unknown result. Those failures return to a read-only preview.
            if(!client.LoggedIn()){begin_login();break;}
            if(retry_task==RetryTask::Book){
                bool sync=retry_sync_only;choose_book(retry_book,sync);
                if(sync && status.state==FOLIO_WR_BOOK)check_sync();
            }
            else if(retry_task==RetryTask::Sync){if(requested_sync_path[0])open_requested_sync();else check_sync();}
            else if(retry_task==RetryTask::Login)begin_login();
            else fetch_shelf();
            break;
        case FOLIO_WR_REFRESH:if(requested_sync_path[0])open_requested_sync();else fetch_shelf();break;
        case FOLIO_WR_RETURN_SHELF:
            // Back navigation uses the loaded list; only Refresh requests it again.
            retry_task=RetryTask::Shelf;
            shelf_page(selected<books.size()?selected/FOLIO_WR_PAGE_SIZE:0);
            break;
        case FOLIO_WR_NEW_QR:case FOLIO_WR_RELOGIN:begin_login();break;
        case FOLIO_WR_SUBMIT_OTP:poll_login(command.value);break;
        case FOLIO_WR_SELECT_BOOK:choose_book(command.index);break;
        case FOLIO_WR_PAGE:{folio_weread_state_t state;portENTER_CRITICAL(&status_mux);state=status.state;portEXIT_CRITICAL(&status_mux);
            if(state==FOLIO_WR_SHELF)shelf_page(command.index);else if(state==FOLIO_WR_BOOK)book_page(command.index);break;}
        case FOLIO_WR_DOWNLOAD_CHAPTER:download(command.index,false);break;
        case FOLIO_WR_DOWNLOAD_ALL:download(0,true);break;
        case FOLIO_WR_RETRY_DOWNLOAD:
            if(status.state==FOLIO_WR_DOWNLOAD_ERROR)download(download_first,download_all);
            break;
        case FOLIO_WR_RETURN_TOC:
            if(status.state==FOLIO_WR_DOWNLOAD_ERROR)book_page(download_first/FOLIO_WR_PAGE_SIZE);
            break;
        case FOLIO_WR_CHECK_SYNC:check_sync();break;
        case FOLIO_WR_USE_CLOUD:use_cloud();break;
        case FOLIO_WR_UPLOAD_POSITION:upload(false);break;
        case FOLIO_WR_ENABLE_TIME:enable_time(true);break;
        case FOLIO_WR_DISABLE_TIME:enable_time(false);break;
        case FOLIO_WR_UPLOAD_TIME:upload(true);break;
        case FOLIO_WR_UPLOAD_SYNC:upload_sync();break;
        case FOLIO_WR_LOGOUT:
            if(!EraseSession()){fail("退出未完成，登录记录未能清除，请重试。");break;}
            session_loaded=true;login_required=false;shelf_cached=false;requested_sync_path[0]=0;client.Logout();books.clear();chapters.clear();chapters_book.clear();chapters_owner.clear();selected=SIZE_MAX;book_path[0]=0;
            /* The cached shelf belongs to the account that just left. */
            {StorageLock lock;unlink(SHELF_FILE);}
            portENTER_CRITICAL(&status_mux);status.qr_url[0]=status.local_path[0]=open_path[0]=0;status.can_read=false;status.item_count=status.count=0;portEXIT_CRITICAL(&status_mux);
            publish(FOLIO_WR_OFF,"已退出并清除设备登录记录，离线书籍仍保留。");break;
        case FOLIO_WR_CANCEL:stopping=true;break;
    }
}
static void worker(void*) {
    worker_task=xTaskGetCurrentTaskHandle();client.SetCancelCheck({cancelled,nullptr});
    try {
        // A restored shelf is immediately usable. Refresh is explicit, never
        // an in-flight background request that blocks the next selected book.
        bool cached=client.LoggedIn() && !login_required && !requested_sync_path[0] && shelf_load(&books);
        if(cached){shelf_cached=true;shelf_page(0);}
        else {
            int64_t deadline=esp_timer_get_time()+INT64_C(30000000);bool ready=false;
            while(!cancelled(nullptr) && esp_timer_get_time()<deadline){
                folio_network_status_t net;folio_network_get_status(&net);
                if(net.connected){ready=true;break;}
                if(!net.connecting){fail(net.message[0]?net.message:"Wi-Fi 连接已停止，请重新连接。");break;}
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if(ready && !cancelled(nullptr)){
                if(client.LoggedIn()){if(requested_sync_path[0])open_requested_sync();else fetch_shelf();}
                else begin_login();
            }
            else if(!cancelled(nullptr) && esp_timer_get_time()>=deadline)fail("Wi-Fi 连接超时，请返回网络设置重试。");
        }
        int64_t next_poll=0;
        while(!stopping){
            if(relogin_requested.exchange(false)){
                xQueueReset(commands);begin_login();
            }
            Command command{};
            if(xQueueReceive(commands,&command,pdMS_TO_TICKS(150))==pdTRUE){
                execute(command);if(!stopping)persist_login();memset(command.value,0,sizeof(command.value));
                portENTER_CRITICAL(&status_mux);
                if(status.busy){status.busy=false;++status.revision;}
                portEXIT_CRITICAL(&status_mux);
            }
            folio_weread_state_t state;portENTER_CRITICAL(&status_mux);state=status.state;portEXIT_CRITICAL(&status_mux);
            if(state==FOLIO_WR_LOGIN && esp_timer_get_time()>=next_poll){next_poll=esp_timer_get_time()+INT64_C(2000000);poll_login(nullptr);}
        }
    }catch(const std::bad_alloc&){fail("内存不足，请更换较小字体或分章下载后重试。");}
     catch(...){fail("操作已停止，请重新打开微信读书。");}
    persist_login();client.Disconnect();
    std::vector<Book>().swap(books);std::vector<Chapter>().swap(chapters);chapters_book.clear();chapters_owner.clear();selected=SIZE_MAX;
    portENTER_CRITICAL(&status_mux);status.running=status.busy=false;++status.revision;portEXIT_CRITICAL(&status_mux);
    worker_task=nullptr;worker_alive=false;vTaskDelete(nullptr);
}
} // namespace

// Overrides the weak no-op in folio_weread_export.inc. The UI polls at 500 ms
// and repaints are expensive, so report at most every 2 s and always the last.
extern "C" void folio_weread_export_progress(size_t done,size_t total) {
    static int64_t next;
    if(!total)return;
    int64_t now=esp_timer_get_time();
    if(done<total && now<next)return;
    next=now+INT64_C(2000000);
    char stage[128];
    snprintf(stage,sizeof(stage),"正在生成 EPUB（%u / %u 章）",(unsigned)done,(unsigned)total);
    publish(FOLIO_WR_WORK,stage,true);
}

extern "C" bool folio_weread_busy(void) {
    portENTER_CRITICAL(&status_mux);
    bool busy=status.busy;
    portEXIT_CRITICAL(&status_mux);
    return busy;
}

static bool start_service(const char* sync_path,char* error,size_t cap) {
    if(cap && error)error[0]=0;
    if(worker_alive){if(stopping){if(cap && error)snprintf(error,cap,"正在关闭连接，请稍候。");return false;}return true;}
    copy(requested_sync_path,sync_path);retry_task=sync_path?RetryTask::Sync:RetryTask::Shelf;
    // No worker is active here. Restore before attempting Wi-Fi so a temporary
    // network failure still shows the saved login, rather than asking for QR.
    if(!restore_login()){
        if(cap&&error){portENTER_CRITICAL(&status_mux);snprintf(error,cap,"%s",status.message);portEXIT_CRITICAL(&status_mux);}
        return false;
    }
    if(!folio_weread_flush()){fail("阅读记录未能保存，请检查 TF 卡。");if(cap && error)snprintf(error,cap,"阅读记录未能保存，请检查 TF 卡。");return false;}
    record_loaded=false;record_path[0]=0;record_error=false;
    char message[192];
    if(!folio_network_start_station(message,sizeof(message))){publish(FOLIO_WR_OFF,message);if(cap && error)snprintf(error,cap,"%s",message);return false;}
    if(!commands)commands=xQueueCreate(1,sizeof(Command));
    if(!commands){folio_network_stop();if(cap && error)snprintf(error,cap,"内存不足，无法启动联网任务。");return false;}
    xQueueReset(commands);stopping=false;relogin_requested=false;worker_alive=true;
    portENTER_CRITICAL(&status_mux);status.running=true;status.qr_url[0]=open_path[0]=0;status.completed=status.total=0;copy(status.title,"微信读书");portEXIT_CRITICAL(&status_mux);
    publish(FOLIO_WR_CONNECTING,"正在连接 Wi-Fi",true);
    if(xTaskCreate(worker,"folio_weread",16384,nullptr,3,nullptr)!=pdPASS){worker_alive=false;folio_network_stop();fail("内存不足，无法启动联网任务。");return false;}
    return true;
}
extern "C" bool folio_weread_start(char* error,size_t cap) {return start_service(nullptr,error,cap);}
extern "C" bool folio_weread_start_sync(const char* path,char* error,size_t cap) {
    if(worker_alive){if(error && cap)snprintf(error,cap,"请先关闭当前微信读书任务。");return false;}
    char id[64];
    if(!path || !folio_weread_cache_book_id(path,id,sizeof(id))){
        fail("仅微信读书下载的书籍支持云端同步。");
        if(error && cap)snprintf(error,cap,"仅微信读书下载的书籍支持云端同步。");
        return false;
    }
    return start_service(path,error,cap);
}
extern "C" bool folio_weread_stop(void) {
    stopping=true;
    if(worker_alive)return false;
    bool okay=folio_network_stop();
    if(okay){portENTER_CRITICAL(&status_mux);status.running=status.busy=false;portEXIT_CRITICAL(&status_mux);}
    return okay;
}
extern "C" bool folio_weread_io_cancelled(void) {
    return cancelled(nullptr) && worker_task.load()==xTaskGetCurrentTaskHandle();
}
extern "C" void folio_weread_get_status(folio_weread_status_t* out) {
    if(!out)return;
    portENTER_CRITICAL(&status_mux);*out=status;portEXIT_CRITICAL(&status_mux);
}
extern "C" bool folio_weread_command(folio_weread_action_t action,size_t index,const char* value) {
    if(action==FOLIO_WR_CANCEL){stopping=true;return true;}
    if(!worker_alive || stopping || !commands)return false;
    if(action==FOLIO_WR_RELOGIN){relogin_requested=true;return true;}
    bool busy;portENTER_CRITICAL(&status_mux);busy=status.busy;portEXIT_CRITICAL(&status_mux);if(busy)return false;
    Command command{action,index,{0}};copy(command.value,value);
    portENTER_CRITICAL(&status_mux);status.busy=true;
    if(action==FOLIO_WR_DOWNLOAD_CHAPTER || action==FOLIO_WR_DOWNLOAD_ALL || action==FOLIO_WR_RETRY_DOWNLOAD)copy(status.message,"正在准备下载，可返回取消");
    ++status.revision;portEXIT_CRITICAL(&status_mux);
    if(xQueueSend(commands,&command,0)!=pdTRUE){portENTER_CRITICAL(&status_mux);status.busy=false;++status.revision;portEXIT_CRITICAL(&status_mux);return false;}
    return true;
}
extern "C" bool folio_weread_flush(void) {
    if(record_error)return false;
    if(!record_loaded || !record_dirty)return true;
    StorageLock lock;if(!lock.held || !folio_weread_time_save(record_path,&record))return false;
    record_dirty=false;return true;
}
// A public EPUB can be copied without its sync cache. Only proven absence is
// ordinary reading; existing ledgers, unsafe paths and I/O errors stay fail-closed.
static bool missing_sync_cache(const char* path) {
    char hidden[256],legacy[288];struct stat info;
    if(!folio_wr_aux(path,"",hidden,sizeof(hidden)))return false;
    for(char slot:{'a','b'}){
        int n=snprintf(legacy,sizeof(legacy),"%s.wrt.%c",path,slot);
        if(n<=0 || (size_t)n>=sizeof(legacy) || !folio_storage_validate_parent(legacy,nullptr,0) ||
           stat(legacy,&info)==0 || errno!=ENOENT)return false;
    }
    // Check each component so missing parents are safe, but links and non-directory
    // components cannot masquerade as an absent cache (the last slash is included).
    for(char* end=hidden+strlen(FOLIO_SD_ROOT)+1;(end=strchr(end,'/'))!=nullptr;++end){
        *end=0;
        bool safe=folio_storage_validate_parent(hidden,nullptr,0);
        bool missing=safe && stat(hidden,&info)!=0 && errno==ENOENT;
        *end='/';
        if(!safe)return false;
        if(missing)return true;
    }
    return false;
}
extern "C" void folio_weread_record(const char* path,uint32_t byte,uint32_t percent,uint32_t seconds) {
    if(worker_alive || !path)return;
    const char* source=path;
    char stable[256];if(folio_wr_identity(path,stable,sizeof(stable)))path=stable;
    if(strlen(path)<4 || strcmp(path+strlen(path)-4,".wrb"))return;
    if(strcmp(path,record_path)){
        if(!folio_weread_flush()){record_error=true;return;}
        StorageLock lock;if(!lock.held)return;
        if(strcmp(source,path) && missing_sync_cache(path))return;
        copy(record_path,path);record_loaded=folio_weread_time_load(path,&record);record_error=!record_loaded;
        if(!record_loaded)return;
        record.book_revision=folio_weread_cache_revision(path);
    }
    if(!record_loaded)return;
    record.position=byte;record.percent=std::min<uint32_t>(percent,100);
    if(!record.book_revision)record.book_revision=folio_weread_cache_revision(path);
    // Only the logged-in account can accrue uploadable time. Wi-Fi may be off;
    // logout/reboot keeps ordinary local statistics but never attributes those
    // anonymous seconds to a previous account's ledger.
    uint8_t owner[32];
    if(seconds && record.enabled && owner_digest(owner) && !memcmp(record.owner,owner,sizeof(owner)))
        folio_weread_time_add(&record,seconds);
    record_dirty=true;
}
extern "C" bool folio_weread_take_open(char* path,size_t cap,uint32_t* position) {
    if(!path || !cap || !position)return false;
    portENTER_CRITICAL(&status_mux);bool ready=open_path[0]!=0;
    if(ready){snprintf(path,cap,"%s",open_path);*position=open_byte;open_path[0]=0;}
    portEXIT_CRITICAL(&status_mux);return ready;
}
extern "C" bool folio_weread_local_position(const char* path,uint32_t* position) {
    if(!path || !position)return false;
    StorageLock lock;folio_weread_time_t t{};
    if(!lock.held || !folio_weread_time_load(path,&t) || !t.book_revision || t.book_revision!=folio_weread_cache_revision(path))return false;
    *position=t.position;return true;
}
