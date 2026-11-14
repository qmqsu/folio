#pragma once
#include <stdint.h>
int epd_rotated_display_width(void);
int epd_rotated_display_height(void);
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer);
