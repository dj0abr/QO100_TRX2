# FT4 und FT8

## FT4/8 über RX2/TX2

Über der Zoom-Kachel **DigiModes → FT4/8** öffnen und unter **Betriebsart**
FT4 oder FT8 auswählen. Die Auswahl gilt für RX2 und TX2. Ein Klick auf die
Frequenzanzeige stimmt RX2 auf 10 489 540 000 Hz und TX2 auf 2 400 040 000 Hz ab.
Der Wasserfall zeigt das Empfangsfenster von 0 bis 4 kHz relativ zum USB-Träger.
Die UTC-Uhr und der Balken zeigen den Fortschritt im aktuellen Slot.
FT4 verwendet 7,5-Sekunden-Slots,
FT8 verwendet 15-Sekunden-Slots. FT4 startet die Dekodierung mit den ersten
72.576 Samples (6,048 Sekunden) jedes Slots. An RX2 startet der erste FT8-Durchlauf
nach 141.696 Samples (11,808 Sekunden). Nach dem vollständigen Slot ergänzt ein
zweiter Decoderaufruf weitere Treffer. Die Decodes folgen der eingestellten RX2-Frequenz. Solange FT4/8 als DigiMode
aktiv ist, sammelt der Decoder UTC-synchrones Audio. Kurze Audiolücken bis 100 ms werden durch Nullen
ersetzt, insgesamt höchstens 200 ms pro Slot. Damit bleibt die Position der
FT8-Symbole im Slot erhalten. RX3 läuft parallel als Festfrequenzmonitor.

Zum Senden RX2 und TX2 abstimmen. Das Feld **Audio (Hz)** gibt den Abstand des
untersten Sendetons vom eingestellten USB-Träger an. Eine Nachricht,
Audiofrequenz, Pegel und Slotfolge auswählen und **Einmal senden** drücken.
Die Webbridge zeigt den geplanten UTC-Slot an und reserviert TX2 bis zum Ende
der Sendung. **Abbrechen** stoppt die Planung oder die laufende Sendung. Bei
belegtem TX2 erscheint eine Fehlermeldung. Freitext ist auf 13 Zeichen
einschließlich Leerzeichen begrenzt. Ein Beispiel ist `TEST DJ0ABR`. Längere
Nachrichten müssen einem Standardformat entsprechen.

Für ein Standard-QSO eigenes Rufzeichen und QTH-Locator eingeben. Mit
**Auto-QSO** startet ein Klick auf eine CQ-Zeile die Antwort mit Locator
im nächsten verfügbaren Slot der passenden UTC-Slotfolge. Auch ältere
Zeilen in der Empfangsliste lassen sich auswählen. Ein Klick auf eine Zeile aus
einem fremden QSO ruft die sendende Station mit dem eigenen Locator. Bei
`C8K DK2DB 73` ist das DK2DB. Auf einen Rapport der gewählten
Gegenstation folgt `R±nn`, auf `RRR` oder `RR73` folgt `73`.

Bei aktiviertem **Auto-Antwort** startet ein Anruf mit Locator an das eigene
Rufzeichen automatisch ein QSO. Ein vorheriger CQ-Ruf ist optional. Im
Empfangsslot nach einem eigenen CQ-Ruf startet auch ein direkt an das eigene
Rufzeichen gerichteter Rapport wie `DJ0ABR UT5UGR +16` ein QSO. Darauf folgt
ein bestätigter Rapport (`R±nn`). Wenn mehrere Stationen rufen, übernimmt
die Automatik den ersten passenden Decode
und bleibt für das laufende QSO bei dieser Gegenstation. Auf den Locator
folgt der Rapport, auf `R±nn` folgt `RR73`. Das abschließende `73` der
Gegenstation beendet das QSO. Die eingestellte TX-Audiofrequenz gilt für das
ganze QSO.

**Abbrechen** stoppt das Auto-QSO auch während der Wartezeit. Bleibt nach einer
Sendung zwei Slots lang eine passende Antwort aus, plant die Automatik die
gleiche Nachricht im nächsten verfügbaren Sendeslot derselben Folge ein.
Nach drei Sendungen pro Schritt und zwei weiteren Slots ohne passende Antwort
endet die Automatik. Ein Frequenzwechsel,
Verbindungsverlust oder der Wechsel zu SSTV oder RTTY beendet die Automatik.
Beim Wechsel zu Zoom, Monitor oder Multimedia-Bake läuft der gewählte DigiMode
im Hintergrund weiter. **Anzeigen** öffnet seine Kachel wieder. **Aus** beendet
den DigiMode einschließlich einer laufenden oder geplanten Sendung.

Ohne Häkchen an **Auto-QSO** bleibt die Antwort ein Entwurf. **CQ rufen**
erstellt den eigenen CQ-Ruf und plant ihn mit einem Klick für den gewählten
UTC-Slot ein. **Einmal senden** plant eine einzelne Aussendung der
eingetragenen TX-Nachricht. Ist die Vorbereitungsfrist
für den nächsten Slot verstrichen, nimmt TX2 den nächsten Slot derselben Folge.

