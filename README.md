# NexBoard

*Früher DeckBar. Technische Namen (Ordner, Dienste `deckbar*.service`, `~/.config/deckbar`,
mDNS `_deckbar._tcp`, Programme `deckbar-*`, Repo) bleiben vorerst gleich, damit bestehende Geräte
und Installationen weiterlaufen.*

Eine Touch-Leiste unter dem Monitor im Stil eines Stream Deck, gebaut auf dem Waveshare
**ESP32-S3-Touch-LCD-3.49** (640 × 172 im Querformat, Board V1 und V2). Die Optik ist im
„Liquid Glass“-Stil gehalten.

- **Kacheln:** Taste, Schalter und Schieberegler für Audio (Lautstärke, Mute von Ausgang und
  Mikrofon), Media-Steuerung mit Cover und Titel, Links, Hotkeys und Shell-Befehle
- **Live-Status:** Mute, Lautstärke und Track folgen dem PC, auch wenn dort umgeschaltet wird.
  Eigene Werte kommen per `POST /api/state` dazu.
- **Mehrere Seiten** mit Wischgesten, Hintergrund wählbar (Standard, Desktop-Hintergrund, eigenes Bild)
- **AION 2:** DPS-Meter der Gruppe als eigene Seite, ein Overlay über dem Spiel (Meter, Boss-Leiste,
  Event-Timer, Namenseffekt) und nach jedem Dungeon-Run ein Bild mit dem Schaden pro Boss nach Discord
- **Discord-Bot:** Gruppen-DPS und Run-Ergebnisse an einen Bot, der Meldungen mehrerer Spieler zusammenführt
- **Verbindung** per WLAN, bei Bedarf per USB-Kabel als Rückfall
- **Ruhemodus:** Ist 30 s lang kein PC verbunden (weder WLAN noch USB), blendet das Display aus und
  geht wieder an, sobald der Daemon da ist. Eine Berührung zeigt kurz den Verbindungsstatus
  (`DECKBAR_SLEEP_AFTER_S`, `DECKBAR_SLEEP_PEEK_S` in `firmware/include/deckbar_config.h`)
- **Web-UI** mit Dashboard und einer Seite je Modul, Leisten-Editor mit Drag & Drop und
  Live-Vorschau, dazu ein SDL-Simulator zum Testen ohne Gerät
- **Modular:** Jeder Teil (Touch-Leiste, AION-2-Mitschnitt, Overlay, Runs, Discord-Bot, Updates)
  lässt sich in der Web-UI einzeln ein- und ausschalten, ohne Neustart

```
┌──────────────┐  WebSocket (JSON, v1)  ┌───────────────────────────────┐
│ Gerät / Sim  │ ◄────────────────────► │ deckbar-daemon (Python)       │
│ shared-ui    │   layout, state  →     │  wpctl · pactl subscribe      │
│ (LVGL 9.3)   │   ← event              │  playerctl --follow           │
└──────────────┘   WLAN oder USB        │  xdg-open · ydotool · shell   │
                                        │  Web-UI  http://localhost:8765│
                                        └───────────────────────────────┘
```

Die Firmware hält keine eigene Logik. Das Gerät rendert, was der Daemon schickt, und meldet nur
zurück, welche Kachel angetippt wurde. Welche Aktion dahinter steckt, weiß allein der Daemon.

| Ordner       | Inhalt |
|--------------|--------|
| `docs/`      | [Protokoll](docs/protocol.md), [JSON-Schema](docs/protocol.schema.json), [Icon-Liste](docs/icons.md) |
| `shared-ui/` | LVGL-UI-Code (C). Läuft unverändert in Firmware **und** Simulator, Hardware nur über `include/deckbar/hal.h` |
| `simulator/` | SDL2-Fenster 640 × 172, RGB565, Maus = Touch, echter WebSocket zum Daemon |
| `daemon/`    | Python-Daemon inkl. Web-Konfiguration, USB-Brücke, systemd-User-Service |
| `capture/`   | `deckbar-capture` (Rust): AION-2-Mitschnitt mit dem Kern des A2Tools DPS Meter (Submodul `capture/a2tools`) |
| `overlay/`   | `deckbar-overlay` (PySide6/QML): Overlay über dem Spiel als layer-shell-Fläche (Meter, Widgets), Tray und Tastenkürzel |
| `firmware/`  | PlatformIO / Arduino-ESP32 3.3.12, Umgebungen für Board V2 und V1 |
| `tools/`     | `gen_assets.py`: Schriften, Icons, Hintergrundbild erzeugen |

