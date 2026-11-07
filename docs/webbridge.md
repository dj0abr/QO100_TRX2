# HTTP-/WebSocket-Adapter

Eigenständiges C++17-Programm **`qo100_webbridge`**. Backend und DSP bleiben unverändert.
HTTP und WebSocket liegen auf **Port 8080**; Backend-UDP bleibt lokal auf `127.0.0.1:40900`.

## Start

Zusätzlich unter Debian: `sudo apt install libboost-dev nlohmann-json3-dev gfortran libfftw3-dev`.
Normaler Projektbuild erzeugt `build/bin/qo100_webbridge`. Backend zuerst starten, dann:

```sh
./build/bin/qo100_webbridge --config config/webbridge.conf
```

Adapter und FT8-Empfänger ohne Pluto-/Backend-Treiber bauen:

```sh
cmake -S webbridge -B build-webbridge -DCMAKE_BUILD_TYPE=Release
cmake --build build-webbridge -j2
./build-webbridge/qo100_webbridge --config config/webbridge.conf
```

`config/webbridge.conf`: Listen-Adresse, HTTP-Port, Backend-Port, Dokumentverzeichnis,
lokale Decoder-Ports (RX3 **40910/40911**, RX4 **40912/40913**), Inaktivitätslimit (Standard **300 s**), Verbindungslimit (**16**) und Sendepuffer je Browser
(**512 KiB**). Relative `web_root`-Pfade beziehen sich auf die Config-Datei.
CLI überschreibt die Datei; Optionen zeigt `--help`.

**Einziger UDP-Teilnehmer:** Während des Adapterbetriebs kein natives Test-GUI direkt
am selben Backend betreiben, da eine neue UDP-Anmeldung die bisherige ersetzt.

## Monitoring RX3 und RX4

RX3 und RX4 besitzen eigene USB-Demodulatoren ohne zusätzliche Zoom-FFTs.
RX2 bleibt manuell abstimmbar und hat derzeit keinen Decoderanschluss.
Der Adapter bleibt der einzige UDP-Abnehmer des Backends:

```text
Backend AUDIO/5 → Adapter → UDP :40910 → RX3 FT8-Decoder
Backend AUDIO/6 → Adapter → UDP :40912 → RX4 PSK-Decoder
Browser ← WebSocket ← Adapter :40911 / :40913 ← UDP UTF-8-Text
```

`./start_all.sh` startet beide Decoder. Einzelstart zusätzlich zum Backend/Adapter:

```sh
./build/bin/qo100_ft8_decoder --port 40910
./build/bin/qo100_beacon_decoder --port 40912
```

Unter **Setup → Monitoring-Empfänger** lassen sich `rx3_frequency_hz` und
`rx4_frequency_hz` als ganze Hz einstellen und dauerhaft speichern.
Startwerte: FT8 **10489540000 Hz**, PSK **10489748821 Hz** (USB-Träger).
Nur der geänderte Monitoringkanal setzt Filter, Epoche und Samplezähler zurück;
RX1, RX2 und der andere Monitor laufen weiter. Normales `tune` erlaubt nur RX1/RX2.

**Decoder-Text anzeigen** über dem Zoom blendet den Monitoringbereich ein.
Radiobuttons **RX3 · FT8** und **RX4 · PSK** wählen die sichtbare Quelle.
Beide Textströme werden immer empfangen, mit getrennten Puffern und Lesepositionen.
Quelle und Sichtbarkeit werden in diesem Browser gespeichert. **Text löschen**
löscht ausschließlich die ausgewählte Quelle in diesem Browser.

Konfiguration in `config/webbridge.conf`:

| Kanal | Audio zum Decoder | Text am Adapter |
|---|---|---|
| RX3 | `rx3_decoder_audio_port = 40910` | `rx3_decoder_text_port = 40911` |
| RX4 | `rx4_decoder_audio_port = 40912` | `rx4_decoder_text_port = 40913` |

Jeweils beide Ports auf **0** deaktiviert einen Anschluss; ohne Config sind beide
Anschlüsse deaktiviert. Das Komplett-Startskript benötigt beide aktivierten Paare.
CLI-Optionen entsprechend `--rx3-decoder-audio-port`, `--rx3-decoder-text-port`,
`--rx4-decoder-audio-port` und `--rx4-decoder-text-port`.
Die bisherigen `decoder_audio_port`/`decoder_text_port` werden dadurch ersetzt.
Alle aktiven Decoder-Ports müssen untereinander und vom Backend-Port verschieden sein.

