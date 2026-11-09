# QO-100 Multimedia-Bakenmonitor

> **In QO100_TRX2 integriert:** Der reguläre Start erfolgt mit `./start_all.sh`
> im Hauptprojekt, über den eigenen RX5 statt über die RX2-Soundkarte.
> Siehe [RX5-Integration](../../docs/mm-beacon.md). Die folgende Dokumentation
> beschreibt auch den ursprünglichen eigenständigen PulseAudio-/Tkinter-Monitor.

Empfang, automatische Frequenzkorrektur und Sammlung der Textkanäle.
Eingang ist demoduliertes USB-Audio von PulseAudio `fromQO100`.
Es gibt keinen Audioausgang und keinen Sendebetrieb.

## Übergabe an das größere Projekt

**Stand: 09.11.2026. Arbeitsverzeichnis: `/Daten/funk/sat/mmdecoder`.**
Dieses Verzeichnis enthält den vollständigen Quellstand, Build, Tests und
Dokumentation. Dateien unter `/tmp` aus der Entwicklung sind zum Betrieb
nicht erforderlich. `data/` enthält bereits gesammelte Live-Daten und soll
bei der Integration erhalten bleiben.

Bereits umgesetzt und vom Anwender im Live-Betrieb bestätigt:

- Extrahierter RX-Kern des HS-Modems mit 8APSK / 7200 bit/s.
- Automatische Akquisition und Frequenznachführung einschließlich Wiederfinden
  nach manueller Änderung der vorgeschalteten Empfangsfrequenz.
- Persistente Sammlung von CW-Skimmer, DX-Cluster und vollständig geprüften
  Bulletin-Dateien; getrennte Tkinter-Ansichten und lokale Textschnittstellen.
- 37 bestandene Tests, einschließlich AFC, Protokoll, Dateirekonstruktion,
  Wiederholungen, Neustarts, Cursor und HTTP.

In den gespeicherten Live-Daten waren beim Dokumentationsabgleich bereits
16 CW-Skimmer-Meldungen, mehr als 1200 DX-Meldungen und 23 vollständige
Bulletin-Inhalte/Versionen vorhanden. Das sind historische Testnachweise,
keine festen Sollwerte. CW ist damit inzwischen auch mit echten Funkdaten
bestätigt; die frühere Einschränkung „nur mit Testdaten geprüft“ ist überholt.

**Empfohlene Integrationsgrenze:** Den Monitor als separaten Headless-Prozess
betreiben und im Hauptprojekt die HTTP-Ereignisse mit Cursor abholen. Damit
bleiben Audioverarbeitung, Synchronisation und Bulletin-Rekonstruktion in
diesem Modul; das Hauptprojekt übernimmt Darstellung und Weiterverarbeitung.
Eine laufende Monitor-Instanz kann bereits über HTTP genutzt werden.

Vor dem Start einer weiteren Instanz zuerst den Zustand prüfen:

```sh
curl --max-time 2 http://127.0.0.1:8765/v1/status.json
```

Nur **eine schreibende Monitor-/Collector-Instanz pro Datenverzeichnis**
betreiben. Der interne Python-Lock schützt Threads einer Collector-Instanz;
er ist keine Prozesssperre für mehrere Programme. Insbesondere keinen
zweiten `Collector` nur zum Lesen des laufenden Datenverzeichnisses öffnen:
sein Konstruktor erzeugt die Exportdateien neu. Zum Lesen HTTP verwenden.
Ein anderer HTTP-Port allein schafft keine Trennung des Datenbestands;
für einen isolierten Paralleltest zusätzlich ein eigenes `--data-dir` wählen.

### Headless starten und beenden

Einmalig bauen, danach den Python-Prozess mit expliziten Pfaden starten:

```sh
make -C /Daten/funk/sat/mmdecoder
/usr/bin/python3 /Daten/funk/sat/mmdecoder/rx_monitor.py \
  --headless \
  --source fromQO100 \
  --data-dir /Daten/funk/sat/mmdecoder/data \
  --http-port 8765
```

Ohne `--seconds` läuft der Headless-Prozess unbegrenzt. Die automatische
Frequenzkorrektur ist dabei standardmäßig aktiv. `./start_rx.sh` ist der
bequeme lokale Starter und führt vorher `make` aus. Wer stdout maschinell
liest, sollte wie oben direkt `rx_monitor.py` starten: Der Shell-Starter
kann vorher normale Build-Meldungen ausgeben.

Der Headless-Prozess behandelt SIGINT und SIGTERM und beendet seine
Unterprozesse `parec` und `rx_probe`, HTTP sowie die Datenbank geordnet.
Ein Prozessmanager des Hauptprojekts soll den gestarteten Python-Prozess
verwalten, beim Beenden SIGTERM senden und mit Timeout auf dessen Ende warten.
Die GUI wird regulär über das Fenster geschlossen. Eine systemd-Unit oder
ein Autostart ist in diesem Projekt bisher nicht eingerichtet.

Unter einem Supervisor stdout/stderr fortlaufend lesen oder in eine Datei
umleiten. Eine ungelesene `subprocess.PIPE` kann sich füllen und den Monitor
blockieren. Für eine Integration über HTTP ist keine Auswertung von stdout
erforderlich. Beispiel für dessen Ausgabeumleitung:

```sh
/usr/bin/python3 /Daten/funk/sat/mmdecoder/rx_monitor.py --headless \
  --data-dir /Daten/funk/sat/mmdecoder/data \
  >> /Daten/funk/sat/mmdecoder/logs/worker.log 2>&1
```

