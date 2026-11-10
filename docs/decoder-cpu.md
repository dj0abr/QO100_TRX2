# FT8 und PSK als begrenzte Hintergrundaufgaben

Audio und Multimedia-RX5 haben Vorrang. Die beiden weniger wichtigen
Monitoringdecoder RX3/FT8 und RX4/PSK erhalten standardmäßig je ein Budget von
**10 % CPU-Zeit eines einzelnen Kerns**. RX5 wird nicht begrenzt.

```ini
# config/backend.conf; anschließend den vollständigen Stack neu starten
rx3_decoder_cpu_percent = 10
rx4_decoder_cpu_percent = 10
```

Zulässig sind 1 bis 10. Zum Ausschalten gelten weiterhin die separaten
`rx3_decoder_enabled`/`rx4_decoder_enabled` beziehungsweise der Decoder-
Hauptschalter. CPU-Budgets gehören nicht zu den gespeicherten GUI-Einstellungen.
`start_all.sh` übernimmt sie auch nach einem Decoder-Neustart. Beim direkten
Aufruf der beiden Empfänger ist das Standardbudget ebenfalls 10; der CLI-
Parameter `--cpu-percent` erlaubt für explizite Offline-Messungen bis 100.

## Priorität und CPU-Zeit

Die Empfangsprogramme starten vor ihren DSP-Threads einen kleinen lokalen
Prozesswächter. Er zählt die CPU-Zeit aller Threads, separat gestarteter Solver
und sich selbst. Prozess-CPU-Uhren liefern feinere Werte als `/proc`-Tickzähler.
Die Prozesssuche berücksichtigt Kinder aus beliebigen Threads, weil der
FT8-Rechenkern aus dem Decode-Worker gestartet wird und eine eigene Prozessgruppe
besitzt. PID-Dateideskriptoren beziehungsweise Startzeitprüfung schützen gegen
PID-Wiederverwendung. Das benötigt keine Root-Rechte, cgroup-Delegation,
`cpulimit`-Installation oder festen Kernbindungen.

Ein kleines CPU-Zeitguthaben entspricht bei 10 % etwa zwei Millisekunden.
Unverbrauchte Zeit sammelt sich nicht zu einem späteren langen Vollastlauf an.
Ist das Guthaben verbraucht, werden die eigenen Rechenprozesse kurz angehalten
und nach der Pause fortgesetzt. Scheduler- und Messgranularität werden über die
folgenden Pausen ausgeglichen; eine kleine Reserve ist eingerechnet.
Bei CPU-Aktivität wird engmaschig geprüft, im Leerlauf deutlich seltener.

Zusätzlich laufen die Worker und ihre Solver mit **nice 19** und **SCHED_IDLE**.
Normale Backend-, Audio- und RX5-Prozesse werden ihnen bei konkurrierender Last
vorgezogen. Falls die Hintergrund-Schedulingklasse nicht verfügbar ist, bleiben
nice 19 und die CPU-Budgetierung aktiv. Die Überwachung selbst muss kurzzeitig
laufen können, um angehaltene Prozesse wieder freizugeben.

SIGINT/SIGTERM setzen die angehaltenen eigenen Prozesse zuerst fort und beenden
sie anschließend. Verwaiste Solver werden aufgeräumt; Parent-Death-Signale
beenden auch angehaltene native Worker/Solver bei abruptem Ende ihres Wächters
beziehungsweise Empfängers. Nur eigene Prozesse werden gesteuert.

In `htop` erscheinen pro Dienst ein Wächter und ein gleichnamiger
Empfangsprozess, bei FT8 zusätzlich zeitweise `ft8-decoder`. Die Quote gilt
für deren Summe einschließlich aller Threads und der Überwachung.

## Effizienter PSK-Decoder in C++

Die frühere Python-Fassung bearbeitete alle zehn Sekunden bis zu 40 Sekunden
Audio erneut: Hilbert-FFT, mehrere volle FIR-Vor-/Rückläufe, Taktsuche und
insbesondere Viterbi-Schleifen mit Python-/NumPy-Skalaroperationen. Das erklärt
hohe Puls- statt gleichmäßiger Streaminglast.

