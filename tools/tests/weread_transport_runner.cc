#include "folio_weread_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
using namespace folio::weread;
struct fake_client { esp_http_client_config_t config; size_t cursor=0; };
static std::string payload, sent_cookie;
static int status=200, init_count=0, close_count=0, clean_count=0, read_count=0, opens=0;
static int64_t known_length=-1, ticks=0, read_delay=0;
static bool complete=true, fail_open=false, fail_write=false, fail_headers=false, fail_read=false, fail_open_once=false;
static int header_waits,header_calls,body_waits,body_wait_after,tls_error,tls_result,tls_flags;
static int64_t open_delay;
static std::vector<std::string> cookies;
int64_t esp_timer_get_time(){ return ticks; }
int esp_crt_bundle_attach(void*) {return 0;}
static int expected_connect_timeout=10000;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* c) {
 assert(c->transport_type==HTTP_TRANSPORT_OVER_SSL && c->disable_auto_redirect);
 assert(!c->skip_cert_common_name_check&&std::string(c->common_name)=="weread.qq.com");
 assert(c->crt_bundle_attach==esp_crt_bundle_attach&&c->timeout_ms>0&&c->timeout_ms<=expected_connect_timeout);
 ++init_count; return new fake_client{*c,0};
}
int esp_http_client_close(esp_http_client_handle_t){++close_count;return 0;}
int esp_http_client_cleanup(esp_http_client_handle_t h){++clean_count;delete h;return 0;}
int esp_http_client_set_method(esp_http_client_handle_t,int){return 0;}
int esp_http_client_set_url(esp_http_client_handle_t,const char*){return 0;}
int esp_http_client_delete_header(esp_http_client_handle_t,const char* k){if(std::string(k)=="Cookie")sent_cookie.clear();return 0;}
int esp_http_client_set_header(esp_http_client_handle_t,const char* k,const char* v){if(std::string(k)=="Cookie")sent_cookie=v;return 0;}
int esp_http_client_open(esp_http_client_handle_t h,int){++opens;h->cursor=0;ticks+=std::min<int64_t>(open_delay,h->config.timeout_ms*1000LL);if(fail_open_once){fail_open_once=false;return -1;}return fail_open || open_delay>h->config.timeout_ms*1000LL?-1:0;}
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t,int* code,int* flags){*code=tls_error;*flags=tls_flags;return tls_result;}
int esp_http_client_set_timeout_ms(esp_http_client_handle_t h,int ms){assert(ms>0&&ms<=20000);h->config.timeout_ms=ms;return 0;}
int esp_http_client_write(esp_http_client_handle_t,const char*,int n){return fail_write?-1:n;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h){
 assert(h->config.timeout_ms>0&&h->config.timeout_ms<=3000);++header_calls;if(fail_headers)return -1;
 if(header_waits-- >0){ticks+=h->config.timeout_ms*1000LL;return -ESP_ERR_HTTP_EAGAIN;}
 for(const auto& cookie:cookies){esp_http_client_event_t event{HTTP_EVENT_ON_HEADER,h->config.user_data,"Set-Cookie",cookie.c_str()}; if(h->config.event_handler(&event)!=0)return -1;}
 return known_length>=0?known_length:0;
}
int esp_http_client_get_status_code(esp_http_client_handle_t){return status;}
int esp_http_client_read(esp_http_client_handle_t h,char* p,int n){
 ++read_count;ticks+=read_delay;if(fail_read)return -1;
 if((int)h->cursor>=body_wait_after && body_waits>0){--body_waits;ticks+=3000000;return -ESP_ERR_HTTP_EAGAIN;}
 const size_t take=std::min<size_t>({static_cast<size_t>(n),payload.size()-h->cursor,113});
 std::memcpy(p,payload.data()+h->cursor,take);h->cursor+=take;return static_cast<int>(take);
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h){return complete&&h->cursor==payload.size();}
static void reset(){EspTransportDisconnect();fail_write=fail_headers=fail_read=fail_open_once=false;body_waits=body_wait_after=tls_error=tls_result=tls_flags=0;open_delay=0;header_waits=header_calls=0;payload="{\"ok\":true}";sent_cookie.clear();status=200;init_count=close_count=clean_count=read_count=opens=0;known_length=-1;ticks=read_delay=0;complete=true;fail_open=false;cookies.clear();}
int main(){
 HttpRequest q;q.url="https://weread.qq.com/web/shelf/sync";q.method="GET";q.cookie="wr_vid=123; wr_skey=FIXTURE";
 reset();{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&r.body==payload&&r.sent&&clean_count==0&&close_count==0&&sent_cookie==q.cookie);}
 for(const char* bad:{"http://weread.qq.com/x","https://weread.qq.com.evil/x","https://weread.qq.com@evil/x","https://evil/weread.qq.com/x"}){
  reset();std::string saved=q.url;q.url=bad;HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::InvalidArgument&&init_count==0&&!r.sent);q.url=saved;
 }
 reset();open_delay=5000000;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&opens==1&&clean_count==0);}
 reset();header_waits=2;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None && header_calls==3 && opens==1 && clean_count==0);}
 reset();header_waits=20;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Timeout && header_calls==5 && opens==1 && clean_count==1);}
 reset();header_waits=20;{HttpResponse r;EspTransport(q,&r,{[](void*){return header_calls>=2;},nullptr},nullptr);assert(r.failure==ErrorCode::Cancelled && opens==1 && clean_count==1);}
 // Waiting during the body preserves partial bytes and never resends a POST.
 reset();payload=std::string(400,'x');body_wait_after=113;body_waits=2;q.method="POST";q.body="{}";
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&r.body==payload&&opens==1&&clean_count==0);}
 reset();body_waits=20;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Timeout&&opens==1&&clean_count==1);}
 reset();body_waits=20;{HttpResponse r;EspTransport(q,&r,{[](void*){return read_count>=2;},nullptr},nullptr);assert(r.failure==ErrorCode::Cancelled&&opens==1&&clean_count==1);}
 reset();body_waits=12;q.timeout_ms=60000;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&r.body==payload&&opens==1);}
 q.method="GET";q.body.clear();q.timeout_ms=15000;
 reset();fail_open=true;tls_error=-0x008d;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::NoMemory&&clean_count==1);}
 reset();status=302;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.status==302&&init_count==1&&opens==1&&read_count==0&&clean_count==1);}
 reset();known_length=1024;q.max_response=100;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Limit&&read_count==0&&clean_count==1);}
 reset();payload=std::string(101,'x');{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Limit&&clean_count==1);}
 q.max_response=kMaxChapterBytes;
 reset();complete=false;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Transport&&r.sent&&clean_count==1&&r.phase==RequestPhase::Body&&r.received_bytes==payload.size());}
 reset();cookies.assign(17,"wr_skey=TEST");{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Limit&&clean_count==1);}
 reset();cookies={std::string(2049,'x')};{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Limit&&clean_count==1);}
 reset();{HttpResponse r;EspTransport(q,&r,{[](void*){return true;},nullptr},nullptr);assert(r.failure==ErrorCode::Cancelled&&!r.sent&&init_count==0);}
 reset();read_delay=16000000;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Timeout&&clean_count==1);}
 reset();fail_open=true;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Transport&&r.sent&&clean_count==1&&r.phase==RequestPhase::Connect&&r.received_bytes==0);}
 reset();fail_open=true;tls_result=0x8001;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::NameResolution);}
 reset();fail_open=true;tls_result=0x8006;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Connection);}
 reset();fail_open=true;tls_flags=8;{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Certificate);}
 // A fully consumed response reuses its handle, with missing cookies removed.
 reset();{HttpResponse first;EspTransport(q,&first,{},nullptr);assert(first.failure==ErrorCode::None);}
 q.cookie.clear();{HttpResponse next;EspTransport(q,&next,{},nullptr);assert(next.failure==ErrorCode::None&&init_count==1&&opens==2&&sent_cookie.empty());}
 // A stale GET reconnects once, while a possibly applied reading-time POST never does.
 fail_open_once=true;{HttpResponse next;EspTransport(q,&next,{},nullptr);assert(next.failure==ErrorCode::None&&init_count==2&&opens==4);}
 // Reconnection shares the original 15 s budget, including bounded final I/O.
 reset();{HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None);}
 open_delay=9000000;fail_open_once=true;
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Timeout&&ticks==15000000&&opens==3&&clean_count==2);}
 reset();open_delay=9500000;header_waits=3;
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Timeout&&ticks==15000000&&header_calls==2);}
 for(int phase=0;phase<5;++phase){
  reset();{HttpResponse first;EspTransport(q,&first,{},nullptr);assert(first.failure==ErrorCode::None);}
  q.method="POST";q.url="https://weread.qq.com/web/book/read";q.body="{\"rt\":60}";
  fail_open=phase==0;fail_write=phase==1;fail_headers=phase==2;fail_read=phase==3;complete=phase!=4;
  {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure!=ErrorCode::None&&r.sent&&opens==2&&init_count==1&&clean_count==1);}
  q.method="GET";q.url="https://weread.qq.com/web/shelf/sync";q.body.clear();
 }
 reset();q.reader_tokens=true;q.timeout_ms=60000;expected_connect_timeout=20000;open_delay=15000000;payload=std::string(20000,'x')+"\"psvts\":\"p_test\","+std::string(9000,'x')+"\"pclts\":\"pc_test\",\"token\":\"t_test\"";
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&r.body.size()<128&&r.body.find("p_test")!=std::string::npos&&r.body.find("t_test")!=std::string::npos&&clean_count==0);}
 // Token metadata may finish before HTML, but its partially read connection
 // must be closed. A chapter remains subject to full-body verification.
 reset();payload="<script>{\"psvts\":\"p_test\",\"pclts\":\"pc_test\",\"token\":\"t_test\"}</script>"+std::string(200000,'x');
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&r.phase==RequestPhase::Complete&&r.received_bytes<512&&clean_count==1&&r.body.find("t_test")!=std::string::npos);}
 q.reader_tokens=false;q.timeout_ms=15000;expected_connect_timeout=10000;payload="ok";
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::None&&init_count==2&&clean_count==1&&r.body=="ok");}
 // Missing optional fields cannot excuse a truncated response; a 401 carrying
 // token-shaped data is not a successful metadata probe either.
 reset();q.reader_tokens=true;q.timeout_ms=60000;expected_connect_timeout=20000;
 payload="\"psvts\":\"p_test\"";complete=false;
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.failure==ErrorCode::Transport&&clean_count==1);}
 reset();status=401;payload="\"psvts\":\"p_test\",\"pclts\":\"pc_test\",\"token\":\"t_test\""+std::string(1000,'x');
 {HttpResponse r;EspTransport(q,&r,{},nullptr);assert(r.status==401&&r.received_bytes==payload.size());}
 EspTransportDisconnect();
 std::puts("WeRead ESP transport: verified TLS config, exact origin, redirects, bounded streams, cancellation, timeout and cleanup tests passed");
}
