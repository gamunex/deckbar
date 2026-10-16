/*
 * NexBoard-Firmware: WLAN + mDNS + WebSocket oder USB, rendert den gemeinsamen shared-ui-Code.
 *
 * WLAN hat Vorrang, USB ist der Rückfall: Dasselbe JSON-Protokoll läuft zeilenweise mit Präfix "@@"
 * über die USB-Schnittstelle (Gegenstück: daemon/deckbar_daemon/usb.py). Ohne USB-Verbindung meldet
 * sich das Gerät alle 2 s mit "@@DECKBAR WLAN" (läuft über WLAN) oder "@@DECKBAR" (WebSocket seit
 * USB_FALLBACK_MS weg, USB erwünscht). Nur dann nimmt es "@@OPEN" an. Der WLAN-Teil sucht weiter;
 * steht der WebSocket wieder, wird USB verlassen. Ohne Lebenszeichen ("@@KEEP") für 15 s oder
 * nach "@@CLOSE" endet die USB-Verbindung ebenfalls.
 *
 * Threads:
 *   - LVGL-Task (Kern 1): lv_timer_handler() unter lvgl_mux, arbeitet die Eingangs-Queue ab.
 *   - loop() (Arduino, Kern 1): WLAN, mDNS-Suche, WebSocketsClient. Die mDNS-Suche blockiert
 *     bis zu 3 s, deshalb läuft USB getrennt davon.
 *   - usb (Kern 0): USB-Zeilen lesen und schreiben.
 *   - lcd_tx (Kern 0): Frame-Übertragung, siehe display.c.
 *   Ausgehende Nachrichten kommen über die Ausgangs-Queue, abgeholt von der aktiven Verbindung.
 */
#include <Arduino.h>
#include <ESPmDNS.h>
#include <FFat.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "deckbar/hal.h"
#include "deckbar/ui.h"
#include "deckbar_config.h"
#include "display.h"
#include "lvgl.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h fehlt – secrets.example.h kopieren und ausfüllen"
#endif
#ifndef DECKBAR_PORT
#define DECKBAR_PORT DECKBAR_DEFAULT_PORT
#endif

static const char *TAG = "deckbar";

/* ------------------------------------------------------------------ Queues */
enum { EV_OPEN, EV_CLOSE, EV_CLOSE_NOWIFI, EV_MSG, EV_BIN, EV_DETAIL };
struct net_ev_t {
    uint8_t type;
    char *data; /* ps_malloc, Empfänger gibt frei */
    size_t len;
};

static QueueHandle_t q_in;  /* Netz -> LVGL */
static QueueHandle_t q_out; /* LVGL -> Netz (char*, ps_malloc) */
static SemaphoreHandle_t lvgl_mux;
static volatile bool want_reconnect;
static volatile bool ws_connected;
static volatile bool usb_link;          /* Daemon über USB verbunden (nur solange kein WebSocket) */
static volatile uint32_t ws_down_since; /* seit wann kein WebSocket steht */
static volatile bool detail_stale;      /* Statuszeile nach USB neu setzen */

static void push_in(uint8_t type, const char *data = nullptr, size_t len = 0)
{
    net_ev_t ev = {type, nullptr, len};
    if (data) {
        ev.data = (char *)ps_malloc(len + 1);
        if (!ev.data) return;
        memcpy(ev.data, data, len);
        ev.data[len] = 0;
    }
    /* Bilder (art/bg) kommen in Dutzenden Teilen am Stück: lieber die Netzwerkschleife kurz
     * bremsen (TCP puffert), als Nachrichten zu verwerfen – sonst wird ein Bild nie vollständig. */
    if (xQueueSend(q_in, &ev, pdMS_TO_TICKS(2000)) != pdTRUE) {
        log_printf("Eingangs-Queue voll, Nachricht verworfen\n");
        free(ev.data);
    }
}

/* ------------------------------------------------------------------ HAL für shared-ui */
extern "C" bool deckbar_hal_send(const char *text, size_t len)
{
#if DECKBAR_TOUCH_DEBUG
    log_printf("TXM %.*s\n", (int)(len > 120 ? 120 : len), text);
#endif
    if (!ws_connected && !usb_link) return false;
    char *copy = (char *)ps_malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, text, len);
    copy[len] = 0;
    if (xQueueSend(q_out, &copy, 0) != pdTRUE) {
        free(copy);
        return false;
    }
    return true;
}

extern "C" void deckbar_hal_reconnect(void)
{
    want_reconnect = true;
}