**Auto-CQ** plant bei freiem TX2 einen CQ-Ruf für den nächsten verfügbaren
Slot der gewählten Sendefolge. Nach dem Ruf bleibt der folgende Slot für
Antworten frei. Mit **Auto-Antwort** kann daraus direkt ein QSO entstehen.
Während eines laufenden QSOs pausiert Auto-CQ und setzt nach dessen Ende
wieder ein. **Abbrechen** schaltet Auto-CQ aus. Nach einem Neuladen oder einer
unterbrochenen Verbindung muss Auto-CQ erneut aktiviert werden.

### ADIF-Logbuch

Die FT4/8-Kachel protokolliert QSOs mit ausgetauschten Rapporten, sobald die
Bestätigung gesendet wurde oder nach dem eigenen bestätigten Rapport ein
abschließendes `73` eintrifft. Das gilt für Auto-QSOs und manuell gesendete
Antwortvorschläge aus der Empfangsliste. Die Webbridge speichert die Datei
unter `data/ft4-ft8.adi` auf ihrem Rechner. In `config/webbridge.conf` lässt sich der
Pfad mit `ft_log_path` ändern. Die Datei kann direkt in ein ADIF-Logbuch
importiert werden und enthält Rufzeichen, UTC-Zeiten, Rapporte, bekannte
Locatoren sowie Sende- und Empfangsfrequenz für QO-100. Ein leerer Pfad
schaltet Logbuch und CQ-Markierung aus.

In der oberen Empfangsliste erscheint ein CQ-Ruf rot, wenn das Rufzeichen
der Gegenstation in den vergangenen 72 Stunden im Logbuch fehlt. FT4 und FT8
werden dabei gemeinsam betrachtet. Die untere Liste zeigt weiterhin die
Nachrichten mit dem eigenen Rufzeichen.

`start_all.sh` startet den RX2-Dienst separat mit `--rx 2`. Die beiden
`rx2_decoder_*`-Ports in `config/webbridge.conf` sind standardmäßig 40918 und
40919. Bei eigener Konfiguration beide Ports übernehmen. Zwei Nullwerte
deaktivieren die Funktion.

Für den Sendestart ist die UTC-Systemuhr des Rechners maßgeblich. Die Webbridge
beginnt mit der PCM-Übergabe bei Slotbeginn plus 500 ms abzüglich
`ft8_tx_lead_ms`. Die voreingestellten 80 ms sind ein Schätzwert für DSP und
Pluto-Puffer. TX2 beginnt bis zu eine Sekunde vor der PCM-Übergabe mit der
Vorbereitung. Ein später Sendeauftrag kann denselben Slot noch nutzen,
wenn bis zur Übergabe mindestens 200 ms bleiben. Beim Standardvorlauf
liegt diese Annahmefrist 220 ms nach der Slotgrenze. Das Signal wird
vollständig gesendet. Für den Abgleich am Gerät den tatsächlichen HF-Start messen und
bei einem zu späten Signal den Vorlauf um die gemessene Abweichung erhöhen.
Der Integrationstest prüft den UTC-Start der PCM-Übergabe und die Dekodierbarkeit
des erzeugten Signals mit dem synthetischen Backend.

## FT8 über RX3

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
Es darf jeweils nur ein Decoder denselben UDP-Port belegen.

Unter **Setup → Monitoring-Empfänger** den USB-Träger als **RX3 · FT8 (Hz)**
eingeben und **Änderungen speichern**. Startwert: **10489540000 Hz**.
Oben in der Zoom-Kachel **Monitor → FT8** auswählen. Der Text nutzt die gesamte Kachel.
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
dessen Laufzeit in verstrichener Zeit (Standard 45000 ms).

Der komplette FT8-Prozessbaum einschließlich des WSJT-X-Rechenkerns hat
standardmäßig **10 % eines CPU-Kerns** als Budget. `rx3_decoder_cpu_percent` in
`backend.conf` kann es auf 1–10 % einstellen; Einzelstart: `--cpu-percent 10`.
Alle Rechenprozesse laufen mit nice 19/SCHED_IDLE. Der Monitoring-Empfänger
verwendet die schnelle WSJT-X-Stufe `--depth 1`. Der eigenständige Rechenkern
`ft8-decoder` behält für direkte Offline-Aufrufe seinen bisherigen Default.
Ein laufender Slot und höchstens der neueste wartende Slot werden behalten.
Langsame, veraltete oder nach 45 Sekunden nicht fertige Dekodierungen dürfen
entfallen; Audiowiedergabe und RX5 haben Vorrang. Die Begrenzung erfolgt auch
beim Einzelstart und benötigt keine Root-Rechte oder zusätzliche Programme.
[Details und Tests der CPU-Budgetierung](decoder-cpu.md).

Die Decodierung läuft separat und blockiert
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
