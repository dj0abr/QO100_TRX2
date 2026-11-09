# RX5: Multimedia-Bakenmonitor

RX5 ist ein eigener, ständig laufender USB-Empfänger für die QO-100-MM-Bake.
Die Vorgabe **10489993000 Hz** kann unter **Setup → Monitoring-Empfänger →
RX5 · MM-Bake (Hz)** geändert werden. Das verändert weder RX1/RX2 noch RX3/RX4.
Der feste USB-Durchlass von 0 bis 4000 Hz erhält den niedrigen Piloten für die AFC.
RX5 verwendet keine Sprachkompression und keine Rauschreduktion.

`./build_all.sh` baut den übernommenen HS-Modem-Empfangskern mit der gebündelten
liquid-dsp-Bibliothek. `./start_all.sh` startet den Headless-Monitor automatisch;
ein ausgefallener MM-Monitor wird separat neu gestartet. Es werden weder eine
zusätzliche virtuelle Soundkarte noch eine PulseAudio-Verbindung benötigt.
Die RX2-Geräte `fromQO100`/`toQO100` bleiben für externe Digitalprogramme verfügbar.

```text
Pluto IQ → RX5 USB → Webbridge → UDP PCM16, 12 kHz → MM-Monitor
                                                      ↓
                                      48 kHz → AFC / 8APSK / FEC / CRC
                                                      ↓
                                      Textsammler / SQLite / Bulletin-Archiv
                                                      ↓
                                    lokales HTTP → Webbridge → WebSocket → Browser
```

Oben in der Zoom-Kachel stehen **DX-Cluster**, **CW-Skimmer** und **QO100-Bulletin**
neben Zoom, FT8- und PSK-Monitor zur Auswahl. Die gewählte Ansicht nutzt den gesamten
Inhalt der Kachel. Die Auswahl ändert nur die lokale Anzeige.
Für DX/CW werden die jüngsten 200 Zeilen übernommen. Ein Bulletin wird erst nach
vollständigem Empfang, Datei-CRC und ZIP-Prüfung angezeigt; bis dahin bleibt das
vorherige Bulletin erhalten. Die Oberfläche zeigt Klartext und führt empfangenes
HTML nicht aus. Neue Bulletins werden von Anfang an angezeigt.
Textaktualisierungen erfolgen gebündelt; unveränderte MM-Texte werden nicht erneut
übertragen. Pro Ansicht werden höchstens etwa 64 KiB übertragen; bei längeren
Bulletins markiert `[…]` die gekürzte Anzeige. Der vollständige Inhalt bleibt im Archiv.
FT8 behält seine Begrenzung auf 50 Zeilen.

## RX5-Diagnose und Pilot bei 500 Hz

Die siebte Ansicht **RX5-Diagnose** zeigt das RX5-Audiospektrum von 0 bis 4000 Hz,
die gemessene Pilotenfrequenz, AFC-Korrektur, verbleibende PLL-Abweichung,
Audiopegel, Empfangsfrequenz und AFC-Zustand sowie die 8APSK-Konstellation.
Im Spektrum ist der Pilotsuchbereich von 20 bis 900 Hz hinterlegt; bei gültiger
Synchronisation markieren gelbe Linien die ungefähren Grenzen des Datensignals.
Eine Warnung weist auf zu wenig Abstand zur Filtergrenze oder einen fehlenden
eindeutigen Piloten hin. Bei Synchronisation werden nur Pilotkandidaten nahe der
vom Modem bestätigten Frequenz übernommen. Ohne Synchronisation ist die Messung
ein Kandidat; erst FEC-/CRC-gültige Frames bestätigen den Empfang.

Ein Pilot bei **500 Hz** wird vom integrierten Decoder automatisch gefunden und
nachgeführt. Dafür wurden die frühere Pilotsuche **20–350 Hz** und die zugehörige
Begrenzung der Mischerkorrektur erweitert. Der feste Abstand zwischen Pilot und
Datenmitte ist 1400 Hz: Bei einem 500-Hz-Piloten liegt die Mitte bei **1900 Hz**.
Die Datenbandbreite beträgt etwa 2880 Hz, somit liegen die Daten ungefähr zwischen
**460 und 3340 Hz** im 0–4000-Hz-Filter. Das schafft deutlich mehr Abstimmreserve.

