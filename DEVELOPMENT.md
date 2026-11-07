# QO100_TRX2

C++-Backend für einen QO-100-Transceiver mit getrenntem GUI über UDP.
Der aktuelle Stand empfängt **600 kHz**, berechnet das Gesamtspektrum sowie
einen abstimmbaren **Zoom von ±25 kHz** und demoduliert **zwei unabhängig abstimmbare USB-Audiokanäle**.
Das Python-Test-GUI zeigt beide Wasserfälle und spielt RX1 über die
Default-Soundkartenausgabe ab. RX2 wird für Tests getrennt empfangen und überwacht.
Das GUI sendet Mikrofon-Audio als USB mit unabhängig einstellbarer TX-Frequenz.

**Für GUI-Entwickler:** [UDP-Schnittstelle](docs/udp-protocol.md) ·
[eigenständiges C++-Beispiel](examples/gui_udp.cpp).

## Komplett bauen und starten

```sh
./build_all.sh
./start_all.sh
```

Das erste Skript baut Backend, Pluto-Treiber, WebSocket-Adapter und beide
FT8-Programme. Es konfiguriert den Webadapter auch dann mit, wenn er zuvor im
Build abgeschaltet war. Standardmäßig werden höchstens vier parallele
Build-Jobs verwendet; optional z. B. `CMAKE_BUILD_PARALLEL_LEVEL=2 ./build_all.sh`.
Die benötigten Entwicklungspakete stehen unter **Bauen**.

Das zweite Skript startet Backend, FT8-Empfänger und Webadapter mit
`config/backend.conf` bzw. `config/webbridge.conf`. Der Decoder-Port wird aus
der Adapter-Konfiguration übernommen. Danach **http://localhost:8080/** oder
vom anderen Rechner **http://BACKEND-IP:8080/** öffnen (bzw. die HTTPS-Adresse
der OPNsense). Bei geändertem HTTP-Port zeigt das Skript die passende URL an.
Der Pluto-Treiber wird automatisch vom Backend gestartet.

**Das Start-Terminal offen lassen; Strg+C beendet den gesamten Verbund.**
Getrennte Logs liegen in `build/run/stack-DATUM-UHRZEIT-PID/`.
Belegte Ports führen vor dem Start zum Abbruch: zuvor einzeln gestartete
Programme müssen erst beendet werden. Endet später Backend oder Webadapter, werden auch
die übrigen beendet. Einzelne Monitoringdecoder werden separat neu gestartet. Für einen Test ohne Pluto: `./start_all.sh --source synthetic`.
Alternative Konfigurationen: `--backend-config DATEI --webbridge-config DATEI`.