## Einrichtung (CachyOS / Arch, KDE Plasma)

### Pakete

```sh
sudo pacman -S --needed sdl2 playerctl ydotool wireplumber libpulse uv
uv tool install cmake && uv tool install ninja     # nur für den Simulator (oder: pacman -S cmake ninja)
uv tool install platformio                         # nur für die Firmware
sudo pacman -S --needed rust libpcap               # nur für den AION-2-Mitschnitt
systemctl --user enable --now ydotool              # ydotoold für Hotkeys
```

`libpulse` bringt `pactl` mit. Der Daemon nutzt es nur für `pactl subscribe`, also
ereignisbasierte Updates statt Polling. Fehlt ein Werkzeug, läuft der Rest trotzdem, und die
Web-UI zeigt an, was fehlt.

### Daemon

```sh
cd daemon
uv sync
uv run deckbar-daemon          # -v für Debug-Ausgaben, --open öffnet die Web-UI
```

Beim ersten Start legt der Daemon in `~/.config/deckbar/` zwei Dateien an:

- `config.yaml`: Port, **Token** (zufällig erzeugt), Name, bevorzugter Player, USB-Rückfall
- `layout.yaml`: Seiten und Kacheln mit Aktionen, kommentiertes Beispiel
- `modules.yaml`: welche Module eingeschaltet sind (frische Installation: Touch-Leiste und Updates)

Beide Dateien liegen außerhalb des Repos. Handänderungen an `layout.yaml` übernimmt der Daemon
live (Prüfung einmal pro Sekunde). Ist die Datei ungültig, bleibt das alte Layout aktiv, und die
Web-UI zeigt den Fehler an.

### Autostart als systemd-User-Service

```sh
uv tool install --editable ./daemon                 # legt ~/.local/bin/deckbar-daemon an
cp daemon/systemd/deckbar.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now deckbar.service
journalctl --user -u deckbar -f
```

Öffnen Links oder GUI-Programme aus dem Dienst heraus nicht, fehlt die Grafik-Umgebung im
systemd-User-Kontext: `systemctl --user import-environment WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP`.

Das Gerät erreicht den Daemon über Port 8765. Ist eine Firewall aktiv (z. B. `ufw`), muss dieser
Port im Heimnetz offen sein. Die Web-UI und `/api/*` antworten trotzdem nur auf localhost.

## Web-UI und Module

<http://localhost:8765> (nur von diesem PC aus erreichbar). Die Startseite ist ein Dashboard mit
einer Karte je eingeschaltetem Modul; jedes Modul hat eine eigene Seite mit Tabs, thematisch
gruppiert in der Seitenleiste (Geräte, Spiele, Community, System). Hell und dunkel folgen dem
System oder lassen sich umschalten, die Seite passt sich auch an schmale Fenster an.

| Modul | Inhalt | braucht |
|---|---|---|
| Touch-Leiste | Editor (Seiten, Kacheln, Aktionen, Live-Vorschau), Design, Geräte | – |
| AION 2 · DPS-Mitschnitt | Live-Ansicht, Quelle, Sprache, Seitenwechsel im Kampf | – |
| AION 2 · Overlay | Overlay-Programm steuern, Widgets, Event-Timer | DPS-Mitschnitt |
| AION 2 · Dungeon-Runs | Discord-Webhook, Vorschau, Namen der Mitspieler | DPS-Mitschnitt |
| Discord-Bot | Gruppen-DPS und Run-API eines Bots | DPS-Mitschnitt |
| Updates | Stand von Repo und A2Tools-Kern, Aktualisieren | – |

