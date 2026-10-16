#pragma once
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C, IO-Expander, Panel, LVGL-Display + Touch. lv_init() muss vorher gelaufen sein. */
lv_display_t *display_init(void);
/* Backlight einschalten (erst nach dem ersten gerenderten Frame, sonst sieht man Müll). */
void display_backlight_on(void);
void display_set_brightness(uint8_t level);
/* Helligkeit weich auf target blenden (blockiert ms Millisekunden). 0 schaltet die Beleuchtung ganz ab. */
void display_fade_backlight(uint8_t target, uint32_t ms);
/* Ruhemodus: Berührungen nicht an LVGL weitergeben, nur als Weckruf merken. */
void display_block_input(bool on);
/* true, wenn seit dem letzten Aufruf im Ruhemodus berührt wurde. */
bool display_take_touch_wake(void);

#ifdef __cplusplus
}
#endif
