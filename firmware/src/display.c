/*
 * Display, Touch und Backlight für das Waveshare ESP32-S3-Touch-LCD-3.49 V2.
 *
 * Init-Reihenfolge, QSPI-Parameter, Reset über den TCA9554 und das Touch-Protokoll sind
 * 1:1 aus dem V2-Beispiel (10_LVGL_V9_Test/lvgl_port.c) übernommen.
 *
 * Eigene Änderung gegenüber dem Beispiel (Performance):
 *   - LVGL rendert im DIRECT-Modus (nur geänderte Bereiche) in zwei 640x172-Puffer im PSRAM.
 *   - Der AXS15231B nimmt per QSPI nur ganze Frames ab Zeile 0 an (kein RASET, siehe
 *     panel_axs15231b_draw_bitmap). Deshalb wird nach dem letzten Bereich eines Frames der
 *     komplette Puffer übertragen – in einem eigenen Task, damit LVGL währenddessen schon
 *     den nächsten Frame in den anderen Puffer rendern kann.
 *   - Drehung ins Hochformat und RGB565-Byte-Swap passieren in einem Durchgang direkt in
 *     zwei DMA-Puffer (Ping-Pong), statt über einen dritten Vollbildpuffer.
 */
#include "display.h"

#include <string.h>

#if DECKBAR_BOARD_V1
#include "board_v1.h"
#else
#include "board_v2.h"
#endif
#include "deckbar/ui.h"
#include "deckbar_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "drivers/esp_io_expander_tca9554.h"
#include "drivers/esp_lcd_axs15231b.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#if DECKBAR_TOUCH_DEBUG
#include "esp32-hal-log.h"
#include "esp_timer.h"
#endif
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "display";

#define LW DECKBAR_W        /* logisch (quer) */
#define LH DECKBAR_H
#define PW BOARD_LCD_PHYS_W /* physisch (hoch) */
#define PH BOARD_LCD_PHYS_H
#define CHUNK_ROWS 64       /* wie LVGL_DMA_BUFF_LEN im Beispiel: 172 * 64 * 2 Byte */
#define CHUNK_BYTES (PW * CHUNK_ROWS * 2)

#if (PH % CHUNK_ROWS) != 0
#error "PH muss ein Vielfaches von CHUNK_ROWS sein"
#endif

static esp_lcd_panel_handle_t panel;
static esp_io_expander_handle_t expander;
static i2c_master_bus_handle_t bus0, bus1;
static i2c_master_dev_handle_t touch_dev;

static uint16_t *dma_buf[2];
static SemaphoreHandle_t dma_free; /* zählt freie DMA-Puffer (0..2) */
static TaskHandle_t tx_task;
static lv_display_t *disp;
static const uint16_t *volatile tx_frame;
static volatile bool tx_busy;      /* Frame wird gerade übertragen */
static SemaphoreHandle_t tx_done;  /* weckt den LVGL-Task, wenn tx_busy false wird */

/* Das Beispiel nutzt LCD_CMD_BITS 32 / PARAM 8 und nur Sleep-Out + Display-On */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

/* ------------------------------------------------------------------ I2C + IO-Expander */
static void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C0_SDA,
        .scl_io_num = BOARD_I2C0_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus0));
    cfg.i2c_port = I2C_NUM_1;
    cfg.sda_io_num = BOARD_TOUCH_SDA;
    cfg.scl_io_num = BOARD_TOUCH_SCL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus1));

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus1, &dev, &touch_dev));
}

static void exio_init(void)
{
#if DECKBAR_BOARD_V1
    /* V1: Reset und Backlight hängen direkt am ESP32, Expander wird nicht gebraucht */
    gpio_config_t rst = {.pin_bit_mask = 1ULL << BOARD_LCD_RST, .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(BOARD_LCD_RST, 1);
    return;
#endif
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(bus0, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &expander));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, BOARD_EXIO_TOUCH_INT, IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, BOARD_EXIO_BL_EN | BOARD_EXIO_LCD_RST, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, BOARD_EXIO_BL_EN, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, BOARD_EXIO_LCD_RST, 1));
}