Decoder binden an Loopback und antworten von ihrem Audioport an die Absenderadresse
des Audiopakets. PCM: 12 kHz, Mono, PCM16 Big Endian, 240 Samples pro Paket,
64-Byte-Header und 480 Byte Audio. Session, Epoche, Samplezähler und Frequenz bleiben
unverändert. **Transport-IDs 5/6** bezeichnen RX3/RX4; 3/4 bleiben TX1/Audio-Echo.
Kein Decoder meldet sich am Backend als GUI an.

Textdatagramme dürfen höchstens 4096 Byte gültiges UTF-8 enthalten. Der Adapter
prüft den Absender und verteilt beide Quellen an Bediener und Zuschauer:

```json
{"type":"decoder_text","stream":3,"text":"FT8-Meldung\n"}
{"type":"decoder_text","stream":4,"text":"PSK-Bakentext\n"}
```

`stream` im JSON ist die **logische Empfängernummer**. Text kann keine
Backend-Kommandos auslösen. Pro Quelle bleiben im Browser maximal 16384 Zeichen;
bei FT8 (RX3) werden zusätzlich nur die letzten 50 Zeilen aufbewahrt.
Ein fehlender Decoder unterbricht weder QSO noch den anderen Decoder. Das Startskript
startet einen während des Betriebs beendeten Decoder nach spätestens etwa zwei Sekunden
neu. Ein Ende von Backend oder Webadapter beendet weiterhin den Verbund.
[FT8: Uhrzeit und Tests](ft8-decoder.md), [PSK-Bake](beacon-decoder.md).

## OPNsense und Webseite

```text
Browser -- HTTPS/WSS --> OPNsense -- HTTP/WS --> Adapter -- lokales UDP --> Backend
```

OPNsense leitet die Webseite und **`/ws`** an denselben Adapter weiter. WebSocket-Upgrades
zulassen und den ursprünglichen **Host-Header beibehalten**. TLS/Zertifikate liegen bei
OPNsense; der Adapter verwendet ausschließlich HTTP/WS. Origin muss bei Browser-Anfragen
zum Host passen. Zugangsbeschränkungen können im vorgeschalteten Proxy erfolgen.

`/` liefert `web/index.html`, weitere Dateien stammen aus `web_root`; `/health` prüft den
HTTP-Dienst. Für Mikrofon und AudioWorklet die HTTPS-Adresse der OPNsense verwenden;
`http://localhost`/`127.0.0.1` eignet sich ebenfalls für lokale Browsertests.

## WebGUI

`/` öffnet das WebGUI. Der Statusbereich zeigt RX1, TX als Uplink und zugehörige
Downlinkfrequenz, RX2 sowie Backend-, Hardware- und Rollenstatus. Fullband und
der Zoom des ausgewählten Empfängers enthalten jeweils Spektrum, Frequenzachse und Wasserfall. Cyan markiert
RX1, Gelb RX2 und die gestrichelte rote Linie die TX-Downlinkposition. Der RX1-
Durchlassbereich wird eingezeichnet. Die Anzeigen stammen ausschließlich vom Backend.

### Sprache

Unter **Setup → Sprache** stehen **Automatisch**, **Deutsch** und **English**
zur Auswahl. Automatisch verwendet die bevorzugte Browsersprache: Deutsch bei
`de`/`de-*`, sonst Englisch. Eine manuelle Auswahl wird lokal im Browser gespeichert.
Beschriftungen, Menüs und laufende Buttontexte wechseln sofort, ohne Audio oder
WebSocket-Verbindung neu zu starten. Technische Fehler- und Diagnosemeldungen
bleiben in ihrer bisherigen Sprache.

### Abstimmung

RX1 oder RX2 durch Klick auf seine Frequenzanzeige auswählen; die aktive Anzeige
wird hervorgehoben. Frequenzanzeige doppelklicken (TX einfach anklicken), um eine
Frequenz direkt in MHz mit bis zu sechs Nachkommastellen einzugeben.

