# FT4 und FT8

## FT4/8 über RX2/TX2

Unter **DigiModes → FT4/8** erscheinen der RX2-Audiowasserfall, die UTC-Uhr,
der Slotfortschritt und die Nachrichtenlisten. **Nur CQ** filtert die obere
Liste. Die untere zeigt Nachrichten mit eigenem Rufzeichen und gesendete
Nachrichten. RX2 dekodiert auch im eigenen Sende-Slot. Das über QO-100
empfangene Signal erscheint mit seinem SNR. Die untere Liste zeigt auch
empfangenen Freitext, der zur eigenen Sendung im selben Slot passt. Die
QSO-Automatik verarbeitet weiterhin die Antworten aus den Empfangs-Slots.
**Default-QRG** stimmt RX2 auf 10 489 540 000 Hz und TX2 auf
2 400 040 000 Hz. **Betriebsart** wählt FT4 oder FT8 für beide Richtungen.

Rufzeichen, QTH-Locator, Audiofrequenz und TX-Pegel sind für den Bediener
auch während eines QSOs bearbeitbar. Eine Sekunde nach der letzten Eingabe
speichert die Webbridge die gültigen Werte, standardmäßig in
`data/ft-station.json`. Die Audiofrequenz ist der Abstand des untersten
Sendetons vom TX2-USB-Träger. Änderungen an Rufzeichen, Locator oder
Audiofrequenz beenden ein laufendes FT-QSO beim Speichern. Ein neuer TX-Pegel
wird auch während einer Sendung übernommen. RX2 dekodiert bei aktivem
FT-DigiMode auch ohne Stationsdaten und bei stummem Browser-Audio.

FT-TX wird automatisch freigegeben, sobald der Bediener FT4/8 ausgewählt
hat, die Stationsdaten gültig sind und der Sendeweg bereit ist. Mit **CQ rufen** geht
ein CQ in den nächsten verfügbaren Slot. Die Slotfolge lässt sich dafür
manuell auswählen. Ein Klick auf einen Decode startet ein QSO mit dessen
Absender im passenden Gegenslot. libft8 führt den Austausch und eventuelle
Wiederholungen aus. **Auto-Modus** bietet **Aus**, **CQ rufen** und
**CQ und Direktanrufe beantworten**. **Einmal senden** überträgt bis zu
13 Zeichen Freitext im nächsten verfügbaren Slot.

**Abbrechen** beendet das QSO, die Automatik und gegebenenfalls die
laufende Aussendung. Eine Umstimmung von RX2 oder TX2 und ein Wechsel zwischen
FT4 und FT8 beenden einen laufenden Austausch. Die Sendebereitschaft kehrt
nach der Umstellung automatisch zurück. **DigiModes → Aus**, ein anderer
DigiMode oder der Verlust des Bedieners beendet den FT-Betrieb. Beim Wechsel
der Anzeige zwischen Zoom und Monitor bleibt der gewählte DigiMode aktiv.
TX2 wird mit SSTV, RTTY, Wasserfalltext und externem Digitalaudio geteilt.
Ein belegter Audioweg kann eine FT-Sendung verhindern.

### ADIF und QRZ

libft8 hängt abgeschlossene QSOs an `data/ft4-ft8.adi` an. Der
Uplink steht in `FREQ`/`BAND`, der Downlink in `FREQ_RX`/`BAND_RX`, die
Satellitenkennung in `PROP_MODE=SAT` und `SAT_NAME=QO-100`. Maßgeblich
sind die bestätigten Trägerfrequenzen von TX2 und RX2 bei QSO-Beginn.
Mit **ADIF herunterladen** kann der Bediener die Datei abrufen.

Vorhandene ADIF-Einträge werden für den 72-Stunden-Abgleich eingelesen.
Ein neuer CQ erscheint rot. **CQ und Direktanrufe beantworten**
überspringt CQs kürzlich gearbeiteter Stationen. Direktanrufe an das
eigene Rufzeichen bleiben für die Automatik zulässig. FT4 und FT8 zählen
gemeinsam. Ein leerer `ft_log_path` deaktiviert die Datei und die
CQ-Markierung.

`start_all.sh` liest den optionalen QRZ-Schlüssel beim Start aus
`~/.config/qo100-trx2/qrz.env` des startenden Benutzers. Die Datei mit
Rechten `600` enthält diese Zuweisung.