static void lcd_reset(void)
{
#if DECKBAR_BOARD_V1
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    return;
#endif
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, BOARD_EXIO_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, BOARD_EXIO_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, BOARD_EXIO_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
}

/* ------------------------------------------------------------------ Backlight */
static void backlight_off_early(void)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&c));
#if DECKBAR_BOARD_V1
    gpio_set_level(BOARD_LCD_BL, 1); /* V1: invertiert, High = dunkel */
#else
    gpio_set_level(BOARD_LCD_BL, 0);
#endif
}

static uint8_t bl_level; /* zuletzt gesetzte Helligkeit, 0 = aus */

void display_set_brightness(uint8_t level)
{
    /* Waveshare: LCD_PWM_MODE_x = 0xff - x, d. h. Tastverhältnis invertiert */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0xFF - level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void backlight_on(uint8_t level)
{
    /* Reihenfolge wie im V2-Beispiel: GPIO + BL_EN an, danach PWM (lcd_bl_pwm_bsp_init) */
#if !DECKBAR_BOARD_V1
    gpio_set_level(BOARD_LCD_BL, 1);
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, BOARD_EXIO_BL_EN, 1));
#endif
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 50 * 1000,
        .clk_cfg = LEDC_SLOW_CLK_RC_FAST,
    };
    ledc_channel_config_t ch = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_3,
        .duty = 0xFF - level,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_timer_config(&t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_channel_config(&ch));
    bl_level = level;
}

void display_fade_backlight(uint8_t target, uint32_t ms)
{
#if !DECKBAR_BOARD_V1
    if (target > 0 && bl_level == 0) esp_io_expander_set_level(expander, BOARD_EXIO_BL_EN, 1);
#endif
    int from = bl_level;
    int steps = ms / 10 > 0 ? (int)(ms / 10) : 1;
    for (int i = 1; i <= steps; i++) {
        /* Smoothstep, damit Anfang und Ende weich auslaufen */
        float t = (float)i / steps;
        float e = t * t * (3.0f - 2.0f * t);
        display_set_brightness((uint8_t)(from + (target - from) * e + 0.5f));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bl_level = target;
    /* Ganz abschalten: Tastverhältnis 255/256 lässt sonst einen schwachen Schimmer.
     * display_set_brightness() schaltet das PWM beim Einblenden wieder ein. */
#if DECKBAR_BOARD_V1
    if (target == 0) ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1); /* V1: High = dunkel */
#else
    if (target == 0) esp_io_expander_set_level(expander, BOARD_EXIO_BL_EN, 0);
#endif
}

/* ------------------------------------------------------------------ Übertragung */
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(dma_free, &woken);
    return woken == pdTRUE;
}

/* Physische Zeilen [y0, y0+CHUNK_ROWS) aus dem Querformat-Frame erzeugen (drehen + swap).
 * Gelesen wird zeilenweise aus dem PSRAM-Framebuffer (je CHUNK_ROWS zusammenhängende Pixel),
 * verstreut geschrieben wird nur in den DMA-Puffer im internen RAM. Spaltenweises Lesen
 * (1280 Byte Abstand pro Pixel) hat jeden Zugriff zum Cache-Miss gemacht: ~60 ms pro Frame. */
static void IRAM_ATTR convert_chunk(const uint16_t *src, uint16_t *dst, int y0)
{
    for (int px = 0; px < PW; px++) {
        uint16_t *out = dst + px;
#if DECKBAR_ROTATION == 270
        /* physisch (px, py) <- logisch (x = py, y = PW-1-px) */
        const uint16_t *row = src + (PW - 1 - px) * LW + y0;
        for (int i = 0; i < CHUNK_ROWS; i++) {
            uint16_t c = row[i];
            out[i * PW] = (uint16_t)((c >> 8) | (c << 8));
        }
#else
        /* 90°: physisch (px, py) <- logisch (x = PH-1-py, y = px) */
        const uint16_t *row = src + px * LW + (PH - 1 - y0);
        for (int i = 0; i < CHUNK_ROWS; i++) {
            uint16_t c = row[-i];
            out[i * PW] = (uint16_t)((c >> 8) | (c << 8));
        }
#endif
    }
}

