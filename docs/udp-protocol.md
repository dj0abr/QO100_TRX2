# GUI-UDP-Schnittstelle · QTR2 v1

Aktueller Vertrag für ein externes GUI. Alle Frequenzen sind **ganze Hz**:
RX = QO-100-Downlink, TX = QO-100-Uplink. Hardware-/Umsetzerkorrekturen macht das Backend.

## Verbindung und Ablauf

- Backend: **UDP/IPv4, Port 40900** (konfigurierbar). Das GUI verwendet **denselben Socket** zum Senden und Empfangen; Antworten gehen an dessen Absender-IP/-Port. Kein eigener Audio-Port.
- **Ein GUI-Teilnehmer**: Eine neue Anmeldung übernimmt die Verbindung. `SUBSCRIBE` alle **2 s** senden; ohne Anmeldung endet der Datenstrom nach standardmäßig **8 s**.
- Start: `SUBSCRIBE` → `SETTINGS_CONTROL` abfragen (`session=0`) → Settings anzeigen → `TUNE` für RX1/RX2 mit geladenen Frequenzen → passende `STATE` abwarten → Audio annehmen. TX startet mit PTT aus.
- Jede Kommandogruppe und jeder RX-Kanal hat eine eigene steigende **uint32 Request-ID** (Umlauf erlaubt). Änderung = neue ID; bei Paketverlust dieselbe Anfrage wiederholen, z. B. alle **250 ms**. ACK nach **Typ, Stream, Request und Session** zuordnen. Alte Anfragen werden ignoriert.
- Die **Session** identifiziert einen Backend-Lauf. Bei Wechsel erneut Settings laden, Stream-Puffer/Epochen verwerfen und PTT ausschalten. Schreibkommandos tragen die aktuelle Session; Settings-Discovery erlaubt `session=0`.

## Gemeinsamer Header

**64 Byte**, maximal **1200 Byte pro Datagramm**. Alle Zahlen **Big Endian**, alle nicht genannten Felder **0**. Keine C++-Structs direkt übertragen. Ab Byte 64 folgt ggf. die Nutzlast.

| Offset | Byte | Name | Bedeutung je Nachricht |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `QTR2` |
| 4 | 1 | version | `1` |
| 5 | 1 | type | Nachrichtentyp, siehe unten |
| 6 | 2 | header_size | `64` |
| 8 | 4 | stream | `0` Gesamt-FFT/Settings/Loop, `1` RX1/Zoom, `2` RX2/Zoom, `3` TX, `4` Audio-Echo, `5` RX3/FT8, `6` RX4/PSK, `7` TX2 |
| 12 | 4 | id | Request-ID, FFT-Frame-ID oder Audio-Epoche |
| 16 | 8 | timestamp | Sendezeit Unix-ns bei FFT/Audio/Loop-State; sonst 0 |
| 24 | 8 | index | Sample-Index, bestätigte Epoche oder Settings-Revision |
| 32 | 8 | frequency | Frequenz bzw. Audio-Filtermitte |
| 40 | 4 | rate | Samples/s |
| 44 | 4 | size | FFT-Größe bzw. Steuerwert |
| 48 | 4 | first | Erster FFT-Bin bzw. Steuerwert/Flags |
| 52 | 2 | count | Anzahl Werte bzw. Payload-Bytes |
| 54 | 2 | format | Datenformat, Set/Query oder Status |
| 56 | 4 | total | Gesamtanzahl bzw. Steuerwert |
| 60 | 4 | session | Backend-Session |

`SUBSCRIBE` (**Typ 1**): nur magic, version, type und header_size setzen; **Byte 8–63 bleiben 0**.

## Settings lesen und speichern

| Nachricht | Stream | Felder / Payload |
|---|---:|---|
| **17 SETTINGS_CONTROL: lesen** | 0 | `id=Request`, `session=0` oder aktuelle Session; Rest 0, keine Payload |
| **17 SETTINGS_CONTROL: ändern** | 0 | `id=neuer Request`, `session=aktuell`, `index=zuletzt gelesene Revision`, `format=1`; `size=count=total=Payload-Bytes` |
| **18 SETTINGS_STATE** | 0 | `id=Request`, `session=aktuell`, `index=Revision`, `format=Status`, `first=Flags`; `size=count=total=Payload-Bytes` |

