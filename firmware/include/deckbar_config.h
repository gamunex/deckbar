#pragma once
/* Einstellungen der NexBoard-Firmware. Zugangsdaten stehen in secrets.h (gitignored). */

#define DECKBAR_FW_VERSION "0.5.0"

/*
 * Ausrichtung im Querformat. 270 entspricht der Drehung aus dem Waveshare-Beispiel
 * (Rotated = USER_DISP_ROT_90 -> LV_DISPLAY_ROTATION_270). Steht das Bild auf dem Kopf,
 * auf 90 umstellen – Touch wird automatisch mitgedreht.
 */
#ifndef DECKBAR_ROTATION
#define DECKBAR_ROTATION 270
#endif

/* Helligkeit 0..255 */
#ifndef DECKBAR_BRIGHTNESS
#define DECKBAR_BRIGHTNESS 255
#endif

/* FPS-/CPU-Anzeige von LVGL (Performance-Messung auf der Hardware) */
#ifndef DECKBAR_PERF_MONITOR
#define DECKBAR_PERF_MONITOR 0
#endif

/* Rohdaten des Touch-Controllers seriell ausgeben (nur bei Änderung + 1x pro Sekunde Statistik) */
#ifndef DECKBAR_TOUCH_DEBUG
#define DECKBAR_TOUCH_DEBUG 0
#endif

/* Rohkoordinaten (Touch-Controller), in denen keine Berührung beginnen darf. Der ortsfeste
 * Geisterpunkt (roh ≈ 468..505, 103..105) trat nur an einer gestörten Stromversorgung auf,
 * daher standardmäßig aus (X1 > X2). Bei Bedarf per Build-Flag wieder setzen:
 *   -DDECKBAR_TOUCH_GHOST_X1=455 -DDECKBAR_TOUCH_GHOST_X2=520 -DDECKBAR_TOUCH_GHOST_Y1=95 -DDECKBAR_TOUCH_GHOST_Y2=115 */
#ifndef DECKBAR_TOUCH_GHOST_X1
#define DECKBAR_TOUCH_GHOST_X1 1
#define DECKBAR_TOUCH_GHOST_X2 0
#define DECKBAR_TOUCH_GHOST_Y1 1
#define DECKBAR_TOUCH_GHOST_Y2 0
#endif

/* Ruhemodus: Display aus, wenn so lange kein PC verbunden ist (weder WLAN noch USB). 0 = nie.
 * Wach wird es sofort, sobald der Daemon wieder da ist, oder kurz bei einer Berührung. */
#ifndef DECKBAR_SLEEP_AFTER_S
#define DECKBAR_SLEEP_AFTER_S 30
#endif
/* So lange bleibt die Statusanzeige nach einer Berührung im Ruhemodus an. */
#ifndef DECKBAR_SLEEP_PEEK_S
#define DECKBAR_SLEEP_PEEK_S 15
#endif

#define DECKBAR_DEFAULT_PORT 8765