Ein Klick auf das **↻-Symbol rechts an der Fullband-Frequenzachse** schaltet zyklisch
zwischen **Bandplan → Downlink → Uplink → Pluto RX → Pluto TX → Bandplan** um.
Die aktive Darstellung steht neben FULLBAND. Nach jedem Neuladen wird wieder
**Bandplan** angezeigt; diese Auswahl wird nicht gespeichert. Die vier
Frequenzdarstellungen verwenden MHz; die Umrechnung erfolgt intern in Hz:

- Uplink = Downlink − 8 089 500 000 Hz.
- Pluto RX = Downlink − LNB-Referenz × 390.
- Pluto TX = Uplink − (2 400 250 000 Hz − Pluto TX-Basisfrequenz).

Der farbige Bandplan richtet sich nach den tatsächlichen FFT-Frequenzen.
Quelle: [AMSAT-DL NB-Bandplan und Betriebsrichtlinien](https://amsat-dl.org/p4-a-nb-transponder-bandplan-und-betriebsrichtlinien/),
abgerufen am 06.11.2026. Die Baken samt Schutzbereichen tragen **B**, die übrigen
Felder abhängig vom Platz z. B. **CW only**, **NB dig**, **Digital**, **SSB only**,
**RS** (Rundspruch), **NF** (Notfunk) und **Mixed + Contest**. Tooltips zeigen die
vollständigen Angaben und Downlinkgrenzen. Der Rundspruch-/Notfunk-Übergang liegt
bei 10489,857500 MHz (7,5 kHz je Bereich, entsprechend den Uplinkgrenzen).
Die unmittelbar benachbarten Multimedia- und oberen CW-Bakenbereiche
10489,990000–10490,000000 MHz werden als gemeinsamer B-Block dargestellt.
Außerhalb des Bandplans bleibt die Leiste leer. Beschriftungen werden passend
gekürzt oder bei Platzmangel ausgelassen; die farbigen Frequenzgrenzen bleiben
maßstäblich. Die Zoom-Achse bleibt eine Frequenzanzeige.

LNB-Referenz und TX-Basisfrequenz stammen aus den aktuellen Setup-Einstellungen;
Frequenzkorrekturen werden in diesen nominalen Achsenanzeigen nicht addiert.
Die Umschaltung ändert nur die Fullband-Beschriftung und ist auch für Zuschauer
verfügbar. Abstimmung, Zoom-Achse, Audio und Wasserfall-Historie bleiben erhalten.

| Eingabe in Spektrum oder Wasserfall | RX1 ausgewählt | RX2 ausgewählt |
|---|---|---|
| Linksklick | RX1 und TX1 gemeinsam; Versatz bleibt erhalten | RX2 und TX2 gemeinsam; eigener Versatz bleibt erhalten |
| Rechtsklick | Nur RX1 | Nur RX2 |
| Mausrad | Ausgewählten RX abstimmen | Ausgewählten RX abstimmen |
| Shift + Mausrad | Nur TX1 | Nur TX2 |
| Strg + Mausrad | RX1 und TX1 gemeinsam | RX2 und TX2 gemeinsam |

**Mausrad-Schrittweite:** Fullband **250 Hz**, Zoom **25 Hz**. Die untere
Anzeige folgt der Auswahl RX1/RX2 mit eigenem Spektrum, Wasserfall und
Frequenzachse (±25 kHz). Klicks und Mausrad bedienen den ausgewählten RX gemäß
obiger Tabelle. Beim Empfängerwechsel beginnt eine neue Zoom-Historie;
Rauschpegelschätzung und Mittelung werden zurückgesetzt, damit sich RX1/RX2
nicht vermischen. Der Monitoringtext stammt unabhängig davon aus RX3/FT8 und RX4/PSK.
Beim Abstimmen laufen Audio und Wasserfälle weiter. Die Zoom-Historie wird
passend zur neuen Frequenzachse seitlich verschoben; auch Schritte kleiner als
ein Bildpunkt summieren sich korrekt.
Strg + Mausrad erhält den Versatz des ausgewählten RX/TX-Paars und hat bei
Shift Vorrang. Die Kopiertasten und die TX-Anzeige gehören zum ausgewählten Paar.
Zuordnung: Downlink = Uplink + 8 089 500 000 Hz. Hardwarekorrekturen bleiben im Backend.
RX2-Audio geht gleichzeitig an den Browser und den lokalen Audioadapter für externe
Programme. Die Lautstärke im Browser verändert den externen Audioweg nicht.
[RX2/TX2: Geräte, Digitalprofil und gemeinsamer IQ-Ausgang](digital-audio.md).

Mobil bestimmt ein Umschalter für beide Paare, ob Tippen RX und TX gemeinsam oder
nur RX verstellt. Horizontal auf einer Frequenzanzeige wischen stimmt diesen Kanal in
**25-Hz-Schritten** fein ab; rechts erhöht, links verringert die Frequenz.
Vertikales Wischen scrollt die Seite. Die QSO-Leiste bleibt am unteren Rand erreichbar.

### Audio, PTT und Einstellungen

**Audio / Mikrofon** aktiviert bei RX1 die Wiedergabe und fragt beim Bediener die
Mikrofonfreigabe ab. Das Mikrofon bleibt zusammen mit Audio aktiv; einen separaten
Mikrofonschalter im Setup gibt es nicht. **Audio stoppen** beendet auch die Aufnahme.
Nach einem Mikrofonfehler kann bei laufendem Audio ein Aufnahmegerät im Setup
gewählt werden, um die Aufnahme erneut zu starten.
Bei RX2 startet die Audiotaste ausschließlich die Wiedergabe von RX2. Zuschauer aktivieren ausschließlich die Wiedergabe des ausgewählten Empfängers. **PTT**
erfordert ein eingeschaltetes Mikrofon. Unter **Setup → PTT-Taste** stehen zwei
Bedienarten zur Wahl: **Drücken/Halten** (Standard) oder **Anklicken zum Umschalten**.
Im Schaltbetrieb beendet ein zweiter Klick die Aussendung; nach spätestens
**5 Minuten** wird automatisch abgeschaltet, unabhängig von weiterer Bedienaktivität.
Die verbleibende Zeit steht auf der PTT-Taste. Optional aktiviert **Leertaste = PTT**
dieselbe Bedienart über die Tastatur. Wiederholte Tastendrücke bei gehaltener
Leertaste schalten nicht mehrfach; Eingabefelder und offene Dialoge sind ausgenommen.
Fokusverlust, ausgeblendete Seite, Setup, Einstellungswechsel und Verbindungsverlust
beenden die lokale Sendefreigabe. Haltebetrieb endet zusätzlich beim Loslassen
oder Pointer-Abbruch. TX-Audio fließt erst nach bestätigter Freigabe vom Backend.
Die bestehenden Abschaltungen des Adapters bleiben zusätzlich aktiv.

Das aufklappbare Menü **Wasserfall** bestimmt den Grundrauschpegel von **Fullband**
und **Zoom** getrennt aus den vollständigen FFT-Daten. Dazu werden etwa 5 kHz breite
Abschnitte über ihren Median bewertet; das 20-%-Quantil dieser Abschnittspegel
schätzt den ruhigen Untergrund. FFT-Ränder, die unmittelbare DC-Umgebung und
geclippte Bins werden ausgelassen. Im Zoom zählen ausschließlich die sichtbaren
±25 kHz. Ein Filter mit 3 Sekunden Zeitkonstante beruhigt die Schätzung unabhängig
von Moving Average. Bei zu wenigen verwertbaren Abschnitten oder mehr als 6 dB
Streuung zwischen dem 5-%- und 35-%-Quantil wird der letzte Pegel gehalten und im
Menü entsprechend gekennzeichnet. Eine durchgehend belegte, gleichmäßig starke
Breitbandaussendung lässt sich allein damit nicht sicher von Rauschen unterscheiden.

Zwei gemeinsame Regler stellen **Abstände zum Rauschpegel in dB** für Fullband
und Zoom ein: Untergrenze **−10 bis +10 dB**, Standard **−5 dB**;
Obergrenze **+30 bis +40 dB**, Standard **+40 dB**. Beide Regler haben
**1-dB-Schritte**; die automatisch berechneten
dBFS-Grenzen folgen dem Rauschpegel kontinuierlich. Bei −60 dBFS Rauschen ergibt
sich standardmäßig −65 bis −20 dBFS. Die Rauschschätzung bleibt für beide Bereiche
getrennt. Das Menü zeigt Rauschpegel und resultierenden
Anzeigebereich an. Die Grenzen gelten für Spektrum und neue Wasserfallzeilen;
die bestehende Historie bleibt erhalten. Nach Session-/Loop-Wechsel wird neu
gemessen. Vor der ersten zuverlässigen Schätzung wird vorläufig −100 dBFS verwendet.
Nur die gemeinsamen Abstände werden im Browser gespeichert; beim Umstieg werden
die bisherigen Fullband-Abstände übernommen und auf den neuen Bereich begrenzt.
Alte absolute Anzeigegrenzen
werden ignoriert. Wasserfall- und PTT-Einstellungen werden ausschließlich im
jeweiligen Browser gespeichert. Beim erneuten Laden bleibt PTT immer aus.

Die Auswahl **Farbe · Wasserfall und Spektrum** schaltet beide Bereiche gemeinsam
auf **Blau**, **Rot** oder **Grün**. Auch die vorhandene Wasserfall-Historie wird
umgefärbt und bleibt erhalten. Die Farbauswahl wird im Browser gespeichert;
die Kennfarben der RX-/TX-Markierungen bleiben unverändert.

**Moving Average** im Wasserfall-Menü stellt gemeinsam für beide Spektrumanzeigen
die zeitliche Mittelung von **1 bis 20 Messungen** ein. **1** zeigt die aktuelle
FFT ohne Glättung; höhere Werte bilden ein gleitendes Mittel über die letzten
N vollständigen FFT-Messungen. Gemittelt wird die lineare Leistung, anschließend
wird sie wieder in dBFS dargestellt. Solange weniger als N Messungen vorliegen,
wird über die vorhandenen gemittelt. Wasserfälle und Audio bleiben ungeglättet.
Die Einstellung wird im Browser gespeichert. Bei neuer Frequenzmitte, FFT-Geometrie,
Session oder Loop werden nur passende Spektraldaten zur Mittelung verwendet.

Nur das obere **Fullband-Spektrum** beschriftet Spitzen mit **mehr als 15 dB
Abstand zum geschätzten Rauschpegel**; im Zoom entfallen die Spitzenbeschriftungen.
Gemessen wird der höchste Abstand innerhalb der letzten **1 Sekunde**
aus den FFT-Daten vor Moving Average, jeweils bezogen auf den zum Messzeitpunkt
geschätzten Rauschpegel. Horizontal über der Spitze und auf fester Höhe knapp
unter dem oberen Spektrumrand steht der gerundete
Zahlenwert mit Einheit: **+25dB** bedeutet 25 dB über Rauschen. Plateaus bekommen
eine gemeinsame Beschriftung. Bei Überlappung haben stärkere Spitzen Vorrang;
Beschriftungen am rechten Rand oder über den linken Pegelangaben werden ausgelassen.
Die Messung beginnt nach einer zuverlässigen Rauschschätzung und startet nach
Änderungen der FFT-Geometrie einschließlich der Frequenzmitte neu. Empfangene
Maximalwerte verfallen nach 1 Sekunde auch bei ausbleibenden FFTs. Die Messung
beeinflusst weder Wasserfall noch Audio.

Das **Filtermenü** enthält Bandbreiten-Presets, untere/obere Filtergrenze und RX1-
Rauschreduktion. Filterränder bleiben innerhalb 100–4000 Hz. Die QSO-Leiste enthält
die Kopiertasten, Lautstärke, Mikrofon-Kompressor und PTT. Kompressor 0 ist Bypass,
100 % Normalbetrieb. Der Lautstärkeregler bildet die Backend-Verstärkung 0–20 auf
0–100 % ab; Zuschauer können die Lautstärke lokal ändern.

**Setup** öffnet einen Dialog in derselben Verbindung: RX1-Audiowiedergabe,
Mikrofon, PTT-Bedienart, Pluto-Verbindung, Frequenzkorrekturen, TX-Dämpfung
und Test-Loops. Änderungen werden mit Settings-Revision und ausschließlich den
geänderten Schlüsseln gespeichert. Fehler und Revisionskonflikte werden angezeigt.
Hardwareänderungen und Loop-Wechsel löschen die Sendefreigabe. Bei Hardwarefehlern
bleibt Setup zugänglich, PTT ist gesperrt. Nach Abmeldung wird nur durch explizites
**Verbinden** erneut angemeldet; Zuschauer werden nicht automatisch befördert.

Audio verwendet im Funkprotokoll 12-kHz-PCM und einen adaptiven Jitterpuffer.
Der AudioContext läuft mit der nativen Geräte-Samplerate, das AudioWorklet
übernimmt die Umrechnung zwischen Gerät und 12 kHz. 12, 44,1 und 48 kHz sind getestet.
Bei einer reinen RX-Abstimmung bleiben **Audio-Epoche und Sample-Zähler** erhalten.
Das Backend ändert nur die Mischfrequenz; Filter, NR, Resampler, Oszillatorphase
und Zoom-FFT laufen weiter. Bereits gepuffertes Audio wird kontinuierlich ausgespielt.
Der Adapter veröffentlicht bestätigte Frequenz-/TX-/Loop-Zustände vor den
zugehörigen neuen Mediendaten.
Session-, Kanal- und Loop-Wechsel sowie sonstige Epochenwechsel
verwerfen weiterhin alte Audiodaten.
Mikrofon und AudioWorklet benötigen HTTPS oder localhost.

Unter **Setup → Audio** lassen sich Aufnahme- und Wiedergabegerät auswählen.
Zur Auswahl stehen **Systemstandard** und die verfügbaren Audiogeräte. Ohne
explizite Geräteauswahl gilt der Systemstandard. Die virtuellen Digitalgeräte
QO100_RX2, QO100_TX2 und QO100_RX2_Internal samt Monitorvarianten sind ausgeblendet.
Nach der Mikrofonfreigabe werden die Gerätenamen angezeigt. Die Auswahl wird lokal
im jeweiligen Browser gespeichert. Bei veralteten Geräte-IDs wird nach der Freigabe
auf den Systemstandard zurückgefallen. Eine abgelehnte Ausgabezuordnung erzeugt eine Warnung und
bricht eine funktionierende Mikrofonaufnahme nicht ab.
Die Aufnahme wird mit verbindlicher Geräte-ID angefordert und vor der Verwendung
anhand des geöffneten Tracks geprüft. Ein vorübergehend für die Freigabe geöffneter
Systemstandard wird durch das gewählte Mikrofon ersetzt; eine falsche Quelle
wird nicht an den Sender angeschlossen. Wird die Auswahl abgelehnt, bleibt die
Wiedergabe aktiv und das Mikrofon aus, mit einer Fehlermeldung im WebGUI.
Ein Gerätewechsel beendet PTT. Browser ohne
`AudioContext.setSinkId` verwenden die Systemausgabe; deren Zuordnung erfolgt
beispielsweise über pavucontrol. Audiofehler bleiben im Statusbereich sichtbar;
ein blockierter Start bricht nach sechs Sekunden mit einer Meldung ab.

Das ursprüngliche technische Test-GUI bleibt unter **`/test.html`** erreichbar;
die statische Entwurfsvorschau unter **`/layout-preview.html`**.

## Rollen und Abschaltung

- Eine **neue WebSocket-Verbindung** erhält bei freiem Steuerplatz die Rolle `operator`.
  Weitere sind `viewer`. Bestehende Zuschauer werden **nie automatisch befördert**.
- Zuschauer erhalten FFT, RX1-Audio, RX3-/RX4-Decoder-Text und Betriebszustand. Kommandos, Settings-Abfragen und
  TX-Audio von Zuschauern werden vor der UDP-Ausgabe abgewiesen. `state` liest nur den Cache.
- Bediener meldet echte Maus-/Tastatur-/Touch-Aktivität mit `activity`, gedrosselt auf etwa
  einmal pro Sekunde. Der Adapter misst selbst: **300 s ohne Activity → Verbindung schließen**.
  Audio, FFT, Heartbeats und PTT-Keepalives zählen nicht als Bedienaktivität.
- Alle Browser können `heartbeat` etwa jede Sekunde senden. Beim Bediener führen **6 s**
  ohne Heartbeat zur Abschaltung. Zuschauer dürfen passiv bleiben; WebSocket-Ping prüft die Verbindung.
- Verlust/Timeout/Shutdown: TX-Berechtigung sofort löschen, Audiopakete verwerfen, **PTT aus,
  Loop normal**. Ein frisch angemeldeter Bediener darf erst nach bestätigter Abschaltung steuern.
  Nach Timeout **keine automatische Wiederanmeldung**; die Testseite zeigt „Verbinden“.
- TX benötigt zusätzlich `ptt_keepalive` etwa alle **250 ms** während der gedrückten Taste.
  Nach **750 ms** ohne Freigabe setzt der Adapter PTT aus. Das Backend hat zusätzlich seine
  eigene 1,5-s-Lease. Ein später Keepalive schaltet PTT nicht wieder ein.
- Bei Backend-Neustart: Session neu ermitteln, Puffer leeren, Zustände neu laden; PTT bleibt aus.
  Langsame Browser erhalten begrenzte Puffer; alte FFT-Fragmente werden verworfen, dauerhaft
  zurückliegende Verbindungen getrennt. Andere Teilnehmer laufen weiter.

Hardware-Settings und Änderungen an `initial_tx_hz` löschen ebenfalls die PTT-Freigabe und
stellen Loops auf Normal. Bis zum Abschluss der Änderung und bestätigter Abschaltung bleibt
`ready=false`; anschließend ist ein neuer bewusster PTT-Tastendruck erforderlich.

## WebSocket-Vertrag

Ein Socket, URL **`/ws`**. Textnachrichten = JSON; Binärnachrichten = genau ein unverändertes
[QTR2-Datagramm](udp-protocol.md). Der Adapter verwaltet selbst UDP-IDs, Wiederholungen,
Subscription und Backend-Sessions. Browser-`id` ist optional, uint32, und wird in ACK/Fehler zurückgegeben.

```js
const url = new URL('/ws', location.href);
url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';
ws.onmessage = e => {
  if (typeof e.data === 'string') console.log(JSON.parse(e.data));
  else { /* QTR2-Header + FFT/PCM aus e.data lesen */ }
};
// Nach role=operator UND state.ready=true:
// ws.send(JSON.stringify({type:'tune',id:1,stream:1,frequency_hz:10489748500}));
```

Alle Frequenzen ganze Hz, RX = Downlink, TX = Uplink. Revisionen und Nanosekunden-Zeitstempel
im JSON sind **Dezimalstrings**, um 64-Bit-Werte verlustfrei zu erhalten.

| Browser → Adapter: `type` | Weitere JSON-Felder |
|---|---|
| `state` | Keine; Cache-Abfrage, auch für Zuschauer |
| `heartbeat` | Keine; Verbindungsprüfung, keine Bedienaktivität |
| `activity` | Keine; nur Bediener, echte GUI-Eingabe |
| `tune` | `stream`: 1/2, `frequency_hz` |
| `tx` | `channel`: 1/2 (Standard 1), `frequency_hz`; bei TX1 `ptt`: true/false, bei TX2 nur `ptt:false` zur Abstimmung |
| `ptt_keepalive` | Keine; nur solange PTT bewusst gehalten wird |
| `filter` | `low_hz`, `high_hz` (Audio-Hz, 100…4000, mindestens 500 Abstand); alternativ bisherige `bandwidth_hz`, `center_hz` |
| `nr` | `percent`: 0…100 |
| `compressor` | `percent`: 0…140, Zehntelprozent möglich |
| `loop` | `mode`: 0 Normal, 1 Audio, 2 HF |
| `settings_get` | Keine; Snapshot beim Backend abfragen |
| `settings_set` | `revision`: zuletzt gelesener String, `values`: Objekt mit geänderten Schlüsseln |

Bei `filter`, `nr`, `compressor`, `loop` ersetzt `query:true` die Wertefelder.
Wertebereiche und Settings-Schlüssel entsprechen der [UDP-Referenz](udp-protocol.md).

| Adapter → Browser: `type` | Inhalt |
|---|---|
| `role` | `role`: `operator`/`viewer`, `client_id`, `owner_present`, `clients`, `idle_seconds` |
| `state` | `ready`, `session`, `settings`, `revision`, `flags`, `settings_status`, `rx`, `tx`, `filter`, `nr`, `compressor`, `loop` |
| `ack` | `id`, `command`, `status` (Backend-Status), `revision`, `flags` |
| `decoder_text` | `stream=3/4`, `text`: reine Decoder-Textausgabe für alle Browser |
| `error` | `id`, `code`, `message`; z. B. `read_only`, `not_ready`, `busy`, `superseded`, `timeout`, `backend_reset`, `invalid` |
| `closed` | `reason`: `idle` oder `heartbeat_timeout`; danach Verbindung geschlossen |

`state` kommt beim Verbinden und bei Änderungen. `rx["1"]` bis `rx["4"]` enthalten
`frequency_hz`, `epoch`, `status`; `tx` zusätzlich `ptt`, `audio_fifo`, `pluto_fifo`, `ppm`. `tx2` enthält dieselben Felder für TX2; dort bedeutet `ptt` die Bereitschaft des externen Audiowegs.
`loop` enthält `mode`, `target`, `generation`, `timestamp_ns`, `rx_lo_hz`, `status`.
`filter` enthält `low_hz`, `high_hz` und abgeleitet `bandwidth_hz`, `center_hz`; NR/Kompressor `percent`, Kompressor zusätzlich
`compression_db`/`limiting_db`. Noch nicht geladene Unterobjekte sind leer. Fehlerdetails bei
Settings stehen im Snapshot (`error`/`hardware_error`). ACK-Status 0 bestätigt die Ausführung;
Status 2 bei Settings bedeutet Revisionskonflikt: aktuellen Snapshot übernehmen und neu versuchen.

**Binär RX:** FFT Typ 2 der Streams **0/1/2** (Fullband/RX1-Zoom/RX2-Zoom) und Audio Typ 5 der Streams **1/2/4**, unverändert an alle Browser. Die Zooms verwenden jeweils 8192 Bins bei 75 kHz, zentriert auf ihrer eigenen RX-Frequenz. Das WebGUI zeigt den ausgewählten RX als ±25-kHz-Ausschnitt; das einfache Test-GUI verwendet weiterhin RX1. **RX2-Audio (Stream 2)** geht zusätzlich an den lokalen Digital-Audioadapter. **RX3/RX4-Audio (Stream 5/6)** geht ausschließlich an die jeweiligen lokalen Decoder. Fragment-, Session- und
Epochenregeln aus der UDP-Referenz gelten auch hier. **Binär TX:** ausschließlich Typ 8,
544 Byte mit 240 PCM16-Samples bei 12 kHz. Erst nach bestätigtem `tx.ptt=true` senden, mit
aktueller Session/TX-Epoche und bestätigter TX-Frequenz; Sample-Index 0, 240, 480, … .

## Test

`ctest --test-dir build -R webbridge_udp --output-on-failure` startet ein privates synthetisches
Backend und prüft echte HTTP-/WebSocket-Verbindungen inklusive Paketverlust, Rechteprüfung,
TX-Audio-Loop, langsamen Zuschauern, Abschaltung, Neustart und Inaktivität. Kein Pluto-Zugriff.
Optionaler Browsertest: `tests/test_webbridge_browser.py` mit Chromium/ChromeDriver; Argumente:
Adapter-Binary, Backend-Binary, Backend-Config, `web`-Verzeichnis, Screenshot-Pfad.

## WebGUI-Tests

Ohne zusätzliche JavaScript-Abhängigkeiten:

```sh
node tests/test_web_gui.mjs
node tests/test_ptt_control.mjs
```

Der vollständige Browsertest verwendet Chromium/ChromeDriver und ausschließlich
ein privates synthetisches Backend mit eigenen Ports; er greift nicht auf die
laufende Station zu:

```sh
/usr/bin/python3 tests/test_web_gui_browser.py \
  build/bin/qo100_webbridge build/bin/qo100_backend \
  config/backend.conf web /tmp/qo100-webgui.png
```

Geprüft werden Live-FFT, Split-Abstimmung, Rechtsklick, Mausrad/Shift/Strg, RX2,
Wasserfall-Historie und weiterlaufende Zoom-Frames während zwölf Abstimmvorgängen.
Eine zusätzliche AudioWorklet-Sonde prüft dabei die tatsächliche PCM-Ausgabe
auf Stilleintervalle, auch bei vier FT8-Textschüben mit jeweils 100 Zeilen und
voller Texthistorie. Decodertexte werden im Browser für 50 ms gesammelt und
gemeinsam dargestellt; das Textlayout bleibt auf das Monitorfeld begrenzt.
PTT-Tests prüfen Halten, Schalten, die optionale Leertaste,
Tastenwiederholung, ausgeschlossene Eingabefelder und den Fünf-Minuten-Timeout
mit simulierter Zeit. Weitere Prüfungen:
Kopiertasten, DSP, Settings, Mikrofonpakete, PTT-Loslassen/Fokusverlust, echte
Touch-Ereignisse, Bildschirmbreiten 320–1920 Pixel, Backend-Neustart, Zuschauer
und explizites Trennen. Die Browserprüfung des ursprünglichen Test-GUIs unter
`tests/test_webbridge_browser.py` verwendet jetzt `/test.html`.
