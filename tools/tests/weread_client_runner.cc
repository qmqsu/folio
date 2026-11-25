#include "folio_weread_client.h"
#include "folio_weread_crypto.h"
#include "vendor/cjson/cJSON.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <algorithm>
using namespace folio::weread;
static uint64_t mock_milliseconds;
namespace folio::weread { uint64_t TestMonotonicMilliseconds(); }
uint64_t folio::weread::TestMonotonicMilliseconds(){return mock_milliseconds;}
struct Step { const char* path; std::string body; int status = 200; ErrorCode failure = ErrorCode::None; bool sent = true; std::vector<std::string> cookies; uint32_t delay_ms = 0; };
struct Mock {
    std::vector<Step> steps;
    size_t index = 0;
    std::vector<std::string> posts;
    std::vector<std::string> cookies;
    std::vector<uint32_t> timeouts;
    bool throw_oom = false;
    static void run(const HttpRequest& q, HttpResponse* r, CancelCheck, void* ctx) {
        auto& m = *static_cast<Mock*>(ctx);
        if (m.throw_oom) throw std::bad_alloc();
        assert(q.url.rfind("https://weread.qq.com/", 0) == 0);
        assert(m.index < m.steps.size());
        const auto& s = m.steps[m.index++];
        assert(q.url.find(s.path) != std::string::npos);
        m.timeouts.push_back(q.timeout_ms);
        mock_milliseconds+=std::min(q.timeout_ms,s.delay_ms);
        m.cookies.push_back(q.cookie);
        if (q.method == "POST") m.posts.push_back(q.body);
        r->status = s.status; r->failure = s.failure; r->sent = s.sent;
        r->set_cookies = s.cookies;
        if (!q.consume) r->body = s.body;
        else if (s.body.size() > q.max_response) r->failure = ErrorCode::Limit;
        else if (s.status == 200) {
            for (size_t i = 0; i < s.body.size() && r->failure == ErrorCode::None; i += 17)
                r->failure = q.consume(q.consume_context, s.body.data()+i, std::min<size_t>(17, s.body.size()-i));
        }
        if(s.delay_ms>q.timeout_ms){r->failure=ErrorCode::Timeout;r->body.clear();}
    }
    void reset(std::vector<Step> s) { steps = std::move(s); index = 0; posts.clear(); cookies.clear();timeouts.clear();mock_milliseconds=0; }
};
static bool fetch_shelf(Client& client, std::vector<Book>* out, Error* error) {
    struct Collector : ShelfSink {
        std::vector<Book> rows;
        bool append(const Book& b) override { rows.push_back(b); return true; }
    } sink;
    std::vector<uint16_t> order;
    out->clear();
    if (!client.FetchShelf(sink, &order, error)) return false;
    for (auto i : order) out->push_back(std::move(sink.rows[i]));
    return true;
}
static void login(Client& c, Mock& m, const char* id = "123") {
    c.Logout();
    m.reset({{"getLoginUid", "{\"uid\":\"qr_123\"}"},
             {"getLoginInfo", "{\"succeed\":true}", 200, ErrorCode::None, true, {std::string("wr_vid=") + id + "; Secure", "wr_skey=TEST_COOKIE; Secure", "wr_rt=TEST_REFRESH; Secure"}}});
    QrSession q; QrState state; Error e;
    assert(c.BeginQrLogin(&q, &e)); assert(q.url == "https://weread.qq.com/web/confirm?uid=qr_123");
    assert(c.PollQrLogin(q, "", &state, &e)); assert(state == QrState::Complete && c.LoggedIn());
}
static std::string load(const std::string& p) { std::ifstream f(p, std::ios::binary); assert(f); return {std::istreambuf_iterator<char>(f), {}}; }
static const char* progress0 = "{\"book\":{\"bookId\":\"book1\",\"chapterUid\":\"c1\",\"chapterOffset\":0,\"progress\":0}}";
static const char* progress12 = "{\"book\":{\"bookId\":\"book1\",\"chapterUid\":\"c1\",\"chapterOffset\":12,\"progress\":10}}";
static const char* tokens = "<script>window.x={\"psvts\":\"p_test\",\"pclts\":\"pc_test\",\"token\":\"t_test\"}</script>";
int main(int argc, char** argv) {
    assert(argc == 2); std::string dir = argv[1];
    Mock m; Client c(Mock::run, &m); Error e; QrState state; QrSession qr;
    assert(!c.LoggedIn());
    m.reset({{"getLoginUid", "{\"uid\":\"bad&x=1\"}"}}); assert(!c.BeginQrLogin(&qr, &e));
    login(c, m);
    qr.uid = "qr_123";
    for (const auto& p : std::vector<std::pair<const char*, QrState>>{{"NEED_OTP", QrState::NeedOtp},{"OTP_NOT_MATCH",QrState::OtpMismatch},{"OTP_EXPIRED",QrState::Expired}}) {
        m.reset({{"getLoginInfo", std::string("{\"logicCode\":\"") + p.first + "\"}"}});
        assert(c.PollQrLogin(qr, "1234", &state, &e) && state == p.second);
    }
    assert(!c.LoggedIn()); assert(!c.PollQrLogin(qr,"12&4",&state,&e));
    login(c,m);
    std::vector<Book> shelf;
    m.reset({{"shelf/sync", "{\"books\":[{\"bookId\":\"book1\",\"title\":\"中文书\",\"author\":\"作者\",\"cover\":\"https://img.example/a.jpg\"}],\"bookProgress\":[{\"bookId\":\"book1\",\"chapterUid\":\"c1\",\"chapterOffset\":12,\"updateTime\":123}]}"}});
    assert(fetch_shelf(c,&shelf,&e) && shelf.size()==1 && !shelf[0].cover.empty());
    for(const char* url:{"http://img.qpic.cn/a.jpg","//img.qpic.cn/a.jpg"}){
        m.reset({{"shelf/sync",std::string("{\"books\":[{\"bookId\":\"book1\",\"cover\":\"")+url+"\"}]}"}});
        assert(fetch_shelf(c,&shelf,&e)&&shelf[0].cover=="https://img.qpic.cn/a.jpg");
    }
    m.reset({{"shelf/sync",load(dir+"/shelf128.json")}}); assert(fetch_shelf(c,&shelf,&e)&&shelf.size()==128);
    m.reset({{"shelf/sync",load(dir+"/shelf129.json")}}); assert(fetch_shelf(c,&shelf,&e)&&shelf.size()==129);
    m.reset({{"shelf/sync",load(dir+"/shelf1024.json")}}); assert(fetch_shelf(c,&shelf,&e)&&shelf.size()==1024&&shelf.back().id=="b1023");
    m.reset({{"shelf/sync",load(dir+"/shelf6000.json")}});
    assert(fetch_shelf(c,&shelf,&e) && shelf.size()==6000 && shelf.front().id=="b5999" && shelf.back().id=="b5998");
    m.reset({{"shelf/sync",R"({"books":[],"errcode":-2012})"}});
    assert(!fetch_shelf(c,&shelf,&e) && e.code==ErrorCode::SessionExpired);
    m.reset({{"shelf/sync","{\"books\":[{\"bookId\":\"x\",\"author\":\""+std::string(511,'a')+"中\",\"cover\":\""+std::string(2100,'x')+"\"}]}"}});
    assert(fetch_shelf(c,&shelf,&e) && shelf[0].author.size()==511 && shelf[0].cover.empty());
    std::string capacity="{\"books\":[";
    for(size_t i=0;i<kMaxShelfBooks;++i){
        if(i)capacity+=',';
        capacity+="{\"bookId\":\"b"+std::to_string(i)+"\"}";
    }
    m.reset({{"shelf/sync",capacity+"]}"}});assert(fetch_shelf(c,&shelf,&e) && shelf.size()==kMaxShelfBooks);
    m.reset({{"shelf/sync",capacity+",{\"bookId\":\"extra\"}]}"}});assert(!fetch_shelf(c,&shelf,&e) && e.code==ErrorCode::Limit);
    for (auto invalid : {"{\"books\":[{\"bookId\":\"../x\"}]}","{\"books\":[{\"bookId\":\"x\"},{\"bookId\":\"x\"}]}","{}","[]","{\"books\":[]}junk"}) {
        m.reset({{"shelf/sync",invalid}}); assert(!fetch_shelf(c,&shelf,&e));
    }
    m.reset({{"shelf/sync",std::string(40,'[')+std::string(40,']')}}); assert(!fetch_shelf(c,&shelf,&e));
    m.reset({{"shelf/sync","{\"books\":[{\"bookId\":\"book1\\u0000evil\"}]}"}}); assert(!fetch_shelf(c,&shelf,&e));
    m.reset({{"shelf/sync",std::string(kMaxShelfBytes+1,'x')}}); assert(!fetch_shelf(c,&shelf,&e)&&e.code==ErrorCode::Limit);
    std::vector<Chapter> chapters;
    m.reset({{"chapterInfos",load(dir+"/chapters512.json")}}); assert(c.FetchChapters("book1",&chapters,&e)&&chapters.size()==512);
    m.reset({{"chapterInfos",load(dir+"/chapters513.json")}}); assert(!c.FetchChapters("book1",&chapters,&e)&&e.code==ErrorCode::Limit&&chapters.empty());
    Progress p;
    for (auto body : {progress12,"{\"data\":{\"bookId\":\"book1\",\"chapterUid\":\"c1\",\"chapterOffset\":12}}","{\"bookProgress\":[{\"bookId\":\"other\",\"chapterUid\":\"c2\",\"chapterOffset\":1},{\"bookId\":\"book1\",\"chapterUid\":\"c1\",\"chapterOffset\":12}]}"}) {
        m.reset({{"getProgress",body}}); assert(c.FetchProgress("book1",&p,&e)&&p.has_offset&&p.chapter_offset==12&&!p.has_reading_time);
    }
    for (const auto& sample : std::vector<std::pair<std::string,uint32_t>>{
        {"\"readingTime\":3600",3600}, {"\"readingTime\":0",0},
        {"\"recordReadingTime\":\"75\"",75},
        {"\"readingTime\":28086,\"recordReadingTime\":0",28086},
        {"\"readingTime\":4294967295",UINT32_MAX}}) {
        m.reset({{"getProgress","{\"book\":{\"bookId\":\"book1\",\"chapterUid\":\"c1\",\"chapterOffset\":12,"+sample.first+"}}"}});
        assert(c.FetchProgress("book1",&p,&e)&&p.has_reading_time&&p.reading_seconds==sample.second);
        assert(m.index==1&&m.posts.empty());
    }
    for (const char* invalid : {"-1","1.5","4294967296","\"NaN\"","null","true"}) {
        m.reset({{"getProgress",std::string("{\"chapterUid\":\"c1\",\"chapterOffset\":12,\"readingTime\":")+invalid+"}"}});
        assert(c.FetchProgress("book1",&p,&e)&&!p.has_reading_time&&p.reading_seconds==0);
    }
    for (auto body : {"{\"book\":{\"bookId\":\"other\",\"chapterUid\":\"c1\",\"chapterOffset\":12}}","{\"chapterUid\":\"c1\",\"progress\":80}","{\"chapterUid\":\"c1\",\"chapterOffset\":-1}","{\"chapterUid\":\"c1\",\"chapterOffset\":4294967296}"}) {
        m.reset({{"getProgress",body}}); assert(!c.FetchProgress("book1",&p,&e));
    }
    Chapter ch; ch.uid="c1";ch.index=1;ch.title="章节";
    m.reset({{"getProgress",progress0},{"/web/reader/",tokens},{"/web/book/read","{\"succ\":1}"},{"/web/book/read","{\"succ\":1}"},{"getProgress",progress12}});
    assert(c.WritePosition("book1",ch,12,10,&p,&e)==Submission::Confirmed && m.index==5 && m.posts.size()==2);
    for (const auto& body:m.posts) {
        assert(body.find("\"rt\"")==std::string::npos);
        cJSON* j=cJSON_Parse(body.c_str()); assert(j); std::vector<std::pair<std::string,std::string>> params;
        for(cJSON* f=j->child;f;f=f->next) { if(std::string(f->string)=="s")continue; params.emplace_back(f->string,cJSON_IsString(f)?f->valuestring:std::to_string(static_cast<uint64_t>(f->valuedouble))); }
        assert(crypto::Sign(crypto::SortedQuery(params))==cJSON_GetObjectItem(j,"s")->valuestring); cJSON_Delete(j);
    }
    m.reset({{"getProgress",progress12}}); assert(c.WritePosition("book1",ch,12,10,&p,&e)==Submission::Confirmed&&m.posts.empty());
    c.Disconnect();m.reset({{"getProgress",progress0},{"/web/reader/",tokens},{"/web/book/read","",200,ErrorCode::Timeout,true},{"getProgress",progress12}});
    assert(c.WritePosition("book1",ch,12,10,&p,&e)==Submission::Confirmed&&m.posts.size()==1);
    // Position validation already fetched this book's tokens. A time upload
    // reuses them and performs exactly one POST, without another reader GET.
    m.reset({{"/web/book/read","{\"succ\":1}"}});
    assert(c.SubmitReadingTime("book1",ch,12,10,60,&e)==Submission::Confirmed&&m.posts.size()==1&&m.posts[0].find("\"rt\":60")!=std::string::npos);
    assert(m.index==1);
    for (auto failure : {ErrorCode::Timeout,ErrorCode::Transport,ErrorCode::Cancelled}) {
        c.Disconnect();m.reset({{"/web/reader/",tokens},{"/web/book/read","",200,failure,true}});
        assert(c.SubmitReadingTime("book1",ch,12,10,60,&e)==Submission::Unknown&&m.index==2&&m.posts.size()==1);
    }
    for (auto body : {"", "{}", "broken"}) {
        c.Disconnect();m.reset({{"/web/reader/",tokens},{"/web/book/read",body}}); assert(c.SubmitReadingTime("book1",ch,12,10,60,&e)==Submission::Unknown);
    }
    c.Disconnect();m.reset({{"/web/reader/",tokens},{"/web/book/read","{\"succ\":0}"}}); assert(c.SubmitReadingTime("book1",ch,12,10,60,&e)==Submission::Rejected);
    c.Disconnect();m.reset({{"/web/reader/",tokens},{"/web/book/read","",403}}); assert(c.SubmitReadingTime("book1",ch,12,10,60,&e)==Submission::Rejected);
    m.reset({}); assert(c.SubmitReadingTime("book1",ch,12,10,301,&e)==Submission::Rejected&&m.index==0);
    c.SetCancelCheck({[](void*){return true;},nullptr}); assert(c.SubmitReadingTime("book1",ch,12,10,60,&e)==Submission::Rejected&&m.index==0); c.SetCancelCheck({});
    ChapterContent content;
    for (bool txt : {false,true}) {
        c.Disconnect();
        std::vector<Step> steps={{"/web/reader/",tokens},{"/e_0",txt?"{\"bookId\":\"book1\"}":load(dir+"/x0")}};
        steps.push_back({txt?"/t_0":"/e_1",load(dir+(txt?"/t0":"/x1"))}); steps.push_back({txt?"/t_1":"/e_3",load(dir+(txt?"/t1":"/x2"))});
        m.reset(std::move(steps)); assert(c.FetchChapter("book1",ch,&content,&e));
        assert(content.xhtml==!txt && content.bytes==load(dir+(txt?"/plain":"/xhtml")));
    }
    for(auto failure:{ErrorCode::Connection,ErrorCode::NameResolution,ErrorCode::Timeout,ErrorCode::Transport}) {
        c.Disconnect();m.reset({{"/web/reader/","",0,failure},{"/web/reader/",tokens},{"/e_0",load(dir+"/x0")},{"/e_1",load(dir+"/x1")},{"/e_3",load(dir+"/x2")}});
        assert(c.FetchChapter("book1",ch,&content,&e)&&m.index==5&&e.code==ErrorCode::None);
        c.Disconnect();m.reset({{"/web/reader/","",0,failure},{"/web/reader/","",0,failure}});
        assert(!c.FetchChapter("book1",ch,&content,&e)&&m.index==2&&e.code==failure);
    }
    c.Disconnect();m.reset({{"/web/reader/","",0,ErrorCode::Certificate}});
    assert(!c.FetchChapter("book1",ch,&content,&e)&&m.index==1&&e.code==ErrorCode::Certificate);
    // The token GET and its retry share one minute; never start a new request
    // after the operation budget expired, even if the caller allows retries.
    c.Disconnect();m.reset({{"/web/reader/","",0,ErrorCode::Timeout,true,{},60000}});
    assert(!c.FetchChapter("book1",ch,&content,&e)&&m.index==1&&e.code==ErrorCode::Timeout);
    // Completed shards retain their bytes, but the entire chapter is bounded.
    c.Disconnect();m.reset({{"/web/reader/",tokens,200,ErrorCode::None,true,{},40000},
        {"/e_0",load(dir+"/x0"),200,ErrorCode::None,true,{},40000},
        {"/e_1",load(dir+"/x1"),200,ErrorCode::None,true,{},30000},
        {"/e_3",load(dir+"/x2"),200,ErrorCode::None,true,{},20000}});
    assert(!c.FetchChapter("book1",ch,&content,&e)&&m.index==4&&e.code==ErrorCode::Timeout&&content.bytes.empty());
    assert(mock_milliseconds==120000 && m.timeouts==std::vector<uint32_t>({60000,60000,40000,10000}));
    c.Disconnect();m.reset({{"/web/reader/",tokens},{"/e_0","",0,ErrorCode::Timeout},{"/e_0","",0,ErrorCode::Timeout}});
    assert(!c.FetchChapter("book1",ch,&content,&e)&&m.index==3&&e.code==ErrorCode::Timeout);
    c.Disconnect(); m.reset({{"/web/reader/",tokens},{"/e_0",std::string("PK\003\004",4)}}); assert(!c.FetchChapter("book1",ch,&content,&e)&&e.code==ErrorCode::Unsupported);
    c.Disconnect(); m.reset({{"/web/reader/",tokens},{"/e_0","{\"errcode\":-2012}"}}); assert(!c.FetchChapter("book1",ch,&content,&e)&&e.code==ErrorCode::SessionExpired);
    std::vector<std::string> broken={load(dir+"/x0"),load(dir+"/x1"),load(dir+"/x2")}; broken[1][33]^=1;
    std::string decoded; assert(!crypto::DecodeShards(&broken,&decoded,kMaxChapterBytes));
    broken={load(dir+"/x0"),load(dir+"/x1"),load(dir+"/x2")}; assert(!crypto::DecodeShards(&broken,&decoded,4));
    broken={load(dir+"/x0"),load(dir+"/x1"),load(dir+"/x2"),"D41D8CD98F00B204E9800998ECF8427E"}; assert(crypto::DecodeShards(&broken,&decoded,kMaxChapterBytes)&&decoded==load(dir+"/xhtml"));

    login(c,m,"456"); assert(std::string(c.AccountId())=="456");
    m.reset({{"shelf/sync","{\"books\":[]}",200,ErrorCode::None,true,{"wr_vid=789"}}}); assert(!fetch_shelf(c,&shelf,&e)&&!c.LoggedIn());
    login(c,m); c.Disconnect(); assert(c.LoggedIn());
    m.throw_oom=true; assert(!fetch_shelf(c,&shelf,&e)&&e.code==ErrorCode::NoMemory); m.throw_oom=false;
    c.Logout(); assert(!c.LoggedIn()&&!*c.AccountId());
    assert(crypto::EncodeId("123456789")==load(dir+"/id_expected"));
    std::printf("WeRead client: login/OTP, bounded parsing, raw XHTML/TXT, signature, readback, uncertain-time, cancellation, account isolation and OOM tests passed\n");
}