Payload: **UTF-8 `schlüssel=wert\n`**, maximal 1136 Byte. Dezimalpunkt `.` verwenden.
Antwort = vollständiger Snapshot; Änderung = **nur geänderte Schlüssel**. Beispiel:

```text
rx_correction_hz=123
rx1_nr_percent=60
```

Status: **0** gespeichert/OK, **1** ungültig, **2** Revision veraltet (Snapshot neu einlesen, Änderung mit neuer ID erneut senden), **3** Speicherfehler, **4** Hardware-Umschaltung, **5** Hardwarefehler/Rückkehr, **6** belegt.
Flags (`first`): Bit **0** laufende Betriebsänderungen noch nicht gespeichert, Bit **1** Hardware-Umschaltung/Rückkehr läuft, Bit **2** Hardware offline. Solange Bit 1 gesetzt ist, weiter auf die endgültige Bestätigung warten.

Änderungssätze werden vollständig validiert. Fehler lassen die bisherigen Werte bestehen.
**OK auf eine Settings-Änderung bestätigt dauerhafte Speicherung**; Hardwareänderungen zusätzlich erst nach erfolgreichem Start mit frischen IQ-Daten. Abstimm-/DSP-Kommandos unten speichern spätestens nach ca. 500 ms; unveränderte Heartbeats schreiben nichts. PTT und Loops bleiben flüchtig.

| Schreibbarer Schlüssel | Wert / Bereich |
|---|---|
| `pluto_connection`, `pluto_ip` | `usb` oder `network`; IPv4-Adresse |
| `lnb_reference_hz` | Standard `25000000`; 10–30 MHz, resultierender LO muss gültig bleiben |
| `rx_correction_hz`, `tx_correction_hz` | −1000000…+1000000 Hz; positiv erhöht den jeweiligen Hardware-LO |
| `pluto_tx_base_hz` | Nominale Pluto-TX-Mitte, Standard `1290250000` |
| `tx_attenuation_db` | 0…40 dB, Schritte 0.25 dB |
| `initial_rx_hz`, `initial_rx2_hz`, `initial_tx_hz` | Aktuelle und nächste Startfrequenzen; RX-/TX-Mitte ±275000 Hz |
| `rx1_filter_low_hz`, `rx1_filter_high_hz` | Audio-Unter-/Obergrenze 100…4000 Hz, mindestens 500 Hz Abstand |
| `rx1_nr_percent`, `gui_nr_strength` | NR 0…100 % (0 = aus); gemerkter GUI-Reglerwert 1…100 |
| `tx1_compressor_percent` | 0…140 %, 100 = Normalbetrieb, 0 = Bypass |
| `gui_audio_gain`, `gui_display_floor` | GUI-Wiedergabe 0…20 (0 = stumm); Anzeige −160…−40 dBFS |
| `gui_wheel_step_hz` | 1…1000 Hz |

Nur lesbar: `settings_schema=1`, `rx_center_hz`, `tx_center_hz`, `pluto_rx_hz`, `pluto_tx_hz`, `hardware_online` (0/1), optional `error`/`hardware_error`.
Berechnung: `Pluto RX = rx_center_hz − lnb_reference_hz × 390 + rx_correction_hz`; `Pluto TX = pluto_tx_base_hz + tx_correction_hz`. Endgültige LOs: 70 MHz…6 GHz. `pluto_*_hz` sind berechnete Sollwerte.
GUI-Schlüssel werden vom Backend gespeichert, aber **vom GUI ausgeführt**. Keine lokale Settings-Datei des GUIs nötig.

## RX, FFT und Audio

| Typ | Stream | Felder / Payload |
|---|---:|---|
| **3 TUNE** → | 1/2 | `id=Request`, `frequency=RX-Träger`, `session=aktuell` |
| **4 STATE** ← | 1/2 | `id=Request`, `frequency=bestätigter Träger`, `index=Audio-Epoche`, `rate=12000`, `format=0 OK / 1 außerhalb des Fensters` |
| **2 SPECTRUM** ← | 0/1/2 | `id=Frame`, `timestamp`, `index=exklusives Sample-Ende`, `frequency=Mitte`, `rate`, `size=total=FFT-Größe`, `first`, `count`, `format=1`; Payload: `count × int16` |
| **5 AUDIO** ← | 1/2/4 | `id=Epoche`, `timestamp`, `index=Sample-Start`, `frequency=Träger`, `rate=12000`, `count=total=240`, `format=2`; Payload: **240 × int16 PCM**, 544 Byte inkl. Header |

