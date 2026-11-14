// Include the actual service to exercise its private orchestration without a
// background task or account. RTOS here is only a lifecycle/queue surface.
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../main/net/folio_weread.cpp"
#include "nvs.h"
#include "../../main/net/weread/folio_weread_crypto.h"
#include "../../main/net/weread/vendor/cjson/cJSON.h"

namespace fixture {
static std::string directory, account="accountA";
static std::string uid="c0";
static uint32_t offset=0, updated=10;
static int percent=0, read_posts=0, time_posts=0, network_calls=0;
static bool fail_ack_save=false, rename_failure=false;
static int64_t tick=1000000;
static bool station=true;
static int tasks_created=0;
static bool login_timeout, expire_progress, unknown_time;
static uint32_t cloud_seconds=1234;
static std::vector<unsigned char> stored_session,pending_session;
static bool nvs_fail_commit;static int nvs_commits;
static int shelf_failures,shelf_calls,reader_failures,toc_calls,toc_failures;
static int shard_failures,shard_calls[3];
static std::vector<std::string> raw;
static std::string cover_data;static size_t cover_at;static int cover_requests,cover_header_waits,cover_body_waits;static bool cover_test;
static std::string load(const std::string& p){std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
static std::string str(cJSON* j,const char* name){cJSON* v=cJSON_GetObjectItem(j,name);assert(cJSON_IsString(v));return v->valuestring;}
static int chapter_index(const std::string& encoded){for(int i=0;i<4;++i)if(folio::weread::crypto::EncodeId("c"+std::to_string(i))==encoded)return i;assert(false);return -1;}
static std::string toc(){std::string out="{\"data\":[{\"bookId\":\"book1\",\"updated\":[";for(int i=0;i<4;++i){if(i)out+=',';out+="{\"chapterUid\":\"c"+std::to_string(i)+"\",\"chapterIdx\":"+std::to_string(i)+",\"title\":\"Chapter "+std::to_string(i)+"\",\"wordCount\":40}";}return out+"]}]}";}
static void login(const char* id){
 account=id;Error error;QrSession session;QrState state;
 assert(client.BeginQrLogin(&session,&error));assert(client.PollQrLogin(session,"",&state,&error)&&state==QrState::Complete);
}
static void select_book(){fetch_shelf();assert(books.size()==1);choose_book(0);assert(selected==0&&chapters.size()==4);}
static void forget_record(){assert(folio_weread_flush());record_loaded=false;record_dirty=false;record_error=false;record_path[0]=0;record={};}
static folio_weread_time_t ledger(){folio_weread_time_t t{};assert(load_time(&t,false));return t;}
}
namespace folio::weread {
void EspTransportDisconnect() noexcept {}
void EspTransport(const HttpRequest& q,HttpResponse* r,CancelCheck cancel,void*){
 using namespace fixture;++network_calls;assert(!cancel());assert(q.url.rfind("https://weread.qq.com/",0)==0);
 r->status=200;r->sent=true;
 if(q.url.find("getLoginUid")!=std::string::npos){r->body="{\"uid\":\"fixture_qr\"}";return;}
 if(q.url.find("getLoginInfo")!=std::string::npos){assert(q.timeout_ms==30000);if(login_timeout){r->failure=ErrorCode::Timeout;return;}r->body="{\"succeed\":true}";r->set_cookies={"wr_vid="+account,"wr_skey=NO_REAL_ACCOUNT"};return;}
 assert(q.cookie.find("wr_vid="+account)!=std::string::npos);
 if(q.url.find("shelf/sync")!=std::string::npos){++shelf_calls;if(shelf_failures>0){--shelf_failures;r->failure=ErrorCode::Transport;return;}r->body="{\"books\":[{\"bookId\":\"book1\",\"title\":\"Service Fixture\",\"author\":\"Test\"}]}";return;}
 if(q.url.find("chapterInfos")!=std::string::npos){++toc_calls;if(toc_failures>0){--toc_failures;r->failure=ErrorCode::Transport;return;}r->body=toc();return;}
 if(q.url.find("/web/reader/")!=std::string::npos){if(reader_failures>0){--reader_failures;r->failure=ErrorCode::Connection;return;}assert(q.timeout_ms==60000);r->body="{\"psvts\":\"fixture_ps\",\"token\":\"fixture_token\"}";return;}
 if(q.url.find("/web/book/getProgress")!=std::string::npos){if(expire_progress){r->status=401;r->body="{}";return;}r->body="{\"book\":{\"bookId\":\"book1\",\"chapterUid\":\""+uid+"\",\"chapterOffset\":"+std::to_string(offset)+",\"progress\":"+std::to_string(percent)+",\"updateTime\":"+std::to_string(updated)+",\"readingTime\":"+std::to_string(cloud_seconds)+"}}";return;}
 if(q.url.find("/web/book/chapter/e_")!=std::string::npos){assert(q.timeout_ms==60000);cJSON* j=cJSON_Parse(q.body.c_str());assert(j);int ch=chapter_index(str(j,"c"));cJSON_Delete(j);char tail=q.url.back();int part=tail=='0'?0:tail=='1'?1:2;++shard_calls[part];if(part==1&&shard_failures>0){--shard_failures;r->failure=ErrorCode::Timeout;return;}r->body=load(directory+"/c"+std::to_string(ch)+"_"+std::to_string(part));return;}
 if(q.url=="https://weread.qq.com/web/book/read"){
  ++read_posts;cJSON* j=cJSON_Parse(q.body.c_str());assert(j);int ch=chapter_index(str(j,"c"));uid="c"+std::to_string(ch);offset=cJSON_GetObjectItem(j,"co")->valueint;percent=cJSON_GetObjectItem(j,"pr")->valueint;++updated;
  if(cJSON_GetObjectItem(j,"rt")){++time_posts;cloud_seconds+=(uint32_t)cJSON_GetObjectItem(j,"rt")->valueint;if(fail_ack_save){rename_failure=true;fail_ack_save=false;}}
  cJSON_Delete(j);if(unknown_time){unknown_time=false;r->failure=ErrorCode::Timeout;return;}r->body="{\"succ\":1}";return;
 }
 assert(false && "unexpected protocol request");
}
}
extern "C" {
esp_err_t nvs_flash_init(){return ESP_OK;}
esp_err_t nvs_open(const char* name,nvs_open_mode_t,nvs_handle_t* h){assert(!strcmp(name,"folio_weread"));*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t){fixture::pending_session.clear();}
esp_err_t nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size){
 assert(!strcmp(key,"session"));auto& data=fixture::stored_session;if(data.empty())return ESP_ERR_NVS_NOT_FOUND;
 if(!out){*size=data.size();return ESP_OK;}if(*size<data.size())return ESP_FAIL;
 memcpy(out,data.data(),data.size());*size=data.size();return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t,const char* key,const void* data,size_t size){assert(!strcmp(key,"session"));const auto* p=(const unsigned char*)data;fixture::pending_session.assign(p,p+size);return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t,const char* key){assert(!strcmp(key,"session"));fixture::pending_session.clear();return fixture::stored_session.empty()?ESP_ERR_NVS_NOT_FOUND:ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t){++fixture::nvs_commits;if(fixture::nvs_fail_commit)return ESP_FAIL;fixture::stored_session=fixture::pending_session;return ESP_OK;}
int folio_test_time_rename(const char* from,const char* to){
 if(fixture::rename_failure){fixture::rename_failure=false;errno=EIO;return -1;}
 struct stat st;if(!lstat(to,&st)){errno=EEXIST;return -1;}if(errno!=ENOENT)return -1;
 return rename(from,to);
}
psa_status_t psa_crypto_init(void){return PSA_SUCCESS;}
psa_status_t psa_hash_compute(psa_algorithm_t alg,const uint8_t* p,size_t n,uint8_t* out,size_t cap,size_t* written){assert(alg==PSA_ALG_SHA_256&&cap>=32);CC_SHA256(p,(CC_LONG)n,out);*written=32;return PSA_SUCCESS;}
int64_t esp_timer_get_time(void){return fixture::tick;}
void folio_network_get_status(folio_network_status_t* out){*out={};out->connected=out->clock_valid=out->running=fixture::station;}
bool folio_network_start_station(char* error,size_t cap){if(error&&cap)*error=0;fixture::station=true;return true;}
bool folio_network_stop(void){fixture::station=false;return true;}
bool folio_network_take_utc(uint32_t*){return false;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return reinterpret_cast<void*>(1);}
void vTaskDelay(TickType_t delay){fixture::tick+=static_cast<int64_t>(delay)*1000;}
void vTaskDelete(TaskHandle_t){}
BaseType_t xTaskCreate(void (*)(void*),const char*,uint32_t stack,void*,unsigned,TaskHandle_t*){assert(stack>=16384);++fixture::tasks_created;return pdPASS;}
struct fake_queue {size_t size;bool used;unsigned char data[256];};
QueueHandle_t xQueueCreate(unsigned count,unsigned size){assert(count==1&&size<=256);static fake_queue q{};q.size=size;return &q;}
BaseType_t xQueueReset(QueueHandle_t q){static_cast<fake_queue*>(q)->used=false;return pdTRUE;}
BaseType_t xQueueSend(QueueHandle_t q,const void* p,TickType_t){auto* v=static_cast<fake_queue*>(q);if(v->used)return pdFALSE;memcpy(v->data,p,v->size);v->used=true;return pdTRUE;}
BaseType_t xQueueReceive(QueueHandle_t q,void* p,TickType_t){auto* v=static_cast<fake_queue*>(q);if(!v->used)return pdFALSE;memcpy(p,v->data,v->size);v->used=false;return pdTRUE;}
int esp_crt_bundle_attach(void*){return 0;}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config){
 assert(fixture::cover_test && config->timeout_ms==5000 && config->disable_auto_redirect && config->crt_bundle_attach==esp_crt_bundle_attach);
 assert(std::string(config->url)=="https://img.qpic.cn/cover.png");++fixture::cover_requests;fixture::cover_at=0;return reinterpret_cast<esp_http_client_handle_t>(1);
}
int esp_http_client_set_timeout_ms(esp_http_client_handle_t,int ms){assert(ms==2000);return 0;}
int esp_http_client_close(esp_http_client_handle_t){return 0;}
int esp_http_client_cleanup(esp_http_client_handle_t){return 0;}
int esp_http_client_open(esp_http_client_handle_t,int){return 0;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t){if(fixture::cover_header_waits-->0)return -ESP_ERR_HTTP_EAGAIN;return fixture::cover_data.size();}
int esp_http_client_get_status_code(esp_http_client_handle_t){return 200;}
int esp_http_client_read(esp_http_client_handle_t,char* out,int size){
 if(fixture::cover_body_waits-->0)return -ESP_ERR_HTTP_EAGAIN;
 size_t n=std::min<size_t>(size,fixture::cover_data.size()-fixture::cover_at);memcpy(out,fixture::cover_data.data()+fixture::cover_at,n);fixture::cover_at+=n;return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t){return fixture::cover_at==fixture::cover_data.size();}

}
static void test_standalone_epub(){
 using namespace fixture;
 const char* portable=FOLIO_SD_ROOT "/books/Portable [wr-706f727461626c65].epub";
 assert(mkdir(FOLIO_SD_ROOT "/books",0700)==0);
 {std::ifstream in(directory+"/with-cover.epub",std::ios::binary);std::ofstream out(portable,std::ios::binary);assert(in&&out);out<<in.rdbuf();assert(out.good());}
 assert(access(FOLIO_SD_ROOT "/.folio",F_OK)!=0&&errno==ENOENT);
 // Opening the EPUB may create ordinary text caches, never a WeRead ledger.
 char error[192],hidden[256];folio_epub_t epub{};
 assert(folio_epub_prepare(portable,&epub,error,sizeof(error))&&epub.chapter_count>0);folio_epub_free(&epub);
 assert(folio_wr_aux(portable,"",hidden,sizeof(hidden)));
 assert(access(hidden,F_OK)!=0&&errno==ENOENT);
 folio_weread_record(portable,0,0,0);
 assert(folio_weread_flush());
 assert(!record_loaded&&!record_dirty&&!record_error&&!record_path[0]);
 folio_weread_record(portable,3,1,30);assert(folio_weread_flush());
 assert(!record_loaded&&!record_dirty&&!record_error&&!record_path[0]);
 assert(access(hidden,F_OK)!=0&&errno==ENOENT&&read_posts==0&&time_posts==0);
 assert(folio_weread_start(error,sizeof(error))&&tasks_created==1);
 worker_alive=false;assert(folio_weread_stop());stopping=false;station=true;
 // A healthy local download must still record after reading the portable copy.
 login("accountA");select_book();download(0,true);assert(status.can_read);
 const std::string healthy=book_path;
 enable_time(true);folio_weread_record(healthy.c_str(),3,1,60);
 assert(record_dirty&&record.pending==60);
 folio_weread_record(portable,6,2,30);assert(!record_dirty&&folio_weread_flush());
 auto t=ledger();assert(t.pending==60&&t.position==3);
 assert(!record_error&&!record_dirty&&record.pending==60);
 assert(access(hidden,F_OK)!=0&&errno==ENOENT&&read_posts==0&&time_posts==0);
 assert(folio_weread_start(error,sizeof(error))&&tasks_created==2);
 worker_alive=false;assert(folio_weread_stop());stopping=false;station=true;
 // Existing but corrupt ledgers remain fail-closed, even with no cache manifest.
 assert(mkdir(hidden,0700)==0);
 char slot[288];assert(folio_wr_aux(portable,"time.a",slot,sizeof(slot)));
 {std::ofstream out(slot,std::ios::binary);out<<"corrupt acknowledgement";assert(out.good());}
 const std::string corrupt=load(slot);assert(!folio_weread_cache_revision(portable));
 folio_weread_record(portable,0,0,0);assert(record_error&&!folio_weread_flush());
 assert(!folio_weread_start(error,sizeof(error))&&tasks_created==2);
 folio_weread_record(healthy.c_str(),9,3,60);assert(record_error&&!folio_weread_flush());
 assert(load(slot)==corrupt);t=ledger();assert(t.pending==60&&t.position==3);
 assert(read_posts==0&&time_posts==0);
 std::puts("Standalone EPUB isolation passed: clean card, zero-second and timed record, flush/start, healthy ledger preservation, corrupt ledger fail-closed; no time POSTs");
}
int main(int argc,char** argv){
 using namespace fixture;assert(argc==2||argc==3);directory=argv[1];for(int i=0;i<4;++i)raw.push_back(load(directory+"/raw"+std::to_string(i)));
 assert(mkdir(FOLIO_SD_ROOT,0700)==0);client.SetCancelCheck({cancelled,nullptr});
 if(argc==3){assert(!strcmp(argv[2],"--standalone"));test_standalone_epub();return 0;}
 // Real NVS store, with only the flash driver simulated. No real credentials.
 Session remembered;copy(remembered.vid,"accountA");copy(remembered.skey,"saved_fixture");copy(remembered.rt,"renew_fixture");
 assert(SaveSession(remembered));int commits=nvs_commits;assert(SaveSession(remembered)&&nvs_commits==commits);
 client.Logout();session_loaded=false;assert(restore_login()&&client.LoggedIn());
 Session restored;assert(client.ExportSession(&restored)&&!strcmp(restored.rt,"renew_fixture"));
 copy(remembered.skey,"rotated_fixture");nvs_fail_commit=true;assert(!SaveSession(remembered));
 assert(LoadSession(&restored)==SessionLoad::Loaded&&!strcmp(restored.skey,"saved_fixture"));
 execute({FOLIO_WR_LOGOUT,0,{}});assert(client.LoggedIn()&&status.state==FOLIO_WR_ERROR);
 nvs_fail_commit=false;execute({FOLIO_WR_LOGOUT,0,{}});assert(!client.LoggedIn()&&LoadSession(&restored)==SessionLoad::Missing);
 assert(SaveSession(remembered));stored_session.back()^=1;assert(LoadSession(&restored)==SessionLoad::Invalid);
 session_loaded=false;assert(!restore_login()&&!client.LoggedIn()&&stored_session.empty());
 memset(remembered.skey,'x',sizeof(remembered.skey));assert(!SaveSession(remembered)&&!client.RestoreSession(remembered));
 // An unscanned long poll must preserve the same QR; bounded expiry, offline
 // errors and explicit OTP timeouts must still leave the waiting state.
 begin_login();assert(status.state==FOLIO_WR_LOGIN);std::string pending_qr=status.qr_url;
 login_timeout=true;poll_login(nullptr);assert(status.state==FOLIO_WR_LOGIN&&pending_qr==status.qr_url);
 tick+=181000000;poll_login(nullptr);assert(status.state==FOLIO_WR_ERROR);
 begin_login();station=false;poll_login(nullptr);assert(status.state==FOLIO_WR_ERROR);station=true;
 begin_login();poll_login("1234");assert(status.state==FOLIO_WR_ERROR);
 begin_login();login_timeout=false;poll_login(nullptr);assert(client.LoggedIn()&&status.qr_url[0]==0&&books.size()==1);
 // Simulated reboot restores the completed QR session without another QR request.
 assert(LoadSession(&restored)==SessionLoad::Loaded);client.Logout();session_loaded=false;
 int requests_before=network_calls;assert(restore_login()&&client.LoggedIn());fetch_shelf();
 assert(network_calls==requests_before+1&&status.state==FOLIO_WR_SHELF);

 login("accountA");select_book();
 // Navigation cache is account-bound, rejects damage, and is only a hint.
 std::vector<Book> cached_shelf;requests_before=network_calls;
 assert(shelf_load(&cached_shelf)&&cached_shelf.size()==1&&network_calls==requests_before);
 login("accountB");assert(!shelf_load(&cached_shelf));login("accountA");
 {FILE* f=fopen(SHELF_FILE,"r+b");assert(f);assert(fseek(f,sizeof(shelf_header_t)+3,SEEK_SET)==0);int c=fgetc(f);assert(c!=EOF);assert(fseek(f,-1,SEEK_CUR)==0);assert(fputc(c^1,f)!=EOF);assert(fclose(f)==0);}
 assert(!shelf_load(&cached_shelf));shelf_save(books);assert(shelf_load(&cached_shelf));
 // Reopening a recently fetched directory skips the network, but expiry does not.
 int toc_before=toc_calls;choose_book(0);assert(toc_calls==toc_before);
 // Back restores the selected shelf page without HTTP, even while offline;
 // selecting the same book again retains the current TOC cache.
 auto single_shelf=books;books.resize(FOLIO_WR_PAGE_SIZE+2,books.front());selected=FOLIO_WR_PAGE_SIZE+1;
 requests_before=network_calls;station=false;execute({FOLIO_WR_RETURN_SHELF,0,{}});
 assert(status.state==FOLIO_WR_SHELF&&status.page==1&&status.item_count==2&&!status.busy&&network_calls==requests_before);
 station=true;execute({FOLIO_WR_SELECT_BOOK,selected,{}});
 assert(status.state==FOLIO_WR_BOOK&&toc_calls==toc_before&&network_calls==requests_before);
 books=std::move(single_shelf);selected=0;
 tick+=301000000;choose_book(0);assert(toc_calls==toc_before+1);
 // Retrying a failed directory resumes that read, never a shelf or a write.
 chapters_fetched=0;toc_failures=2;choose_book(0);assert(status.state==FOLIO_WR_ERROR&&!status.login_required);
 int retry_shelf=shelf_calls,retry_writes=read_posts;execute({FOLIO_WR_RETRY,0,{}});
 assert(status.state==FOLIO_WR_BOOK&&shelf_calls==retry_shelf&&read_posts==retry_writes);
 // A transient shelf failure is retried once without logout. Persistent failure
 // retains the previous shelf and does not retry indefinitely or write progress.
 int shelf_before=shelf_calls;shelf_failures=1;fetch_shelf();
 assert(shelf_calls==shelf_before+2&&status.state==FOLIO_WR_SHELF&&client.LoggedIn());
 shelf_before=shelf_calls;shelf_failures=3;int writes_before=read_posts;
 fetch_shelf();assert(shelf_calls==shelf_before+2&&status.state==FOLIO_WR_SHELF&&client.LoggedIn()&&books.size()==1&&read_posts==writes_before);
 shelf_failures=0;select_book();
 // A selected-chapter download publishes only that chapter.
 reader_failures=99;client.Disconnect();download(0,false);assert(status.state==FOLIO_WR_DOWNLOAD_ERROR);
 execute({FOLIO_WR_RETURN_TOC,0,{}});assert(status.state==FOLIO_WR_BOOK);
 reader_failures=99;download(0,false);assert(status.state==FOLIO_WR_DOWNLOAD_ERROR);
 reader_failures=0;execute({FOLIO_WR_RETRY_DOWNLOAD,0,{}});assert(status.state==FOLIO_WR_BOOK&&status.can_read);
 int first_before=shard_calls[0],middle_before=shard_calls[1],last_before=shard_calls[2];shard_failures=1;
 download(0,false);assert(status.can_read&&shard_calls[0]==first_before+1&&shard_calls[1]==middle_before+2&&shard_calls[2]==last_before+1);
 const std::string path=book_path;assert(path.find("[wr-626f6f6b31].epub")!=std::string::npos);
 assert(status.completed==1 && status.total==1);
 const char* presence_ids[]={"c0","c1","missing"};bool hints[3]={};
 assert(folio_weread_cache_presence(book_path,presence_ids,3,hints)&&hints[0]&&!hints[1]&&!hints[2]);
 int no_cover=cover_requests;choose_book(0);book_page(0);assert(cover_requests==no_cover && status.can_read);
 assert(status.items[0].cached&&!status.items[1].cached);
 assert(folio_weread_cache_chapter_available(book_path,chapters[0].uid.c_str()));
 assert(!folio_weread_cache_chapter_available(book_path,chapters[1].uid.c_str()));
 assert(!folio_weread_cache_chapter_available(book_path,chapters[2].uid.c_str()));
 forget_record();folio_weread_record(path.c_str(),0,0,0);assert(folio_weread_flush());
 enable_time(true);forget_record();folio_weread_record(path.c_str(),3,1,20);assert(folio_weread_flush());
 auto t=ledger();assert(t.pending==20&&t.enabled);uint8_t owner_a[32];memcpy(owner_a,t.owner,32);
 // Cover is independently fetched, tolerates EAGAIN and reuses the saved image.
 cover_test=true;cover_data=load("tools/tests/fixtures/ui-cover-light.png");cover_header_waits=2;cover_body_waits=2;
 Book cover_book=books[selected];cover_book.cover="https://img.qpic.cn/cover.png";
 int cover_before=cover_requests;bool changed=false;assert(fetch_cover(cover_book,&changed)&&changed&&cover_requests==cover_before+1);
 assert(fetch_cover(cover_book,&changed)&&!changed&&cover_requests==cover_before+1);
 char hidden_cover[256];assert(folio_wr_aux(path.c_str(),"cover.png",hidden_cover,sizeof(hidden_cover)));std::string cover_path=hidden_cover;assert(load(cover_path)==cover_data);
 char exported[256],export_error[192];assert(folio_weread_cache_export(path.c_str(),exported,sizeof(exported),export_error,sizeof(export_error)));
 {std::ifstream in(exported,std::ios::binary);std::ofstream out(directory+"/with-cover.epub",std::ios::binary);out<<in.rdbuf();}
 folio_epub_t epub{};if(!folio_epub_prepare(exported,&epub,export_error,sizeof(export_error))){fprintf(stderr,"EPUB prepare: %s\n",export_error);assert(false);}assert(epub.chapter_count>=1);folio_epub_free(&epub);
 assert(unlink(cover_path.c_str())==0);
 cover_book.cover="https://evil.example/cover.png";assert(!fetch_cover(cover_book)&&cover_requests==cover_before+1);cover_test=false;
 // Logout and a different account preserve local position, but cannot add A's seconds.
 execute({FOLIO_WR_LOGOUT,0,{}});forget_record();folio_weread_record(path.c_str(),6,2,30);assert(folio_weread_flush());
 copy(book_path,path.c_str());t=ledger();assert(t.pending==20&&t.position==6);
 login("accountB");select_book();forget_record();folio_weread_record(path.c_str(),9,3,40);assert(folio_weread_flush());
 t=ledger();assert(t.pending==20&&t.position==9&&!memcmp(t.owner,owner_a,32));
 // Partial cache allows verified cloud->local, but cannot upload position or time.
 login("accountA");select_book();forget_record();folio_weread_record(path.c_str(),0,0,0);assert(folio_weread_flush());
 folio_weread_position_t anchor{};char error[192];assert(folio_weread_cache_local_to_cloud(book_path,0,&anchor,error,sizeof(error)));
 uid=anchor.chapter_uid;offset=anchor.offset_utf16;updated=10;
 check_sync();assert(!status.sync_local&&status.sync_cloud);
 int sent=read_posts;upload(false);upload(true);assert(read_posts==sent);
 // Fetch the remaining chapter and reselect via the encoded cache-path API.
 download(0,true);select_book();assert(status.can_read&&complete_cache());
 forget_record();folio_weread_record(path.c_str(),0,0,40);assert(folio_weread_flush());t=ledger();assert(t.pending==60);
 check_sync();assert(status.sync_local&&status.sync_cloud);
 // Manual direction: viewing sync sends no writes; stale cloud state blocks either direction.
 int manual_posts=read_posts;++updated;upload(false);
 assert(read_posts==manual_posts&&status.state==FOLIO_WR_ERROR);
 check_sync();++updated;use_cloud();assert(!open_path[0]&&status.state==FOLIO_WR_ERROR);
 check_sync();assert(read_posts==manual_posts);upload(false);
 assert(read_posts==manual_posts&&status.state!=FOLIO_WR_ERROR); // Already equal: no redundant write.
 offset+=3;++updated;check_sync();upload(false);
 assert(read_posts==manual_posts+2&&status.state!=FOLIO_WR_ERROR);
 check_sync();use_cloud();assert(open_path[0]);open_path[0]=0;
 check_sync();
 // Server ACK succeeds, then the *real* ledger's acknowledgement rename fails.
 fail_ack_save=true;int time_before=time_posts;upload(true);assert(time_posts==time_before+1&&status.state==FOLIO_WR_ERROR);
 // Simulate reboot of service RAM. Persisted intent must reload as uncertain.
 record_loaded=record_dirty=record_error=false;record_path[0]=0;record={};client.Logout();login("accountA");select_book();
 t=ledger();assert(t.pending==0&&t.inflight==0&&t.uncertain==60&&t.confirmed==0);
 check_sync();assert(status.sync_local);time_before=time_posts;upload(true);assert(time_posts==time_before);
 // Preview uses only the progress GET: source downloads are deferred until a direction is chosen.
 int shards_before=shard_calls[0]+shard_calls[1]+shard_calls[2];check_sync();
 assert(shards_before==shard_calls[0]+shard_calls[1]+shard_calls[2]);
 assert(status.cloud_time_available && status.cloud_total_seconds==cloud_seconds);
 t=ledger();uint64_t pending_before=t.pending,confirmed_before=t.confirmed,uncertain_before=t.uncertain;
 use_cloud();open_path[0]=0;t=ledger();
 assert(t.cloud_valid && t.cloud_seconds==cloud_seconds && t.pending==pending_before && t.confirmed==confirmed_before && t.uncertain==uncertain_before);
 // Expiry has a finite error state and cannot keep writing or spinning until an explicit relogin.
 expire_progress=true;check_sync();assert(status.state==FOLIO_WR_ERROR && status.login_required && !status.busy);
 requests_before=network_calls;execute({FOLIO_WR_UPLOAD_SYNC,0,{}});assert(network_calls==requests_before);
 copy(requested_sync_path,path.c_str());execute({FOLIO_WR_RELOGIN,0,{}});
 assert(status.state==FOLIO_WR_LOGIN && !status.login_required && books.empty());
 expire_progress=false;int before_direct_shelf=shelf_calls;poll_login(nullptr);
 assert(status.state==FOLIO_WR_SYNC && shelf_calls==before_direct_shelf && !strcmp(requested_sync_path,path.c_str()));
 // A single manual upload drains acknowledged <=300-second chunks without a duplicate position write.
 forget_record();folio_weread_record(path.c_str(),0,0,601);assert(folio_weread_flush());
 check_sync();time_before=time_posts;upload_sync();t=ledger();
 assert(time_posts==time_before+3 && t.pending==0 && t.confirmed==confirmed_before+601 && status.state==FOLIO_WR_SYNC);
 // A possibly accepted write is quarantined exactly once; later chunks are not sent.
 forget_record();folio_weread_record(path.c_str(),0,0,601);assert(folio_weread_flush());check_sync();
 time_before=time_posts;unknown_time=true;upload_sync();t=ledger();
 assert(time_posts==time_before+1 && t.pending==301 && t.uncertain==uncertain_before+300);
 int retry_posts=read_posts;execute({FOLIO_WR_RETRY,0,{}});assert(status.state==FOLIO_WR_SYNC&&read_posts==retry_posts);
 requested_sync_path[0]=0;
 // Confirmed cloud handoff is removed by logout before it can be consumed.
 check_sync();use_cloud();assert(open_path[0]);execute({FOLIO_WR_LOGOUT,0,{}});
 char opened[256];uint32_t position;assert(!folio_weread_take_open(opened,sizeof(opened),&position));
 // A new session also discards any previous handoff; fake xTaskCreate never runs a worker.
 login("accountA");select_book();check_sync();use_cloud();assert(open_path[0]);
 assert(folio_weread_start(error,sizeof(error))&&tasks_created==1);assert(!folio_weread_take_open(opened,sizeof(opened),&position));
 // Relogin is accepted even while a network operation is busy, without touching Client on this thread.
 assert(folio_weread_command(FOLIO_WR_RELOGIN,0,nullptr) && relogin_requested.load());
 relogin_requested=false;worker_alive=false;assert(folio_weread_stop());
 assert(!folio_weread_start_sync("/not-a-book.epub",error,sizeof(error)));
 assert(folio_weread_start_sync(path.c_str(),error,sizeof(error)) && tasks_created==2 && !strcmp(requested_sync_path,path.c_str()));
 worker_alive=false;assert(folio_weread_stop());
 std::printf("WeRead service integration passed: actual client/cache/storage/time; account isolation, partial-cache gate, lost ACK persistence, and stale handoff lifecycle (%d mock requests)\n",network_calls);
}