Audiozugriff setzt die PulseAudio-Sitzung des angemeldeten Benutzers voraus.
Den Monitor im gleichen Benutzerkontext wie den bestehenden Empfang starten;
ein beliebiger Root-/Systemdienst hat nicht automatisch Zugriff auf dessen
PulseAudio-Server. Das Modul erstellt `fromQO100` nicht und stimmt auch den
vorgeschalteten Funk-/SDR-Empfänger nicht ab. `fromQO100` muss bereits das
demodulierte USB-Audio der Bake liefern, **keine Stereo-I/Q-Daten**.

HTTP-Bereitschaft und Funksynchronisation sind zwei verschiedene Zustände:
Ein erfolgreicher HTTP-Aufruf bestätigt den laufenden Textserver. Ob aktuelle
Funkdaten dekodiert werden, steht in `receiver.locked`. Bereits gespeicherte
Texte bleiben auch ohne aktuelle Synchronisation abrufbar.

## Start

```sh
cd /Daten/funk/sat/mmdecoder
./start_rx.sh
```

Das Fenster enthält getrennte Ansichten für CW-Skimmer, DX-Cluster und
Bulletins. Unter **Empfang / AFC** stehen Audiopegel, das lokale Spektrum,
Hilfsträger und Konstellation zur Verfügung. Standard:
48 kHz, mono, 8APSK, 7200 bit/s. Die Frequenzkorrektur (AFC) ist standardmäßig
aktiv; ein manueller Feinabgleich des Hilfsträgers auf 100 Hz ist nicht nötig.
Das komplette Baken-Signal einschließlich Hilfsträger muss im nutzbaren
Audio-Durchlassbereich liegen. Bereits weggefilterte Signalanteile lassen
sich mit einer nachträglichen Frequenzkorrektur nicht zurückholen.

## Textdaten sammeln und abholen

Der Monitor verarbeitet nur die drei gewünschten Nutzdatenkanäle:

- **DX-Cluster:** externer Typ 0, Längenbyte und nullterminierter Text.
  Reporter, Frequenz (kHz), DX-Rufzeichen, Kommentar, UTC und Locator werden
  getrennt und zusätzlich als lesbare Zeile gespeichert.
- **CW-Skimmer:** externer Typ 2, Text direkt nach dem Typbyte. Das bekannte
  Format enthält Reporter, Frequenz, Rufzeichen, Signalwert, WPM, Kommentar
  und UTC. Unbekannte Textformate bleiben als Originaltext erhalten.
- **Bulletins:** Nur `.blt`-Dateien werden zusammengesetzt. Der 55-Byte-Kopf,
  Dateilänge und Blocknummern ordnen die Daten zu. Die Datei-CRC16 und die
  ZIP-Prüfung müssen stimmen, bevor der Text veröffentlicht wird. Die
  Metadatenzeile `~Typ:Seite:Titel~` wird getrennt, HTML wird in reinen Text
  umgewandelt. Inhalte von script/style werden nicht übernommen.

WB-/NB-Spektrumsdaten, Bilder, HTML-Empfangsseiten und sonstige Dateien werden
vom Datensammler verworfen. Das lokal aus Audio berechnete Diagnose-Spektrum
bleibt verfügbar. Die Zuordnung von DX und CW wurde zusätzlich mit der über
die Bake empfangenen `qo100info.html` abgeglichen.

Die Speicherung erfolgt im Verzeichnis `data` (änderbar mit `--data-dir`):

| Datei | Inhalt |
|---|---|
| `cw-skimmer.txt` | Bis zu 1000 zuletzt empfangene CW-Meldungen |
| `dx-cluster.txt` | Bis zu 1000 zuletzt empfangene DX-Meldungen |
| `bulletin.txt` | Zuletzt vollständig empfangenes Bulletin |
| `bulletins/*.txt` | Alle Bulletin-Versionen, mit Dateiname und Inhaltshash |
| `events.jsonl` | Alle gesammelten Texte mit fortlaufender `seq` als JSON Lines |
| `receiver.sqlite3` | Verbindlicher Datenbestand einschließlich unvollständiger Bulletins |

Die Textdateien sind UTF-8 und werden atomar aktualisiert. Das bisherige
vollständige Bulletin bleibt lesbar, während ein neues noch unvollständig ist.
Die Bulletin-Auswahl im Fenster bietet die gespeicherten Dateien/Versionen
nach Empfangszeit an; "zuletzt empfangen" bezeichnet nicht zwingend die
inhaltlich neueste Rundspruch-Ausgabe.

Identische DX-/CW-Texte innerhalb von 120 Sekunden werden als Wiederholung
unterdrückt. Spätere Wiederholungen bleiben als neue Meldung erhalten.
Identische Bulletin-Inhalte werden je Dateiname dauerhaft dedupliziert;
geänderte Inhalte bleiben als eigene Version erhalten. Meldungen, Cursor und
unvollständige Bulletin-Blöcke überstehen einen Programmneustart. Bei Einstieg
mitten in eine Datei muss zunächst deren nächster Dateikopf empfangen werden;
danach können gespeicherte Blöcke aus früheren Durchläufen ergänzt werden.
Maximal 32 unvollständige Dateien werden zwischengespeichert.

Während der Monitor läuft, ist die lokale HTTP-Schnittstelle verfügbar:

| Endpunkt | Ausgabe |
|---|---|
| `/v1/cw-skimmer.txt` | CW-Textansicht |
| `/v1/dx-cluster.txt` | DX-Textansicht |
| `/v1/bulletin.txt` | Letztes vollständiges Bulletin |
| `/v1/bulletins.json` | Verzeichnis mit Metadaten und Sequenznummern |
| `/v1/bulletins/123.txt` | Bulletin mit der Sequenznummer 123 |
| `/v1/events?after=123&limit=500` | Neue Ereignisse als JSONL, maximal 1000 je Abfrage |
| `/v1/status.json` | Zähler, fehlende Bulletin-Blöcke und aktueller Empfangsstatus |

