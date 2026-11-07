[English](README.md) · [Deutsch](README.de.md)

# QO100_TRX2

QO-100 Software-Transceiver mit einem ADALM-Pluto und einem Einplatinenrechner (SBC), z. B. Raspberry Pi oder Odroid.
Läuft auch auf Desktop-PCs mit einem Linux auf Debian-Basis.
Das Projekt ist ein voll funktionsfähiger Software-Transceiver für QO-100, der im Webbrowser bedient wird.

![QO100_TRX2 WebGUI](docs/WebGUI.png)

## Benötigte Hardware

- Pluto SDR, angeschlossen über USB oder einen USB-Ethernet-Adapter.
- SBC wie Raspberry Pi 4, Odroid C4/N2 oder ein Desktop-PC mit Debian, Ubuntu, Linux Mint usw. **Ein 64-Bit-Betriebssystem verwenden.**
- USB-Soundstick, falls der für Audio verwendete Rechner keinen Mikrofoneingang hat. Mikrofon und Lautsprecher/Kopfhörer werden am **Rechner mit dem Browser** angeschlossen.
- Satellitenschüssel, QO-100-Feed/LNB und ein 2,4-GHz-Verstärker, z. B. der 46-dB-Verstärker von AMSAT-DL.

## Installation

Auf dem SBC oder Linux-PC ein Terminal öffnen:

```sh
sudo apt update
sudo apt install -y git
git clone https://github.com/dj0abr/QO100_TRX2.git
cd QO100_TRX2
./build_all.sh --install-deps
```

Das Skript installiert die benötigten Pakete und baut alle Programme. Auf einem SBC werden fehlende Bibliotheken automatisch für dessen Prozessor gebaut.
Für spätere Builds genügt `./build_all.sh`.

## Software starten

Im Projektverzeichnis:

```sh
./start_all.sh
```

Die Bedienoberfläche im Browser öffnen:

- **Auf demselben Rechner:** <http://localhost:8080/>
- **Von einem anderen Rechner:** `http://<SBC-IP>:8080/` oder die HTTPS-Adresse des Reverse-Proxys.

**Mikrofonzugriff von einem anderen Rechner erfordert HTTPS; auf localhost funktioniert auch HTTP.**
Das Terminal offen lassen. **Strg+C** beendet alle Programme.

## Konfiguration

1. Das GUI verbindet sich automatisch. Falls getrennt, auf **Verbinden** klicken. Auf die Anzeige **Backend verbunden** achten.
2. **Setup** öffnen: USB oder Netzwerk/IP für den Pluto wählen, LNB-Referenz (normalerweise 25 MHz), RX-/TX-Korrekturen und TX-Dämpfung passend zur Station einstellen. Auf **Änderungen speichern** klicken und auf **Hardware bereit** warten.
3. Auf **Audio starten** klicken und den Mikrofonzugriff erlauben. Bei Bedarf unter **Setup → Audio** die Geräte für Wiedergabe und Aufnahme wählen.

![Verbindungsstatus und Audio-Bedienung](docs/verbindung.png)

<details>
<summary>Setup anzeigen</summary>

![Setup](docs/setup.png)

</details>

Der erste Browser erhält die Steuerung; weitere Browser können zuschauen und mithören. Nach fünf Minuten ohne Bedienung neu verbinden, um die Steuerung wieder zu erhalten, sofern sie frei ist.

## Bedienung

### Die beiden Wasserfälle

**Fullband** zeigt den gesamten QO-100-Schmalbandtransponder. **Zoom** zeigt zur Feinabstimmung ±25 kHz um den ausgewählten Empfänger. **RX1** oder **RX2** anklicken, um den Empfänger auszuwählen; der Zoom folgt der Auswahl.

### Abstimmen mit der Maus

Funktioniert im Spektrum und im Wasserfall:

| Aktion | Wirkung |
|---|---|
| Linksklick | RX1 abstimmen und TX um denselben Betrag verschieben; der Versatz bleibt erhalten. Bei ausgewähltem RX2 wird nur RX2 abgestimmt. |
| Rechtsklick | Nur den ausgewählten Empfänger abstimmen. |
| Mausrad | Den ausgewählten Empfänger abstimmen: 250-Hz-Schritte im Fullband, 25 Hz im Zoom. |
| Strg + Mausrad | Den ausgewählten Empfänger und TX gemeinsam verschieben. |
| Umschalt + Mausrad | Nur TX abstimmen. |

Ein Doppelklick auf eine RX-Frequenz öffnet die direkte Frequenzeingabe; für TX genügt ein Klick auf die TX-Frequenz.

![Empfangs- und Sendefrequenzen](docs/frequency.png)

### Menü Wasserfall

Farbe, Spektrumglättung (**Moving Average**) und Anzeigepegel wählen. Größere Mittelungswerte ergeben ein ruhigeres Spektrum, kleinere reagieren schneller. **Untergrenze/Obergrenze** so einstellen, dass der Wasserfall gut erkennbar ist.

![Wasserfall-Einstellungen](docs/wasserfallsetup.png)

### Menü Filter

Eine Bandbreitenvorgabe für RX1 wählen oder **untere und obere Filtergrenze** getrennt einstellen. Der Durchlassbereich ist im RX1-Zoom markiert. **Rauschreduktion** verringert das Audiorauschen; mit einem niedrigen Wert beginnen und nach Gehör erhöhen.

![RX1-Filtereinstellungen](docs/filter.png)

### QSO-Bedienelemente

- **RX1 → TX:** TX auf die zu RX1 passende Uplinkfrequenz setzen.
- **TX → RX1:** RX1 auf die zu TX passende Downlinkfrequenz setzen.
- **Lautstärke:** Wiedergabelautstärke von Lautsprecher/Kopfhörer.
- **Kompressor:** hebt die mittlere Sprachlautstärke an. **100 %** ist der normale Ausgangswert; **0 %** schaltet den Kompressor aus.
- **PTT:** Zum Senden die Taste gedrückt halten. Unter **Setup** lassen sich alternativ Ein-/Ausschalten per Klick und optional die Leertaste aktivieren.

![QSO-Bedienelemente](docs/operation1.png)

### Bandplan und Frequenzanzeige

Der Bandplan zeigt die Bereiche für die verschiedenen Betriebsarten. Mit **↻** rechts an der Fullband-Skala zwischen Bandplan, Downlink-, Uplink- und Pluto-Frequenzen wechseln.

![Umschalter für Bandplan und Frequenzanzeige](docs/bandplan.png)

## Zwei Empfänger

- **RX1:** normaler QO-100-Sprechfunk mit einstellbarem Audiofilter und Rauschreduktion.
- **RX2:** unabhängiger FT8-Empfang, während das QSO über RX1 weiterläuft. RX2 auswählen, auf FT8-Aktivität abstimmen und **Decoder-Text anzeigen** anklicken. Die decodierten Nachrichten erscheinen unter dem Zoom.

![Decodierte FT8-Nachrichten](docs/decoderoutput.png)

Die ersten Nachrichten erscheinen nach etwa 15–30 Sekunden zuzüglich Rechenzeit. Die Uhr des SBC/PC muss synchronisiert sein.

Technische Einzelheiten stehen in [DEVELOPMENT.md](DEVELOPMENT.md).