static Preferences prefs;

extern "C" bool deckbar_hal_store(const char *key, const char *data, size_t len)
{
    return prefs.putBytes(key, data, len) == len;
}

extern "C" char *deckbar_hal_load(const char *key)
{
    size_t n = prefs.getBytesLength(key);
    if (n == 0) return nullptr;
    char *buf = (char *)malloc(n + 1);
    if (!buf) return nullptr;
    if (prefs.getBytes(key, buf, n) != n) {
        free(buf);
        return nullptr;
    }
    buf[n] = 0;
    return buf;
}

/* Größere Blobs (Hintergrund ~220 KB) im FAT-Bereich des Flashs (Partition ffat, 9 MB):
 * NVS (Preferences) ist dafür zu klein. */
static bool ffat_ok;

static String blob_path(const char *key)
{
    return String("/") + key + ".bin";
}

extern "C" bool deckbar_hal_store_bin(const char *key, const void *head, size_t head_len, const void *data, size_t len)
{
    if (!ffat_ok) return false;
    String path = blob_path(key), tmp = path + ".tmp";
    if (!head && !len) return !FFat.exists(path) || FFat.remove(path);
    uint32_t t0 = millis();
    File f = FFat.open(tmp, FILE_WRITE);
    if (!f) return false;
    bool ok = f.write((const uint8_t *)head, head_len) == head_len && f.write((const uint8_t *)data, len) == len;
    f.close();
    if (ok) {
        FFat.remove(path);
        ok = FFat.rename(tmp, path);
    }
    log_printf("Blob %s gespeichert (%u Byte, %u ms): %s\n", key, (unsigned)(head_len + len),
               (unsigned)(millis() - t0), ok ? "ok" : "Fehler");
    return ok;
}

extern "C" uint8_t *deckbar_hal_load_bin(const char *key, size_t *len)
{
    if (!ffat_ok) return nullptr;
    File f = FFat.open(blob_path(key), FILE_READ);
    if (!f) return nullptr;
    size_t n = f.size();
    uint8_t *buf = n ? (uint8_t *)ps_malloc(n) : nullptr;
    if (buf && f.read(buf, n) == n) *len = n;
    else {
        free(buf);
        buf = nullptr;
    }
    f.close();
    return buf;
}

/* ------------------------------------------------------------------ LVGL-Task */
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ Ruhemodus
 * Ist DECKBAR_SLEEP_AFTER_S lang kein Daemon angemeldet (egal ob über WLAN oder USB), blendet das
 * Display aus und LVGL zeichnet nicht mehr. WLAN und USB suchen dabei weiter. Sobald der Daemon
 * wieder da ist, blendet es ein. Eine Berührung weckt es für DECKBAR_SLEEP_PEEK_S, damit man den
 * Verbindungsstatus sehen kann; diese Berührung löst nichts aus. */
static lv_display_t *disp;
#if DECKBAR_SLEEP_AFTER_S > 0
static bool asleep;
static uint32_t offline_since; /* 0 = gerade online */
static uint32_t peek_until;    /* 0 = kein Aufwachen per Berührung aktiv */

/* Unter lvgl_mux aufrufen. Rückgabe: 1 = einblenden, -1 = ausblenden, 0 = nichts tun. */
static int sleep_check(void)
{
    uint32_t now = millis();
    bool online = deckbar_ui_net_state() == DECKBAR_NET_ONLINE;
    if (online) {
        offline_since = 0;
        peek_until = 0;
    } else if (!offline_since) {
        offline_since = now ? now : 1;
    }

    if (asleep) {
        bool touched = display_take_touch_wake();
        if (!online && !touched) return 0;
        asleep = false;
        peek_until = online ? 0 : now + DECKBAR_SLEEP_PEEK_S * 1000u;
        display_block_input(false);
        lv_display_enable_invalidation(disp, true);
        lv_obj_invalidate(lv_screen_active());
        log_printf("Ruhemodus aus (%s)\n", online ? "PC verbunden" : "Berührung");
        return 1;
    }

    if (online) return 0;
    bool long_offline = now - offline_since >= DECKBAR_SLEEP_AFTER_S * 1000u;
    bool peek_over = !peek_until || (int32_t)(now - peek_until) >= 0;
    if (!long_offline || !peek_over) return 0;
    asleep = true;
    display_block_input(true);
    lv_display_enable_invalidation(disp, false); /* letztes Bild bleibt beim Ausblenden stehen */
    log_printf("Ruhemodus an: seit %lu s kein PC verbunden\n", (unsigned long)((now - offline_since) / 1000));
    return -1;
}
#endif