Standardadresse ist `http://127.0.0.1:8765`. Mit `--http-port PORT` ist der Port
änderbar; `--no-http` schaltet die HTTP-Ausgabe ab. Es wird nur auf dem lokalen
Rechner gelauscht. Beispiel für externe Programme:

```sh
curl http://127.0.0.1:8765/v1/bulletin.txt
curl 'http://127.0.0.1:8765/v1/events?after=0&limit=500'
curl 'http://127.0.0.1:8765/v1/events?after=123&channel=cw-skimmer'
```

Zur inkrementellen Abholung die zuletzt gelesene `seq` speichern und bei der
nächsten Abfrage als `after` verwenden. Ohne Kanalfilter enthält die Ausgabe
alle drei Kanäle. Während das Programm beendet ist, bleiben die Textdateien
und die Datenbank verfügbar; HTTP wird zusammen mit dem Monitor beendet.

### Ereignisschema und verlässliche Cursor-Verarbeitung

`/v1/events` liefert eine endliche Antwort mit einem JSON-Objekt pro Zeile,
keinen dauerhaft offenen Stream und kein JSON-Array. Content-Type ist
`application/x-ndjson; charset=utf-8`. Bei keinen neuen Ereignissen ist die
Antwort HTTP 200 mit leerem Body. Die Textendpunkte verwenden entsprechend
`text/plain; charset=utf-8`; auch sie dürfen anfangs leer sein.

Jedes gespeicherte Text-Ereignis hat folgende Felder:

| Feld | Typ und Bedeutung |
|---|---|
| `seq` | Integer; globale, aufsteigende Sequenz im jeweiligen Datenbestand |
| `channel` | `cw-skimmer`, `dx-cluster` oder `bulletin` |
| `received_at` | ISO-8601-Zeitstempel mit Zeitzone; lokale Empfangszeit, nicht die Zeitangabe des Spotters |
| `raw_text` | Dekodierter Originaltext; bei Bulletins einschließlich etwaiger Metadaten-/HTML-Anteile |
| `text` | Für Anzeige und Textausgabe aufbereiteter Klartext |
| `fields` | Kanalabhängige Metadaten als JSON-Objekt |

Text-Ereignisse haben derzeit **kein** zusätzliches Feld `event`.
Frequenz-, Signal- und WPM-Werte in Spot-Metadaten bleiben Strings, weil die
Quelle das Textformat vorgibt. Bei unbekanntem Spot-Format ist `fields` leer;
`raw_text` und `text` bleiben trotzdem verfügbar. Zusätzliche zukünftige
JSON-Felder sollen konsumierende Programme ignorieren können.

DX-Metadaten eines tatsächlich empfangenen Beispiels:

```json
{
  "reporter": "SA5OKK",
  "frequency": "21227.0",
  "callsign": "SQ2OK",
  "comment": "CQ USA",
  "utc": "1336Z",
  "locator": "JO94"
}
```

CW-Metadaten eines tatsächlich empfangenen Beispiels:

```json
{
  "reporter": "DK0SB-#",
  "frequency": "10489523.7",
  "callsign": "R2DA",
  "signal_db": "23",
  "wpm": "22",
  "comment": "CQ",
  "utc": "1406Z"
}
```

DX-Frequenzen werden als kHz dargestellt. Beim CW-Kanal bleibt der
Frequenzwert unverändert aus der Quelle erhalten; der Code versieht ihn
nicht mit einer Einheit und führt keine Umrechnung durch. Bei Übernahme in
ein vereinheitlichtes Frequenzmodell des Hauptprojekts diese Quellkonvention
explizit berücksichtigen. `utc` ist eine Quellenangabe wie `1406Z` ohne
eigenes Datum; sie ersetzt nicht `received_at`.

Bulletin-Metadaten enthalten immer `filename`, `sha256`, `file_id` und `bytes`.
`sha256` bezieht sich auf die ursprünglichen entpackten Datei-Bytes, `bytes`
ist deren Länge. `file_id` ist die protokollspezifische CRC16 der übertragenen
ZIP-Datei. Wenn die erste Textzeile dem Schema `~Typ:Seite:Titel~` entspricht,
kommen `category` (String), `page` (Integer) und `title` (String) hinzu.
Beispiel: `category="R"`, `page=3`, `title="42"` bei `DLRS202642.blt`.
Neue Versionen derselben Datei haben eine eigene `seq`; diese ist auch die
ID für `/v1/bulletins/<seq>.txt`. Das Bulletin-Verzeichnis liefert alle
gespeicherten Versionen, nach Empfangszeit absteigend, ohne `text`/`raw_text`.

Für verlustarme inkrementelle Abholung:

1. Den zuletzt erfolgreich verarbeiteten Cursor im Hauptprojekt speichern;
   beim allerersten Zugriff `after=0` verwenden.
2. Die Zeilen einer Antwort in aufsteigender `seq` verarbeiten.
3. Erst **nach** erfolgreicher Verarbeitung den Cursor auf deren `seq` setzen.
4. Bei voller Antwort sofort die nächste Seite abholen; bei leerer Antwort
   warten und erneut abfragen. `limit` ist auf 1 bis 1000 begrenzt, Standard 500.
5. Wiederholte Zustellung nach einem Client-Absturz anhand von `seq` erkennen.

Minimaler Polling-Baustein, der nur die Standardbibliothek benötigt:

