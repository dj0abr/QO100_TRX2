# QO100_TRX2

C++-Backend für einen QO-100-Transceiver mit getrenntem GUI über UDP.
Der aktuelle Stand empfängt **600 kHz**, berechnet das Gesamtspektrum sowie
einen abstimmbaren **Zoom von ±25 kHz** und demoduliert **zwei unabhängig abstimmbare USB-Audiokanäle**.
Das Python-Test-GUI zeigt beide Wasserfälle und spielt RX1 über die
Default-Soundkartenausgabe ab. RX2 wird für Tests getrennt empfangen und überwacht.
Das GUI sendet Mikrofon-Audio als USB mit unabhängig einstellbarer TX-Frequenz.

## Bauen

Benötigt werden Linux, CMake, ein C++17-Compiler, libiio (0.x-API),
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

Der normale Build erzeugt **beide Programme aus Quellcode**:
`build/bin/qo100_backend` und `build/bin/pluto`.
Auch ein gezielter Build von `qo100_backend` baut den Treiber mit.
Die Originalkopie unter `external/Pluto/` ist im Projekt enthalten; beim Bauen
erfolgt kein Download. Das mitkopierte ursprüngliche Binary wird nicht benutzt.

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
Bei einem unerwarteten Treiberende oder ausbleibenden I/Q-Daten beendet sich das
Backend mit Fehlermeldung. Bei hartem Tod des Backends erhält der Treiber unter
Linux zusätzlich ein Elternprozess-Todessignal.

Der Treiber läuft im Vollduplexbetrieb mit `RX_ONLY 0` und `TX_RAW_IQ 1`.
Das Backend erzeugt fertige IQ-Samples; der zusätzliche direkte Treibermodus
fügt weder Markerton noch Interpolation oder Pegeländerungen hinzu. Ohne
TX-Audio werden Nullsamples durchgehend weitergegeben. Der Treiber startet
in `build/run/pluto-<PID>/` mit eigener Konfiguration und Diagnose-Log.

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

Mit dem **Mausrad über einem der beiden Wasserfälle** wird nur TX verstellt:
nach oben **+10 Hz**, nach unten **−10 Hz** je Rastung. RX1 und die Mitte seines
Zooms bleiben dabei stehen. Der aktuelle Versatz steht neben der PTT-Taste.
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
enthalten Sample-Zähler und Abstimmungsgeneration. Das GUI puffert kurzzeitig,
sortiert anhand der Sample-Indizes und ersetzt fehlende Pakete durch Stille.
Der Puffer bleibt auf höchstens 0,5 s begrenzt. Ein langsam nachgeregelter
Sinc-Resampler gleicht die Taktabweichung zwischen Pluto und Soundkarte aus.
Die Soundkarte wird über ALSA **default** geöffnet; ALSA kann bei Bedarf auf
die Hardware-Samplerate umsetzen. Der Audiothread arbeitet unabhängig von Tk.

Bei einer tatsächlichen Frequenzänderung werden Kanalfilter, Zoom und
Audiogeneration zurückgesetzt. Das GUI verwirft alte Audiopakete nach der
Bestätigung. Wiederholte Abstimmkommandos setzen den Kanal nicht erneut zurück;
verspätete ältere Befehle werden ignoriert. Ein vollständiger ±25-kHz-Zoom muss
innerhalb des 600-kHz-Empfangsfensters bleiben.

## RX1: Bandbreite und Lage des Durchlassbereichs

RX1 besitzt im Test-GUI zwei unabhängig einstellbare Regler:

- **BW:** 500 bis 3900 Hz, in 1-Hz-Schritten einstellbar.
- **Mitte / Lage:** Mittenfrequenz des Audio-Durchlassbereichs in Hz relativ
  zum eingestellten USB-Träger. Die Grenzen sind `Mitte ± BW/2`.

Beispiel: **BW 500 Hz, Mitte 700 Hz** ergibt einen Durchlassbereich von
**450–950 Hz**. Bei gleicher Breite lässt sich dieser Bereich entlang des
Audiospektrums verschieben. Die Abstimmfrequenz und die Mitte des Zooms ändern
sich dabei nicht. Die Filtergrenzen bleiben innerhalb **100–4000 Hz**; das GUI
begrenzt den Lageregler passend zur gewählten Breite.

Die Tasten **500 Hz, 1.2 kHz, 1.8 kHz, 2.7 kHz und Full BW** setzen die Breite.
Die bisherige Mitte bleibt erhalten, soweit sie zur neuen Breite passt.
**Full BW** bedeutet weiterhin **100–4000 Hz**, also 3900 Hz Breite bei 2050 Hz Mitte.
Die Grenzen bezeichnen den nominellen Durchlassbereich mit etwa −6 dB an den
Kanten; der FIR hat endliche Übergangsbereiche.

Beim Start liest das GUI die aktuelle Filtereinstellung aus dem Backend.
Die Startwerte stehen in `config/backend.conf`:

```ini
rx1_filter_bandwidth_hz = 3900
rx1_filter_center_hz = 2050
```

Ungültige Kombinationen verhindern den Backend-Start bzw. werden bei UDP-Steuerung
mit Fehlerstatus abgelehnt. Änderungen werden zwischen gleich langen, mit der
Sample-Historie vorgefüllten Filtern über 20 ms überblendet. Bei schneller
Bedienung wird die jeweils neueste Einstellung nachgeführt. Audiozähler,
Audiogeneration und Zoom laufen ohne Rücksetzung weiter. **RX2 und TX behalten
ihren bisherigen Durchlassbereich 100–4000 Hz.** Die Audioübertragung bleibt
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
verworfen. Ein zusätzlicher RX2-Wasserfall ist in diesem Schritt nicht nötig.

Beide Audioströme sind **12 kHz, Mono, PCM16**, jeweils 240 Samples pro Paket.
Die Stream-ID 1 kennzeichnet RX1, die Stream-ID 2 RX2. Alle Daten und
Steuerbefehle verwenden weiterhin denselben GUI-UDP-Port. Reine Audionutzdaten:
24 kB/s je Kanal, zusammen 48 kB/s. Beim Retune werden nur Filter und Audioepoch
des angesprochenen Kanals zurückgesetzt. Eine lokale Änderung der
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
Taktkorrektur. Ein analytischer 513-Tap-FIR erzeugt USB mit +100..4000 Hz Durchlassbereich.
Danach erfolgen exakte liquid-Polyphasen-Interpolation um Faktor 100 auf
1,2 MS/s, Begrenzung der tatsächlichen SSB-Hüllkurvenspitzen und digitales Mischen auf die
TX-Frequenz und I16/Q16-Konvertierung mit 6 dB digitaler Pegelreserve.
Die tatsächliche AD9361-Ausgangsdämpfung steht in `tx_attenuation_db`
(0..89,75 dB; Projektkonfiguration 0 dB für die getestete Station). Änderungen
daran werden beim Backend-Start übernommen. Der Sende-/Empfangstest über QO-100
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
