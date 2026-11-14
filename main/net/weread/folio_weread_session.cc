#include "folio_weread_session.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <cstdint>
#include <cstring>
#include <cstddef>
namespace folio::weread {
namespace {
struct Record {uint32_t magic=0x46575331,version=1;Session session;uint32_t checksum=0;};
static_assert(sizeof(Session)==2112,"Update stored session format if fields change");
uint32_t checksum(const Record& r){
    uint32_t h=2166136261u;const auto* p=reinterpret_cast<const unsigned char*>(&r);
    for(size_t i=0;i<offsetof(Record,checksum);++i)h=(h^p[i])*16777619u;
    return h;
}
struct Handle {nvs_handle_t h=0;bool opened=false;~Handle(){if(opened)nvs_close(h);}
    esp_err_t open(nvs_open_mode_t mode){esp_err_t e=nvs_flash_init();if(e!=ESP_OK)return e;
        e=nvs_open("folio_weread",mode,&h);opened=e==ESP_OK;return e;}};
}
SessionLoad LoadSession(Session* out){
    if(!out)return SessionLoad::Failed;
    *out=Session{};
    Handle h;esp_err_t e=h.open(NVS_READONLY);
    if(e==ESP_ERR_NVS_NOT_FOUND)return SessionLoad::Missing;
    if(e!=ESP_OK)return SessionLoad::Failed;
    size_t size=0;e=nvs_get_blob(h.h,"session",nullptr,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND)return SessionLoad::Missing;
    if(e!=ESP_OK)return SessionLoad::Failed;
    if(size!=sizeof(Record))return SessionLoad::Invalid;
    Record r;e=nvs_get_blob(h.h,"session",&r,&size);
    if(e!=ESP_OK)return SessionLoad::Failed;
    if(size!=sizeof(r)||r.magic!=0x46575331||r.version!=1||r.checksum!=checksum(r)||!r.session.Valid())return SessionLoad::Invalid;
    *out=r.session;return SessionLoad::Loaded;
}
bool SaveSession(const Session& session){
    if(!session.Valid())return false;
    Session previous;
    if(LoadSession(&previous)==SessionLoad::Loaded&&!memcmp(&session,&previous,sizeof(session)))return true;
    Record r;r.session=session;r.checksum=checksum(r);
    Handle h;if(h.open(NVS_READWRITE)!=ESP_OK)return false;
    return nvs_set_blob(h.h,"session",&r,sizeof(r))==ESP_OK&&nvs_commit(h.h)==ESP_OK;
}
bool EraseSession(){
    Handle h;esp_err_t e=h.open(NVS_READWRITE);
    if(e!=ESP_OK)return false;
    e=nvs_erase_key(h.h,"session");
    if(e==ESP_ERR_NVS_NOT_FOUND)return true;
    return e==ESP_OK&&nvs_commit(h.h)==ESP_OK;
}
}