```python
import json
from urllib.request import urlopen

def poll_once(after, consume, save_cursor):
    url = f"http://127.0.0.1:8765/v1/events?after={after}&limit=500"
    with urlopen(url, timeout=5) as response:
        for line in response:
            if not line.strip():
                continue
            event = json.loads(line)
            consume(event)                  # Im Hauptprojekt verarbeiten.
            save_cursor(event["seq"])       # Erst danach dauerhaft bestätigen.
            after = event["seq"]
    return after
```

Der Aufrufer kümmert sich um Wiederholung, Fehlerbehandlung und Speicherung
des Cursors. Keine Datenbank- oder Monitor-Neustarts wegen einer einzelnen
fehlgeschlagenen HTTP-Anfrage auslösen. Für genau-einmalige Übernahme müssen
Verarbeitung und Cursor im Hauptprojekt zusammen transaktional oder die
Verarbeitung anhand von `seq` idempotent sein.

Für einen gemeinsamen Cursor alle Kanäle abholen und lokal filtern. Wer
`channel=cw-skimmer`, `channel=dx-cluster` oder `channel=bulletin` getrennt
abfragt, braucht getrennte Cursor pro Kanal. Lücken in kanalgefilterten
Sequenzen sind normal. Es gibt keine globale Uhrzeit-Sortiergarantie:
nachträglich importierte ältere Daten bekommen neue Sequenznummern.

Der Cursor übersteht einen normalen Programmneustart, ist aber an
`receiver.sqlite3` gebunden. Bei Wechsel/Neuanlage des Datenverzeichnisses
beginnt ein neuer Sequenzraum. Nicht unbemerkt den alten Cursor auf einen
anderen Datenbestand anwenden. Ein `after`, das größer als
`status.last_sequence` ist, kann auf einen solchen Wechsel hinweisen.

### Empfangsstatus und HTTP-Eigenschaften

`/v1/status.json` enthält:

| Feld | Bedeutung |
|---|---|
| `counts` | Dauerhaft gespeicherte Ereignisse je Kanal, nach Duplikatunterdrückung |
| `last_sequence` | Höchste bisher gespeicherte `seq` |
| `pending_bulletins` | Liste mit `filename`, `expected` und `received` für noch unvollständige Dateien |
| `malformed_frames` | In dieser Collector-Laufzeit abgewiesene ungültige Eingaben |
| `last_error` | Letzter Fehler des Datensammlers; leer, wenn keiner vorliegt bzw. nach erfolgreichem Bulletin |
| `receiver` | Letzter RX-Status; `locked` wird nach mehr als 2 s ohne Aktualisierung auf false gesetzt |

Wichtige RX-Felder: `locked`, `valid_frames`, `recent_frames`, `frame_age_s`,
`rms_dbfs`, `peak_dbfs`, `pll_hz`, `center_hz`, `frequency_offset_hz`,
`afc_enabled` und `afc_state`. Letzteres ist `searching`, `acquiring`,
`tracking`, `hold` oder `manual`. `audio_seconds` ist seit Prozessstart
verarbeitete Audiozeit und kein Datum. `valid_frames` zählt alle gültigen
Funkframes einschließlich der anschließend verworfenen Spektrumsdaten;
es ist daher nicht gleich der Zahl gesammelter Texte.

Vor der ersten RX-Aktualisierung kann `receiver` nur `locked=false` enthalten.
Clients sollen nicht verlangen, dass alle Diagnosefelder bereits existieren.
PulseAudio-/Unterprozessfehler sind zusätzlich im GUI-Fußbereich bzw. in der
Headless-Ausgabe sichtbar; `last_error` ist kein vollständiges Prozesslog.

Die API bietet nur GET-Abfragen. Es gibt keinen WebSocket, SSE, Login oder
Steuerendpunkt. Antworten haben `Cache-Control: no-store`. Ungültige Cursor,
Limits oder Kanäle ergeben HTTP 400; unbekannte Endpunkte/Bulletin-IDs 404.
Fehlerantworten sind nicht als JSON spezifiziert. Fehlt der Server vollständig,
tritt stattdessen ein Verbindungsfehler auf.

Der Server bindet ausschließlich `127.0.0.1`. Für eine Browseroberfläche des
Hauptprojekts die Abfragen gegebenenfalls über dessen Backend weiterreichen:
der aktuelle Textserver sendet keine CORS-Header. `--http-port 0` wählt einen
freien Port und ist für Tests/Einbettung gedacht; dessen Adresse liefert beim
Python-Objekt `TextAPI.url`. Für einen extern gestarteten Worker einen festen
konfigurierten Port verwenden.

### Speicher- und Wiederanlaufverhalten

SQLite ist die maßgebliche Quelle; Exportdateien werden daraus erzeugt.
`events.jsonl` wird beim Start vollständig neu geschrieben und anschließend
ergänzt. Deshalb für einen dauerhaften Consumer den HTTP-Cursor verwenden,
nicht dauerhaft auf einen offenen Dateideskriptor oder Byteoffset der
JSONL-Datei vertrauen. Die drei `.txt`-Momentaufnahmen werden per Umbenennung
ersetzt; zum erneuten Lesen die Datei jeweils neu öffnen.

Die GUI zeigt bis zu 500 aktuelle Spots je Kanal, die Text-Momentaufnahmen
bis zu 1000. SQLite und die Ereignisdatei bewahren weiterhin alle gesammelten
Ereignisse auf; eine automatische Aufbewahrungsfrist/Rotation ist bisher
nicht implementiert. Das Programm wächst daher bei längerem Betrieb mit
seinem Datenbestand. Bei einer Sicherung bevorzugt den Monitor geordnet
beenden und dann `data/` vollständig sichern; SQLite läuft im WAL-Modus.