**Monitoring:** RX3/FT8 und RX4/PSK besitzen eigene USB-Demodulatoren und feste,
im WebGUI unter **Setup → Monitoring-Empfänger** als ganze Hz einstellbare Frequenzen.
`./start_all.sh` startet beide externen Decoder. Die Webbridge leitet die getrennten
Audioströme (Transport-IDs 5/6) lokal weiter und verteilt beide Textquellen gleichzeitig
an die Browser. Radiobuttons wählen die sichtbare Quelle mit getrennten Textpuffern.
RX2 bleibt unabhängig abstimmbar und besitzt keinen Monitoringdecoderanschluss.
Das zugehörige TX2 und die virtuellen Audiogeräte für FLdigi/WSJT-X sind unter
[RX2/TX2-Digitalbetrieb](docs/digital-audio.md) beschrieben. Die Browserauswahl
RX1/RX2 wählt das bediente Paar und den hörbaren Audiokanal.
[Architektur, Ports und Betrieb](docs/webbridge.md#monitoring-rx3-und-rx4).

**Browser-GUI:** Der separate Adapter `qo100_webbridge` verbindet die GUI-UDP-Schnittstelle
mit HTTP/WebSocket und liefert eine HTML/JS-Testseite aus. Ein Bediener, beliebig weitere
Zuschauer bis zum konfigurierten Verbindungslimit; nach fünf Minuten ohne Bedienung wird
der Bediener abgemeldet. [Start und WebSocket-Protokoll](docs/webbridge.md).

```sh
# Zusätzliche Header-Pakete unter Debian/Ubuntu:
sudo apt install libboost-dev nlohmann-json3-dev gfortran libfftw3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/qo100_webbridge --config config/webbridge.conf
# http://BACKEND-IP:8080/ (Mikrofon über die HTTPS-Adresse der OPNsense)
```

Der Adapter läuft zusätzlich zum Backend. Währenddessen darf kein weiteres GUI direkt
den Backend-UDP-Port abonnieren. Für einen Build ohne Adapter: `-DQO100_BUILD_WEBBRIDGE=OFF`.

## Bauen

Benötigt werden Linux, CMake >= 3.20, ein C++17-Compiler, libiio (0.x-API),
libad9361, libsamplerate und FFTW3 mit Float-Unterstützung.
Für das Test-GUI: Python 3, tkinter, numpy und Pillow; zur Audioausgabe die
Systembibliotheken libasound und libsamplerate. Es ist kein zusätzliches
Python-Audiopaket nötig. Unter Debian/Ubuntu heißen die zusätzlichen
Pakete `libsamplerate0-dev` und `libasound2`.
Auf diesem Rechner sind diese Abhängigkeiten bereits vorhanden.

```bash
cd /Daten/funk/sat/QO100_TRX2
make
make test
```

Alternativ ohne Make-Wrapper:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**liquid-dsp 1.8.3 ist bereits im Projekt enthalten**: statische und dynamische
Bibliothek für Linux x86-64, passender Header, MIT-Lizenz und Prüfsummen unter
[`external/liquid-dsp-1.8.3/`](external/liquid-dsp-1.8.3/README.md).
Backend und Pluto-Treiber verwenden dieselbe statisch eingebundene Version.
Eine systemweit installierte liquid-Version wird nicht benutzt; der normale
Build benötigt dafür weder Download noch Bibliotheksquellcode.
Beim Umzug auf einen anderen Linux-x86-64-PC dieses Verzeichnis mitkopieren
und ein neues `build/` konfigurieren. Weitere Plattformen brauchen passende
Binärdateien; ein optionaler Neuaufbau erfolgt mit `python3 tools/build_liquid.py`.

Der normale Build erzeugt Backend und Pluto-Treiber aus Quellcode:
`build/bin/qo100_backend` und `build/bin/pluto`. Mit eingeschaltetem Webadapter
kommen `qo100_webbridge`, `qo100_ft8_decoder` und dessen Rechenkern `ft8-decoder`
im selben Verzeichnis hinzu.
Auch ein gezielter Build von `qo100_backend` baut den Treiber mit.
Die Originalkopie unter `external/Pluto/` ist im Projekt enthalten; beim Bauen
erfolgt kein Download. Das mitkopierte ursprüngliche Binary wird nicht benutzt.

**SpeexDSP 1.2.1 ist ebenfalls lokal enthalten**: statische Bibliothek für Linux
x86-64, öffentliche Header, BSD-Lizenz und Prüfsummen unter
[`external/speexdsp-1.2.1/`](external/speexdsp-1.2.1/README.md).
Das Backend bindet diese Bibliothek statisch ein. Ein Systempaket oder Download
beim normalen Build ist dafür nicht erforderlich. Für andere Linux-Architekturen
gibt es den optionalen Helfer `python3 tools/build_speex.py`.

## RX1 Audio-Rauschreduktion (Testversion)

Neben den RX1-Bandbreiten-Presets stehen **RX1 NR** und ein Stärkeregler von
**0 bis 100 %**. Zum ersten Hörvergleich eine Station einstellen, etwa **60 %**
wählen und **RX1 NR** abwechselnd ein- und ausschalten. Beim Ausschalten bleibt
der gewählte Reglerwert für den nächsten Vergleich erhalten. Der Status daneben
zeigt den vom Backend bestätigten Wert.

Die SpeexDSP-Rauschreduktion sitzt nach USB-Demodulation/Bandbreitenfilter im
12-kHz-PCM-Pfad von RX1, vor der UDP-Ausgabe. Sie verarbeitet die vorhandenen
240-Sample-Blöcke. Der Regler setzt die maximal erlaubte Rauschdämpfung auf
bis zu 30 dB (1-dB-Schritte); das ist keine garantierte Verbesserung des
Signal-Rausch-Abstands. 50 % entsprechen maximal 15 dB. Starke Einstellungen
können schwache Sprachanteile beeinträchtigen. Verständlichkeit und angenehmer
Klang müssen deshalb im Funkbetrieb beurteilt werden.

**0 % bzw. ausgeschaltet = unverändertes PCM** nach einer kurzen Überblendung
von 20 ms. Die Rauschschätzung läuft auch während der Überbrückung mit, damit
kein manuelles Einlernen nötig ist. Aktive NR fügt durch die überlappte Analyse
20 ms Audioverzögerung hinzu. Umschalten setzt weder Samplezähler noch RX-Epoch
zurück. Beim normalen Abstimmen bleiben Rauschschätzung und RX-Filterzustände erhalten.
AGC, VAD und Echounterdrückung sind deaktiviert. RX2 für externe Digitalprogramme,
beide Wasserfälle, TX und der direkte Audio-Loop bleiben unverändert.

Konfigurationswert `rx1_nr_percent = 0` ist der Standard; erlaubt sind ganze
Prozentwerte 0..100. Die GUI fragt den konfigurierten Wert zunächst ab. Änderungen
über UDP werden in `backend.settings` gespeichert; `backend.conf` bleibt die Vorgabe. Protokoll: **NR_CONTROL
(15)** und **NR_STATE (16)** über den vorhandenen GUI-Port, nur Stream 1.
Details stehen in [`docs/udp-protocol.md`](docs/udp-protocol.md).

## Starten

Die Konfiguration steht in [`config/backend.conf`](config/backend.conf).
Voreingestellt sind:

| Bedeutung | Frequenz |
|---|---:|
| QO-100 RX-Anzeigemitte | 10.489,750 MHz |
| Tatsächliche Pluto-RX-Mitte | 739,750 MHz |
| QO-100 TX-Bandmitte | 2.400,250 MHz |
| Tatsächliche Pluto-TX-Mitte | 1.290,250 MHz |

```bash
./start_backend.sh
```

Das Backend bindet zuerst den lokalen I/Q-Port und startet anschließend **seinen
eigenen** Pluto-Treiber mit einer erzeugten Config-Datei. Der Pluto wird über
`ip:192.168.10.25` angesprochen. Mit `Ctrl+C` werden Backend und Kindprozess beendet.
Bei einem unerwarteten Treiberende oder ausbleibenden I/Q-Daten meldet das
Backend den Fehler, sperrt TX und versucht die Verbindung erneut. Die Settings
bleiben währenddessen über UDP erreichbar. Bei hartem Tod des Backends erhält der Treiber unter
Linux zusätzlich ein Elternprozess-Todessignal.

Der Treiber läuft im Vollduplexbetrieb mit `RX_ONLY 0` und `TX_RAW_IQ 1`.
Das Backend erzeugt fertige IQ-Samples; der zusätzliche direkte Treibermodus
fügt weder Markerton noch Interpolation oder Pegeländerungen hinzu. Ohne
TX-Audio werden Nullsamples durchgehend weitergegeben. Der Treiber startet
in `build/run/pluto-<PID>-<Startnummer>/` mit eigener Konfiguration und Diagnose-Log.

In einem zweiten Terminal:

```bash
./start_gui.sh
```

Die Startfrequenz beträgt **10489,900000 MHz** (USB-Träger, nicht die
Mitte des Audio-Durchlassbereichs). Im Feld **RX1 MHz (USB / Audio)** kann die Frequenz
mit Punkt oder Komma eingegeben und mit Enter oder **Tune** übernommen werden.
Der Zoom ist auf diese Trägerfrequenz zentriert; das Audio kommt aus dem oberen
Seitenband. Das GUI zeigt die vom Backend bestätigte Frequenz an.
**Audio gain** steuert die lokale Wiedergabeverstärkung; 0 schaltet stumm.
Mit `./start_gui.sh --no-audio` ist ein reiner Anzeigetest möglich.

GUI auf einem anderen Rechner mit Python/numpy/Pillow/tkinter:

```bash
python3 gui/test_gui.py --host IP_DES_BACKEND_RECHNERS
```

Das GUI meldet sich alle zwei Sekunden am Backend an. Dieses schickt Daten an
die Absenderadresse der Anmeldung. Es wird zunächst **ein GUI-Teilnehmer**
unterstützt; eine neue Anmeldung übernimmt den Datenstrom. Ohne weitere Anmeldung
endet die Übertragung nach acht Sekunden. UDP-Port 40900 muss im LAN erreichbar sein.
Anzeigefrequenzen sind Downlinkfrequenzen; die FFT ist von -300 bis knapp +300 kHz
um die konfigurierte Mitte sortiert. Der NB-Transponder liegt damit innerhalb
der Anzeige, mit jeweils 50 kHz Reserve.

## Abstimmung mit der Maus

Ein **Linksklick in den oberen oder unteren Wasserfall** stimmt RX1 auf die
Frequenz unter dem Mauszeiger ab. Ein Klick in die zugehörige Spektrumkurve
funktioniert genauso. TX folgt auf die passende QO-100-Uplinkfrequenz und behält
den eingestellten TX-Versatz bei. RX2 bleibt unabhängig.

Mit dem **Mausrad über einem der beiden Wasserfälle** wird nur RX1 verstellt:
nach oben **+10 Hz**, nach unten **−10 Hz** je Rastung (Standardschrittweite).
Der Zoom folgt RX1; TX und RX2 behalten ihre Frequenz. Der dadurch geänderte
TX-Versatz steht neben der PTT-Taste.
Eine andere Schrittweite lässt sich beim Start wählen, beispielsweise:

```bash
./start_gui.sh --wheel-step-hz 50
```

Die Zuordnung lautet `TX = RX − 8089,500 MHz + Offset`. Beispielsweise ergeben
RX **10489,900000 MHz** und Offset **+30 Hz** die TX-Frequenz **2400,400030 MHz**.
Der nächste Wasserfall-Klick behält diese +30 Hz bei. Manuelle Frequenzeingaben
mit **Tune/Enter** bleiben unabhängig und setzen den daraus resultierenden
Versatz für die weitere Mausbedienung.

**Blaue RX1- und orange TX-Markierungen** zeigen die bestätigten Trägerpositionen
in beiden Darstellungen. Für die TX-Markierung wird die Uplinkfrequenz auf die
Downlinkskala umgerechnet. Die Maus verwendet die Geometrie des tatsächlich
angezeigten Bildes, auch nach einer Fenstergrößenänderung oder während eines
Zoom-Neuaufbaus. Klicks in Beschriftungen und Ränder werden ignoriert; die
bisherigen Kanalgrenzen mit 25 kHz Randreserve bleiben bestehen. Klick und
Mausrad schalten PTT nicht ein.

Ein GUI-/UDP-Test mit synthetischen Samples und abgeschalteter Audioausgabe
prüft beide Wasserfälle, Mausrad, Frequenzzuordnung und RX2-Unabhängigkeit:

```bash
/usr/bin/python3 tests/test_gui_mouse.py
```

Dieser Test benötigt einen Bildschirm und gehört nicht zu CTest. Er startet
sein eigenes synthetisches Backend auf einem freien GUI-Port. Ergebnisbild und
Protokoll liegen unter `build/captures/mouse-*`. Die Frequenz-/Geometrieprüfung
ohne Bildschirm ist als `gui_tuning` in CTest enthalten.

## Die zwei Schnittstellen

1. **Pluto-Anbindung ausschließlich lokal**: `127.0.0.1`,
   Port 40809 für RX-IQ, Port 40808 für TX-IQ und Port 40810 für die
   TX-FIFO-Rückmeldung.
   Der QO100-Treiberbuild bindet auch im Normalmodus nur Loopback und erlaubt
   ausgehende Treiber-UDP-Daten ausschließlich an `127.0.0.1`.
2. **GUI-Anbindung**: ein bidirektionaler UDP-Port, standardmäßig
   `0.0.0.0:40900`. Anmeldung, Abstimmung mit Bestätigung, Gesamt-/Zoom-FFT und RX-Audio
   sowie TX-Audio und Software-PTT verwenden unterschiedliche Nachrichtentypen.
   Es gibt keinen Webserver und keine zusätzliche GUI-Verbindung.

Nur die Verbindung **vom libiio-Treiber zur Hardware** verwendet das Netz.

## FFT und Datenformat

Pluto: 1,2 MS/s, Treiber-Dezimation 2, Backend: 600 kS/s komplex.
Die bestehende Treiber-Dezimation bleibt unverändert. Ihre vorgelagerte
Bandbegrenzung bestimmt daher weiterhin das Aliasingverhalten; das Backend
fügt an dieser Schnittstelle keinen neuen Dezimationsschritt hinzu.
Der QO100-Build verwendet 24.000 Hardware-Samples je Empfangsblock (20 ms).
Die Original-Vorgabe von 180.000 wird nur für diesen Build überschrieben.

Das Backend verarbeitet I16/Q16 Little Endian und akzeptiert auch kürzere letzte
Treiberpakete. FFT: 8.192 Punkte, Hann-Fenster, 50 % Überlappung,
lineare Leistungsmittelung vor der logarithmischen Darstellung. Voreinstellung:
im Mittel 20 FFT-Zeilen/s; die einzelnen Zeitabstände sind durch das FFT-Hopraster gerastert.
Die Binbreite beträgt 73,2421875 Hz.

Die Pegel sind auf einen komplexen Vollaussteuerungston normierte **dBFS je Bin**.
Sie sind nicht auf dBm kalibriert; Rauschpegel hängen von Fenster und FFT-Länge ab.
Auf dem GUI-Link sind alle Ganzzahlen Big Endian und ein Paket höchstens
1.200 Byte lang. Details: [`docs/udp-protocol.md`](docs/udp-protocol.md).
Die kleinen Pakete gelten für den neuen GUI-Link; die vorhandenen größeren
Treiberpakete bleiben auf Loopback.

## Zoom und USB-Audio

Ein unabhängiger digitaler Mischer verschiebt die gewünschte Frequenz auf null.
Ein liquid-dsp-321-Tap-Kaiser-FIR mit Dezimation 8 erzeugt **75 kS/s komplex** für den Zoom.
Die Zoom-FFT hat 8192 Punkte (9,155 Hz/Bin); angezeigt werden die mittleren
50 kHz. Wegen des 50-%-Hoprasters werden maximal etwa 18,3 Zoomzeilen/s erzeugt.
Das Gesamtspektrum läuft parallel weiter.

Ein zweikanaliger Sinc-Resampler reduziert den Kanal auf 12 kS/s komplex.
Ein komplexer liquid-dsp-513-Tap-FIR wählt standardmäßig das obere Seitenband **+100 bis +4000 Hz**;
an den Grenzen liegt der übliche Filterübergang. Anschließend wird der Realteil
als Mono-Audio ausgegeben. Es gibt keine AGC und keine Sprach-/Rauschbearbeitung.

Das Audio verwendet **12 kHz, Mono, PCM16** und Pakete mit 240 Samples/20 ms.
Das sind **24 kB/s reine Audiodaten**; mit dem QTR2-Header 27,2 kB/s UDP-Nutzlast.
Die Übertragung bleibt innerhalb derselben GUI-UDP-Schnittstelle. Metadaten
enthalten Sample-Zähler und Audio-Stromgeneration. Das GUI puffert kurzzeitig,
sortiert anhand der Sample-Indizes und ersetzt fehlende Pakete durch Stille.
Der Puffer bleibt auf höchstens 0,5 s begrenzt. Ein langsam nachgeregelter
Sinc-Resampler gleicht die Taktabweichung zwischen Pluto und Soundkarte aus.
Die Soundkarte wird über ALSA **default** geöffnet; ALSA kann bei Bedarf auf
die Hardware-Samplerate umsetzen. Der Audiothread arbeitet unabhängig von Tk.

Bei einer normalen RX-Frequenzänderung wird nur die Mischfrequenz geändert.
Oszillatorphase, Kanalfilter, NR, Resampler, Zoom und Audiopuffer laufen weiter;
Audio-Epoche und Sample-Zähler bleiben kontinuierlich. Die Tonhöhe folgt der
Abstimmung ohne Stummschaltung. Nur echte Hardware-/Signalweg-Resets wechseln
die Audio-Epoche und leeren die Puffer. Verspätete ältere Befehle werden ignoriert. Ein vollständiger ±25-kHz-Zoom muss
innerhalb des 600-kHz-Empfangsfensters bleiben.

## RX1: Untere und obere Filtergrenze

RX1 besitzt im Python-Test-GUI und im WebGUI zwei Regler: **untere Grenze**
und **obere Grenze**, jeweils in Audio-Hz relativ zum USB-Träger. Eine Änderung
verschiebt nur die jeweilige Grenze; die andere bleibt stehen. Zulässig sind
**100–4000 Hz** bei mindestens **500 Hz Abstand**. Die Regler begrenzen sich
entsprechend. Beispiel: unten **300 Hz**, oben **2700 Hz** ergibt **2400 Hz BW**.
Abstimmfrequenz und Zoom-Mitte ändern sich dadurch nicht.

Bandbreiten-Presets behalten die untere Grenze, soweit die neue Breite hineinpasst.
**Full BW** setzt **100–4000 Hz**. Die Grenzen bezeichnen den nominellen
Durchlassbereich mit etwa −6 dB an den Kanten; der FIR hat endliche Übergangsbereiche.

Die GUIs lesen die Werte beim Start vom Backend. Das Backend speichert Änderungen
dauerhaft; die Vorgaben in `config/backend.conf` lauten:

```ini
rx1_filter_low_hz = 100
rx1_filter_high_hz = 4000
```

Alte Konfigurationen und gespeicherte Werte mit `rx1_filter_bandwidth_hz` und
`rx1_filter_center_hz` werden übernommen und beim nächsten Speichern als Grenzen
abgelegt. Auch bisherige halbe Hertz an den Rändern bleiben erhalten.

Ungültige Kombinationen verhindern den Backend-Start bzw. werden bei UDP-Steuerung
mit Fehlerstatus abgelehnt. Änderungen werden zwischen gleich langen, mit der
Sample-Historie vorgefüllten Filtern über 20 ms überblendet. Bei schneller
Bedienung wird die jeweils neueste Einstellung nachgeführt. Audiozähler,
Audiogeneration und Zoom laufen ohne Rücksetzung weiter. **RX2 behält seinen
Durchlassbereich 100–4000 Hz; TX ist fest auf 100–3000 Hz begrenzt.** Die Audioübertragung bleibt
bei 12 kHz Mono und verwendet denselben GUI-UDP-Port.

Die automatischen Tests prüfen Filterkurven, Übergänge beim Verstellen und die
UDP-Steuerung einschließlich durchgehender Audioströme beider Empfänger.

## Zweiter Empfänger für späteren Digitalbetrieb

RX2 läuft gleichzeitig mit RX1 und erhält denselben breiten I/Q-Strom. Beide
USB-Demodulatoren besitzen eigene Mischer, Filterzustände, Resampler,
Abstimmsequenzen, Audiogenerationen und Sample-Zähler. Eine Abstimmung von RX2
setzt RX1 weder zurück noch unterbricht sie dessen Audio; umgekehrt ebenso.
Das vorhandene Gesamtspektrum und der Zoom von RX1 laufen gemeinsam weiter.

Im Test-GUI gibt es eine zweite Eingabe **RX2 MHz (Digital-Test)** mit
**Tune RX2** oder Enter. Der Startwert ist **10489,600000 MHz**; im Backend
steht er unter `initial_rx2_hz`, im GUI kann er mit `--rx2-frequency` gewählt
werden. Empfangsstatistik und bestätigte Frequenz werden separat angezeigt:
Paketanzahl, Sample-Anzahl, RMS und Spitzenpegel. RX2 wird weder auf die
Soundkarte gemischt noch einer Sprachbearbeitung oder einem Digitaldecoder
zugeführt. Seine kurze Test-Queue ist begrenzt und wird nach der Auswertung
verworfen. Das Backend erzeugt zusätzlich einen unabhängigen RX2-Zoom
(8192 Bins bei 75 kHz, nutzbarer Ausschnitt ±25 kHz um RX2). Dieser kommt als
`SPECTRUM`, Stream 2, über UDP und den Webadapter bis zum Browser. Die Webseiten
und die beiden bestehenden Anzeigen des Test-GUIs bleiben vorerst unverändert.

Beide Audioströme sind **12 kHz, Mono, PCM16**, jeweils 240 Samples pro Paket.
Die Stream-ID 1 kennzeichnet RX1, die Stream-ID 2 RX2. Alle Daten und
Steuerbefehle verwenden weiterhin denselben GUI-UDP-Port. Reine Audionutzdaten:
24 kB/s je Kanal, zusammen 48 kB/s. Beim normalen Retune bleiben Audio-Epoche,
Filter, Resampler, Oszillatorphase und Sample-Takt erhalten. Beide Audioströme
laufen für eine unterbrechungsfreie Abstimmung weiter. Eine lokale Änderung der
RX1-Wiedergabelautstärke verändert die übertragenen Samples von RX2 nicht.

```bash
./start_gui.sh --rx-frequency 10489.900000 --rx2-frequency 10489.600000
```

Der Hardwaretest speichert zusätzlich `build/captures/rx2-usb.wav`, damit sich
der zweite Audiostrom prüfen lässt. Es sind weiterhin keine Digitalmodems oder
externen Audio-Routing-Verbindungen Bestandteil des Backends.

## Mikrofon und USB-Sender

Die Testvorgaben sind **RX 10489,900 MHz** und **TX 2400,400 MHz**.
Die TX-Frequenz ist die Uplink-Trägerfrequenz; bei der konfigurierten Pluto-Mitte
1290,250 MHz entsteht sie durch einen digitalen Versatz von +150 kHz.
RX und TX sind unabhängig abstimmbar. Die RX-Bandmitte bleibt 10489,750 MHz.

Im GUI **TX MHz (USB)** eingeben und **Tune TX** drücken. Zum Sprechen die
Taste **PTT halten — Mikrofon senden** mit der Maus gedrückt halten.
Loslassen, Escape, Fokusverlust oder Schließen beendet den Mikrofonversand.
Beim Start und nach einem Backend-Neustart ist PTT aus. Der GUI-Status zeigt
die bestätigte TX-Frequenz, Paketanzahl, FIFO-Füllstände und Taktkorrektur.
`--tx-frequency 2400.400000` setzt einen anderen Startwert.

Der eingebundene Treiber besitzt keinen PTT-Steuerbefehl. Seine Erwähnung
von GPO0/GPO1 betrifft eine externe Schaltung und ist keine Software-Schnittstelle.
Die GUI-Taste ist deshalb eine **Software-PTT für die Audiodaten**, keine
Steuerung einer externen Endstufe. Der Hardware-Datenstrom läuft mit Nullsamples
weiter, solange kein freigegebenes Audio vorliegt.

Die Default-Soundkarte wird nur während PTT als Mono-Eingang mit 48 kHz geöffnet.
Ein Sinc-Tiefpass reduziert auf 12 kHz. UDP enthält 240 PCM16-Samples je 20 ms,
über denselben bidirektionalen GUI-Port wie RX und FFT. Eine bestätigte
TX-Generation schützt vor alten Paketen nach PTT oder Frequenzänderungen.

Im Backend puffert ein begrenzter, nach Sample-Indizes sortierter FIFO das Audio:
maximal 240 ms, Zielwert 120 ms in der Mitte. Ein langsam geregelter adaptiver
Sinc-Resampler gleicht Soundkarten- und Pluto-Takt bis ±5000 ppm aus.
Fehlende Paketintervalle werden mit Stille gefüllt. Bei länger als 200 ms
fehlendem Audio wird der Modulator geleert; ein Neustart wartet erneut auf
120 ms Vorfüllung. Ohne gültige PTT-Steuerung für 1,5 s oder bei GUI-Wechsel
wird die Sendefreigabe gelöscht.

Der einstellbare Sprachkompressor von TX1 verarbeitet das 12-kHz-Audio nach der
Taktkorrektur. Ein analytischer 513-Tap-FIR erzeugt USB mit festem
**+100 bis +3000 Hz** Durchlassbereich (rund 3 kHz Sendebandbreite). Die nominellen
Filtergrenzen liegen bei −6 dB; die endliche Flankenbreite beträgt etwa 100 Hz
bis zum Sperrbereich. Der Filter sitzt nach dem Kompressor und unterdrückt damit
auch dessen neu entstandene Frequenzanteile oberhalb des Sprachbands.
Danach erfolgen exakte liquid-Polyphasen-Interpolation um Faktor 100 auf
1,2 MS/s, Begrenzung der tatsächlichen SSB-Hüllkurvenspitzen und digitales Mischen auf die
TX-Frequenz und I16/Q16-Konvertierung mit 6 dB digitaler Pegelreserve.
Die tatsächliche AD9361-Ausgangsdämpfung steht in `tx_attenuation_db`
(0..40 dB; Projektkonfiguration 0 dB für die getestete Station). Änderungen
können über den Einstellungsdialog übernommen und dauerhaft gespeichert werden. Der Sende-/Empfangstest über QO-100
wurde mit 0 dB TX-Dämpfung erfolgreich durchgeführt.

Der Treiber meldet nach jedem Hardwareblock den TX-Füllstand. Das Backend
hält vier von acht Plätzen gefüllt (80 ms) und erzeugt neue Blöcke im Takt
dieser Rückmeldung. Der Audiopuffer regelt den unabhängigen Mikrofontakt.
Bei leerem Treiber-FIFO wird ein frischer Nullblock an den Pluto übertragen.
Bereits eingereihte Blöcke und Hardwarepuffer erzeugen beim PTT-Loslassen eine
kurze Nachlaufzeit. Es wird kein alter IQ-Block wiederholt.

Die automatischen Tests prüfen USB-Pegel und Seitenbandunterdrückung,
Taktabweichungen von ±1500 ppm über je fünf simulierte Minuten und die komplette
UDP-TX-Kette mit einem lokalen Treiber-Doppel. Dabei wird keine Hardware benutzt.

## Sprachkompressor für Modulator 1

Der GUI-Regler **TX1 Sprachkompressor (%)** stellt die Stärke von **60 bis 140 %**
in 0,1-%-Schritten ein. **100 % Normal** stellt die im Funkbetrieb geprüfte
Normaleinstellung wieder her. Leise Sprachanteile werden bei 60 % um bis zu
**10,8 dB**, bei 100 % um bis zu **18 dB**, bei 140 % um bis zu **25,2 dB** angehoben.
Laute Anteile werden zunehmend verdichtet: bei 100 % mit 4:1, bei 140 % mit
8,8:1 oberhalb der weichen Kompressionsschwelle. Die Kennlinie bei 100 % bleibt
gegenüber dem erfolgreichen QSO-Test unverändert.
Der Startwert ist **100 %** und kann in `config/backend.conf` gewählt werden:

```ini
tx1_compressor_percent = 100
```

Das GUI liest den Backend-Wert beim Verbinden aus und zeigt die bestätigte
Stärke sowie aktuelle Verdichtung und Spitzenbegrenzung in dB an. Änderungen
werden weich nachgeführt (60–140 % in 16 ms) und unterbrechen weder FIFO noch PTT oder
Audiogeneration. Derselbe GUI-UDP-Port überträgt Einstellung und Rückmeldung.

Ein eigener Spitzenbegrenzer berücksichtigt auch Überhöhungen durch SSB-Filter
und Interpolation. Er begrenzt die komplexe Ausgangshüllkurve auf nominell 0,5,
erhält die bisherige digitale Pegelreserve und bleibt auch bei Sprachkompressor
0 % aktiv. Der Backend-/UDP-Bereich bleibt 0–140 %, sodass ein Bypass für Tests
oder externe Digitalanwendungen verfügbar ist. Die Vorausschau des Spitzenbegrenzers
fügt in allen Einstellungen **5 ms** Verzögerung
hinzu. Fehlendes UDP-Audio und PTT aus erzeugen weiterhin exakt Nullsamples.
Die Hardware-TX-Dämpfung bleibt unabhängig davon auf dem konfigurierten Wert.

Die Verarbeitung gehört zur einzelnen TX-Instanz; RX1/RX2 werden nicht verändert.
Ein weiterer Modulator kann später ohne Sprachverdichtung betrieben werden,
etwa für externe Digitalprogramme. Einzelheiten und Messprüfungen:
[`docs/speech-compressor.md`](docs/speech-compressor.md).

## Testschleifen: Audio-Loop und HF-Loop

Oben im Test-GUI kann zwischen **Normal**, **Audio-Loop** und **HF-Loop** gewählt
werden. Dieselben Modi sind über UDP steuerbar; die Einstellungen gelten jeweils
bis zum Ausschalten oder zum Ende der GUI-Verbindung. Ein Moduswechsel beendet
eine laufende Mikrofonfreigabe. Danach die Mikrofon-/PTT-Taste neu drücken.

**Audio-Loop:** Die Taste **Audio testen halten** gedrückt halten und sprechen.
Das GUI sendet weiterhin 12-kHz-Mono-Audio. Das Backend schickt die empfangenen
PCM-Samples sofort und unverändert auf Audiostream **4** zurück, ohne FIFO,
Kompressor oder Modulator zu durchlaufen. Das GUI spielt diesen Rückkanal auf
ALSA default ab und unterdrückt währenddessen die RX1-Wiedergabe. Die
Wiedergabeverstärkung startet im Audio-Loop mit Faktor 1 und wird beim Verlassen
auf den vorherigen Wert zurückgesetzt. Der HF-TX-Datenstrom besteht in diesem
Modus immer aus Nullsamples, auch bei gedrückter Mikrofontaste. RX-Spektren und
beide normalen RX-Audioströme laufen parallel weiter.

**HF-Loop:** Der lokale Pluto-Treiber wird kurz neu gestartet, wobei sein RX-LO
auf denselben Wert wie der TX-LO gesetzt wird: mit der Projektkonfiguration
**1290,250 MHz**. RX1 folgt zusätzlich dem digitalen TX-Frequenzversatz. Damit
empfängt und demoduliert er genau die tatsächlich erzeugte Pluto-Sendefrequenz,
auch nach weiterer TX-Abstimmung. Über die normale PTT-Taste kann man senden
und sich zurückhören. Dieser Hörtest benötigt eine HF-Einkopplung vom TX zum RX;
die passende Kopplung/Dämpfung gehört zum HF-Aufbau.

Im HF-Loop bekommt auch RX2 den auf das TX-Band umgestellten Hardwarestrom.
RX1-Abstimmung, Wasserfall-Klick und Mausrad sind gesperrt; die manuelle
TX-Abstimmung funktioniert weiter. Die Frequenzanzeige bleibt auf die zugehörige
QO-100-Downlinkskala umgerechnet. **Normal** stellt den RX-LO wieder auf
**739,750 MHz** und die zuvor gewählten RX-Frequenzen zurück. Filter- und
Kompressoreinstellungen bleiben erhalten. Die Umschaltung wird erst nach
verifizierten LO-Einstellungen und frischen I/Q-Daten bestätigt.

Beide Testmodi benutzen den vorhandenen GUI-UDP-Port. Die neuen Befehle heißen
`LOOP_CONTROL` (Typ 13) und `LOOP_STATE` (Typ 14); Moduswerte sind 0 Normal,
1 Audio-Loop und 2 HF-Loop. Audio-Echo ist `AUDIO`, Stream 4, 12 kHz Mono PCM16.
Details: [`docs/udp-protocol.md`](docs/udp-protocol.md).

Die automatische Prüfung `test_loops_udp` verwendet einen lokalen Treiberersatz
und prüft bitgenauen Echo-Inhalt, Paketumsortierung, HF-Stummschaltung,
TX-IQ-Rückempfang, Frequenznachführung und Rückkehr zum Normalbetrieb. Ein
zusätzlicher Hardwaretest prüft beide echten Pluto-LOs und die Rückkehr zur
Empfangsfrequenz **mit ausgeschalteter PTT**; dafür darf kein anderes Backend laufen:

```bash
/usr/bin/python3 tests/test_loop_hardware.py
```

## Ohne Hardware testen

```bash
./start_backend.sh --source synthetic
./start_gui.sh --rx-frequency 10489.748500
```

Das Gesamtspektrum enthält zwei Referenztöne bei -150 kHz (-12,04 dBFS)
und +75 kHz (-20 dBFS). Zusätzlich erzeugt die Quelle relativ zur synthetischen
USB-Testfrequenz 10489,748500 MHz einen gewünschten +1-kHz-Ton und einen stärkeren -2-kHz-Ton.
Für RX2 kommen ein +1700-Hz-Nutzton und ein stärkerer -2200-Hz-Ton hinzu.
Am Audioausgang bleibt der 1-kHz-Ton. Der Pluto-Treiber wird in diesem Modus
nicht gestartet. Für einen stillen Test kann `--no-audio` verwendet werden.

Headless-Empfang, Speichern eines FFT-Frames und eines gerenderten Testbilds:

```bash
mkdir -p build/captures
./start_gui.sh --headless --frames 100 --timeout 20 \
  --capture build/captures/rx.npz --screenshot build/captures/rx.png
```

Ein vollständiger Hardwaretest startet und beendet das Backend selbst, prüft
die tatsächliche RX-Frequenz und Abtastrate sowie
den Empfang von 120 vollständigen FFT-Zeilen. Dafür wird zusätzlich `iio_attr`
aus den libiio-Werkzeugen benötigt. Es darf dabei kein anderes Backend laufen:

```bash
/usr/bin/python3 tests/test_hardware.py --gui
```

Der Hardwaretest prüft zusätzlich Zoom, 12-kHz-Audio und bei `--gui` die
Ausgabe an ALSA default. `audio-playback.json` enthält geschriebene Samples,
Audiofehler und Underruns; `rx-usb.wav` enthält das empfangene Mono-Audio.
Ergebnis und Aufnahmen liegen in `build/captures/`. Ohne `--gui` läuft dieser
Test vollständig ohne Bildschirm. Er gehört ausdrücklich nicht zu `make test`.

Die Umstellung auf liquid-dsp umfasst RX-/TX-Mischer, FIR-Filter und die feste
TX-Interpolation. FFTW sowie libsamplerate für Audio-Taktkorrektur bleiben im
Einsatz. Filterkurven, Nebenlinien, Frequenzgenauigkeit und Bibliothekszuordnung
werden zusätzlich automatisch geprüft. Einzelheiten:
[`docs/dsp-liquid-migration.md`](docs/dsp-liquid-migration.md).

## Projektaufteilung

- `src/`: Konfiguration, FFT, UDP-Protokoll, Kindprozessverwaltung und Backend.
- `include/qo100/`: Schnittstellen der C++-Bausteine.
- `gui/`: einfaches Test-GUI und unabhängiger Python-Protokollempfänger.
- `tests/`: DSP-/Protokoll-/Audiopuffertests und Ende-zu-Ende-Test mit bekannten Testtönen,
  USB-Seitenbandunterdrückung, Zoom und unabhängiger Abstimmung beider Empfänger.
- `external/Pluto/`: eingebundene Kopie des bestehenden Treibers.
- `docs/pluto-integration.patch`: nachvollziehbare lokale Änderungen am Treiber.

Digitale Betriebsarten werden später über getrennte Audioströme externer
Programme angebunden. Im Backend werden keine Digitalmodems implementiert.
Künftige Empfänger erhalten eigene Stream-/Kanal-IDs innerhalb derselben
GUI-Schnittstelle.

Die ursprünglichen Lizenzdateien und Copyright-Hinweise des Pluto-Projekts sind
unter `external/Pluto/` unverändert enthalten. Herkunft und lokale Anpassungen:
[`docs/pluto-upstream.md`](docs/pluto-upstream.md).

## Zentrale, dauerhaft gespeicherte Settings

Über **Einstellungen** im Test-GUI werden Pluto-Verbindung (**Netzwerk/IP oder
USB**), LNB-Referenz, RX-/TX-Frequenzkorrektur, nominale Pluto-TX-Mitte,
TX-Dämpfung **0..40 dB in 0,25-dB-Schritten** und Mausrad-Schrittweite eingestellt.
Der Dialog zeigt die daraus berechneten Hardwarefrequenzen. **Übernehmen und
speichern** sendet nur die geänderten Werte an das Backend; erst nach Prüfung,
Übernahme und erfolgreicher Speicherung erscheint die Bestätigung.

Das Backend ist die zentrale Ablage, auch für reine GUI-Einstellungen. Es merkt
sich außerdem RX1-/RX2-/TX-Trägerfrequenzen, untere/obere RX1-Filtergrenze, NR samt
letztem Reglerwert, TX-Kompressor, lokale Audioverstärkung und Display-Untergrenze.
Beim Öffnen fragt das GUI zuerst diese Werte ab. Die bisherigen fest eingebauten
GUI-Startfrequenzen überschreiben damit keine Backend-Einstellungen mehr.
Explizite CLI-Frequenzargumente gelten weiterhin als bewusste Vorgabe.

Die Vorgaben aus `config/backend.conf` werden beim Start mit der lokalen Datei
**`config/backend.settings`** überlagert. Die Datei ist lesbarer `key=value`-Text,
wird vom Backend atomar geschrieben und ist von Git ausgenommen. Beim Umzug die
Datei mitkopieren. Ohne GUI verwendet das Backend dieselben gespeicherten Werte
und berechnet die LOs bereits vor dem Start des Pluto-Treibers. Ein eigener
Dateipfad ist über `--settings DATEI` möglich. Fehlende Dateien verwenden die
Vorgaben; fehlerhafte Dateien werden mit einer Fehlermeldung abgewiesen.

```
Pluto RX = RX-Anzeigemitte − LNB-Referenz × 390 + RX-Korrektur
Pluto TX = nominale Pluto-TX-Mitte + TX-Korrektur
```

Standard: 10489,750 MHz − 25 MHz × 390 = **739,750 MHz**. Eine positive Korrektur
erhöht den jeweiligen Hardware-LO. Die endgültigen LOs müssen innerhalb
70 MHz..6 GHz bleiben. Korrekturen sind in ganzen Hz bis ±1 MHz einstellbar.
Die digitale Abstimmung des USB-Trägers innerhalb des Empfangs-/Sendefensters
bleibt unabhängig von dieser Kalibrierung.

Verbindungs-, LO- oder Dämpfungsänderungen starten den lokalen Pluto-Kindprozess
kurz neu. PTT wird dabei ausgeschaltet und erfordert anschließend einen neuen
Tastendruck. Die Bestätigung folgt erst auf frische IQ-Daten, nachdem der Treiber
beide LOs und TX-Dämpfung zurückgelesen hat. Bei Fehlern wird die vorherige
Konfiguration wieder gestartet. Auch bei fehlender Pluto-Verbindung bleibt der
Settings-Endpunkt erreichbar. USB findet automatisch den ersten erkannten
Pluto über libiio; das Kommandozeilenprogramm `iio_info` wird dazu nicht benötigt.

Häufige Änderungen über die bisherigen DSP-/Abstimmkommandos werden für die
Dateizugriffe auf höchstens einen Schreibvorgang pro 500 ms gebündelt und beim
geordneten Beenden abschließend gespeichert. Explizite Settings-Transaktionen
werden sofort dauerhaft gespeichert. **PTT und Test-Loops werden nicht
wiederhergestellt**; der Start erfolgt immer mit PTT aus und Normalbetrieb.

UDP: **SETTINGS_CONTROL (17)** / **SETTINGS_STATE (18)** über denselben GUI-Port,
mit Session, eigener Request-Sequenz, Revision, Wiederholung und Bestätigung.
Die vollständige Tabelle aller Schlüssel, Wertebereiche und Zustände steht in
[`docs/udp-protocol.md`](docs/udp-protocol.md). Das erweiterbare Register liegt in
`settings_values()`; Validierung und Hardwareberechnung verwenden dieselbe
Konfigurationslogik für Datei und UDP.

Tests ohne Hardware: `make test`. Optionaler echter Pluto-Test für Korrektur,
Dämpfung, Wiederherstellung und Start ohne GUI (PTT bleibt aus):

```sh
python3 tests/test_settings_live.py
```

Vor dem Hardwaretest laufendes Backend und GUI beenden. Ergebnisse liegen unter
`build/captures/settings-live.json` und `settings-live.log`.

Der AD9361-Treiber rundet angeforderte LO-Frequenzen auf sein PLL-Raster. Die
Rückleseprüfung erlaubt deshalb höchstens 5 Hz Abweichung und protokolliert
Soll- und Istwert. Die Oberfläche zeigt die berechnete Sollfrequenz. Grundlage:
[Analog Devices: LO-Steuerung](https://developer.analog.com/docs/linux/drivers/iio-transceiver/ad9361.html).