static void lvgl_task(void *arg)
{
    bool backlight = false;
    for (;;) {
        uint32_t delay_ms = 5;
        int fade = 0;
        if (xSemaphoreTake(lvgl_mux, portMAX_DELAY) == pdTRUE) {
            net_ev_t ev;
            while (xQueueReceive(q_in, &ev, 0) == pdTRUE) {
                switch (ev.type) {
                case EV_OPEN: deckbar_ui_net_connected(); break;
                case EV_CLOSE: deckbar_ui_net_disconnected(false); break;
                case EV_CLOSE_NOWIFI: deckbar_ui_net_disconnected(true); break;
                case EV_MSG:
#if DECKBAR_TOUCH_DEBUG
                    log_printf("RX %u %.100s\n", (unsigned)ev.len, ev.data);
#endif
                    deckbar_ui_net_message(ev.data, ev.len);
                    break;
                case EV_BIN: deckbar_ui_net_binary((const uint8_t *)ev.data, ev.len); break;
                case EV_DETAIL: deckbar_ui_set_detail(ev.data); break;
                }
                free(ev.data);
            }
#if DECKBAR_SLEEP_AFTER_S > 0
            if (backlight) fade = sleep_check();
#endif
            delay_ms = lv_timer_handler();
            xSemaphoreGive(lvgl_mux);
        }
        if (!backlight) {
            /* erst einschalten, wenn der erste Frame im Panel steht */
            vTaskDelay(pdMS_TO_TICKS(60));
            display_backlight_on();
            backlight = true;
        }
        if (fade > 0) {
            vTaskDelay(pdMS_TO_TICKS(40)); /* neu gezeichneten Frame erst ins Panel lassen */
            display_fade_backlight(DECKBAR_BRIGHTNESS, 250);
        } else if (fade < 0) {
            display_fade_backlight(0, 800);
        }
        if (delay_ms > 30) delay_ms = 30;
        if (delay_ms < 2) delay_ms = 2;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ------------------------------------------------------------------ Netzwerk */
static WebSocketsClient ws;
static char device_id[24];
static char detail[64];
static IPAddress daemon_ip;
static uint16_t daemon_port = DECKBAR_PORT;
static bool ws_started;
static uint32_t last_mdns_try;
static uint32_t ws_started_at;
static bool wifi_was_up;

static void set_detail(const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(buf, detail) == 0 && !(detail_stale && !usb_link)) return;
    strcpy(detail, buf);
    log_printf("NET %s (WiFi-Status %d)\n", buf, (int)WiFi.status());
    if (usb_link) return; /* Anzeige gehört gerade der USB-Verbindung */
    detail_stale = false;
    push_in(EV_DETAIL, buf, strlen(buf));
}

static void ws_event(WStype_t type, uint8_t *payload, size_t len)
{
    switch (type) {
    case WStype_CONNECTED:
        ws_connected = true; /* zuerst, damit die USB-Task kein OPEN mehr annimmt */
        if (usb_link) {
            usb_link = false;
            log_printf("WLAN wieder da, verlasse USB\n");
            push_in(EV_CLOSE);
        }
        set_detail("Daemon %s:%u", daemon_ip.toString().c_str(), daemon_port);
        push_in(EV_OPEN);
        break;
    case WStype_DISCONNECTED:
        if (ws_connected) log_printf("WS getrennt (frei: intern %u, PSRAM %u)\n", (unsigned)ESP.getFreeHeap(),
                                     (unsigned)ESP.getFreePsram());
        if (ws_connected) {
            push_in(EV_CLOSE);
            ws_down_since = millis();
        }
        ws_connected = false;
        break;
    case WStype_TEXT:
        push_in(EV_MSG, (const char *)payload, len);
        break;
    case WStype_BIN: /* Bildteile (hello.caps "bin") */
        push_in(EV_BIN, (const char *)payload, len);
        break;
    case WStype_ERROR: log_printf("WS-Fehler %.*s\n", (int)len, payload ? (const char *)payload : ""); break;
    default: break;
    }
}

static bool resolve_daemon(void)
{
#ifdef DECKBAR_HOST
    if (daemon_ip.fromString(DECKBAR_HOST)) return true;
    IPAddress ip;
    if (WiFi.hostByName(DECKBAR_HOST, ip)) {
        daemon_ip = ip;
        return true;
    }
    set_detail("Host %s nicht auflösbar", DECKBAR_HOST);
    return false;
#else
    set_detail("Suche _deckbar._tcp per mDNS …");
    int n = MDNS.queryService("deckbar", "tcp");
    if (n <= 0) {
        set_detail("Kein Daemon per mDNS gefunden");
        return false;
    }
    daemon_ip = MDNS.address(0);
    daemon_port = MDNS.port(0);
    if (daemon_ip == IPAddress(0, 0, 0, 0)) {
        /* Antwort ohne A-Eintrag: Hostnamen einzeln auflösen */
        String host = MDNS.hostname(0);
        if (host.endsWith(".local")) host.remove(host.length() - 6);
        daemon_ip = MDNS.queryHost(host, 2000);
        if (daemon_ip == IPAddress(0, 0, 0, 0)) {
            set_detail("Daemon %s ohne Adresse", host.c_str());
            return false;
        }
    }
    return true;
#endif
}

static void stop_ws(void)
{
    if (ws_started) ws.disconnect();
    ws_started = false;
    if (ws_connected) {
        push_in(EV_CLOSE);
        ws_down_since = millis();
    }
    ws_connected = false;
}

static void net_loop(void)
{
    bool wifi_up = WiFi.status() == WL_CONNECTED;
    static uint32_t last_rssi_log;
    if (wifi_up && millis() - last_rssi_log > 5000) {
        last_rssi_log = millis();
        log_printf("RSSI %d dBm, Kanal %d, BSSID %s\n", (int)WiFi.RSSI(), (int)WiFi.channel(), WiFi.BSSIDstr().c_str());
    }
    if (!wifi_up) {
        if (wifi_was_up || ws_started) {
            stop_ws();
            if (!usb_link) push_in(EV_CLOSE_NOWIFI);
        }
        wifi_was_up = false;
        set_detail("WLAN „%s“ …", WIFI_SSID);
        return;
    }
    if (!wifi_was_up) {
        wifi_was_up = true;
        MDNS.end();
        MDNS.begin(device_id);
        if (!usb_link) push_in(EV_CLOSE); /* Anzeige: "Suche Daemon" */
        set_detail("WLAN ok, IP %s", WiFi.localIP().toString().c_str());
    }

    if (want_reconnect) {
        want_reconnect = false;
        ESP_LOGW(TAG, "Neuverbindung angefordert");
        stop_ws();
    }

    /* Nach 10 s ohne Verbindung neu per mDNS suchen (Daemon evtl. auf anderer IP) */
    if (ws_started && !ws_connected && millis() - ws_started_at > 10000) stop_ws();

    if (!ws_started) {
        if (millis() - last_mdns_try < 3000 && last_mdns_try) return;
        last_mdns_try = millis();
        if (!resolve_daemon()) return;
        set_detail("Verbinde %s:%u", daemon_ip.toString().c_str(), daemon_port);
        ws.begin(daemon_ip, daemon_port, "/ws");
        ws.onEvent(ws_event);
        ws.setReconnectInterval(2000);
        ws_started = true;
        ws_started_at = millis();
    }

    ws.loop();

    char *out;
    while (ws_connected && xQueueReceive(q_out, &out, 0) == pdTRUE) {
        ws.sendTXT(out, strlen(out));
        free(out);
    }
}

/* ------------------------------------------------------------------ USB */
#define USB_LINE_MAX (20 * 1024)
#define USB_TIMEOUT_MS 15000
#define USB_BEACON_MS 2000
#define USB_FALLBACK_MS 10000 /* so lange ohne WebSocket, bevor USB angeboten wird (auch nach dem Start) */
static char *usb_buf;
static size_t usb_len;
static bool usb_overflow;
static uint32_t usb_last_rx, usb_last_beacon;

static void usb_write(const char *payload, size_t len)
{
    char *line = (char *)ps_malloc(len + 3);
    if (!line) return;
    line[0] = line[1] = '@';
    memcpy(line + 2, payload, len);
    line[len + 2] = '\n';
    Serial.write((const uint8_t *)line, len + 3); /* ein Aufruf, damit Logs nicht dazwischenfunken */
    free(line);
}

static bool usb_wanted(void)
{
    return !ws_connected && millis() - ws_down_since > USB_FALLBACK_MS;
}

static void usb_leave(const char *why)
{
    log_printf("USB: %s, zurück auf WLAN\n", why);
    usb_link = false;
    detail_stale = true;
    push_in(WiFi.status() == WL_CONNECTED ? EV_CLOSE : EV_CLOSE_NOWIFI);
}

static void usb_line(char *line, size_t len)
{
    char *p = strstr(line, "@@");
    if (!p) return; /* Echo oder Müll */
    p += 2;
    len -= (size_t)(p - line);
    usb_last_rx = millis();
    if (!strcmp(p, "OPEN")) {
        if (!usb_wanted()) return; /* WLAN steht, die nächste Kennung sagt es dem PC */
        usb_link = true;
        push_in(EV_DETAIL, "USB verbunden", 13);
        push_in(EV_OPEN);
    } else if (!strcmp(p, "CLOSE")) {
        if (usb_link) usb_leave("PC hat getrennt");
    } else if (p[0] == '{' && usb_link) {
        push_in(EV_MSG, p, len);
    }
}

static void usb_loop(void)
{
    if (!usb_buf) usb_buf = (char *)ps_malloc(USB_LINE_MAX + 1);
    if (!usb_buf) return;
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n') {
            while (usb_len && usb_buf[usb_len - 1] == '\r') usb_len--;
            usb_buf[usb_len] = 0;
            if (!usb_overflow) usb_line(usb_buf, usb_len);
            usb_len = 0;
            usb_overflow = false;
        } else if (usb_len < USB_LINE_MAX) {
            usb_buf[usb_len++] = (char)c;
        } else {
            usb_overflow = true;
        }
    }
    if (usb_link && millis() - usb_last_rx > USB_TIMEOUT_MS) usb_leave("kein Lebenszeichen");
    if (!usb_link && millis() - usb_last_beacon > USB_BEACON_MS) {
        usb_last_beacon = millis();
        if (usb_wanted())
            usb_write("DECKBAR", 7);
        else
            usb_write("DECKBAR WLAN", 12);
    }

    char *out;
    while (usb_link && xQueueReceive(q_out, &out, 0) == pdTRUE) {
        usb_write(out, strlen(out));
        free(out);
    }
}