Ein Bulletin wird ausschließlich nach vollständiger Rekonstruktion,
Datei-CRC16-Prüfung und erfolgreicher ZIP-Prüfung veröffentlicht. Bis dahin
bleibt der vorherige Klartext gültig. Unterstützt sind maximal 1024 Blöcke
mit 219 Bytes, also 224201 Bytes ZIP-Nutzdatei nach Abzug des Dateikopfs,
und maximal 4 MiB entpackter Bulletin-Text. Die ZIP-Datei muss genau eine
`.blt`-Datei enthalten. ZIP-Pfade werden nie auf das Dateisystem extrahiert.
Text wird bevorzugt als UTF-8, ersatzweise als CP1252 dekodiert.

Bei Audio-/Decoder-Ende beendet der Headless-Modus sich mit Fehlerstatus;
ein automatischer Neustart der Unterprozesse oder Wiederverbindung bei
verschwundener PulseAudio-Quelle ist bisher nicht implementiert. Das kann
der Prozessmanager des Hauptprojekts mit begrenzter Wiederholrate übernehmen.
Im GUI-Modus lässt sich der Empfang über den Neustart-Knopf erneut starten.
Bloßer Funk-Synchronisationsverlust ist dagegen kein Grund zum Prozessneustart:
dafür ist die vorhandene AFC-Wiederakquisition zuständig.

Bestehende Rohaufzeichnungen lassen sich nachträglich importieren. Ein
wiederholter Import erzeugt keine zusätzlichen identischen Ereignisse:

```sh
./start_rx.sh --import-logs
/usr/bin/python3 rx_monitor.py --replay-only
```

## Automatische Abstimmung

Die Korrektur geschieht im internen Audio-Downmixer. Bei einem Hilfsträger
von beispielsweise 125 Hz wird die Empfangsmitte auf ungefähr 1525 Hz gesetzt.
Damit sieht der nachfolgende Demodulator wieder das passende Basisband.
Die Hardwarefrequenz und die PulseAudio-Konfiguration werden nicht verändert.

1. **Suche:** Drei überlappende FFT-Spektren suchen einen schmalen Träger
   zwischen 20 und 900 Hz. Zwei aufeinanderfolgende Schätzungen müssen auf
   1,5 Hz übereinstimmen; die lokale Überhöhung muss mindestens 12 dB betragen.
2. **Prüfe Träger:** Die passende Signalmitte (Pilot + 1400 Hz) wird gesetzt.
   Erst gültige Frames bestätigen den Empfang. Liefert ein Kandidat nach fünf
   Sekunden keine Bestätigung, wird er 15 Sekunden zurückgestellt und der
   nächste Kandidat versucht.
3. **Nachführen:** Bei gültigen Daten verfolgt die vorhandene PLL die
   Restabweichung. Ihre Frequenzkorrektur wird langsam in den vorgeschalteten
   Mischer übertragen (maximal 1 Hz/s, ohne Phasensprung). Ein fremder Ton
   kann einen laufenden, gültigen Empfang dadurch nicht wegziehen.
4. **Halte Frequenz / erneute Suche:** Bei kurzem Ausfall bleibt die Frequenz
   erhalten. Nach mehr als 2,5 Sekunden ohne gültigen Frame darf neu gesucht
   werden. Bei digitaler Stille bleibt der Mischer stehen; bei Rückkehr des
   Signals wird der Tracker neu initialisiert.

Die Pilotsuche entspricht einem groben Versatzbereich von -80 bis +800 Hz.
Das nutzbare Audiofilter kann diesen Bereich einschränken. Synthetisch geprüft
sind Starts zwischen -75 und +750 Hz, Drift mit 1 Hz/s, ein Frequenzsprung
von 70 Hz, ein Ausfall mit verändert zurückkehrender Frequenz und ein stärkerer
Störton. Die alte echte Aufnahme mit rund +25 Hz Versatz wird ohne manuelle
Vorgabe automatisch dekodiert.

**Grün** bedeutet mindestens zwei FEC- und CRC-gültige Blöcke in den letzten
zwei Sekunden, davon einer in der letzten Sekunde. Ein Header-Kandidat oder
ein schmaler Träger reicht dazu nicht. Der Lock erlischt bei Signalverlust.
Die Zeitfenster beziehen sich im C++-Kern auf die Audiozeit; die Oberfläche
prüft zusätzlich, ob überhaupt noch aktuelle Daten ankommen.

Die numerische Pilotschätzung in der Oberfläche dient der Kontrolle
(20–900 Hz, mindestens 10 dB lokale Überhöhung) und entscheidet nicht über
die automatische Abstimmung. Diese läuft auch ohne Oberfläche vollständig
im C++-Kern. Aussteuerung ist dBFS, kein kalibriertes S-Meter oder SNR-Maß.

Für einen Vergleich lässt sich die AFC explizit abschalten:

```sh
./start_rx.sh --manual --center-hz 1500
./start_rx.sh --manual --center-hz 1525
```

Im manuellen Modus ist nur die normale interne PLL aktiv; die vorgegebene
Mischerfrequenz bleibt fest. Ohne `--manual` ist `--center-hz` die Anfangsmitte,
welche die AFC anschließend nachführen darf.

## Ohne Fenster / Aufzeichnung

```sh
./start_rx.sh --headless --seconds 15
./start_rx.sh --headless --seconds 15 --record logs/aufnahme.f32
./build/rx_probe < logs/aufnahme.f32
./build/rx_probe --manual --center-hz 1525 < logs/aufnahme.f32
```

