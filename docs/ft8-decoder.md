# FT8 über RX3

Der eigenständige Empfänger **`qo100_ft8_decoder`** ersetzt den Test-Dummy.
Er verwendet den Decoder aus kwWebRXpp mit dessen vendorten WSJT-X-Quellen.
Quellen, Lizenz und Original-Testaufnahme liegen vollständig unter
[`external/ft8`](../external/ft8/ORIGIN.md); kwWebRXpp wird danach nicht mehr benötigt.

## Start

Backend und Webadapter wie bisher starten. Den alten Dummy beenden und in einem
zusätzlichen Terminal aufrufen:

```sh
./build/bin/qo100_ft8_decoder
```

Der normale Projektbuild erzeugt **`qo100_ft8_decoder`** und den automatisch
aufgerufenen Rechenkern **`ft8-decoder`** nebeneinander in `build/bin`.
Der bisherige Name `qo100_dummy_decoder` verweist ebenfalls auf den FT8-Empfänger.
Es darf jeweils nur ein Decoder denselben UDP-Port belegen.

Unter **Setup → Monitoring-Empfänger** den USB-Träger als **RX3 · FT8 (Hz)**
eingeben und **Änderungen speichern**. Startwert: **10489540000 Hz**.
Oben in der Zoom-Kachel **FT8-Monitor** auswählen; der Text nutzt die gesamte Kachel.
RX4 dekodiert gleichzeitig die PSK-Bake; beide Textpuffer laufen unabhängig weiter. Empfangene Zeilen enthalten:

```text
133430  16   0.255 2571.375 ~ W1FC F5BZB -08
```

Felder: **UTC-Slot, SNR in dB, Zeitversatz in s, Audiofrequenz in Hz, Nachricht**.
Der erste vollständige Slot muss zunächst gesammelt werden: bei einem Start
mitten im Slot dauert das etwa **15–30 Sekunden plus Rechenzeit**. Eine korrekt
synchronisierte UTC-Systemuhr auf dem Backend-Rechner ist erforderlich.

## Daten und Zeit

Die Schnittstelle verwendet **RX3-PCM, 12 kHz, mono** über lokales UDP
an **127.0.0.1:40910** (QTR2-Stream **5**); decodierte Zeilen als UTF-8 an den Absender zurück.
Der Adapter verteilt sie als `decoder_text` an Bediener und Zuschauer.
[Ports und Paketformat](webbridge.md#monitoring-rx3-und-rx4).

Der Empfänger bildet aus Sample-Zähler und Backend-Sendezeit komplette
**180000-Sample-Blöcke** für die UTC-Grenzen 00/15/30/45 s. UDP-Ankunftsjitter
verschiebt den Sample-Takt nicht. Paketverluste, Frequenz-/Session-/Epochenwechsel
oder Zeitsprünge verwerfen den betroffenen Teilslot. Veraltete Ergebnisse einer
vorherigen Abstimmung werden nicht an den Browser ausgegeben.

Die Sendezeit ist kein präziser HF-Aufnahmezeitstempel; konstante Verzögerungen
erscheinen als Zeitversatz im Decoder. Optional lässt sich die Zuordnung korrigieren:

```sh
./build/bin/qo100_ft8_decoder --port 40910 --time-offset-ms 0
```

`--time-offset-ms`: −5000 bis +5000 ms; positiv ordnet Audio einer späteren UTC-Zeit
zu. Alternativ gilt `FT8_TIME_OFFSET_MS`. Ohne Angabe ist der Offset 0.
`--decoder DATEI` wählt einen anderen Rechenkern, `--decode-timeout-ms` begrenzt
dessen Laufzeit (Standard 45000 ms). Die Decodierung läuft separat und blockiert
den UDP-Empfang nicht. RX1, TX und Backend-DSP werden dafür nicht verändert.

## Build und Test

Zusätzlich benötigt: **GNU Fortran**; FFTW3 und Boost werden bereits verwendet.

```sh
sudo apt install gfortran libfftw3-dev libboost-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure -R 'ft8|monitor_decoders'
```

Die Tests prüfen UTC-Slots, Paketverluste und Abstimmung sowie echte FT8-Decodes
aus erzeugten Signalen und der Originalaufnahme bis zu zwei WebSocket-Clients.
Der WSJT-X-Anteil bleibt einschließlich Lizenz und Herkunftsnachweis erhalten.