**FFT:** Stream 0 = 600 kHz Gesamtspektrum; Stream 1/2 = unabhängige 75-kHz-Zooms um RX1/RX2; für eine Anzeige von ±25 kHz den mittleren 50-kHz-Ausschnitt verwenden. Standard-FFT jeweils 8192 Bins. Bin `k`: `frequency + (k − size/2) × rate/size`.
Pegel = vorzeichenbehafteter int16 / **100 dBFS**. Maximal 568 Bins pro Datagramm.
Fragmente anhand **(Session, Stream, Frame-ID)** sammeln; Metadaten müssen übereinstimmen.
Erst anzeigen, wenn alle Bins vorliegen; Duplikate ignorieren, alte/unvollständige Frames verwerfen (z. B. nach 500 ms).

**Audio:** 12 kHz, mono, PCM16 Big Endian; Float-Normierung durch 32768. RX1 = Sprache, RX2 = unabhängiger Kanal für externe Digitalprogramme. Nur Audio mit bestätigter **(Session, Stream, Epoche)** annehmen. Eine normale RX-Abstimmung ändert nur die Mischfrequenz: **Audio-Epoche und Sample-Index laufen unverändert weiter**, ebenso Oszillatorphase, Filter, NR und Resampler. Bereits gepuffertes Audio bleibt gültig und wird kontinuierlich ausgespielt; die Frequenzänderung ist in den Frequenzfeldern von STATE/AUDIO erkennbar. Erst ein echter Hardware-/Signalweg-Reset wechselt die RX-Epoche und beginnt den Sample-Index neu. Bei Session-, Stream- oder Epochenwechsel alte Puffer verwerfen; alte Audiopakete bleiben gesperrt.
Sample-Indizes erkennen Verlust, Duplikate und Reihenfolge. Für die Soundkarte: Jitterpuffer, Lücken mit Stille füllen und Soundkartentakt durch leichtes Resampling ausgleichen. FFT und Audio werden nicht nachgesendet.
`STATE` kann auch spontan eine neue Epoche melden. Letztes `TUNE` mit gleicher ID z. B. jede Sekunde wiederholen, damit verlorene Epochenmeldungen nachgeholt werden.

## TX

| Typ | Stream | Felder / Payload |
|---|---:|---|
| **6 TX_CONTROL** → | 3 | `id=Request`, `session=aktuell`, `frequency=TX-Träger`, `format=0 PTT aus / 1 an` |
| **7 TX_STATE** ← | 3 | `id=Request`, `index=TX-Epoche`, `frequency=bestätigt`, `rate=12000`, `format=0 OK / 1 Frequenz ungültig / 2 wartet`; `first=PTT`, `size=Audio-FIFO-Samples`, `count=Pluto-FIFO-Blöcke`, `total=vorzeichenbehaftete int32 Taktkorrektur in ppm` |
| **8 TX_AUDIO** → | 3 | Wie AUDIO, aber `id=bestätigte TX-Epoche`, `timestamp=0`, `frequency=bestätigter TX-Träger`, `session=aktuell`; genau 240 PCM16-Samples, `index=0,240,480,…` |

PTT nur nach Benutzeraktion einschalten. **Erst nach passendem OK mit `first=1` Audio senden**, in 20-ms-Blöcken; Index bei neuer TX-Epoche wieder 0. `TX_CONTROL` mindestens alle **500 ms** wiederholen, auch während Audio fließt. Nach **1,5 s** ohne gültiges Control fällt PTT ab; erneutes Einschalten benötigt eine **neue Request-ID**.
Keine Audiopakete seit 200 ms → Stille; kurze Paketlücken werden ebenfalls mit Stille gefüllt. RX und TX sind unabhängig. Hardwareänderungen erzwingen PTT aus.
TX-USB ist im Backend fest auf **100–3000 Hz** begrenzt (nominelle Filtergrenzen,
mit endlicher Flanke); PCM bleibt bei 12 kHz. RX-Filterbefehle verändern TX nicht.

## Filter, Kompressor, NR und Test-Loops