Aufzeichnungen: 48000 Samples/s, mono, Float32 Little Endian, ohne Dateikopf.
`parec` verwendet ausdrücklich `fromQO100`, nicht das Standardmikrofon.
Quelle bei Bedarf mit `--source NAME` ändern. Die bestehende Audio-Routing-
Konfiguration und die Lautstärken werden nicht verändert.

In `logs/rx-*.jsonl` steht der Empfangsstatus etwa einmal je Audiosekunde.
Mit `--raw-frames` lassen sich zusätzlich alle CRC-gültigen Rohframes mit
Zeitstempel, Rahmentyp, Blocknummer, Anfang/Ende und Nutzdaten als Hex
protokollieren. Standardmäßig landen ausschließlich die gewünschten
Textkanäle im persistenten Datensammler; die übrigen Nutzdaten werden nicht
gespeichert.
Der Kern selbst schreibt Statusmeldungen etwa zehnmal je Audiosekunde.
Die zusätzlichen Statusfelder `afc_enabled`, `afc_state`, `center_hz` und
`frequency_offset_hz` beschreiben den AFC-Zustand und die Mischerfrequenz
bzw. deren Versatz zu 1500 Hz. `pll_hz` ist die verbleibende Korrektur der
inneren PLL; bei Lock ist `center_hz + pll_hz` die geschätzte Signalmitte.
Die aufbereiteten Text-Ereignisse stehen unabhängig davon in `data/events.jsonl`
und über die HTTP-Schnittstelle bereit.

## Aufbau und Voraussetzungen

- `src/rx_probe.cpp`: Audio-Downmixer/Vorfilter und JSONL-Ausgabe.
- `src/pilot_afc.cpp`: FFT-basierte Pilotsuche für die automatische Abstimmung.
- `vendor/hsmodem/`: extrahierter Symboltracker und Frame-Decoder aus dem Original.
- `rx_monitor.py`: PulseAudio-Aufnahme, Spektrum und Tkinter-Oberfläche.
- `data_collector.py`: kanalgetrennte Sammlung, Bulletin-Rekonstruktion,
  SQLite-Persistenz, Textdateien und lokale HTTP-Schnittstelle.
- `UPSTREAM.md`: Originalversion und dokumentierte Anpassungen.

### Alternative: direkt in Python einbetten

Das Projekt ist bisher ein Quellverzeichnis, kein installierbares pip-Paket.
Die Python-Dateien erwarten dieses Verzeichnis im Modul-Suchpfad. Für eine
gemeinsame Python-Anwendung sind folgende Einstiegspunkte vorhanden:

| Objekt | Aufgabe |
|---|---|
| `data_collector.Collector(directory)` | Datenbank und Exporte öffnen; ohne Audio-/NumPy-/Tk-Abhängigkeit |
| `Collector.ingest(event)` | RX-Frame/Status übernehmen; Liste neu gesammelter Text-Ereignisse zurückgeben |
| `Collector.events(after=0, limit=500, channel=None)` | Persistente Ereignisse ab Cursor lesen |
| `Collector.recent(channel, limit=1000)` | Jüngste Kanal-Ereignisse in Empfangsreihenfolge lesen |
| `Collector.bulletins()` / `.text(channel)` / `.status()` | Bulletin-Verzeichnis, Text oder Status lesen |
| `Collector.begin_session()` | Aktiven Datei-Kontext/RX-Status zurücksetzen; gespeicherte Daten und Blöcke bleiben erhalten |
| `Collector.close()` | Datenbank schließen |
| `data_collector.TextAPI(collector, port=8765)` | Lokale HTTP-Schnittstelle im eigenen Thread starten; `.url`, `.close()` |
| `rx_monitor.Receiver(source, center, log_path, record=None, manual=False, collector=None, raw_frames=False)` | PulseAudio und C++-Decoder mit Hintergrundthreads starten |
| `Receiver.snapshot()` / `.close()` | Diagnosemomentaufnahme lesen bzw. Empfang samt Unterprozessen beenden |

`Receiver.snapshot()` liefert `(status, status_at, audio, events, errors)`.
`status_at` stammt von `time.monotonic()`, `audio` ist ein NumPy-Array.
Die `events`-Warteschlange wird beim Lesen geleert und enthält höchstens
100 Einträge; sie ist für die Oberfläche gedacht. Für dauerhafte/verlässliche
Weiterverarbeitung `Collector.events()` oder HTTP mit Cursor verwenden.
Mit angeschlossenem Collector enthält die Queue die aufbereiteten Texte;
ohne Collector enthält sie rohe Frame-Ereignisse.

`Receiver` importiert NumPy; Tkinter wird erst innerhalb von `run_gui()`
importiert. Der Headless-Empfang benötigt deshalb keinen grafischen Desktop,
weiterhin aber die erreichbare PulseAudio-Benutzersitzung. Eine eigene GUI
kann Receiver/Collector verwenden und die Tkinter-Testoberfläche weglassen.

Die Reihenfolge beim Beenden ist: `Receiver.close()`, danach gegebenenfalls
`TextAPI.close()`, zuletzt `Collector.close()`. Ein Receiver-Neustart verwendet
denselben Collector weiter. Für einen Offline-Test des Sammlers ohne Audio
und ohne Zugriff auf den produktiven Datenbestand:

```python
import sys
from datetime import datetime, timezone
from tempfile import TemporaryDirectory

sys.path.insert(0, "/Daten/funk/sat/mmdecoder")
from data_collector import Collector

payload = b"\x00\xd9DL1AAA~14025.0~VK9TEST~CQ DX~1300Z~JO40"
with TemporaryDirectory() as directory:
    collector = Collector(directory)
    try:
        messages = collector.ingest({
            "event": "frame",
            "frame_type": 8,
            "frame_number": 0,
            "frame_info": 3,
            "payload_hex": payload.ljust(219, b"\x00").hex(),
            "received_at": datetime.now(timezone.utc).isoformat(),
        })
        assert messages[0]["fields"]["callsign"] == "VK9TEST"
        print(collector.text("dx-cluster"))
    finally:
        collector.close()
```