Unter **System → Module** wird jedes Modul ein- oder ausgeschaltet, sofort und ohne Neustart.
Ausgeschaltete Module laufen nicht im Hintergrund (kein Mitschnitt, keine Abfragen) und
verschwinden aus der Seitenleiste; benötigte Module werden mit eingeschaltet, abhängige mit aus.
Ist die Touch-Leiste aus, weist der Daemon Geräte mit dem Fehler `disabled` ab. **System →
Einstellungen** bearbeitet `config.yaml` (Name, Player, Adresse, Port, USB, mDNS) und startet den
Daemon bei Bedarf neu.

Ein neues Modul besteht aus einem Eintrag in `MODULES` (`daemon/deckbar_daemon/modules.py`), seinen
Aufgaben (`NexBoard._register_modules` in `server.py`) und einer Seite in
`daemon/deckbar_daemon/web/js/modules/` (Tabs, Dashboard-Karte, Inhalt). Die Web-UI kommt ohne
Framework und ohne Build-Schritt aus (ES-Module).

**Leisten-Editor** (Touch-Leiste → Editor): Seiten und Kacheln anlegen, per Drag & Drop sortieren
und mit Icon, Farbe und Aktion versehen. Jede Änderung wird nach 0,35 s gespeichert und sofort an
alle verbundenen Geräte geschickt. Im Modus „Testen“ verhält sich die Vorschau wie ein Gerät.

**Hintergrund** (Touch-Leiste → Design): Standard (in der Firmware eingebaut), Desktop-Hintergrund (folgt
einem Wechsel in Plasma) oder ein eigenes Bild, per Upload oder als Pfad. Hochgeladene Bilder
(höchstens 20 MB) landen verkleinert in `~/.config/deckbar/backgrounds/`. Das Gerät bekommt das Bild
auf 640 × 172 zugeschnitten, weichgezeichnet und leicht abgedunkelt. Es speichert es nicht, nach
einem Neustart erscheint kurz das Standardbild.

**Eigene Werte** auf die Leiste bringen (für `bind` bei Schaltern und Reglern):

```sh
curl -X POST localhost:8765/api/state -H 'Content-Type: application/json' -d '{"obs_rec": true}'
```

## Verbindung: WLAN, USB als Rückfall

Das Gerät verbindet sich per WLAN und findet den Daemon per mDNS (`_deckbar._tcp`), alternativ
über `DECKBAR_HOST` in `secrets.h`.

Hängt es zusätzlich per USB am PC, springt das Kabel ein, wenn der WebSocket 10 s lang nicht
steht, etwa bei schlechtem Empfang oder ohne WLAN. Das WLAN sucht im Hintergrund weiter. Sobald
der WebSocket wieder steht, verlässt das Gerät USB.

Über USB läuft dasselbe JSON-Protokoll, zeilenweise mit Präfix `@@`:

| Richtung    | Zeile            | Bedeutung |
|-------------|------------------|-----------|
| Gerät → PC  | `@@DECKBAR WLAN` | Kennung alle 2 s: läuft über WLAN |
|             | `@@DECKBAR`      | Kennung alle 2 s: kein WebSocket, bitte USB |
|             | `@@{…}`          | Protokollnachricht |
| PC → Gerät  | `@@OPEN`         | Brücke steht, Gerät schickt `hello` |
|             | `@@KEEP`         | Lebenszeichen alle 5 s, ohne fällt das Gerät nach 15 s zurück |
|             | `@@CLOSE`        | Brücke getrennt |
|             | `@@{…}`          | Protokollnachricht |

Alle anderen Zeilen sind Log-Ausgaben. Die Brücke (`daemon/deckbar_daemon/usb.py`) öffnet
Espressif-Ports nur passiv (DTR/RTS unverändert, also kein Reset) und schreibt erst nach der
Kennung. Fremde ESP32 am selben PC (z. B. WLED) bekommen nichts geschickt. Einstellung in
`config.yaml`: `usb: auto` (Standard), `off` oder ein fester Port `/dev/serial/by-id/…`.

## AION 2

### DPS-Meter

