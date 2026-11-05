# HTTP-/WebSocket-Adapter

Eigenständiges C++17-Programm **`qo100_webbridge`**. Backend und DSP bleiben unverändert.
HTTP und WebSocket liegen auf **Port 8080**; Backend-UDP bleibt lokal auf `127.0.0.1:40900`.

## Start

Zusätzlich unter Debian: `sudo apt install libboost-dev nlohmann-json3-dev`.
Normaler Projektbuild erzeugt `build/bin/qo100_webbridge`. Backend zuerst starten, dann:

```sh
./build/bin/qo100_webbridge --config config/webbridge.conf
```

Nur den Adapter bauen, ohne Pluto-/DSP-Abhängigkeiten:

```sh
cmake -S webbridge -B build-webbridge -DCMAKE_BUILD_TYPE=Release
cmake --build build-webbridge -j2
./build-webbridge/qo100_webbridge --config config/webbridge.conf
```

`config/webbridge.conf`: Listen-Adresse, HTTP-Port, Backend-Port, Dokumentverzeichnis,
Inaktivitätslimit (Standard **300 s**), Verbindungslimit (**16**) und Sendepuffer je Browser
(**512 KiB**). Relative `web_root`-Pfade beziehen sich auf die Config-Datei.
CLI überschreibt die Datei; Optionen zeigt `--help`.

**Einziger UDP-Teilnehmer:** Während des Adapterbetriebs kein natives Test-GUI direkt
am selben Backend betreiben, da eine neue UDP-Anmeldung die bisherige ersetzt.

## OPNsense und Webseite

```text
Browser -- HTTPS/WSS --> OPNsense -- HTTP/WS --> Adapter -- lokales UDP --> Backend
```

OPNsense leitet die Webseite und **`/ws`** an denselben Adapter weiter. WebSocket-Upgrades
zulassen und den ursprünglichen **Host-Header beibehalten**. TLS/Zertifikate liegen bei
OPNsense; der Adapter verwendet ausschließlich HTTP/WS. Origin muss bei Browser-Anfragen
zum Host passen. Zugangsbeschränkungen können im vorgeschalteten Proxy erfolgen.

`/` liefert `web/index.html`, weitere Dateien stammen aus `web_root`; `/health` prüft den
HTTP-Dienst. Für Mikrofon und AudioWorklet die HTTPS-Adresse der OPNsense verwenden;
`http://localhost`/`127.0.0.1` eignet sich ebenfalls für lokale Browsertests.

Die Testseite zeigt beide Wasserfälle, RX1/RX2-Audio, unabhängige Frequenzen, RX1-Filter/NR,
TX-Kompressor, Test-Loops und Settings. **Audio einschalten** startet die Wiedergabe;
**Mikrofon einschalten**, dann **PTT halten** sendet. Wasserfall-Klick stimmt RX1 ab.
Web Audio arbeitet bei 12 kHz und übernimmt die Umrechnung zu Mikrofon/Soundkarte.
Wiedergabe hat einen kleinen adaptiven Jitterpuffer; Audio wird ohne Codec übertragen.

## Rollen und Abschaltung

- Eine **neue WebSocket-Verbindung** erhält bei freiem Steuerplatz die Rolle `operator`.
  Weitere sind `viewer`. Bestehende Zuschauer werden **nie automatisch befördert**.
- Zuschauer erhalten FFT, RX-Audio und Betriebszustand. Kommandos, Settings-Abfragen und
  TX-Audio von Zuschauern werden vor der UDP-Ausgabe abgewiesen. `state` liest nur den Cache.
- Bediener meldet echte Maus-/Tastatur-/Touch-Aktivität mit `activity`, gedrosselt auf etwa
  einmal pro Sekunde. Der Adapter misst selbst: **300 s ohne Activity → Verbindung schließen**.
  Audio, FFT, Heartbeats und PTT-Keepalives zählen nicht als Bedienaktivität.
- Alle Browser können `heartbeat` etwa jede Sekunde senden. Beim Bediener führen **6 s**
  ohne Heartbeat zur Abschaltung. Zuschauer dürfen passiv bleiben; WebSocket-Ping prüft die Verbindung.
- Verlust/Timeout/Shutdown: TX-Berechtigung sofort löschen, Audiopakete verwerfen, **PTT aus,
  Loop normal**. Ein frisch angemeldeter Bediener darf erst nach bestätigter Abschaltung steuern.
  Nach Timeout **keine automatische Wiederanmeldung**; die Testseite zeigt „Verbinden“.
- TX benötigt zusätzlich `ptt_keepalive` etwa alle **250 ms** während der gedrückten Taste.
  Nach **750 ms** ohne Freigabe setzt der Adapter PTT aus. Das Backend hat zusätzlich seine
  eigene 1,5-s-Lease. Ein später Keepalive schaltet PTT nicht wieder ein.
- Bei Backend-Neustart: Session neu ermitteln, Puffer leeren, Zustände neu laden; PTT bleibt aus.
  Langsame Browser erhalten begrenzte Puffer; alte FFT-Fragmente werden verworfen, dauerhaft
  zurückliegende Verbindungen getrennt. Andere Teilnehmer laufen weiter.