```sh
export QO100_QRZ_API_KEY='DEIN_QRZ_LOGBOOK_API_KEY'
```

Eine bereits gesetzte Umgebungsvariable `QO100_QRZ_API_KEY` hat Vorrang.
Mit einem Schlüssel lädt libft8 neue Einträge nach dem lokalen Speichern
an QRZ hoch.

### Audiozeit und Konfiguration

Die Webbridge übergibt RX2 in 12-kHz-Mono-PCM16 an libft8. Sie verankert
den Backend-Samplezähler anhand des ersten UTC-Paketzeitstempels.
Die Systemuhr des Backends muss synchronisiert sein. `ft_rx_offset_ms`
korrigiert einen konstanten Aufnahmeversatz. Ein positiver Wert ordnet
Samples einer späteren UTC-Zeit zu. Frequenz-, Session- und Epochenwechsel
verwerfen ausstehende Decodes. Kleine Paketlücken behalten ihre Länge im
Sampletakt. FT4 verwendet 7,5 s, FT8 15 s pro Slot.

`ft_ptt_lead_ms` plant Zeit für das Bereitstellen von TX2 und die
Relaisbestätigung ein. `ft8_tx_lead_ms` kompensiert die Latenz zwischen
PCM-Übergabe und Aussendung für FT4 und FT8. Die Voreinstellungen sind
450 ms und 80 ms. Den tatsächlichen HF-Start und die PTT-Zeiten bei der
jeweiligen Pluto-/Shelly-Konfiguration messen. Bei einem verpassten
Startfenster meldet die Webbridge den Fehler an libft8.

`ft_enabled`, `ft_station_path`, `ft_log_path` und die Zeitparameter
stehen in `config/webbridge.conf`. Relative Dateipfade beziehen sich auf
diese Konfigurationsdatei. In älteren Konfigurationen kann das bisherige
RX2-Portpaar weiterhin den Ein-/Aus-Zustand vorgeben. `ft_enabled` hat
Vorrang. `start_all.sh` berücksichtigt außerdem `rx2_enabled` und
`external_decoders_enabled`.

## FT8 über RX3

**Setup → Monitoring-Empfänger → RX3 · FT8 (Hz)** setzt die feste
RX3-Frequenz. Der Startwert ist 10 489 540 000 Hz. **Monitor → FT8**
zeigt UTC-Slot, SNR, Zeitversatz, Audiofrequenz und Nachricht unabhängig
vom RX2-DigiMode. Stationsdaten sind für RX3 nicht erforderlich.

`qo100_ft8_decoder` ist ein separater libft8-Empfänger. `start_all.sh`
startet und überwacht ihn. Für einen Einzelstart:

```sh
./build/bin/qo100_ft8_decoder --port 40910 --time-offset-ms 0
```

RX3 erhält PCM über Loopback-UDP und liefert Text an die Webbridge
zurück. Beim Start mitten im Slot wartet der Dienst auf den nächsten
vollständigen 15-s-Slot. Paketverluste, Retunes und Zeitsprünge verwerfen
den Teilslot. Der Worker dekodiert mit Tiefe 1 und hält höchstens einen
wartenden Slot. `--decode-timeout-ms` begrenzt das Alter ausgegebener
Ergebnisse. `rx3_decoder_cpu_percent` begrenzt die CPU-Last, standardmäßig
auf 10 % eines Kerns. `FT8_TIME_OFFSET_MS` oder `--time-offset-ms` korrigiert
die Zuordnung am Monitor. [CPU-Budget](decoder-cpu.md)

## Build und Tests

QO100 verwendet libft8 ab 0.3.2. `build_all.sh` lädt bei Bedarf den
festgelegten Quellstand von GitHub und baut die Bibliothek lokal.
`LIBFT8_SOURCE_DIR` wählt einen lokalen Checkout, `LIBFT8_ROOT` ein bereits
installiertes Paket. Für einen direkten CMake-Build das Präfix als
`-Dlibft8_ROOT=/pfad/zur/installation` angeben.

```sh
ctest --test-dir build --output-on-failure -R 'ft8|parallel_monitor_recordings'
```

Die Tests prüfen UTC-Ausrichtung, Paketverlust und Retunes, FT8-Decodes
auf RX2 und RX3 sowie FT4/FT8 über TX2 am synthetischen Backend.
Ein FT4-QSO überprüft alle Satellitenfelder im vorhandenen ADIF-Log.
