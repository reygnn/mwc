# wayrdp – Entwurf

Nov 9, 2026 · @Uli

## Ziel und Abgrenzung

wayrdp ist ein eigenständiger RDP-Server für wlroots-Compositoren. Er ersetzt xrdp + wayvnc: mstsc verbindet sich über den Bitvise-Tunnel direkt mit wayrdp, und wayrdp holt das Bild von mwc über Wayland-Protokolle. Grundlage ist ein Fork von wayvnc 0.10.2 (ISC-Lizenz), dessen VNC-Schicht (neatvnc) durch FreeRDP 3 ersetzt wird.

**Ziele für v1**

- mstsc unter Windows 10/11 als Client, ohne zusätzliche Software auf dem Notebook
- eine dauerhafte Sitzung: Trennen und Wiederverbinden zeigt denselben Desktop
- Zwischenablage mit vollem Unicode, in beide Richtungen
- die Bildschirmgröße folgt dem mstsc-Fenster
- Anmeldung per NLA, Lauschen nur auf localhost
- mwc selbst verarbeitet keine Daten aus dem Netz

**Nicht-Ziele für v1**

- mehrere Benutzer oder ein Session-Broker: eine wayrdp-Instanz pro Benutzer, wie bei wayvnc
- Audio, Laufwerks- und Druckerumleitung, Dateien über die Zwischenablage
- mehrere Monitore
- H.264 über die GFX-Pipeline (Kandidat für v2)

## Architektur

> *Diagramm: Architektur · Notebook, wayrdp, mwc (nur im Doc)*

Das Bild läuft links von mwc über den Encoder zum Peer, Eingaben, Zwischenablage und Größe laufen rechts zurück. Nur die blauen Teile sind neu.

wayrdp ist ein einzelner Prozess pro Benutzer, ein normaler Wayland-Client von mwc. Er läuft als systemd-User-Dienst neben `mwc.service` und löst xrdp und wayvnc ab. Stürzt wayrdp ab, läuft die mwc-Sitzung weiter; systemd startet wayrdp neu.

- **Event-Loop:** aml, wie in wayvnc. Der Wayland-Socket, die Dateideskriptoren des FreeRDP-Listeners, des Peers und des Kanal-Managers hängen als `aml_handler` an derselben Schleife.
- **Threads:** Das Kodieren der Frames läuft als `aml_work` in einem Worker-Thread. Der Hauptthread nimmt Eingaben und Wayland-Ereignisse ohne Verzögerung an.
- **Verbindungen:** genau eine aktive RDP-Verbindung. Eine neue Verbindung löst die alte ab, wie beim Wiederverbinden mit mstsc.

## Wayland-Seite

Die Wayland-Seite kommt fast unverändert aus wayvnc 0.10.2. Sie nutzt nur Protokolle, die mwc anbietet. Die beiden für Mauszeiger und Größenänderung sind seit mwc bf8e9a1 da; wlr-output-management allerdings nur mit `Mwm*outputManagement: true` in mwmrc (Abschnitt *Dynamische Größe und Mauszeiger*).

| Zweck | Protokoll | in mwc | wayvnc-Code |
| --- | --- | --- | --- |
| Bild holen | wlr-screencopy-unstable-v1 | vorhanden | `screencopy.c`, `buffer.c`, `shm.c` |
| Bild und Mauszeiger holen | ext-image-copy-capture-v1 + ext-image-capture-source-v1 | vorhanden (seit bf8e9a1) | `ext-image-copy-capture.c` |
| Tastatur | virtual-keyboard-unstable-v1 | vorhanden (`MWC_VIRTUAL_INPUT=1`) | `keyboard.c` |
| Maus | wlr-virtual-pointer-unstable-v1 | vorhanden (`MWC_VIRTUAL_INPUT=1`) | `pointer.c` |
| Zwischenablage | ext-data-control-v1, wlr-data-control-unstable-v1 | vorhanden | `data-control.c` |
| Größe ändern | wlr-output-management-unstable-v1 | vorhanden (seit bf8e9a1), nur mit `Mwm*outputManagement: true` | `output-management.c` |
| Ausgaben finden | xdg-output-unstable-v1 | vorhanden | `output.c` |

