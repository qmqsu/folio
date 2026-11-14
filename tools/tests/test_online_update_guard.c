#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include "http_parser.h"
#include "folio_online_guard.h"
#include "freertos/task.h"
#include "lwip/sys.h"

// Narrow adapter for the EXACT SDK functions included below. Socket and parser
// I/O are real; no artificial clock, successful read, or external time jump.
#define ESP_FAIL -1
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define ESP_LOG_WARN 1
#define ESP_LOG_DEBUG 2
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(tag, ...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOG_LEVEL(level, ...) ((void)(level))
#define HTTP_EVENT_ERROR 1
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN -2
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT 0
#define HTTP_STATE_REQ_COMPLETE_HEADER 1
#define HTTP_STATE_REQ_COMPLETE_DATA 2
#define HTTP_STATE_RES_COMPLETE_HEADER 3
#define HTTP_STATE_RES_ON_DATA_START 4
typedef int esp_err_t;
typedef int esp_log_level_t;
typedef struct {
    int raw_len, len; char* raw_data; char* output_ptr; char data[4096];
} esp_http_buffer_t;
typedef struct {
    esp_http_buffer_t* buffer; bool is_chunked; int status_code;
    int64_t content_length, data_process;
} test_response_t;
typedef struct {
    test_response_t* response;
    int transport, timeout_ms, buffer_size_rx, state;
    bool is_chunk_complete;
    http_parser* parser;
    http_parser_settings* parser_settings;
} test_client_t;
typedef test_client_t* esp_http_client_handle_t;
static const char* esp_err_to_name(int error) { (void)error; return "test-error"; }
static void esp_http_client_cached_buf_cleanup(esp_http_buffer_t* c) { (void)c; }
static void* esp_transport_get_error_handle(int fd) { (void)fd; return NULL; }
static void http_dispatch_event(test_client_t* c, int event, void* data, int len) { (void)c;(void)event;(void)data;(void)len; }
static void http_dispatch_event_to_event_loop(int event, void* data, size_t len) { (void)event;(void)data;(void)len; }
static int esp_transport_translate_error(int result) { return result; }
static _Atomic unsigned transport_reads, shutdown_calls;
static _Atomic bool fail_shutdown_once;
int online_guard_test_shutdown(int fd,int how) {
    ++shutdown_calls;
    if(atomic_exchange(&fail_shutdown_once,false)){errno=ENOMEM;return -1;}
    return shutdown(fd,how);
}
static int esp_transport_read(int fd, char* bytes, int count, int timeout_ms) {
    fd_set readset; FD_ZERO(&readset); FD_SET(fd,&readset);
    struct timeval timeout={timeout_ms/1000,(timeout_ms%1000)*1000};
    int result=select(fd+1,&readset,NULL,NULL,&timeout);
    if(result<=0)return result;
    ++transport_reads;
    ssize_t n=recv(fd,bytes,(size_t)count,0);
    return n==0?ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN:(int)n;
}
#include "online_update_sdk_http.inc"

static pthread_t guard_thread;
static TaskFunction_t guard_function;
static void* guard_context;
static bool guard_live, fail_reservation;
static _Atomic unsigned reserved_resources;
static _Thread_local sys_sem_t reserved_syssem;
sys_sem_t* sys_thread_sem_get(void) { if(fail_reservation)return NULL;assert(!reserved_syssem);reserved_syssem=1;++reserved_resources;return &reserved_syssem; }
void sys_thread_sem_deinit(void) { assert(reserved_syssem && reserved_resources);reserved_syssem=0;--reserved_resources; }
static _Atomic bool cancel_flag, producer_stop, body_release;
static _Atomic unsigned sent_bytes;
static folio_online_guard_t guard;
static void* guard_run(void* ignored) { (void)ignored;guard_function(guard_context);return NULL; }
int xTaskCreate(TaskFunction_t function,const char* name,unsigned stack,void* context,unsigned priority,void* handle) {
    assert(!strcmp(name,"folio_http_guard") && stack>=3072 && priority && !handle && !guard_live);
    guard_function=function;guard_context=context;guard_live=true;
    assert(pthread_create(&guard_thread,NULL,guard_run,NULL)==0);return pdPASS;
}
void vTaskDelete(void* task) { assert(!task); }
void vTaskDelay(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000};while(nanosleep(&t,&t)<0)assert(errno==EINTR); }
int64_t esp_timer_get_time(void) { struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static bool should_cancel(void* context) { assert(!context);return atomic_load(&cancel_flag); }
static void join_guard(void) { folio_online_guard_stop(&guard);assert(guard_live && pthread_join(guard_thread,NULL)==0);guard_live=false;assert(!reserved_resources); }
static void tcp_pair(int pair[2]) {
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(0x7f000001u),.sin_port=0};
    assert(bind(listener,(struct sockaddr*)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t length=sizeof(address);assert(getsockname(listener,(struct sockaddr*)&address,&length)==0);
    pair[0]=socket(AF_INET,SOCK_STREAM,0);assert(pair[0]>=0 && connect(pair[0],(struct sockaddr*)&address,sizeof(address))==0);
    pair[1]=accept(listener,NULL,NULL);assert(pair[1]>=0 && close(listener)==0);
}
static int parsed_headers(http_parser* parser) {
    test_client_t* client=parser->data;client->state=HTTP_STATE_RES_COMPLETE_HEADER;
    client->response->content_length=parser->content_length==UINT64_MAX?0:(int64_t)parser->content_length;
    client->response->is_chunked=(parser->flags&F_CHUNKED)!=0;return 0;
}
static int parsed_body(http_parser* parser,const char* data,size_t count) {
    test_client_t* c=parser->data;assert(c->response->buffer->output_ptr);
    memcpy(c->response->buffer->output_ptr+c->response->buffer->raw_len,data,count);
    c->response->buffer->raw_len+=(int)count;c->response->data_process+=(int64_t)count;return 0;
}
static int parsed_complete(http_parser* parser) { test_client_t* c=parser->data;c->is_chunk_complete=true;return 0; }
typedef struct { int fd, mode; } producer_t;
static bool send_all(int fd,const char* bytes,size_t count) {
    while(count){ssize_t n=send(fd,bytes,count,0);if(n<=0)return false;bytes+=n;count-=(size_t)n;atomic_fetch_add(&sent_bytes,(unsigned)n);}return true;
}
static void* producer(void* context) {
    producer_t* p=context;
    const char* header=p->mode==0?"HTTP/1.1 200 OK\r\nX-Slow: ":
        p->mode==1?"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n":
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    if(!send_all(p->fd,header,strlen(header)))return NULL;
    while(p->mode && !atomic_load(&body_release) && !atomic_load(&producer_stop))vTaskDelay(1);
    while(!atomic_load(&producer_stop)){
        const char* chunk=p->mode==2?"1\r\na\r\n":"a";
        if(!send_all(p->fd,chunk,strlen(chunk)))break;
        vTaskDelay(25); // A byte/chunk every 25ms, FAR below the 5000ms I/O timeout.
    }
    return NULL;
}
static void* cancel_later(void* ignored) { (void)ignored;vTaskDelay(300);atomic_store(&cancel_flag,true);return NULL; }
static void slow_drip(int mode,bool cancellation) {
    int sockets[2];tcp_pair(sockets);cancel_flag=producer_stop=body_release=false;sent_bytes=transport_reads=shutdown_calls=0;fail_shutdown_once=(mode==0 && !cancellation);
    esp_http_buffer_t buffer={0};test_response_t response={.buffer=&buffer,.content_length=-1};http_parser parser;
    http_parser_init(&parser,HTTP_RESPONSE);
    http_parser_settings callbacks={.on_headers_complete=parsed_headers,.on_body=parsed_body,.on_message_complete=parsed_complete};
    test_client_t client={.response=&response,.transport=sockets[0],.timeout_ms=5000,.buffer_size_rx=4096,
        .state=HTTP_STATE_REQ_COMPLETE_HEADER,.parser=&parser,.parser_settings=&callbacks};parser.data=&client;
    producer_t config={sockets[1],mode};pthread_t sender,canceller;
    assert(pthread_create(&sender,NULL,producer,&config)==0);
    if(mode){assert(esp_http_client_fetch_headers(&client)>=0);}
    int64_t start=esp_timer_get_time();
    assert(folio_online_guard_begin(&guard,start+(cancellation?10000000:300000),should_cancel,NULL));
    assert(folio_online_guard_bind(&guard,sockets[0]));
    if(cancellation)assert(pthread_create(&canceller,NULL,cancel_later,NULL)==0);
    if(mode)body_release=true;
    int64_t result;
    if(mode){char out[4096];result=esp_http_client_read(&client,out,sizeof(out));}
    else result=esp_http_client_fetch_headers(&client);
    int64_t elapsed=esp_timer_get_time()-start;
    assert(folio_online_guard_interrupted(&guard));
    if(mode==0 && !cancellation)assert(shutdown_calls==2);
    assert(!esp_http_client_is_complete_data_received(&client));
    assert(elapsed>=250000 && elapsed<2000000 && transport_reads>=5);
    // Match SDK DISCONNECTED-before-close order, then final worker stop/cleanup.
    folio_online_guard_detach(&guard);join_guard();
    assert(close(sockets[0])==0);producer_stop=true;
    if(cancellation)assert(pthread_join(canceller,NULL)==0);
    assert(pthread_join(sender,NULL)==0 && close(sockets[1])==0);
    printf("SDK %s slow-drip %s: %.1f ms, %u real socket reads, result=%"PRId64"; incomplete rejected\n",
        mode==0?"headers":mode==1?"body":"chunked-body",cancellation?"cancel":"deadline",elapsed/1000.0,(unsigned)transport_reads,result);
}
static void startup_allocation_failure(void) {
    fail_reservation=true;
    assert(!folio_online_guard_begin(&guard,esp_timer_get_time()+10000000,should_cancel,NULL));
    assert(!guard.running && !reserved_resources && guard_live);
    assert(pthread_join(guard_thread,NULL)==0);guard_live=false;fail_reservation=false;
    puts("Guard startup: syssem ENOMEM rejects begin before network allocation; later attempts recover");
}
static void late_bind_retry(void) {
    int sockets[2];tcp_pair(sockets);cancel_flag=true;shutdown_calls=0;fail_shutdown_once=false;
    assert(folio_online_guard_begin(&guard,esp_timer_get_time()+10000000,should_cancel,NULL));
    for(unsigned i=0;i<100 && !folio_online_guard_interrupted(&guard);++i)vTaskDelay(10);
    assert(folio_online_guard_interrupted(&guard));fail_shutdown_once=true;
    assert(!folio_online_guard_bind(&guard,sockets[0]));
    struct timeval timeout={1,0};assert(setsockopt(sockets[0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    char byte;assert(recv(sockets[0],&byte,1,0)==0 && shutdown_calls==2);
    join_guard();assert(close(sockets[0])==0 && close(sockets[1])==0);
    puts("Shutdown ENOMEM: task-tick and already-cancelled bind both retry until actual socket interruption");
}
static void descriptor_reuse(void) {
    for(unsigned i=0;i<12;++i){
        int old[2];tcp_pair(old);cancel_flag=false;
        assert(folio_online_guard_begin(&guard,esp_timer_get_time()+10000000,should_cancel,NULL));
        assert(folio_online_guard_bind(&guard,old[0]));
        int retired=old[0];folio_online_guard_detach(&guard);assert(close(old[0])==0 && close(old[1])==0);
        int fresh[2];tcp_pair(fresh);
        if(fresh[0]!=retired){assert(fresh[1]!=retired && dup2(fresh[0],retired)==retired && close(fresh[0])==0);fresh[0]=retired;}
        cancel_flag=true;vTaskDelay(70);assert(folio_online_guard_interrupted(&guard));
        assert(send(fresh[1],"x",1,0)==1);char x;assert(recv(fresh[0],&x,1,0)==1 && x=='x');
        join_guard();assert(close(fresh[0])==0 && close(fresh[1])==0);
    }
    puts("Socket lifecycle: 12 ordered detach/close/fd-reuse cases retain unrelated new sockets; guard stopped before cleanup");
}
int main(void) {
    signal(SIGPIPE,SIG_IGN);
    startup_allocation_failure();
    for(int mode=0;mode<3;++mode){slow_drip(mode,false);slow_drip(mode,true);}
    late_bind_retry();descriptor_reuse();
    puts("Guard tests passed: actual SDK loops + actual http_parser + localhost TCP + production guard; not ESP TLS/device timing proof");
    return 0;
}