### Direkter Anschluss des RX-Kerns an vorhandene Audioversorgung

Wenn das Hauptprojekt Audio bereits selbst abholt, kann es alternativ nur
`build/rx_probe` als Unterprozess starten. Der aktuelle extrahierte RX-Kern
nutzt **stdin/stdout**, nicht die UDP-Ports 40131–40135 des ursprünglichen
Vollmodems. Für dieses Projekt sind weder `oscardata` noch der originale
HS-Modem-Prozess zusätzlich erforderlich.

- stdin: Headerloses reelles Mono-Audio, 48000 Samples/s, Float32 Little Endian.
  Keine WAV-Header, kein S16-PCM und keine komplexen I/Q-Paare direkt einspeisen.
- stdout: JSON Lines mit `event="status"` und `event="frame"`; fortlaufend lesen,
  damit der Prozess nicht an einer vollen Pipe wartet.
- stderr: Diagnose-/Fehlermeldungen; getrennt von JSON auswerten.
- EOF auf stdin: restliche vollständige Samples verarbeiten und regulär beenden.
- Argumente: `--center-hz 1500` als Anfangsmitte; `--manual` schaltet die AFC ab.

Ein Frame-Ereignis enthält `audio_seconds`, `frame_type`, `frame_number`,
`frame_info` und `payload_hex`. Letzteres hat genau 438 Hex-Zeichen für
219 bereits FEC-/CRC-geprüfte Nutzdatenbytes. `frame_type=8` enthält externe
Streams; deren erstes Nutzdatenbyte ist 0 für DX-Cluster, 2 für CW-Skimmer,
1/3 für die zu verwerfenden NB-/WB-Spektren. Bulletins kommen über
Dateirahmen der Typen 3/4/5; die Auswahl erfolgt anhand der Dateiendung `.blt`.

`frame_info`: 0 = Anfang, 1 = Fortsetzung, 2 = Ende, 3 = Einzelblock.
`frame_number` ist ein 10-Bit-Dateiblockzähler und **keine globale eindeutige
Meldungs-ID**; externe Streams können denselben Zähler wie andere Frames tragen.
Nicht anhand dieses Felds allgemein deduplizieren. Für externe Programme ist
die gespeicherte Text-`seq` die passende Kennung.

Der C++-Kern erzeugt keinen Kalenderzeitstempel. Ein eigener Adapter soll den
Frame-Ereignissen vor `Collector.ingest()` `received_at` mit Zeitzone hinzufügen.
Ohne diesen Wert verwendet der Collector die aktuelle UTC-Zeit. Auch die
Status-Ereignisse regelmäßig an den Collector weitergeben, damit dessen
Frischeprüfung korrekt funktioniert. Die AFC läuft unabhängig vom Python-
Adapter vollständig im C++-Prozess.

Die Headless-Ausgabe von `rx_monitor.py` ist davon zu unterscheiden: Sie mischt
etwa einmal je Sekunde `event="status"`, neue Text-Ereignisse ohne `event`-Feld
und gegebenenfalls `event="error"`. Sie spielt beim Start keine gespeicherte
Historie aus. Eine reine Text-Ereignisfolge einschließlich Historie liefern
`/v1/events` und `data/events.jsonl`.

### Getestete Laufzeit und Pfade

Abhängigkeiten: C++11-Compiler, make, liquid-dsp (hier vorhanden: 1.3.2),
PulseAudio-Werkzeug `parec`, Python 3, NumPy und Tkinter. Der hier getestete
Build verwendet liquid-dsp aus `/usr/local`. Es werden keine Systembibliotheken
installiert oder überschrieben. Voice-Codecs, Mono, WebSocket und libsoundio
sind für diesen RX-Prototyp nicht erforderlich.

Auf dem Entwicklungsrechner geprüft: Linux/Ubuntu-Werkzeuge, Python 3.12.3,
NumPy 1.26.4, Tk 8.6, SQLite 3.45.1, g++ 13.3.0 und liquid-dsp 1.3.2.
Der Build bindet direkt `-lliquid -lm`; die installierte liquid-Bibliothek
verwendet hier ihrerseits `libfftw3f.so.3`. Header und Bibliothek von liquid-dsp
müssen zur verwendeten API/ABI passen. Ein beliebiges neueres liquid-dsp ist
in diesem Projekt nicht als Ersatz getestet.

Das Makefile verwendet `-I/usr/local/include`, `-L/usr/local/lib` und einen
Runtime-Suchpfad `/usr/local/lib`. Bei Übertragung auf einen anderen Rechner
diese Pfade und die dortigen Bibliotheken prüfen. Die Python-Skripte brauchen
keine zusätzlichen pip-Pakete außer NumPy; SQLite, HTTP und ZIP sind Teil
der Standardbibliothek. Der Starter verwendet ausdrücklich `/usr/bin/python3`,
nicht automatisch die Python-Umgebung des Hauptprojekts.

Standardpfade für `data/` und `logs/` leiten sich vom Verzeichnis von
`rx_monitor.py` ab. Explizit übergebene relative Pfade (`--data-dir`, `--log`,
`--record`) beziehen sich dagegen auf das aktuelle Arbeitsverzeichnis des
Python-Prozesses. Der Shell-Starter wechselt zuerst in sein eigenes
Projektverzeichnis. Im Hauptprojekt vorzugsweise absolute Pfade übergeben.
`data/`, `logs/`, `build/`, `backups/` und Audioaufzeichnungen sind in
`.gitignore` als Laufzeit-/Build-Artefakte ausgenommen.