static void usb_task(void *)
{
    for (;;) {
        usb_loop();
        vTaskDelay(1);
    }
}

/* ------------------------------------------------------------------ Arduino */
void setup()
{
    Serial.setRxBufferSize(32 * 1024); /* Bildteile vom Daemon über USB (bis 16 KB pro Zeile) */
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0); /* liest niemand mit, nicht blockieren */
    log_printf("NexBoard %s, Reset-Grund %d\n", DECKBAR_FW_VERSION, (int)esp_reset_reason());
    q_in = xQueueCreate(32, sizeof(net_ev_t));
    q_out = xQueueCreate(32, sizeof(char *));
    lvgl_mux = xSemaphoreCreateMutex();
    prefs.begin("deckbar", false);
    /* beim ersten Start einmal formatieren (dauert ein paar Sekunden) */
    ffat_ok = FFat.begin(true);
    log_printf("FFat %s (%u KB frei)\n", ffat_ok ? "bereit" : "nicht verfügbar",
               ffat_ok ? (unsigned)(FFat.freeBytes() / 1024) : 0);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA); /* WiFi.macAddress() liefert vor WiFi.mode() nur Nullen */
    snprintf(device_id, sizeof device_id, "deckbar-%02x%02x%02x", mac[3], mac[4], mac[5]);

    lv_init();
    lv_log_register_print_cb([](lv_log_level_t, const char *buf) { log_printf("%s", buf); });
    lv_tick_set_cb(tick_cb);
    disp = display_init();

    static deckbar_config_t cfg = {
        .token = DECKBAR_TOKEN,
        .device_id = device_id,
#if DECKBAR_BOARD_V1
        .model = "waveshare-s3-lcd-3.49-v1",
#else
        .model = "waveshare-s3-lcd-3.49-v2",
#endif
        .fw = DECKBAR_FW_VERSION,
        .sim = false,
    };
    deckbar_ui_init(&cfg);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12 * 1024, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(usb_task, "usb", 6 * 1024, NULL, 1, NULL, 0);

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(device_id);
    WiFi.setSleep(false); /* geringere Latenz beim Tippen */
    WiFi.setAutoReconnect(true);
    int nets = WiFi.scanNetworks();
    for (int i = 0; i < nets; i++)
        log_printf("SCAN %-24s %4d dBm Kanal %2d %s\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                   WiFi.BSSIDstr(i).c_str());
    WiFi.scanDelete();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    ESP_LOGI(TAG, "%s gestartet, PSRAM frei: %u KB", device_id, (unsigned)(ESP.getFreePsram() / 1024));
}

void loop()
{
    net_loop();
    delay(1);
}