Das produktive `qo100_beacon_decoder` ist nun ein natives Programm. Es mischt
und FIR-dezimiert auf 4 kHz, führt darauf Träger-/Phasenschätzung und Taktsuche
aus und dekodiert CRC/AO-40-FEC vollständig in C++. Nur bei tiefen Audiotönen
unter 800 Hz wird vorab ein analytisches Signal zur Bildunterdrückung berechnet.
Viterbi verwendet vorberechnete Zweigmetriken und kompakte Vorgängerpfade;
Reed-Solomon nutzt feste GF-Tabellen und prüft die korrigierten Syndrome.
Sechs CRC- und fünf FEC-Nutzlasten der mitgelieferten Aufnahme bleiben bytegleich
zur bisherigen Referenz, auch nach Umsetzung auf den normalen RX4-Audioton.
Unabhängige Testvektoren prüfen Viterbi, Descrambler und bis 16 korrigierte
Bytefehler je RS-Wort. Die vendorten Referenzquellen bleiben unverändert.

Es gibt maximal einen laufenden und einen ersetzbaren neuesten Fensterauftrag.
Verspätete Ergebnisse nach Abstimmung/Sessionwechsel werden verworfen; CRC-/RS-
geprüfte Ausgaben werden über Fenstergrenzen dedupliziert. Wird ein Fenster zu
langsam verarbeitet, ersetzt das aktuelle das ältere wartende Fenster.

## FT8

FT8 verwendete bereits einen nativen C++-/Fortran-Kern aus WSJT-X. Dieser bleibt
erhalten. Für den Monitoringdienst wird die schnelle Dekodierstufe `--depth 1`
verwendet. Es gibt maximal den laufenden und den neuesten wartenden UTC-Slot;
ältere wartende Slots werden ersetzt. Die vorhandene Zeitgrenze von 45 Sekunden
verstrichener Zeit bleibt bestehen. Schwache oder zu spät dekodierte Meldungen
dürfen entfallen, damit der Funkbetrieb ungestört bleibt. Der eigenständige
Offline-Rechenkern behält seine bisherigen Defaults.

## Prüfung und Messung

Abschließend **49/49 Tests bestanden** (79,32 s). Der künstliche Belastungstest
maß bei 10-%-Budget jeweils etwa **9,72 % eines Kerns** einschließlich aller
Rechenprozesse und des Wächters. Bei 5-%-Budget waren es 4,89 %, im Leerlauf
etwa 0,56 %. Normales und abruptes Beenden hinterließ keine laufenden oder
angehaltenen Worker/Solver.

- `beacon_recording`: echte Aufnahme bei 8/12 kHz und tiefem Audioton, bytegleiche
  CRC-/FEC-Ausgaben gegen die bisherige Python-Referenz.
- `beacon_cpp_fec`: unabhängige gr-satellites-Vektoren, RS-Fehlerkorrektur und CRC.
- `decoder_cpu_budget`: künstliche Dauerlast mit mehreren Threads und einem
  Solver in separater Prozessgruppe, aus einem Worker-Thread gestartet;
  gesamte CPU-Zeit inklusive Wächter, Leerlauf sowie normales/abruptes Beenden.
- `beacon_udp_receiver`, `ft8_udp_receiver`, `parallel_monitor_recordings`:
  reale Signale über UDP/Webbridge zu Bediener und Zuschauer, während RX1 läuft.
- Die Supervisor-Tests prüfen auch die konfigurierten Budgets nach Neustarts.

Abschließende Messung auf dem x86-Entwicklungsrechner, mit derselben
vollständigen PSK-Aufnahme:

| Verarbeitung | CPU-Zeit | Verstrichene Zeit | Nutzlasten |
| --- | ---: | ---: | ---: |
| Python-Referenz, nur DSP (8 kHz) | 3,667 s | 3,667 s | 6 CRC + 5 FEC |
| C++, ganzer Prozessbaum, 10-%-Budget (8 kHz) | 0,163 s | 1,650 s | bytegleich |
| C++, normaler RX4-Audioton/12 kHz, 10-%-Budget | 0,170 s | 1,761 s | bytegleich |

Die native Fassung benötigt in diesem Vergleich etwa **22-mal weniger CPU-Zeit**
(etwa 95,5 % weniger). Ihr Wert enthält zusätzlich Dateieingabe, Programmstart
und Überwachung. Das ist keine Raspi-Messung. Der separate Belastungstest
bestätigte das 10-%-Budget auch für mehrere Threads plus separaten Solver;
die CPU-Zeit aller Prozesse einschließlich Überwachung wird dabei summiert.

Messprotokolle dieser Sitzung liegen unter `/tmp/qo100-decoder-budget/`.
Die endgültige Kernlast und Audiostabilität müssen im vollständigen Betrieb
auf dem Raspberry Pi geprüft werden. Der laufende Pluto wurde bei diesen
Aufnahme-/Softwaretests nicht angesprochen.