Die vorhandene RX5-Einstellung wird nicht automatisch verändert. Liegt der Pilot
aktuell bei 100 Hz, die RX5-Frequenz im Setup um **400 Hz niedriger** setzen,
beispielsweise von **10489993220 auf 10489992820 Hz**. Danach in der Diagnose
Pilotenfrequenz und Synchronisation kontrollieren. Die Sucherweiterung unterstützt
weiterhin die bisherige Abstimmung mit einem Piloten bei 100 Hz.

Diagnosedaten werden nur bei geöffneter Diagnoseansicht abonniert, auch für Zuschauer.
Der Webadapter fragt `/v1/diagnostics.json` asynchron höchstens viermal pro Sekunde
ab und reicht begrenzte FFT-/Punktdaten per WebSocket weiter. Ohne Abonnenten gibt
es keine Diagnoseabfragen und keine FFT-Berechnung im MM-Monitor. Die Textkanäle
laufen unabhängig davon weiter. Nach zwei Sekunden ohne aktuelle Audio-/Modemdaten
erlischt die Synchronisationsanzeige; alte Konstellationspunkte und Messkurven
werden nicht als laufender Empfang angezeigt.

## Daten und Schnittstellen

Alle benötigten Quellen, Lizenzhinweise und Originaltests liegen unter
`external/mmdecoder/`. Herkunft und Anpassungen stehen dort in `UPSTREAM.md`.
Eine getrennte Installation unter `/Daten/funk/sat/mmdecoder` ist nicht erforderlich.

Der persistente Datenbestand liegt unter `data/mm-beacon/`:

- `receiver.sqlite3`: verbindlicher Datenbestand samt unvollständigen Bulletins.
- `dx-cluster.txt`, `cw-skimmer.txt`, `bulletin.txt`: Text-Momentaufnahmen.
- `bulletins/`: vollständige Bulletin-Versionen.
- `events.jsonl`: Ereignisexport mit Sequenznummern.

Die vorhandenen Empfangsdaten wurden bei der Integration aus dem ursprünglichen
Projekt übernommen, die SQLite-Datenbank über eine konsistente Backup-Kopie.
Dieses Laufzeitverzeichnis wird nicht in Git eingecheckt. Für eine Sicherung den
Monitor geordnet beenden und das vollständige Verzeichnis kopieren. Mit
`./start_all.sh --mm-data-dir VERZEICHNIS` lässt sich ein anderer Datenbestand wählen.
Eine Prozesssperre verhindert mehrere schreibende integrierte Monitore auf demselben
Datenverzeichnis.

`config/webbridge.conf` enthält:

```ini
rx5_decoder_audio_port = 40916
mm_http_port = 8766
```

Beide Werte auf 0 deaktivieren die Anbindung und den automatischen MM-Prozessstart.
Die HTTP-Schnittstelle bindet ausschließlich `127.0.0.1`; der Webadapter holt die drei
Text-Momentaufnahmen asynchron etwa einmal pro Sekunde ab. Ein fehlender Textserver
blockiert weder Audio noch Bedienung. Neue Browser erhalten die bereits abgeholten
MM-Texte sofort. Die vollständige HTTP-API mit Status, Ereignis-Cursor und
Bulletin-Verzeichnis ist im [übernommenen README](../external/mmdecoder/README.md) beschrieben.

Die Decoderdiagnosen stehen in `build/run/stack-*/decoder-mm.log`.
Zum manuellen Start, wenn keine andere Instanz denselben Datenbestand verwendet:

```sh
./build/bin/qo100_mm_decoder --port 40916 --http-port 8766 --data-dir data/mm-beacon
```

`mm_beacon_chain` prüft mit synthetischem IQ den realen RX5-Demodulator, die
12-/48-kHz-Umsetzung mit 500-Hz-Pilot, AFC/Frame-Dekodierung, Speicherung, alle drei
WebSocket-Textkanäle sowie Diagnoseabonnements für Bediener und Zuschauer.
`mm_decoder_core` prüft außerdem die erweiterte Akquisition und Drift bei 500 Hz;
`mm_diagnostics` prüft Pilotenmessung, Datenbegrenzung, Stille und veraltete Daten.
Alle diese Prüfungen verwenden private Ports und temporäre Datenverzeichnisse.