Alle Nachrichten: 64 Byte, aktuelle Session, eigene Request-ID. **Control:** `format=0` fragt ab (alle Wertefelder 0); `format=1` setzt Werte laut Tabelle. Beim Set von Filter/Kompressor/NR zusätzlich `rate=12000`. **State:** bestätigt die tatsächlich angenommenen Werte; `format=0 OK / 1 ungültig`.

| Control → State | Stream | Werte beim Set → bestätigte State-Felder |
|---|---:|---|
| **9 FILTER_CONTROL → 10 FILTER_STATE** | 1 | `size=BW`, `frequency=Audio-Mitte` → dieselben Werte, `index=RX1-Epoche`, `rate=12000` |
| **11 COMPRESSOR_CONTROL → 12 COMPRESSOR_STATE** | 3 | `size=0…1400` (**Zehntelprozent**, 1000 = 100 %) → `size`, `index=TX-Epoche`, `rate=12000`; `first=Verdichtung`, `total=Spitzenbegrenzung`, beide in 0.001 dB |
| **15 NR_CONTROL → 16 NR_STATE** | 1 | `size=0…100` (**Prozent**, 0 = Bypass) → `size`, `index=RX1-Epoche`, `rate=12000` |
| **13 LOOP_CONTROL → 14 LOOP_STATE** | 0 | `size=0 Normal / 1 Audio / 2 HF` → `size=aktuell`, `first=Zielmodus`, `index=Generation`, `frequency=RX-LO-Sollwert`, `rate=600000`, `timestamp` |

**Filtergrenzen (empfohlen):** Typ 9 mit `format=2` setzt `frequency=untere Grenze ×1000`, `size=obere Grenze ×1000` (0.001 Hz), `rate=12000`.
`format=3` fragt die Grenzen ab (alle Wertefelder 0). Die Antwort Typ 10 enthält
`first=untere Grenze ×1000`, `total=obere Grenze ×1000`; Status weiterhin `format=0/1`.
`frequency`/`size` enthalten zusätzlich gerundete Mitte/BW für Diagnose.
Formate 0/1 bleiben für alte BW-/Mitte-Clients erhalten; deren Antwort hat `first=total=0`.
Settings liefern die neuen Grenzschlüssel; alte BW-/Mitte-Schlüssel werden beim Schreiben weiterhin akzeptiert,
dürfen aber nicht im selben Patch mit Grenzschlüsseln gemischt werden.
Filter: untere/obere Grenze innerhalb 100…4000 Hz, mindestens 500 Hz Abstand. NR wirkt nur auf RX1; Regler 100 erlaubt bis zu 30 dB Rauschdämpfung.
Loop-Status zusätzlich: **2** HF-Modus nicht verfügbar, **3** Umschaltung läuft. Auf Status 0 und Zielmodus warten; neue Generation leert FFT-/Wasserfallpuffer und verwirft ältere FFT-Zeitstempel.
**Audio-Loop:** PTT aktiviert Mikrofonpfad, `TX_AUDIO` kommt unverändert als **AUDIO Stream 4** zurück (TX-Epoche, Uplinkfrequenz); HF bleibt stumm. Dabei Stream 4 statt RX1 abspielen.
**HF-Loop:** Empfänger-LO folgt TX-LO, RX1 folgt TX-Abstimmung; Selbsthören benötigt HF-Kopplung. Normalmodus stellt RX wieder her. Moduswechsel löscht PTT; GUI-Abbruch/Lease-Ende stellt Normal wieder her.

## Kurze C++-Beispiele

Nur C++17/POSIX nötig: [Hilfsfunktionen](../examples/qtr2.hpp), [vollständiges Empfangsbeispiel](../examples/gui_udp.cpp).
Es liest Settings, stimmt beide RX ab, dekodiert PCM-Blöcke und zählt FFT-Fragmente; PTT bleibt aus.

```sh
g++ -std=c++17 -O2 -Wall -Wextra examples/gui_udp.cpp -o /tmp/gui_udp_example
/tmp/gui_udp_example 127.0.0.1 --seconds 5
# Optional: neue RX1-Frequenz oder dauerhaft gespeicherte Änderung:
/tmp/gui_udp_example 127.0.0.1 --rx-hz 10489748500 --set gui_audio_gain=5.5
```

**Anmelden, Settings laden, RX1 abstimmen:** `rpc()` wiederholt Requests und erneuert SUBSCRIBE während des Wartens.

