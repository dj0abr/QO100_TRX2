# RX2/TX2 für externe Digitalprogramme

`./build_all.sh` baut auch `qo100_digital_audio`. Zusätzliche Build-Abhängigkeit:
`libpulse-dev`; zur Laufzeit `pulseaudio-utils` und ein erreichbarer PulseAudio-
Server (oder ein PipeWire-Server mit PulseAudio-Kompatibilität).
`./build_all.sh --install-deps` installiert die benötigten Pakete.

## Start und Geräteauswahl

`./start_all.sh` startet Backend, Webbridge, FT8, PSK und den Audioadapter.
Der Adapter erzeugt zwei virtuelle Audiowege im Audioserver desselben Linux-Benutzers:

| Auswahl im Digitalprogramm | Anzeigename | PulseAudio-Gerät |
|---|---|---|
| Audioeingang / Input | fromQO100 | `fromQO100` |
| Audioausgang / Output | toQO100 | `toQO100` |

Die Namen gelten aus Sicht des Digitalprogramms: Empfangsaudio kommt **fromQO100**,
Sendeaudio geht **toQO100**. `fromQO100_internal` und die Monitoranschlüsse sind
interne Hilfsgeräte. Nach dem Update den Stack neu starten und die alten
QO100_RX2-/QO100_TX2-Zuordnungen im Digitalprogramm durch die neuen Namen ersetzen.

In FLdigi bzw. WSJT-X diese Geräte wählen, vorzugsweise **48 kHz, Mono, PCM16**.
Der Adapter verbindet sie mit dem 12-kHz-Funkprotokoll. Die virtuellen Geräte haben
keine Verbindung zu Mikrofon oder Lautsprecher. Die Systemstandardgeräte werden
nicht ausdrücklich umgestellt. Lautstärke und Audio stoppen im Browser verändern
die externen Audioströme nicht.

Es gibt keine CAT-Frequenzsteuerung oder PTT-Anbindung. In WSJT-X passen dazu
**Rig: None**, **PTT Method: VOX**, **Split Operation: None**. Frequenzen im WebGUI
einstellen. Die interne Sende-/Empfangsfolge des Digitalprogramms bleibt erhalten;
TX2 verarbeitet dessen Audio laufend. Stille erzeugt keinen HF-Träger.

Der Adapter muss unter einem Benutzer mit Zugang zum Audioserver laufen. Ein unter
einem anderen Benutzer bzw. als root gestarteter Stack sieht normalerweise nicht die
Audiogeräte der Desktop-Sitzung. Auf einem Rechner ohne Audioserver kann der Stack
mit `./start_all.sh --no-digital-audio` betrieben und der Adapter separat gestartet werden.
Die gegenwärtige UDP-Anbindung ist ausschließlich lokal; externe Programme laufen
auf dem Rechner des Audioadapters und der Webbridge.

Einzelstart:

```sh
./build/bin/qo100_digital_audio --port 40914 --bridge-port 40915
```

`--device-prefix` erlaubt getrennte Gerätenamen für Tests. Beim regulären Beenden
werden selbst erzeugte Geräte entfernt. Bereits vorhandene virtuelle Geräte werden
nach einem harten Adapterabbruch wiederverwendet. Gleichzeitige Adapter mit demselben
Präfix werden durch eine lokale Sperre verhindert.

## Bedienung im WebGUI

RX1 oder RX2 anklicken wählt das zugehörige RX/TX-Paar und den hörbaren Audiokanal.
Die TX-Anzeige zeigt entsprechend **TX1** oder **TX2**. Für beide Paare gilt:

- Linksklick im Wasserfall/Spektrum: RX und zugehörigen TX um denselben Betrag verschieben.
- Rechtsklick bzw. normales Mausrad: nur RX verändern.
- Shift + Mausrad: nur den zugehörigen TX verändern.
- Strg + Mausrad: RX und zugehörigen TX gemeinsam verändern.
- Frequenzeingaben, Touch-Bedienung und Kopiertasten wirken auf das ausgewählte Paar.

Jedes Paar behält seinen eigenen Versatz. `RX → TX` setzt den passenden Uplink:
`TX = RX − 8089500000 Hz`. Nach einer bestehenden Installation lässt sich damit TX2
einmal an die gespeicherte RX2-Frequenz anpassen. Die TX2-Frequenz wird dauerhaft
als `initial_tx2_hz` gespeichert. Startwert: **2400100000 Hz**.

Bei ausgewähltem RX2 spielt der Browser dessen Audio zur Feinabstimmung ab. Auch
Zuschauer können lokal zwischen RX1 und RX2 umschalten. Der externe RX2-Audioweg
läuft unabhängig von dieser Auswahl weiter. Der feste RX2-Durchlass beträgt
100–4000 Hz ohne Rauschreduktion; der Sprachkompressor gehört zu TX1.