Die Kachel „AION 2 DPS-Meter“ füllt eine Seite und zeigt die DPS der Gruppe in Echtzeit: Rang,
Name, DPS und wahlweise Anteil oder Gesamtschaden, Balken in Klassenfarbe, die eigene Zeile
hervorgehoben. Darüber stehen Bossname, Rest-HP und Kampfdauer.

Die Daten liest die NexBoard selbst aus dem Spielverkehr mit: `deckbar-capture` (Ordner
`capture/`) setzt den Kern des [A2Tools DPS Meter](https://github.com/taengu/A2Tools-DPS-Meter)
ohne dessen Oberfläche zusammen (libpcap, Port-Erkennung, Parser, DPS-Berechnung). Der Daemon
startet das Programm, sobald AION 2 läuft, und beendet es 30 s nach dem Spiel. Gebaut wird ohne
die Online-Funktionen des Meters: kein Konto, keine Uploads, keine Update-Abfrage.

1. Bauen und installieren (einmal und nach jedem Update):

   ```sh
   capture/build.sh --install     # cargo build, Kopie nach ~/.local/bin, sudo setcap cap_net_raw
   systemctl --user restart deckbar
   ```

   Mitschneiden braucht nur `cap_net_raw`, nicht root. Ein Neubau verliert die Rechte, deshalb
   immer mit `--install` bauen.
2. In der Web-UI das Modul „AION 2 · DPS-Mitschnitt“ einschalten, unter Einstellungen als Quelle
   „Eigener Mitschnitt“ wählen (Standard) und die Sprache für Boss- und Skillnamen.
3. Eine Seite mit der Kachel „AION 2 DPS-Meter“ anlegen. Spielerzahl (1–8) und Zusatzwert stehen
   in den Kachel-Eigenschaften.
4. Optional „Bei Kampfbeginn zeigen“: Sobald neuer Schaden kommt, springt die Leiste auf diese
   Seite und nach „Zurück nach“ Sekunden ohne Schaden wieder auf die vorige (nicht, wenn man
   inzwischen selbst weggewischt hat).

Die Liste zeigt die Top N nach DPS am aktuellen Ziel (Bosse zuerst) plus einen selbst; im
Dungeon ist das die Gruppe. Kampf-History im Format des Meters: `~/.local/share/deckbar/aion2/history/`.

`deckbar-capture` spricht Zeilen-JSON über stdin/stdout (Format in `capture/src/main.rs`) und lässt
sich zum Testen auch allein starten. `--save-packets DIR` zeichnet den Mitschnitt auf,
`--replay packets_….txt [--realtime]` spielt ihn ohne Spiel wieder ab:

```sh
capture/target/release/deckbar-capture --replay packets_2026-10-12.txt --history-dir /tmp/test
```

Ein Spiel-Patch ändert meist Opcodes oder Datentabellen. Upstream zieht das schnell nach, die
NexBoard meldet das neue Release (siehe „Updates“).

### Overlay am PC

`deckbar-overlay` legt unter KDE Plasma (Wayland) eine durchsichtige **layer-shell-Fläche** über
den Spielmonitor, Ebene „overlay“, also über dem Vollbildspiel. Darin liegen Widgets:

- **DPS-Meter:** die unveränderte Oberfläche des A2Tools-Meters, mit Details (Skills, Buffs,
  Zeitleiste), Kampf-History und Einstellungen als eigene Fenster. Verschieben wie gewohnt an
  Kopf- oder Fußzeile, die Position bleibt gespeichert.
- **Boss-Leiste:** HP des Ziels, Kampfzeit und Kill-Prognose aus dem HP-Verlauf, außerhalb von
  Kämpfen ausgeblendet.
- **Event-Timer:** Countdown bis zum nächsten Start (Shugo-Festival, Spacetime Rift, Resets …),
  Ereignisse frei einstellbar in der Web-UI.
- **Namenseffekt** auf dem eigenen Namen in der Meter-Zeile: Glanz, Brennen, Gold, Frost, Holo, Leere.

Die Fläche bekommt nie die Tastatur und nimmt Klicks nur dort an, wo es nötig ist:

| Modus | Klicks | |
|---|---|---|
| Entsperrt (Standard) | nur das Meter, alles andere geht ans Spiel | |
| Gesperrt | nur der Schloss-Knopf des Meters | Schloss im Meter, Tray oder `--lock` |
| Bearbeiten | alles, Widgets ziehen, Mausrad = Größe, Esc = fertig | `--edit`, Tray oder Web-UI |

```sh
sudo pacman -S --needed pyside6 qt6-webengine layer-shell-qt
overlay/install.sh        # ~/.local/bin/deckbar-overlay, User-Dienst, Starter für Tastenkürzel
```

Danach startet das Modul „AION 2 · Overlay“ den Dienst, sobald AION 2 läuft, und beendet ihn 30 s
nach Spielende; beim Login startet nichts. Von Hand (ohne Spiel): Web-UI → „Neu starten“. Befehle: `--toggle`
(zeigen/verbergen), `--edit`, `--lock`, `--click`, `--details`, `--history`, `--settings`,
`--reload`, `--quit`; für Tastenkürzel die Starter „NexBoard Overlay ein/aus“ und „… bearbeiten“
in den Systemeinstellungen auf eine Taste legen. Monitor (Standard: Hauptmonitor laut KDE),
Startmodus, Namenseffekt und Widgets stehen in der Web-UI; das Layout liegt in
`~/.config/deckbar/overlay/layout.json`.

Ohne layer-shell (GNOME, X11 oder ohne `layer-shell-qt`) startet automatisch das frühere
Meter-Fenster unter XWayland (`--legacy`): KWin-Regel für die Ebene „Overlay“, Durchklicken per
X11-Input-Shape, ohne die zusätzlichen Widgets.

So funktioniert es: Der Daemon liefert auf Port 8766 (nur localhost) die Overlay-Seite
(`daemon/deckbar_daemon/web/overlay/`) und die Oberfläche aus `capture/a2tools` aus, mit einem
kleinen Ersatz für Tauris JavaScript-API davor (`web/a2shim.js`). Daten-Befehle gehen an
`deckbar-capture` (`a2ui.py`), Fenster-Befehle an die Overlay-Seite bzw. das Programm. Die
Einstellungen der Meter-Oberfläche liegen in `~/.config/deckbar/a2-settings.json`. Nicht dabei sind
die Online-Funktionen, Plugins und die eigenen globalen Tastenkürzel des Meters.

**Alternative: A2Tools-Meter selbst.** Läuft das Meter, kann die NexBoard stattdessen dessen
Stream-Overlay lesen (Quelle „A2Tools-Meter“, `/overlay/events`, Server-Sent Events). Dafür im
Meter das Stream-Overlay einschalten. Port und Schlüssel liest der Daemon aus
`~/.local/share/com.a2tools.dps-meter/settings.json`, sonst von Hand aus der Overlay-URL.

Die Einstellungen liegen in `~/.config/deckbar/dps.yaml` (Modus 600, enthält ggf. den Schlüssel).

### Dungeon-Runs nach Discord

Nach jedem Dungeon-Run postet der Daemon ein Bild mit dem Schaden pro Spieler und Boss an einen
Discord-Webhook (Modul „AION 2 · Dungeon-Runs“, gespeichert in `~/.config/deckbar/runs.yaml`).
Quelle ist die Kampf-History, je nach DPS-Quelle die von `deckbar-capture` oder die des Meters. Ein Run endet, sobald der Endboss liegt, bei einem
Dungeonwechsel oder nach einer einstellbaren Zeit ohne Bosskampf (Details in
`daemon/deckbar_daemon/runs.py`). Die Vorschau und „Letzten Run jetzt posten“ helfen beim Einrichten.

Die History maskiert fremde Namen. Die NexBoard lernt die vollen Namen aus dem Live-Stream und
speichert sie nur lokal (`runs-state.json`). In der Web-UI lassen sie sich von Hand korrigieren.

## Discord-Bot

Das Modul „Discord-Bot“ schickt Ergebnisse an einen Bot, der sie postet, etwa
TheWashBot. Beides ist aus, bis eine Adresse eingetragen
ist, und schickt Charakternamen der Mitspieler an den Server des Bots; nur mit Absprache der Gruppe
einschalten. Einstellungen in `~/.config/deckbar/bot.yaml` (Modus 600).

- **Gruppen-DPS:** Endet ein Kampf, geht der Endstand der eigenen Gruppe (nicht aller Spieler in
  der Nähe) per POST an die Webhook-URL des Bots, Format in `daemon/deckbar_daemon/bot.py`. Filter:
  nur Bosse, nur besiegte, Mindestdauer, Mindestgröße der Gruppe. Wer zur Gruppe gehört, weiß
  `deckbar-capture` ab 0.2 aus dem Gruppen-Roster des Spiels (`party` in `capture/src/view.rs`).
- **Dungeon-Runs:** Nach jedem Run (Modul „Dungeon-Runs“) zusätzlich ein POST mit Bearer-Token an
  die Run-API des Bots. Dungeon- und Bossnamen sind englisch und kommen mit IDs, damit der Bot
  Meldungen verschiedener Spieler zum selben Run zusammenführen kann.

Beide Wege lassen sich mit „Letzten Kampf senden“ bzw. „Letzten Run senden“ prüfen.

## Updates

Der Daemon sieht kurz nach dem Start und dann täglich nach, ob es neue Commits der NexBoard auf
GitHub gibt oder ein neues Release des A2Tools-Meters (nach Spiel-Patches ändern sich dort Opcodes
und Datentabellen). Dann meldet sich das Tray-Symbol des Overlays einmal mit einer Benachrichtigung,
im Tray-Menü steht „Jetzt aktualisieren“, in der Web-UI (Modul „Updates“) ein Hinweis mit Button.
Abschalten über das Modul „Updates“.

Der Button öffnet ein Konsole-Fenster mit `./update.sh`, von Hand geht es genauso:

```sh
./update.sh             # git pull, Submodul, deckbar-capture nur bei Änderungen neu bauen, Neustart
./update.sh --a2tools   # zusätzlich A2Tools auf das neueste Release heben (lokal, bis es im Repo nachgezogen ist)
```

Muss `deckbar-capture` neu gebaut werden, fragt das Skript per `sudo` nach dem Passwort für die
Mitschnitt-Rechte. Bei lokalen Änderungen im Repo bricht es ab, statt etwas zu überschreiben.
Wer das Repo pflegt, zieht ein neues A2Tools-Release mit `git -C capture/a2tools checkout vX.Y.Z`
und einem Commit für alle nach.

## Firmware

Zwei Board-Versionen, erkennbar am QC-Label:

| Board | Umgebung     | Unterschiede |
|-------|--------------|--------------|
| V2 (PCB „Rev1.1“, Label „V2“) | `deckbar` (Standard) | LCD-Reset und Backlight-Freigabe über den IO-Expander TCA9554, Backlight-PWM GPIO42, GPIO21 = TE-*Ausgang* des Displays |
| V1 (ohne „V2“) | `deckbar-v1` | LCD-Reset GPIO21, Backlight-PWM GPIO8 (invertiert), kein Expander |

Pins und Treiber stammen aus <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49-V2> bzw.
dem älteren Repo ohne „-V2“ für V1. Die beiden nicht verwechseln: Mit V1-Pins würde die Firmware
auf V2 den TE-Ausgang des Displays treiben.

```sh
cd firmware
cp include/secrets.example.h include/secrets.h     # WLAN + Token eintragen (gitignored)
./flash.sh                                         # V2; ./flash.sh deckbar-v1 für V1
pio device monitor
```

`flash.sh` hält den Daemon während des Flashens an, weil die USB-Brücke den Port belegt, und
fragt nach dem Port, wenn mehrere ESP32 angeschlossen sind (fest: `PORT=/dev/serial/by-id/… ./flash.sh`).
Klappt der automatische Reset nicht: BOOT gedrückt halten und RESET tippen. Danach bleibt das
Board im Download-Modus, bis es ohne BOOT neu eingesteckt wird.

Das letzte Layout liegt im NVS, damit es nach einem Neustart sofort erscheint. Im seriellen Log
stehen Netzstatus, alle 5 s RSSI und Kanal sowie beim Start die gefundenen WLANs. Weitere
Einstellungen in `include/deckbar_config.h`, auch per `PLATFORMIO_BUILD_FLAGS="-D…"`:

| Define                 | Standard | Bedeutung |
|------------------------|----------|-----------|
| `DECKBAR_ROTATION`     | 270      | Steht das Bild auf dem Kopf: 90 (Touch wird mitgedreht) |
| `DECKBAR_BRIGHTNESS`   | 255      | Backlight 0–255 |
| `DECKBAR_PERF_MONITOR` | 0        | FPS-/CPU-Anzeige von LVGL |
| `DECKBAR_TOUCH_DEBUG`  | 0        | Rohdaten des Touch-Controllers und gesendete Nachrichten seriell ausgeben |
| `DECKBAR_TOUCH_GHOST_*`| aus      | Rechteck, in dem Touch-Punkte verworfen werden (für Boards mit Geisterpunkt) |

**Gut zu wissen:**

- Der AXS15231B nimmt per QSPI nur ganze Frames ab Zeile 0 an. LVGL rendert deshalb im
  DIRECT-Modus, `display.c` überträgt pro Frame das ganze Bild (etwa 14 ms).
- Der Touch-Controller liefert gelegentlich ungültige Frames. Sie werden verworfen, Sprünge
  gefiltert. Geisterklicks traten bisher nur an einer bestimmten Stromversorgung auf.
- In den Glasverläufen sind leichte RGB565-Stufen sichtbar, die Hintergrundbilder werden deshalb
  mit Dithering gewandelt.

## Simulator

```sh
cd simulator
cmake -S . -B build -G Ninja      # lädt LVGL 9.3.0 beim ersten Mal
cmake --build build
./build/deckbar-sim               # Token kommt automatisch aus ~/.config/deckbar/config.yaml
```

Optionen: `--url ws://host:8765/ws`, `--zoom 3`, `--no-perf` (FPS-Anzeige aus), `--no-cache`,
`-v` (gesendete Nachrichten anzeigen). Für automatische Tests gibt es `--gesture "x,y@ms"`,
`"x,y@ms+halten"` und `"x,y>x2,y2@ms"` sowie `--screenshot bild.ppm --after 1500`. Ohne Fenster:
`SDL_VIDEODRIVER=offscreen ./build/deckbar-sim --screenshot …`.

Der Simulator speichert das letzte Layout wie das Gerät (`~/.cache/deckbar-sim/layout.json`). Er
zeigt es also sofort nach dem Start an und blendet „Suche Daemon …“ ein, bis die Verbindung steht.

## Assets neu erzeugen

```sh
uv run --with pillow tools/gen_assets.py              # Schriften + Icons + Hintergrund
uv run --with pillow tools/gen_assets.py --bg foto.jpg --only bg   # eigenes Hintergrundbild
```

Zusätzliche Icons trägt man in `tools/icons.txt` ein (Namen aus
<https://pictogrammers.com/library/mdi/>). Das Skript lädt die Quellen einmalig nach
`tools/.cache/` und braucht `npx` für `lv_font_conv`.

## Tests

```sh
cd daemon && uv run python -m pytest -q
```

## Lizenz

NexBoard steht unter der **GNU General Public License v3.0 oder später** (`LICENSE`).

Teile stammen aus dem [A2Tools DPS Meter](https://github.com/taengu/A2Tools-DPS-Meter) (GPL-3.0):
der Mitschnitt-Kern und die Oberfläche des Overlays als unverändertes Submodul `capture/a2tools`,
die Zeilenaufbereitung in
`capture/src/view.rs` (nach `stream_overlay/snapshot.rs`), die Klassen-Icons in
`daemon/deckbar_daemon/assets/classes/` und das Design der Dungeon-Run-Bilder (nach `src/styles.css`). Übernommene Dateien sind an Ort und Stelle gekennzeichnet, inklusive der
Änderungen.

Weitere Fremdbestandteile:

- Material Design Icons (Pictogrammers, Apache 2.0 / OFL)
- Inter (OFL 1.1)
- cJSON (MIT)
- Treiber in `firmware/src/drivers` und `firmware/src/touch`: Espressif, Apache 2.0
