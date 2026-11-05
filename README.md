# QO100_TRX2

C++-Backend für einen QO-100-Transceiver mit getrenntem GUI über UDP.
Der aktuelle Stand empfängt **600 kHz**, berechnet das Gesamtspektrum sowie
einen abstimmbaren **Zoom von ±25 kHz** und demoduliert **zwei unabhängig abstimmbare USB-Audiokanäle**.
Das Python-Test-GUI zeigt beide Wasserfälle und spielt RX1 über die
Default-Soundkartenausgabe ab. RX2 wird für Tests getrennt empfangen und überwacht. TX-Modulation folgt später.

## Bauen

Benötigt werden Linux, CMake, ein C++17-Compiler, libiio (0.x-API),
libad9361, liquid-dsp, libsamplerate und FFTW3 mit Float-Unterstützung.
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
| TX-Referenz für spätere Erweiterung | 2.400,250 MHz |
| Zugehörige Pluto-TX-Referenz | 1.290,250 MHz |

```bash
./start_backend.sh
```

Das Backend bindet zuerst den lokalen I/Q-Port und startet anschließend **seinen
eigenen** Pluto-Treiber mit einer erzeugten Config-Datei. Der Pluto wird über
`ip:192.168.10.25` angesprochen. Mit `Ctrl+C` werden Backend und Kindprozess beendet.
Bei einem unerwarteten Treiberende oder ausbleibenden I/Q-Daten beendet sich das
Backend mit Fehlermeldung. Bei hartem Tod des Backends erhält der Treiber unter
Linux zusätzlich ein Elternprozess-Todessignal.

Dieser Stand verwendet ausdrücklich `RX_ONLY 1`: kein TX-Streaming, kein
Crossband-Repeater und kein Markerton. Es werden keine TX-Puffer angelegt. Die
TX-Referenzen werden nur für die spätere Erweiterung gespeichert. Der Treiber
wird in `build/run/pluto-<PID>/` mit eigenem `pluto_config.txt` und `pluto.log`
gestartet. Die Logs bleiben für die Diagnose erhalten.

In einem zweiten Terminal:

```bash
./start_gui.sh
```

Die Startfrequenz beträgt **10489,748500 MHz** (USB-Träger, nicht die
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

## Die zwei Schnittstellen

1. **Pluto-Anbindung ausschließlich lokal**: `127.0.0.1`,
   Port 40809 für I/Q vom Treiber zum Backend. Port 40808 für künftige TX-Samples
   und 40810 für den vorhandenen FIFO-Status sind reserviert.
   In RX_ONLY werden TX-Empfang und FIFO-Statustimer nicht gestartet.
   Der QO100-Treiberbuild bindet auch im Normalmodus nur Loopback und erlaubt
   ausgehende Treiber-UDP-Daten ausschließlich an `127.0.0.1`.
2. **GUI-Anbindung**: ein bidirektionaler UDP-Port, standardmäßig
   `0.0.0.0:40900`. Anmeldung, Abstimmung mit Bestätigung, Gesamt-/Zoom-FFT und Audio verwenden
   unterschiedliche Nachrichtentypen. Es gibt keinen Webserver und keine zusätzliche GUI-Verbindung.

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
Ein 321-Tap-Kaiser-FIR mit Dezimation 8 erzeugt **75 kS/s komplex** für den Zoom.
Die Zoom-FFT hat 8192 Punkte (9,155 Hz/Bin); angezeigt werden die mittleren
50 kHz. Wegen des 50-%-Hoprasters werden maximal etwa 18,3 Zoomzeilen/s erzeugt.
Das Gesamtspektrum läuft parallel weiter.

Ein zweikanaliger Sinc-Resampler reduziert den Kanal auf 12 kS/s komplex.
Ein komplexer 513-Tap-FIR wählt das obere Seitenband **+100 bis +4000 Hz**;
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
./start_gui.sh --rx-frequency 10489.748500 --rx2-frequency 10489.600000
```

Der Hardwaretest speichert zusätzlich `build/captures/rx2-usb.wav`, damit sich
der zweite Audiostrom prüfen lässt. Es sind weiterhin keine Digitalmodems oder
externen Audio-Routing-Verbindungen Bestandteil des Backends.

## Ohne Hardware testen

```bash
./start_backend.sh --source synthetic
./start_gui.sh
```

Das Gesamtspektrum enthält zwei Referenztöne bei -150 kHz (-12,04 dBFS)
und +75 kHz (-20 dBFS). Zusätzlich erzeugt die Quelle relativ zur voreingestellten
USB-Frequenz einen gewünschten +1-kHz-Ton und einen stärkeren -2-kHz-Ton.
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