Hardware-Settings und Änderungen an `initial_tx_hz` löschen ebenfalls die PTT-Freigabe und
stellen Loops auf Normal. Bis zum Abschluss der Änderung und bestätigter Abschaltung bleibt
`ready=false`; anschließend ist ein neuer bewusster PTT-Tastendruck erforderlich.

## WebSocket-Vertrag

Ein Socket, URL **`/ws`**. Textnachrichten = JSON; Binärnachrichten = genau ein unverändertes
[QTR2-Datagramm](udp-protocol.md). Der Adapter verwaltet selbst UDP-IDs, Wiederholungen,
Subscription und Backend-Sessions. Browser-`id` ist optional, uint32, und wird in ACK/Fehler zurückgegeben.

```js
const url = new URL('/ws', location.href);
url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';
ws.onmessage = e => {
  if (typeof e.data === 'string') console.log(JSON.parse(e.data));
  else { /* QTR2-Header + FFT/PCM aus e.data lesen */ }
};
// Nach role=operator UND state.ready=true:
// ws.send(JSON.stringify({type:'tune',id:1,stream:1,frequency_hz:10489748500}));
```

Alle Frequenzen ganze Hz, RX = Downlink, TX = Uplink. Revisionen und Nanosekunden-Zeitstempel
im JSON sind **Dezimalstrings**, um 64-Bit-Werte verlustfrei zu erhalten.

| Browser → Adapter: `type` | Weitere JSON-Felder |
|---|---|
| `state` | Keine; Cache-Abfrage, auch für Zuschauer |
| `heartbeat` | Keine; Verbindungsprüfung, keine Bedienaktivität |
| `activity` | Keine; nur Bediener, echte GUI-Eingabe |
| `tune` | `stream`: 1/2, `frequency_hz` |
| `tx` | `frequency_hz`, `ptt`: true/false; neue bewusste Sendeaktion |
| `ptt_keepalive` | Keine; nur solange PTT bewusst gehalten wird |
| `filter` | `bandwidth_hz`, `center_hz` |
| `nr` | `percent`: 0…100 |
| `compressor` | `percent`: 0…140, Zehntelprozent möglich |
| `loop` | `mode`: 0 Normal, 1 Audio, 2 HF |
| `settings_get` | Keine; Snapshot beim Backend abfragen |
| `settings_set` | `revision`: zuletzt gelesener String, `values`: Objekt mit geänderten Schlüsseln |

Bei `filter`, `nr`, `compressor`, `loop` ersetzt `query:true` die Wertefelder.
Wertebereiche und Settings-Schlüssel entsprechen der [UDP-Referenz](udp-protocol.md).

| Adapter → Browser: `type` | Inhalt |
|---|---|
| `role` | `role`: `operator`/`viewer`, `client_id`, `owner_present`, `clients`, `idle_seconds` |
| `state` | `ready`, `session`, `settings`, `revision`, `flags`, `settings_status`, `rx`, `tx`, `filter`, `nr`, `compressor`, `loop` |
| `ack` | `id`, `command`, `status` (Backend-Status), `revision`, `flags` |
| `error` | `id`, `code`, `message`; z. B. `read_only`, `not_ready`, `busy`, `superseded`, `timeout`, `backend_reset`, `invalid` |
| `closed` | `reason`: `idle` oder `heartbeat_timeout`; danach Verbindung geschlossen |

`state` kommt beim Verbinden und bei Änderungen. `rx["1"]`/`rx["2"]` enthalten
`frequency_hz`, `epoch`, `status`; `tx` zusätzlich `ptt`, `audio_fifo`, `pluto_fifo`, `ppm`.
`loop` enthält `mode`, `target`, `generation`, `timestamp_ns`, `rx_lo_hz`, `status`.
`filter` enthält `bandwidth_hz`, `center_hz`; NR/Kompressor `percent`, Kompressor zusätzlich
`compression_db`/`limiting_db`. Noch nicht geladene Unterobjekte sind leer. Fehlerdetails bei
Settings stehen im Snapshot (`error`/`hardware_error`). ACK-Status 0 bestätigt die Ausführung;
Status 2 bei Settings bedeutet Revisionskonflikt: aktuellen Snapshot übernehmen und neu versuchen.

**Binär RX:** FFT Typ 2, Audio Typ 5, unverändert an alle Browser. Fragment-, Session- und
Epochenregeln aus der UDP-Referenz gelten auch hier. **Binär TX:** ausschließlich Typ 8,
544 Byte mit 240 PCM16-Samples bei 12 kHz. Erst nach bestätigtem `tx.ptt=true` senden, mit
aktueller Session/TX-Epoche und bestätigter TX-Frequenz; Sample-Index 0, 240, 480, … .

## Test

`ctest --test-dir build -R webbridge_udp --output-on-failure` startet ein privates synthetisches
Backend und prüft echte HTTP-/WebSocket-Verbindungen inklusive Paketverlust, Rechteprüfung,
TX-Audio-Loop, langsamen Zuschauern, Abschaltung, Neustart und Inaktivität. Kein Pluto-Zugriff.
Optionaler Browsertest: `tests/test_webbridge_browser.py` mit Chromium/ChromeDriver; Argumente:
Adapter-Binary, Backend-Binary, Backend-Config, `web`-Verzeichnis, Screenshot-Pfad.
