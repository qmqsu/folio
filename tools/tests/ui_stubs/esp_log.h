#pragma once
static inline void ui_stub_log(const char* tag,const char* format,...){(void)tag;(void)format;}
#define ESP_LOGW(tag, ...) ui_stub_log(tag,__VA_ARGS__)
#define ESP_LOGE(tag, ...) ui_stub_log(tag,__VA_ARGS__)
#define ESP_LOGI(tag, ...) ui_stub_log(tag,__VA_ARGS__)
#define ESP_LOGD(tag, ...) ui_stub_log(tag,__VA_ARGS__)