Die PTT-Taste steuert ausschließlich das Browsermikrofon für TX1. Bei RX2 zeigt sie
**TX2 / Externes Audio** an. Die Auswahl von RX2 oder ein Fokusverlust kann keinen
Browsermikrofonstrom auf TX2 schalten. TX2 benötigt weder einen geöffneten Browser
noch einen angemeldeten Bediener.

## Signalweg und Pegel

```text
RX2 ── Webbridge ──┬── WebSocket ── Browser-Audiokontrolle
                  └── UDP ── Audioadapter ── fromQO100 ── Digitalprogramm

Digitalprogramm ── toQO100 ── Audioadapter ── UDP ── Webbridge ── TX2

TX1-Modulator ──┐
               ├── IQ-Summe mit 6 dB Reserve ── ein Pluto-Treiber
TX2-Modulator ──┘
```

TX2 besitzt eigene Audiopuffer, Epoche, Samplezähler, Taktkorrektur und Mischfrequenz.
Sein Digitalprofil verwendet einen festen USB-Durchlass 100–4000 Hz und linearen
Pegel ohne Sprachkompression oder den sprachbezogenen Spitzenbegrenzer. Den Pegel
im Digitalprogramm einstellen. Die endgültige IQ-Summe erhält einen festen Faktor
**0,5 (−6 dB)**. Damit sinkt auch TX1 gegenüber dem Stand mit nur einem Modulator
um 6 dB; die Hardware-Dämpfung bleibt unverändert. Nur bei Übersteuerung der Summe
wird ein kompletter IQ-Block zusätzlich gleichmäßig abgesenkt, statt einzelne
I/Q-Samples abzuschneiden. Im normalen Digitalbetrieb greift dieser Schutz nicht ein.

Beide Modulatoren werden vom selben Pluto-FIFO-Takt getrieben. Kein zweiter Prozess
schreibt direkt in den Hardware-IQ-Port. Fehlende TX2-Pakete werden als Stille behandelt;
nach längerer Unterbrechung wird der Puffer geleert. Ein neuer Adapter erhält vor
seinem Sample-Index null eine neue TX2-Epoche. Veraltete Session-/Epochen-/Frequenzpakete
werden verworfen. Frequenzänderungen von TX2 setzen ausschließlich dessen Audioweg zurück.

## Lokales Protokoll und Startverhalten

`config/webbridge.conf`:

```ini
digital_audio_port = 40914
digital_bridge_port = 40915
```

Beide Ports auf 0 deaktiviert die lokale Schnittstelle. Die Webbridge bindet Port
40915 und kommuniziert ausschließlich mit dem Adapter auf 127.0.0.1:40914.
Sie bleibt der einzige UDP-Abonnent des Backends.

RX2 verwendet QTR2 `AUDIO`, Stream 2. TX2 verwendet `TX_CONTROL`, `TX_STATE` und
`TX_AUDIO` mit Transport-ID **7**, unverändert 12 kHz, Mono, 240 PCM16-Samples je Paket.
Der Adapter sendet beim Start eine 64-Byte-Nachricht vom Typ 19, Stream 7, mit einer
zufälligen 64-Bit-Kennung bei Offset 16. Nach bestätigtem Epochenwechsel setzt die
Webbridge diese Kennung in ihre lokalen TX2-Statuspakete bei Offset 16 ein.
So kann ein neuer Adapter keine alten Statuspakete als Freigabe für Index null verwenden.
Diese Kennung betrifft nur den lokalen Adapterlink.

Im WebSocket-Zustand bleibt `tx` der Zustand von TX1; **`tx2`** enthält den Zustand
von TX2. Das dort historisch `ptt` genannte Feld bedeutet bei TX2 **Audioweg bereit**.
Abstimmung: `{"type":"tx","channel":2,"frequency_hz":2400100000,"ptt":false}`.
Browser-TX-Audio bleibt ausschließlich TX1 vorbehalten. Hardwareänderungen und Test-Loops
unterbrechen TX2; nach bestätigter Rückkehr zum Normalbetrieb wird nur frisches
externes Audio übernommen. Eine Browser-Abmeldung unterbricht TX2 im Normalbetrieb nicht.

Der Startverbund startet einen während des Betriebs beendeten Audioadapter separat
neu. Ein nicht erreichbarer Audioserver beendet weder RX1/TX1 noch FT8/PSK.
Adapterdiagnosen stehen in `build/run/stack-*/digital-audio.log`.

## Prüfungen

`tx2_digital_profile` misst Linearität und Durchlass bis 3,5 kHz. `tx2_iq_udp` prüft
beide Träger durch Webbridge, Backend und einen Pluto-Treiberersatz einschließlich
PTT-Unabhängigkeit, Browser-Abmeldung, Abstimmung, Epochen und Audioausfällen.
Dieser Treibertest benötigt freie lokale Pluto-Ports oder einen getrennten
Netzwerk-Namensraum; niemals parallel zur laufenden Station im selben Netz ausführen.
`digital_virtual_audio` prüft die wirklichen virtuellen Geräte und die 48-/12-kHz-
Umsetzung auf einem privaten PulseAudio-Server ohne physische Audiogeräte.