```cpp
#include "qtr2.hpp"
using namespace qtr2;
Udp udp("192.168.10.20");                         // IP des BACKENDS
const auto snapshot = rpc(udp, settings(1), 18); // Query mit Session 0
const auto sid = uint32_t(get(snapshot, 60, 4));
auto tune = header(3, 1, 1, sid);
put(tune, 32, std::stoull(values(snapshot).at("initial_rx_hz")), 8);
const auto rxAck = rpc(udp, tune, 4);             // index = Audio-Epoche
```

**Dauerhaft ändern** (bei Status 2 Snapshot/Revision übernehmen, neue ID vergeben und erneut versuchen):

```cpp
auto change = settings(2, sid, get(snapshot, 24, 8), "rx1_nr_percent=60\n");
auto saved = rpc(udp, change, 18);                // format muss 0 sein
```

**TX ankoppeln** (in den laufenden GUI-Empfangs-/Mikrofonpfad einfügen):

```cpp
auto ptt = header(6, 3, 1, sid);
put(ptt, 32, 2400400000ULL, 8); put(ptt, 54, 1, 2);
auto txAck = rpc(udp, ptt, 7);
// Nur bei format=0 UND first=1 senden; PTT-Control alle 500 ms erneuern!
std::array<int16_t,240> pcm{};                    // hier 20 ms Mikrofon bei 12 kHz
if (get(txAck,54,2)==0 && get(txAck,48,4)==1)
    udp.send(tx_audio(uint32_t(get(txAck,24,8)), 0, get(txAck,32,8), sid, pcm));
// Nächster Audioblock: Index 240. Beim Loslassen neue Control-ID, format=0.
```

## Monitoring-Empfänger RX3 und RX4

Die Transport-IDs **5** und **6** kennzeichnen RX3 und RX4 in `AUDIO`/`STATE`.
Die bestehenden IDs 3/4 für TX/Audio-Echo bleiben unverändert. Kein zusätzlicher Zoom.
`TUNE` erlaubt weiterhin nur RX1/RX2. Monitoringfrequenzen werden mit den persistenten
Settings `rx3_frequency_hz` und `rx4_frequency_hz` gesetzt, jeweils ganze USB-Träger-Hz
innerhalb der RX-Mitte ±275000 Hz. Startwerte: 10489540000 / 10489748821 Hz.
Ein geänderter Monitor setzt ausschließlich seinen DSP, seine Epoche und den Samplezähler
zurück. Andere Kanäle laufen weiter. Bei Anmeldung und nach einem Reset sendet das Backend
Monitoring-`STATE` mit Request-ID 0; ein Abonnent kann diese Zustände direkt übernehmen.
Der Webadapter verwendet in `state.rx` und `decoder_text.stream` die logischen Nummern 3/4.

## TX2 und gemeinsame IQ-Ausgabe

TX2 verwendet `TX_CONTROL`/`TX_STATE`/`TX_AUDIO` mit Stream **7**. Frequenz-,
Session-, Epochen-, PCM- und Sampleindexfelder entsprechen TX1. Bei TX2 bedeutet
`TX_STATE.first` (Offset 48) **Audioweg bereit**, unabhängig von einer PTT.
`TX_CONTROL.format=0` stimmt TX2 ab; `format=1` fordert bei einem Adapterneustart
zusätzlich eine neue Audio-Epoche an. Wiederholte Request-IDs lösen keinen weiteren
Reset aus. Alte Sessions, Epochen oder Frequenzen werden für TX2 ebenfalls verworfen.
Ein fehlender Browser/PTT-Keepalive schaltet TX2 im Normalbetrieb nicht ab. Ausfall
des Audioadapters, Hardwarewechsel oder Test-Loops verwerfen ungeeignetes Audio.

Die zwei Modulatoren werden im selben Pluto-FIFO-Takt berechnet und gemeinsam als
ein IQ-Strom übertragen. Die Summe hat eine feste Verstärkung von 0,5 (6 dB Reserve).
TX1 hat dadurch gegenüber der Version mit einem Sender einen um 6 dB kleineren
IQ-Pegel. Die Hardware-Dämpfung bleibt unverändert.

Der separate Audioadapter meldet sich ausschließlich bei der Webbridge an, nicht
als Backend-Abonnent. [Geräte, UDP-Handschlag und Digitalprofil](digital-audio.md).