Die Bildquelle bleibt wayvncs Abstraktion `screencopy-interface.c`: Sie liefert Puffer mit Damage-Region, egal ob über screencopy oder ext-image-copy-capture. Sie wählt ext-image-copy-capture, sobald mwc es zusammen mit ext-image-capture-source-v1 anbietet, sonst screencopy. Den Mauszeiger als eigenes Bild liefert nur ext-image-copy-capture.

## RDP-Seite

Die RDP-Seite baut auf der Server-Bibliothek von FreeRDP 3 auf, die Ubuntu 26.04 mitliefert (`freerdp3-dev` mit `libfreerdp-server3-3`, für amd64 derzeit Version 3.32.1 über resolute-updates, [Paketseite](https://packages.ubuntu.com/resolute-updates/freerdp3-dev)). Ziel ist FreeRDP ≥ 3.24, die älteste Version, die 26.04 noch auf einigen Architekturen führt ([Paketseite](https://packages.ubuntu.com/resolute/freerdp3-dev)).

- **Listener:** `freerdp_listener_new()` lauscht auf Adresse und Port aus der Konfiguration und nimmt Verbindungen an.
- **Peer:** Pro Verbindung ein `freerdp_peer` mit eigenem Kontext. Die Callbacks `Logon`, `Capabilities`, `PostConnect` und `Activate` führen durch den Verbindungsaufbau, `input->KeyboardEvent`, `MouseEvent` und Co. liefern die Eingaben.
- **Kanäle:** Ein Virtual Channel Manager trägt die Zwischenablage (`cliprdr`) und über `drdynvc` die Größensteuerung (`disp`).

| Einstellung | Wert in v1 | Grund |
| --- | --- | --- |
| `NlaSecurity` / `TlsSecurity` | NLA, wahlweise nur TLS | siehe *Sicherheit und Anmeldung* |
| `RdpSecurity` | aus | veraltete RDP-eigene Verschlüsselung |
| `ColorDepth` | 32 | Ausgabe ist XRGB8888 |
| `RemoteFxCodec` | an | Hauptcodec, siehe *Bildpfad* |
| `SurfaceFrameMarkerEnabled`, `FrameMarkerCommandEnabled` | an | Frames klammern, Flusskontrolle |
| `SupportGraphicsPipeline` | aus | GFX/H.264 erst in v2 |
| `SupportDisplayControl` | an | Größe folgt dem mstsc-Fenster |
| `DesktopResize` | an | Größenwechsel ohne Neuverbindung |
| `RedirectClipboard` | an | Zwischenablage |

Vorlagen für diese Schicht sind FreeRDPs Beispielserver (`server/Sample`) und Shadow-Server, gnome-remote-desktop, Westons RDP-Backend und der 2020 entfernte RDP-Backend von wlroots 0.9.1 (1.273 Zeilen).

## Bildpfad

Jeder Frame geht als RemoteFX in SurfaceBits-Befehlen hinaus, und nur die geänderten 64×64-Kacheln werden kodiert. Die Bildrate richtet sich nach den Bestätigungen des Clients, nicht nach mwc.

1. **Erfassen:** `screencopy-interface.c` liefert nach jedem Commit von HEADLESS-1 einen Puffer (shm, XRGB8888) und die Damage-Region. Das Ratenlimit bleibt wie in wayvnc (`max_rate`, Standard 30 fps).
2. **Übergeben:** Wo wayvnc `nvnc_display_feed_buffer()` aufruft, ruft wayrdp `rdp_frame_submit(buffer, damage)` auf. Die Damage wird auf das 64er-Kachelraster gerundet und mit noch nicht gesendeter Damage vereinigt.
3. **Kodieren:** Ein `aml_work` im Worker-Thread kodiert die Kacheln mit `rfx_encode_messages()`. Der Puffer bleibt so lange reserviert und geht danach zurück in den Pool.
4. **Senden:** Im Hauptthread folgen Frame-Marker *Begin*, die SurfaceBits-Befehle und der Frame-Marker *End* mit fortlaufender Frame-ID.
5. **Flusskontrolle:** Höchstens zwei Frames dürfen unbestätigt sein, sofern der Client Frame Acknowledge unterstützt. Danach pausiert das Erfassen, und Damage sammelt sich an, bis eine Bestätigung kommt.

**Sonderfälle**

- *SuppressOutput* (mstsc minimiert): Erfassen stoppt, beim Wiederanzeigen geht ein ganzer Frame hinaus.
- *RefreshRect* und *Synchronize*: die angefragte Fläche bzw. der ganze Bildschirm wird als Damage markiert.
- Client ohne RemoteFX: Bitmap-Updates mit Planar-Kompression, langsamer, aber von jedem Client unterstützt.

## Eingabe

RDP schickt physische Scancodes. wayrdp übersetzt sie in evdev-Keycodes und speist sie über wayvncs `keyboard_feed_code()` ein. Das Tastaturlayout bleibt auf dem Server, mit `-k de` wie heute bei wayvnc.

| RDP-Ereignis | Umsetzung in wayrdp |
| --- | --- |
| `KeyboardEvent` (Scancode, Flags *extended*/*release*) | `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()` → `keyboard_feed_code()` |
| Pause/Break (Sequenz mit *extended1*) | eigener Sonderfall → `KEY_PAUSE` |
| `SynchronizeEvent` (Caps, Num, Scroll Lock) | mit `keyboard_get_led_state()` vergleichen, abweichende Lock-Tasten einmal drücken. Zusätzlich alle gedrückten Tasten loslassen, damit nach Alt-Tab auf dem Notebook nichts hängen bleibt |
| `UnicodeKeyboardEvent` (Bildschirmtastatur, IME) | `xkb_utf32_to_keysym()` → `keyboard_feed()`. Zeichen, die das Layout nicht hat, fallen in v1 weg |
| `MouseEvent` mit *move* | absolute Position → `motion_absolute` über die Ausgabegröße |
| Tasten 1/2/3, `ExtendedMouseEvent` X1/X2 | `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`, `BTN_SIDE`, `BTN_EXTRA` |
| Rad vertikal/horizontal (*wheel*, *hwheel*) | 9-Bit-Wert mit Vorzeichen-Flag, 120 = eine Raste. Ganze Rasten als `axis_discrete`, Bruchteile hochauflösender Räder als `axis`. Vertikal mit umgekehrtem Vorzeichen |

`pointer.c` aus wayvnc denkt in VNC-Tastenmasken und ganzen Rasten. Es bekommt eine kleine neue Schnittstelle: Bewegung, einzelne Taste, Achse mit Wert in 120stel Rasten.

Das Layout könnte wayrdp auch aus der `KeyboardLayout`-ID von mstsc ableiten, etwa 0x407 für Deutsch. Das ist für v2 vorgesehen; in v1 gilt `-k`.

## Zwischenablage

Text geht als `CF_UNICODETEXT` (UTF-16) ↔ `text/plain;charset=utf-8`, also mit vollem Unicode statt Latin-1. RDP und Wayland liefern Inhalte beide erst auf Anfrage. wayrdp verbindet die beiden Anfragen und kopiert nichts vorab.

**Notebook → Server**

1. mstsc meldet eine neue Zwischenablage mit einer *Format List*, darin `CF_UNICODETEXT`.
2. wayrdp setzt über data-control eine eigene Auswahl mit `text/plain;charset=utf-8`, `UTF8_STRING`, `TEXT` und `STRING`.
3. Fügt ein Programm ein, ruft mwc `send(mime, fd)` dieser Quelle auf.
4. wayrdp schickt einen *Format Data Request*, wandelt die Antwort von UTF-16 in UTF-8 und schreibt sie nicht blockierend in den fd.

**Server → Notebook**

1. Ein Programm kopiert, data-control meldet ein neues Angebot mit Text-MIME-Typ.
2. wayrdp schickt eine *Format List* mit `CF_UNICODETEXT`, ohne Inhalt.
3. mstsc fragt mit *Format Data Request* nach.
4. wayrdp liest das Angebot über eine Pipe, wie `data-control.c` es heute tut, wandelt UTF-8 in UTF-16 und antwortet mit *Format Data Response*.

- Die eigene Auswahl aus Schritt 2 meldet wayrdp nicht an mstsc zurück, sonst entsteht ein Echo.
- Antwortet die Gegenseite nicht, schließt wayrdp den fd nach einer Wartezeit. Inhalte haben eine Obergrenze (Vorschlag: 16 MiB).
- RDP kennt keine Primary Selection; die bleibt lokal in mwc.
- v2: Bilder als `CF_DIB` ↔ `image/png`. Dateien sind nicht vorgesehen.

## Dynamische Größe und Mauszeiger

Beides braucht je ein Protokoll, das mwc seit bf8e9a1 anbietet. Davon profitiert auch wayvnc. wlr-output-management ist allerdings standardmäßig aus und muss in mwmrc eingeschaltet werden, siehe *Risiko: Absturz in wlroots* unten.

**Größe folgt dem mstsc-Fenster**

1. mstsc schickt über den `disp`-Kanal ein Monitor-Layout, beim Verbinden und bei jeder Fensteränderung.
2. wayrdp ruft wayvncs `wlr_output_manager_resize_output()` auf, das über wlr-output-management einen eigenen Modus für HEADLESS-1 setzt.
3. mwc übernimmt den Modus und ordnet das Layout neu, wie heute bei jedem Moduswechsel.
4. Der erste Puffer in neuer Größe setzt `DesktopWidth`/`DesktopHeight` und löst beim Client *DesktopResize* aus. Danach geht ein ganzer Frame hinaus.

- Beim Ziehen des Fensters kommen viele Layouts. wayrdp wendet nur das letzte an, nach einer kurzen Ruhezeit (Vorschlag: 200 ms).
- MS-RDPEDISP erlaubt Breiten und Höhen von 200 bis 8192, die Breite gerade. Andere Werte werden gerundet.
- mwc nimmt weniger als MS-RDPEDISP: höchstens 8192×4320 Pixel (rund 135 MiB je Puffer), höchstens 16384 je Seite, eine Bildrate von 0 (Vorgabe des Backends) oder mindestens 1 Hz, und mindestens ein logisches Pixel. 8192×8192 lehnt mwc also ab. wayrdp rundet deshalb auf mwcs Grenze ab und behandelt ein `failed` sauber: alte Größe behalten, Client nicht trennen.
- Was ein Client gesetzt hat, bleibt in mwc über ein Neuladen erhalten, bis mwmrc für `HEADLESS-1` etwas Neues sagt. Ein geänderter `outputMode` gewinnt dann gegen die Größe von wayrdp.
- Clients ohne `disp` bekommen die Größe, die sie beim Verbinden angeben. `outputMode` in mwmrc bleibt die Startgröße ohne Verbindung.

**Risiko: Absturz in wlroots**

- wlroots 0.19 und 0.20 brechen in `head_send_state` ab (`assert(found)`), wenn ein Client von wlr-output-management ein Objekt für einen Custom-Modus hat, das einem anderen fehlt. Headless-Ausgaben haben nur Custom-Modi, und genau so einen setzt wayrdp. Der Abbruch reißt die ganze mwc-Sitzung mit, nicht nur wayrdp.
- Reproduziert wurde er immer mit einem zweiten Client (etwa `wlr-randr` neben einem dauerhaft verbundenen Client, oder einem, der sich bindet, während der Monitor aus ist) oder mit einem Client, der sein Modus-Objekt freigibt. Mit wayrdp als einzigem Client des Protokolls ist kein Weg dorthin bekannt; geprüft ist das nicht.
- Behoben ist es nur auf wlroots-master (40640950, 2026-09-30), in keinem Release. Deshalb bietet mwc das Protokoll nur mit `Mwm*outputManagement: true` an.
- Für wayrdp heißt das: den Schalter bewusst setzen, wayrdp als einzigen Client des Protokolls laufen lassen (kein kanshi, kein wlr-randr daneben) und das in der Doku sagen. Auf Dauer entweder ein wlroots-Release mit dem Fix abwarten oder den Fix in mwc mitführen.

**Mauszeiger beim Client**

- Ohne Ergänzung in mwc zeichnet screencopy den Zeiger ins Bild, wie heute mit `wayvnc -r`. Er zieht dann bei jeder Bewegung um eine Netzlaufzeit nach.
- Mit ext-image-copy-capture liefert eine Cursor-Sitzung Bild, Hotspot und Position. wayrdp schickt `PointerNew` bzw. `PointerLarge` (32 bpp mit Alpha) und bei Wiederholung `PointerCached`. mstsc zeichnet den Zeiger dann selbst, ohne Verzögerung. wayvnc 0.10 hat diesen Weg schon (`on_cursor_capture_done`).
- wlroots 0.19 hat Cursor-Sitzungen. Sie lesen den Hardware-Cursor der Ausgabe, und den hat die Headless-Ausgabe; mwcs `tests/capturec.c` prüft Bild, Hotspot und Position.
- Haken: Nach jeder Aufnahme, die den Zeiger eingemalt haben will (ext-image-copy-capture mit `paint_cursors`, wlr-screencopy mit `overlay_cursor`, also auch `-r`, `grim -c` oder `screenShot -p`), zeichnet wlroots den Zeiger in Software. Die Cursor-Sitzung bekommt dann kein Bild, bis sich das Zeigerbild ändert, über einem Client-Fenster unter Umständen lange. wayrdp mischt die beiden Wege deshalb nicht, und ein einzelnes `screenShot -p` in der Sitzung kann den Zeiger beim Client stören (in mwcs `TODO`).
- Legt wayrdp seine Aufnahmequelle an, während der Monitor über wlr-output-power aus ist, bleibt sie in wlroots 0.19 bei 0×0 (in 0.20.2 behoben). wayrdp startet mit mwc, also normalerweise bei eingeschaltetem Monitor; *SuppressOutput* berührt das nicht.

| In mwc seit bf8e9a1 | wlroots-Baustein | wofür |
| --- | --- | --- |
| wlr-output-management-unstable-v1, nur mit `Mwm*outputManagement: true` | `wlr_output_manager_v1`, mit Test- und Apply-Handler über `wlr_backend_test`/`wlr_backend_commit` | Größe |
| ext-image-copy-capture-v1 + ext-image-capture-source-v1 | `wlr_ext_image_copy_capture_manager_v1`, `wlr_ext_output_image_capture_source_manager_v1` | Bild und Zeiger |

Jede Größenänderung legt bei headless mit pixman neue Puffer an. Der shm-Allocator von wlroots 0.19.2 gibt sie frei, ohne den Renderer zu benachrichtigen; mwc bringt dafür seit 1ee7c73 einen eigenen mit (`src/shmalloc.c`, für wlroots vor 0.19.3).

## Sicherheit und Anmeldung

wayrdp lauscht nur auf localhost und ist nur über den SSH-Tunnel erreichbar. Die RDP-Anmeldung ist eine zweite Hürde, nicht die einzige.

- **Adresse:** Standard `127.0.0.1`. Eine andere Adresse nur per Option, mit Warnung im Log, wenn sie nicht lokal ist.
- **TLS:** Ein selbstsigniertes Zertifikat entsteht beim ersten Start in `~/.config/wayrdp/` (`tls.crt`, `tls.key`, Rechte 0600). Der Fingerabdruck steht im Log, damit man ihn mit der Warnung von mstsc vergleichen und dort dauerhaft annehmen kann.
- **Benutzer:** Angenommen wird nur der Benutzer, unter dem wayrdp läuft.
- **Fehlversuche:** Nach drei Fehlversuchen nimmt wayrdp für 30 Sekunden keine Anmeldung an (Vorschlag).

| Anmeldemodus | Ablauf | Vorteil | Nachteil |
| --- | --- | --- | --- |
| `nla` (Standard) | CredSSP/NTLM gegen eine SAM-Datei mit dem NT-Hash eines eigenen RDP-Passworts. `wayrdp --set-password` schreibt sie mit Rechten 0600 | Standard bei mstsc, kein Anmeldefenster nötig | eigenes Passwort; der NT-Hash ist ungesalzen, die Datei muss geschützt bleiben |
| `pam` | nur TLS. mstsc schickt die gespeicherten Anmeldedaten mit, wayrdp prüft sie über PAM (Dienst `wayrdp`) | das Linux-Passwort wie heute bei xrdp | mstsc warnt wegen fehlender NLA. Ohne gespeicherte Anmeldedaten wird die Verbindung abgelehnt, denn wayrdp hat keine Anmeldemaske |

**Grenzen**

- `MWC_VIRTUAL_INPUT=1` bleibt nötig. Damit kann jedes Programm in der Sitzung Tastatur und Maus fernsteuern, wie heute mit wayvnc.
- Auch die Aufnahmeprotokolle und, wenn eingeschaltet, wlr-output-management stehen jedem Programm der Sitzung offen (mwcs README). mwc kennt kein security-context-v1.
- FreeRDP verarbeitet die Netzwerkdaten im wayrdp-Prozess, nicht in mwc. Die systemd-Unit härtet ihn (`ProtectSystem=strict`, `PrivateTmp`, `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`, Schreibrechte nur für `~/.config/wayrdp`).
- Im Modus `pam` darf die Unit kein `NoNewPrivileges=yes` setzen: PAM prüft das eigene Passwort über das setgid-Hilfsprogramm `unix_chkpwd`.

## Konfiguration, Kommandozeile, systemd

wayrdp übernimmt Kommandozeile und Konfigurationsdatei von wayvnc, wo es passt. Während des Übergangs läuft es auf Port 3390 neben xrdp; später übernimmt es 3389, und xrdp fällt weg.

```
wayrdp [Optionen] [Adresse [Port]]       Standard: 127.0.0.1 3389
  -C, --config=PFAD        Konfiguration, Standard ~/.config/wayrdp/config
  -k, --keyboard=LAYOUT    Tastaturlayout, z. B. de oder de-nodeadkeys
  -o, --output=NAME        Ausgabe, z. B. HEADLESS-1
  -a, --auth=nla|pam       Anmeldemodus, Standard nla
  -f, --max-fps=N          höchste Bildrate, Standard 30
  -r, --render-cursor      Zeiger ins Bild zeichnen
  -L, --log-level=STUFE    error, warning, info, debug, trace
      --set-password       RDP-Passwort für nla setzen und beenden
```

| Schlüssel in `config` | Standard | Bedeutung |
| --- | --- | --- |
| `address` | `127.0.0.1` | Adresse zum Lauschen |
| `port` | `3389` | Port |
| `auth` | `nla` | `nla` oder `pam` |
| `sam_file` | `~/.config/wayrdp/sam` | NT-Hash für `nla` |
| `certificate_file`, `private_key_file` | `~/.config/wayrdp/tls.crt`, `tls.key` | TLS, beim ersten Start erzeugt |
| `keyboard` | `us` | wie `-k` |
| `output` | erste Ausgabe | wie `-o` |
| `max_rate` | `30` | wie `-f` |

`~/.config/systemd/user/wayrdp.service`, im Übergang auf Port 3390:

```ini
[Unit]
Description=wayrdp for mwc
BindsTo=mwc.service
After=mwc.service

[Service]
Environment=WAYLAND_DISPLAY=wayland-0
ExecStart=/usr/local/bin/wayrdp -k de 127.0.0.1 3390
Restart=on-failure
RestartSec=1
ProtectSystem=strict
PrivateTmp=yes
ReadWritePaths=%h/.config/wayrdp
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
NoNewPrivileges=yes

[Install]
WantedBy=mwc.service
```

**Übergang mit Bitvise**

1. In mwmrc `Mwm*outputManagement: true` setzen und mwc neu starten (der Schalter gilt nur beim Start). Dann wayrdp auf 3390 starten, xrdp und wayvnc bleiben auf 3389 bzw. 5900.
2. In Bitvise eine zweite C2S-Regel `127.0.0.1:13390 → localhost:3390` anlegen und `mstsc /v:127.0.0.1:13390` verbinden.
3. Läuft wayrdp im Alltag: xrdp und wayvnc stoppen und entfernen, wayrdp auf 3389 umstellen. Der RDP-Tab von Bitvise funktioniert dann wieder ohne eigene Regel.

## Code-Struktur und Fork-Plan

wayrdp entsteht als eigenes Repository, ein Fork von `any1/wayvnc` ab Tag v0.10.2 mit Historie. wayvnc hat dort rund 12.500 Zeilen in `src/`. Knapp die Hälfte davon, rund 6.000 Zeilen, bleibt fast unverändert. Neu kommt ein Verzeichnis `src/rdp/` mit etwa 1.800 Zeilen (Schätzung).

| Teil | Dateien | Umgang |
| --- | --- | --- |
| Wayland, Erfassen, Puffer | `wayland.c`, `output.c`, `seat.c`, `screencopy*.c`, `ext-image-copy-capture.c`, `image-source.c`, `buffer.c`, `shm.c`, `pixels.c`, `transform-util.c`, `desktop.c`, `toplevel.c` | bleibt |
| Größe | `output-management.c` | bleibt |
| Tastatur | `keyboard.c` | bleibt, LED-Status ohne neatvnc-Typ |
| Maus | `pointer.c` | neue Schnittstelle (Taste, Achse in 120steln) |
| Zwischenablage Wayland-Seite | `data-control.c` | Senke statt `nvnc_send_cut_text()` → `rdp/clipboard.c` |
| Konfiguration | `cfg.c`, `option-parser.c` | bleibt, neue Schlüssel |
| Hauptprogramm | `main.c` (2.820 Zeilen, rund 250 neatvnc-Aufrufe) | Klebecode weitgehend neu |
| Entfällt in v1 | neatvnc-Subprojekt, `pam_auth.c`, `ctl-*.c`, `json-ipc.c`, `wayvncctl.c`, `table-printer.c` | Steuer-Socket kann später zurückkommen |

| Neu in `src/rdp/` | Inhalt | Zeilen (Schätzung) |
| --- | --- | --- |
| `listener.c`, `peer.c` | Listener, Peer-Lebenszyklus, Settings, aml-Anbindung | 350 |
| `encoder.c` | RemoteFX und Planar, Kacheln, Frame-Marker, Flusskontrolle | 300 |
| `input.c` | Scancodes, Synchronize, Unicode, Maus, Rad | 250 |
| `clipboard.c` | cliprdr ↔ data-control, UTF-8/UTF-16 | 400 |
| `display.c` | `disp`-Kanal, DesktopResize, Entprellen | 150 |
| `cursor.c` | Zeigerbilder, Cache | 150 |
| `auth.c` | SAM-Datei, PAM, Zertifikat erzeugen | 200 |

- **Logging:** Die behaltenen Dateien loggen über `nvnc_log()`. Ein eigenes `wv_log()` mit gleicher Signatur ersetzt es per Suchen und Ersetzen.
- **Build:** meson wie bisher. Abhängigkeiten: `freerdp3`, `freerdp-server3`, `winpr3`, `wayland-client`, `xkbcommon`, `pixman-1`, optional `pam`; aml bleibt Subprojekt.
- **Lizenz:** wayvnc steht unter ISC, FreeRDP unter Apache-2.0. Beide erlauben den Fork; die Copyright-Hinweise von wayvnc bleiben erhalten.

## Meilensteine und Aufwand

> *Diagramm: Meilensteine M0–M5 · 3 Tore · Aufwand geschätzt (nur im Doc)*

Die Meilensteine laufen nacheinander, zusammen rund 4–5 Wochen (Schätzung). Ab Tor 2 läuft der Alltag über wayrdp, xrdp und wayvnc bleiben bis M5 als Rückfallebene.

**Tests**

- **Automatisch:** In CI starten mwc headless und wayrdp. `xfreerdp3` verbindet sich mit `/cert:ignore`, Anmeldetests laufen mit `+auth-only`.
- **Unit-Tests** mit wayvncs `tst.h`: Scancode-Tabelle, Rad-Dekodierung, UTF-8/UTF-16, Rundung der Damage auf Kacheln.
- **Manuell an jedem Tor:** mstsc unter Windows 11 über Bitvise. Bild, Tippen mit Umlauten und AltGr, Rad, Kopieren in beide Richtungen, Fenstergröße ändern, Trennen und Wiederverbinden.

## Risiken und offene Fragen

Das größte Risiko ist, ob das aktuelle mstsc RemoteFX über SurfaceBits ohne GFX-Pipeline sauber annimmt. Das klärt Meilenstein M1 als erstes Tor, bevor weitere Arbeit hineingeht.

| Risiko | Auswirkung | Gegenmaßnahme | geklärt in |
| --- | --- | --- | --- |
| mstsc bevorzugt GFX und behandelt RemoteFX über SurfaceBits schlecht | ruckelndes oder fehlerhaftes Bild | Planar-Fallback; notfalls GFX mit RemoteFX Progressive vorziehen | M1 |
| FreeRDP-Server-API dünn dokumentiert, Änderungen zwischen 3.x-Versionen | Verzögerung | an Beispielserver, Shadow-Server, gnome-remote-desktop und Weston orientieren; gegen FreeRDP ≥ 3.24 bauen und gegen die 26.04-Pakete testen | M0, M1 |
| CPU-Last durch RemoteFX in Software bei 1920×1080 | träge Sitzung | Worker-Thread, nur geänderte Kacheln, höchstens 30 fps; in M1 messen | M1 |
| wlroots 0.19/0.20 bricht in wlr-output-management ab, wenn ein zweiter Client des Protokolls dazukommt | die ganze mwc-Sitzung stürzt ab | wayrdp als einziger Client, `Mwm*outputManagement` bewusst setzen; Fix 40640950 abwarten oder in mwc mitführen | mit der Größensteuerung (`display.c`) |
| eine Aufnahme mit eingemaltem Zeiger (`-r`, `grim -c`, `screenShot -p`) lässt die Cursor-Sitzung ohne Bild | kein oder veralteter Zeiger beim Client | Wege nicht mischen; in mwc das Zeigerbild nach dem Lösen der Software-Sperre neu setzen (`TODO`) | M4 |
| mwc lehnt große Layouts ab (über 8192×4320 Pixel) | Größe folgt dem Fenster nicht | auf mwcs Grenze abrunden, `failed` sauber behandeln | mit der Größensteuerung (`display.c`) |
| NT-Hash in der SAM-Datei ist ungesalzen | Passwort leichter zu knacken, wenn die Datei abhandenkommt | Rechte 0600, nur localhost, eigenes RDP-Passwort | M1 |

**Offene Fragen**

- Kann mwc die virtuellen Eingabeprotokolle nur wayrdp anbieten statt allen Programmen der Sitzung, etwa über einen eigenen Socket oder security-context-v1?
- Eigenes Repository `reygnn/wayrdp` oder ein Verzeichnis im mwc-Repo? Der Entwurf geht von einem eigenen aus, weil wayrdp mit jedem wlroots-Compositor läuft.
- Ist der Name frei? Eine erste Suche fand kein gleichnamiges Projekt.
- Kommt der Steuer-Socket von wayvnc (`wayvncctl`) zurück, etwa zum Trennen eines Clients?

## Quellen

- [wayvnc](https://github.com/any1/wayvnc), Tag v0.10.2: Quellcode, Dateigrößen, Protokolle
- [neatvnc](https://github.com/any1/neatvnc), Tag v1.0.4
- [mwc](https://github.com/reygnn/mwc), Stand bf8e9a1: angebotene Protokolle, Grenzen für Größen, `Mwm*outputManagement`
- wlroots: Commit 40640950 „output-management: filter mode resources by client" (nur master), Release 0.20.2 (Aufnahmequelle hört auf `WLR_OUTPUT_STATE_ENABLED`)
- [wlroots 0.9.1](https://github.com/swaywm/wlroots/tree/0.9.1/backend/rdp): entfernter RDP-Backend
- [freerdp3-dev in resolute-updates](https://packages.ubuntu.com/resolute-updates/freerdp3-dev)
- [freerdp3-dev in resolute](https://packages.ubuntu.com/resolute/freerdp3-dev)
