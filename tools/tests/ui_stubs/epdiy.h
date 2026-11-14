#pragma once
#include <stdint.h>
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { uint8_t* framebuffer; } EpdiyHighlevelState;
enum EpdDrawError { EPD_DRAW_SUCCESS = 0 };
#define MODE_GC16 1
#define MODE_GL16 2
static inline int epd_width(void) { return 1216; }
static inline int epd_height(void) { return 684; }
int epd_rotated_display_width(void);
int epd_rotated_display_height(void);
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer);
void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* framebuffer);
void epd_draw_rect(EpdRect rect, uint8_t color, uint8_t* framebuffer);
void epd_hl_set_all_white(EpdiyHighlevelState* state);
void epd_poweron(void);
void epd_poweroff(void);
void epd_clear(void);
float epd_ambient_temperature(void);
enum EpdDrawError epd_hl_update_screen_from_white(EpdiyHighlevelState* state, int mode, int temperature);
enum EpdDrawError epd_hl_update_screen_full(EpdiyHighlevelState* state, int mode, int temperature);
enum EpdDrawError epd_hl_update_screen(EpdiyHighlevelState* state, int mode, int temperature);
enum EpdDrawError epd_hl_update_area(EpdiyHighlevelState* state, int mode, int temperature,EpdRect area);