static void tx_task_fn(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if DECKBAR_TOUCH_DEBUG
        int64_t t0 = esp_timer_get_time();
        static uint32_t tx_max, tx_sum, tx_n;
        static int64_t t_stat;
#endif
        const uint16_t *frame = tx_frame;
        int b = 0;
        for (int y = 0; y < PH; y += CHUNK_ROWS) {
            xSemaphoreTake(dma_free, portMAX_DELAY); /* Puffer b ist frei */
            convert_chunk(frame, dma_buf[b], y);
            esp_lcd_panel_draw_bitmap(panel, 0, y, PW, y + CHUNK_ROWS, dma_buf[b]);
            b ^= 1;
        }
        /* warten, bis beide DMA-Puffer wieder frei sind */
        xSemaphoreTake(dma_free, portMAX_DELAY);
        xSemaphoreTake(dma_free, portMAX_DELAY);
        xSemaphoreGive(dma_free);
        xSemaphoreGive(dma_free);
#if DECKBAR_TOUCH_DEBUG
        {
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            tx_sum += us; tx_n++; if (us > tx_max) tx_max = us;
            if (t0 - t_stat > 1000000) {
                log_printf("TX n=%lu avg=%luus max=%luus\n", (unsigned long)tx_n,
                           (unsigned long)(tx_sum / tx_n), (unsigned long)tx_max);
                t_stat = t0; tx_max = tx_sum = tx_n = 0;
            }
        }
#endif
        tx_busy = false;
        lv_display_flush_ready(disp);
        xSemaphoreGive(tx_done);
    }
}

/*
 * Ohne diese Funktion wartet LVGL mit while(disp->flushing); – aktiv, auf Kern 1, mit höherer
 * Priorität als die Arduino-loop() (Netzwerk). Bei Dauer-Animationen (laufender Titel) blieb
 * dem Netzwerk so kaum Rechenzeit, große Nachrichten liefen in den 5-s-Lese-Timeout.
 * Der Take hat ein Timeout und prüft das Flag erneut, alte Tokens schaden also nicht.
 */
static void flush_wait_cb(lv_display_t *d)
{
    (void)d;
    while (tx_busy) xSemaphoreTake(tx_done, pdMS_TO_TICKS(20));
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
#if DECKBAR_TOUCH_DEBUG
    /* welche Bereiche werden neu gezeichnet? (1x pro Sekunde: Anzahl Frames + Bounding-Box) */
    static uint32_t n_frames, n_areas;
    static lv_area_t bb;
    static int64_t t_stat;
    if (n_areas == 0) {
        bb = *area;
    } else {
        bb.x1 = LV_MIN(bb.x1, area->x1); bb.y1 = LV_MIN(bb.y1, area->y1);
        bb.x2 = LV_MAX(bb.x2, area->x2); bb.y2 = LV_MAX(bb.y2, area->y2);
    }
    n_areas++;
    if (lv_display_flush_is_last(d)) n_frames++;
    int64_t now = esp_timer_get_time();
    if (now - t_stat > 1000000) {
        log_printf("FL frames=%lu areas=%lu bb=%d,%d-%d,%d\n", (unsigned long)n_frames,
                   (unsigned long)n_areas, bb.x1, bb.y1, bb.x2, bb.y2);
        t_stat = now;
        n_frames = n_areas = 0;
    }
#endif
    if (!lv_display_flush_is_last(d)) {
        lv_display_flush_ready(d);
        return;
    }
    /* DIRECT-Modus: px_map ist der Anfang des vollen Framebuffers */
    tx_frame = (const uint16_t *)px_map;
    tx_busy = true;
    xTaskNotifyGive(tx_task);
}

/* ------------------------------------------------------------------ Touch */
/* ungültige Frames in Folge, bis eine Berührung als beendet gilt (~16 ms pro Abfrage) */
#define TOUCH_RELEASE_MISSES 5
/* größter plausibler Sprung zwischen zwei Abfragen (Manhattan, px) */
#define TOUCH_MAX_JUMP 120
/* Lesen dauert bei 300 kHz ~1,5 ms; ein hängender Bus soll den LVGL-Task nicht 50 ms blockieren */
#define TOUCH_I2C_TIMEOUT_MS 10