Alle CLI-Optionen sind über `rx_monitor.py --help` abrufbar:

| Option | Standard / Verhalten |
|---|---|
| `--source` | `fromQO100` |
| `--center-hz` | 1500; Python-CLI erlaubt ausschließlich Werte zwischen 1400 und 1700 Hz |
| `--manual` | AFC aus; ohne Option ist sie an |
| `--headless` | Empfang ohne Tkinter-Fenster |
| `--seconds` | 0 = unbegrenzt; Laufzeitbegrenzung nur im Headless-Modus |
| `--data-dir` | `data/` unter dem Projektverzeichnis für DB und Textausgaben |
| `--http-port` | 8765; 0 = dynamischer Test-Port |
| `--no-http` | HTTP aus; Textdateien/DB werden weiter aktualisiert |
| `--log` | Neue Datei `logs/rx-YYYYMMDD-HHMMSS.jsonl` |
| `--raw-frames` | Zusätzlich alle gültigen Funkframes ins Diagnose-Log schreiben |
| `--record` | Optionaler Pfad für unverarbeitetes Mono-Float32-Audio |
| `--import-logs` | Vor Live-Start Aufzeichnungen aus dem festen Projektordner `logs/rx-*.jsonl` importieren |
| `--replay-only` | Dieselben Aufzeichnungen importieren, Status ausgeben und ohne Audio/HTTP beenden |

Für einen isolierten Live-Kurztest, während der normale Monitor weiterläuft:

```sh
/usr/bin/python3 /Daten/funk/sat/mmdecoder/rx_monitor.py \
  --headless --seconds 10 --http-port 8766 \
  --data-dir /tmp/mmdecoder-integration-test \
  --log /tmp/mmdecoder-integration-test-rx.jsonl
```

Dabei bei wiederholter Nutzung desselben Testverzeichnisses beachten, dass
auch dessen Datenbank erhalten bleibt. Dieser Befehl startet eine zusätzliche
Audioaufnahme derselben Quelle; für reine Schnittstellenprüfungen reicht es,
die schon laufende Instanz per HTTP abzufragen.

```sh
make
make test
```

Tests erzeugen ausschließlich Audiodaten im Speicher. Geprüft werden bekannte
Nutzdaten, Frequenzversatz, absichtlich falsche CRC trotz gültiger FEC,
Rauschen/Stille, Lock-Verlust, unvollständige Samples und Trägermessung.
Die AFC-Tests prüfen zusätzlich Akquisition, Drift, Sprünge, Störtöne und
erneute Synchronisation. Die Sammler-Tests prüfen reale Protokollformate,
Duplikate, fehlende/vertauchte Blöcke, Neustarts, Datei-CRC, ZIP, neue Versionen,
Unicode/HTML-Text, Cursor, konkurrierende Zugriffe und die HTTP-Endpunkte.

`make test` benötigt keinen Satellitenempfang und verwendet temporäre
Datenverzeichnisse sowie einen dynamischen lokalen HTTP-Port. Es greift
nicht auf den produktiven Textbestand zu. Der Build erzeugt dabei zusätzlich
`build/generate_fixture`, einen ausschließlich für Offline-Audiodaten
verwendeten Testgenerator. Er ist kein Sender für die Soundkarte.

### Kurze Fehlerdiagnose für die Integration

| Beobachtung | Prüfen |
|---|---|
| HTTP-Verbindung abgelehnt | Monitor läuft noch nicht, wurde geschlossen oder anderer Port; gespeicherte Dateien können trotzdem vorhanden sein |
| Port belegt | Zuerst vorhandene Instanz verwenden; für isolierten Paralleltest anderen Port **und** eigenes Datenverzeichnis nehmen |
| `receiver.locked=false`, Texte vorhanden | Historie ist verfügbar, aktueller Empfang fehlt; nicht als frische Spots ausgeben |
| CW-Text leer | Leerer Kanal ist zulässig; es muss gerade eine CW-Meldung ausgestrahlt/empfangen werden |
| Bulletin wartet bei `received < expected` | Wiederholung abwarten; Blöcke bleiben gespeichert; kein Teil-Bulletin wird veröffentlicht |
| Import bringt keine neuen Texte | Möglicherweise bereits dedupliziert oder Aufzeichnung enthält nur Status; Rohframes gibt es nur aus früheren vollständigen Logs bzw. mit `--raw-frames` |
| `fromQO100` fehlt oder Audiozugriff scheitert | `pactl info` und `pactl list short sources` im selben Benutzerkontext prüfen |
| Signal sichtbar, keine gültigen Frames | Ganzes Signal und Hilfsträger im Audiofilter, korrekter USB-Audiokanal, 8APSK/7200; AFC-Zustand und Pegel prüfen |
| `libliquid.so` fehlt | Build-/Runtime-Pfade und `ldd /Daten/funk/sat/mmdecoder/build/rx_probe` prüfen |

Vor Weitergabe bzw. Zusammenführung des RX-Quellcodes `UPSTREAM.md`, `LICENSE`
und die Copyright-/Lizenzhinweise unter `vendor/hsmodem` mit berücksichtigen.
Das Hauptprojekt muss keine Dateien aus dem Original-HS-Modem nachinstallieren;
die hier benötigten Quellbausteine liegen bereits im Verzeichnis.

Original und Copyright-Hinweise siehe `UPSTREAM.md` und `LICENSE`.
