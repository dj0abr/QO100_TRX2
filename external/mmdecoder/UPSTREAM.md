# Herkunft

## Integration in QO100_TRX2

Quellstand aus `/Daten/funk/sat/mmdecoder`, übernommen am 09.11.2026.
Die Quellen und Lizenz sind vollständig lokal enthalten; Build, Logs und Daten
des eigenständigen Entwicklungsprojekts werden nicht benötigt.
`CMakeLists.txt` bindet die gepinnte liquid-dsp-Bibliothek des Hauptprojekts ein.
`rx_probe` unterstützt zusätzlich `--input-rate 12000` mit kontinuierlicher
80-dB-Interpolation auf 48 kHz. Die 8APSK-/FEC-/CRC-Dekodierung bleibt unverändert.
Die Pilotsuche wurde von 20–350 Hz auf 20–900 Hz erweitert, die Nachführgrenze
entsprechend auf eine Modemmitte von 2300 Hz. Damit sind 500-Hz-Piloten samt
Drift automatisch empfangbar. TextAPI unterstützt zusätzlich eine optionale
Diagnose-Momentaufnahme unter `/v1/diagnostics.json`.
Die Originaltests erhalten die Binary-Pfade über Umgebungsvariablen.
`decoder/mm_beacon.py` im Hauptprojekt ersetzt die PulseAudio-Aufnahme durch
RX5-UDP-Audio, ergänzt eine Prozesssperre und nutzt den vorhandenen HTTP-Textserver.
Siehe `docs/mm-beacon.md` im Hauptprojekt für Betrieb und Datensicherung.

## Originalherkunft

QO-100-Modem von DJ0ABR / AMSAT-DL
https://github.com/amsat-dl/QO-100-Modem
Commit cbd30059c18607654c1530b7606c18e375b7f74c (2022-10-24).

Enthaltene RX-Bausteine: Symboltracking, Konstellationsabbildung,
Frame-Erkennung, Descrambler, Reed-Solomon-Decoder und CRC16.
Die ursprünglichen Copyright- und Lizenzhinweise sind erhalten.

Lokale Änderungen: schlanker gemeinsamer Header, Entfernung von Pack(),
Insert(), TX_Scramble(), CRC32 und FEC-Encoder/Initialisierung; zusätzliche
Diagnosezähler im Frame-Decoder. symboltracker.cpp ist unverändert; sein Header
verwendet den installierten liquid-dsp-Header. Der Adapter setzt den Tracker
bei nicht endlichen Ausgaben (möglich nach digitaler Stille) zurück.
Der Audio-Downmixer und Vorfilter in src/rx_probe.cpp verwenden dieselben
Parameter wie liquid_if.cpp (48 kHz, 1500 Hz, Dezimation 5, RRC 0.2).
Die automatische Grobabstimmung in src/pilot_afc.cpp und die Frequenzregelung
im RX-Adapter sind lokale Ergänzungen. Im Automatikmodus folgt der Downmixer
der geschätzten Frequenz; die Symboltracker-Implementierung bleibt unverändert.

Kein Audioausgang, Sender, Voice-Codec, Netzwerkserver oder Original-GUI
wird aus dem Original in das RX-Programm eingebunden. Der Python-Adapter
ergänzt eine eigene lokale HTTP-Textschnittstelle. Tests erzeugen ausschließlich
synthetische Audiodateien, ohne Zugriff auf einen Audioausgang.

Die Textsammler in data_collector.py sind lokale Ergänzungen. DX-Cluster und
CW-Skimmer wurden mit dem JavaScript der am 09.11.2026 empfangenen
qo100info.html abgeglichen: DX-Text nach Typ- und Längenbyte, CW-Text direkt
nach dem Typbyte. Bulletin-Dateikopf und Datei-CRC entsprechen ArraySend.cs,
filesend.cpp und crc16.cpp. Empfangene HTML-Seiten werden nicht ausgeführt.