/* Ruhemodus: Berührungen gehen nicht an LVGL, sondern wecken nur auf. Der Finger, der geweckt hat,
 * wird bis zum Loslassen verschluckt, damit er keine Kachel auslöst. */
static volatile bool input_blocked;
static volatile bool touch_wake;
static bool swallow;

void display_block_input(bool on)
{
    input_blocked = on;
    if (on) touch_wake = false;
}

bool display_take_touch_wake(void)
{
    bool w = touch_wake;
    touch_wake = false;
    return w;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    /* Lesebefehl + Auswertung aus dem Waveshare-Beispiel (TouchInputReadCallback) */
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buf[32] = {0};
#if DECKBAR_TOUCH_DEBUG
    static uint8_t last[8];
    static uint32_t n_read, n_err, n_press, us_max;
    static int64_t t_stat;
    int64_t t0 = esp_timer_get_time();
#endif
    esp_err_t err = i2c_master_transmit_receive(touch_dev, cmd, sizeof cmd, buf, sizeof buf, TOUCH_I2C_TIMEOUT_MS);
#if DECKBAR_TOUCH_DEBUG
    int64_t t1 = esp_timer_get_time();
    n_read++;
    if ((uint32_t)(t1 - t0) > us_max) us_max = (uint32_t)(t1 - t0);
    if (err != ESP_OK) {
        n_err++;
    } else {
        if (buf[1] > 0 && buf[1] < 5) n_press++;
        if (memcmp(last, buf, sizeof last) != 0) {
            memcpy(last, buf, sizeof last);
            log_printf("T %02x %02x %02x %02x %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3],
                       buf[4], buf[5], buf[6], buf[7]);
        }
    }
    if (t1 - t_stat > 1000000) {
        log_printf("TS reads=%lu err=%lu press=%lu max=%luus\n", (unsigned long)n_read,
                   (unsigned long)n_err, (unsigned long)n_press, (unsigned long)us_max);
        t_stat = t1;
        n_read = n_err = n_press = us_max = 0;
    }
#endif
    /*
     * Der Controller liefert während einer Berührung immer wieder ungültige Antworten
     * ("be be be …" oder um ein paar Bytes verschobene Frames). Als "losgelassen" gewertet,
     * wurde daraus bei ruhendem Finger ein Klick alle paar hundert ms. Deshalb:
     *   - gültig ist nur ein Frame mit buf[0] == 0 und 1..4 Punkten
     *   - Ereignis-Bits buf[2] & 0xC0: 0x00 down, 0x80 contact, 0x40 lift
     *   - ungültige Frames ändern nichts; erst nach TOUCH_RELEASE_MISSES ohne gültige
     *     Daten (oder bei "lift") gilt der Finger als weg.
     */
    static lv_point_t last_pt;
    static bool pressed;
    static uint8_t misses;
    bool valid = err == ESP_OK && buf[0] == 0x00 && buf[1] > 0 && buf[1] < 5;
    if (!valid) {
        if (pressed && ++misses < TOUCH_RELEASE_MISSES) {
            data->point = last_pt;
            data->state = LV_INDEV_STATE_PRESSED;
            return;
        }
        pressed = false;
        data->point = last_pt;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int raw_x = ((buf[2] & 0x0f) << 8) | buf[3]; /* lange Achse, 0..640 */
    int raw_y = ((buf[4] & 0x0f) << 8) | buf[5]; /* kurze Achse, 0..172 */
    if (raw_x > PH) raw_x = PH;
    if (raw_y > PW) raw_y = PW;
    /* Beispiel (hochkant): px = raw_y, py = PH - raw_x. Hier ins Querformat umgerechnet. */
#if DECKBAR_ROTATION == 270
    int x = PH - raw_x;       /* = py */
    int y = PW - 1 - raw_y;   /* = PW-1-px */
#else
    int x = raw_x - 1;        /* = PH-1-py */
    int y = raw_y;            /* = px */
#endif
    lv_point_t pt = {LV_CLAMP(0, x, LW - 1), LV_CLAMP(0, y, LH - 1)};
    uint8_t ev = buf[2] & 0xC0; /* 0x00 down, 0x80 contact, 0x40 lift */

    /*
     * Geisterpunkte: Das Gerät meldet nach echten Taps einen zweiten, ortsfesten Punkt
     * (gemessen roh ≈ (468..505, 103..105)), oft ohne "down" und teils sekundenlang.
     *   - Eine Berührung beginnt nur mit einem "down"-Frame außerhalb der Geisterzone.
     *   - Springt der gemeldete Punkt während einer Berührung weiter als TOUCH_MAX_JUMP,
     *     ist das der zweite Punkt -> Frame verwerfen.
     */
    bool ghost = raw_x >= DECKBAR_TOUCH_GHOST_X1 && raw_x <= DECKBAR_TOUCH_GHOST_X2 &&
                 raw_y >= DECKBAR_TOUCH_GHOST_Y1 && raw_y <= DECKBAR_TOUCH_GHOST_Y2;
    if (!pressed) {
        if (ev == 0x00 && !ghost) {
            pressed = true;
            misses = 0;
            last_pt = pt;
        }
    } else if (ev == 0x40) {
        pressed = false;
    } else if (LV_ABS(pt.x - last_pt.x) + LV_ABS(pt.y - last_pt.y) <= TOUCH_MAX_JUMP) {
        misses = 0;
        last_pt = pt;
    } else if (++misses >= TOUCH_RELEASE_MISSES) {
        /* nur noch verworfene Sprünge -> Finger ist weg, sonst bleibt die Berührung hängen */
        pressed = false;
    }
    data->point = last_pt;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (input_blocked && pressed) {
        touch_wake = true;
        swallow = true;
    }
    if (swallow) {
        if (!pressed) swallow = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
#if DECKBAR_TOUCH_DEBUG
    static bool was;
    if (pressed != was) log_printf("P %s raw=(%d,%d) -> (%d,%d)\n", pressed ? "down" : "up", raw_x, raw_y,
                                   (int)last_pt.x, (int)last_pt.y);
    was = pressed;
#endif
}

/* ------------------------------------------------------------------ Init */
lv_display_t *display_init(void)
{
    i2c_init();
    backlight_off_early();
    exio_init();

    spi_bus_config_t bus = {
        .data0_io_num = BOARD_LCD_D0,
        .data1_io_num = BOARD_LCD_D1,
        .sclk_io_num = BOARD_LCD_PCLK,
        .data2_io_num = BOARD_LCD_D2,
        .data3_io_num = BOARD_LCD_D3,
        .max_transfer_sz = CHUNK_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {.quad_mode = true},
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(BOARD_LCD_HOST, &io_cfg, &io));

    axs15231b_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof lcd_init_cmds / sizeof lcd_init_cmds[0],
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &pcfg, &panel));
    lcd_reset();
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    dma_buf[0] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    dma_buf[1] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(dma_buf[0] && dma_buf[1]);
    dma_free = xSemaphoreCreateCounting(2, 2);

    size_t fb_bytes = LW * LH * 2;
    void *fb1 = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_SPIRAM);
    void *fb2 = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_SPIRAM);
    assert(fb1 && fb2);
    memset(fb1, 0, fb_bytes);
    memset(fb2, 0, fb_bytes);

    disp = lv_display_create(LW, LH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb1, fb2, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    tx_done = xSemaphoreCreateBinary();
    lv_display_set_flush_wait_cb(disp, flush_wait_cb);

    /* Übertragung auf Kern 0 (WLAN läuft dort auch, ist aber meist idle), LVGL auf Kern 1 */
    xTaskCreatePinnedToCore(tx_task_fn, "lcd_tx", 4096, NULL, 5, &tx_task, 0);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);
    lv_indev_set_display(touch, disp);

    ESP_LOGI(TAG, "Display bereit (%dx%d, Rotation %d)", LW, LH, DECKBAR_ROTATION);
    return disp;
}

void display_backlight_on(void)
{
    backlight_on(DECKBAR_BRIGHTNESS);
}
