# DSP-Verteilung für einen Vierkern-SBC

Die Thread-Aufteilung ist standardmäßig aktiv; einzelne Demodulatoren und
externe Decoder lassen sich für Lastmessungen abschalten (siehe unten).

| Thread | Arbeit |
| --- | --- |
| Hauptthread | IQ-Eingang, gemeinsame FFT, Steuerung, Metadaten, UDP-Ausgabe |
| `qo100-rx1` | RX1 einschließlich Speex-NR |
| `qo100-rx2-3` | RX2 und FT8-Monitor RX3 |
| `qo100-rx4-5` | PSK-Monitor RX4 und Multimedia-RX5 |
| `qo100-tx` | TX1/TX2, Summierung, Pegelreserve und IQ-Paketierung |

Linux verteilt diese fünf Threads auf die vier verfügbaren Kerne. Es gibt
keine feste Kernbindung, Echtzeitpriorität oder aktive Warteschleife. Der kleine
FFT-/Kommunikationsanteil rechtfertigt keinen fest reservierten Kern.

## Manuelle Lastschalter in backend.conf

Alle Schalter akzeptieren ausschließlich `1` (ein) oder `0` (aus). Ohne Eintrag
und in der mitgelieferten Konfiguration ist alles eingeschaltet. Änderungen
wirken nach einem vollständigen Neustart mit `./start_all.sh`; sie werden nicht
in `backend.settings` gespeichert und nicht vom WebGUI überschrieben.

| Schalter | Wirkung bei 0 |
| --- | --- |
| `rx2_enabled` | RX2-Demodulator aus |
| `rx3_enabled` | RX3-Demodulator und FT8-Decoder aus |
| `rx4_enabled` | RX4-Demodulator und PSK-Decoder aus |
| `rx5_enabled` | RX5-Demodulator und MM-Decoder aus |
| `external_decoders_enabled` | Alle drei externen Decoder aus; aktive RX rechnen weiter |
| `rx3_decoder_enabled` | Nur FT8-Decoder aus |
| `rx4_decoder_enabled` | Nur PSK-Decoder aus |
| `rx5_decoder_enabled` | Nur MM-Decoder aus |

Abgeschaltete Demodulatoren werden nicht konstruiert und verarbeiten keine
Samples. Einzeln abgeschaltete Kanäle werden in ihrer Worker-Gruppe übersprungen;
eine vollständig abgeschaltete Gruppe startet keinen Thread. RX1, die gemeinsame
FFT und alle drei Wasserfälle bleiben aktiv. Die TX-Ausgabe bleibt getaktet;
bei fehlendem Audio werden Modulation und IQ-Rechenschleifen übersprungen. RX2 kann
also weiterhin im Zoom angezeigt und abgestimmt werden, obwohl seine
Audiodemodulation ausgeschaltet ist. Frequenzmetadaten und Steuerantworten
bleiben verfügbar, damit das WebGUI auch mit abgeschalteten RX startet.

Mit `digital_auto_idle=1` pausiert RX2 zusätzlich automatisch ohne externe
Aufnahme oder Browser-Audio; TX2 ohne externe Wiedergabe. Die virtuellen Geräte
bleiben sichtbar. Details: [digital-audio.md](digital-audio.md#automatische-ruhefunktion).

Für FT8 und PSK gelten zusätzlich `rx3_decoder_cpu_percent = 10` und
`rx4_decoder_cpu_percent = 10` (jeweils 1–10 % eines Kerns). Die Begrenzung
umfasst die jeweilige Überwachung, Threads und Unterprozesse; RX5 bleibt
unbegrenzt. Der PSK-Decoder läuft jetzt in C++, FT8 verwendet seine schnelle
native Dekodierstufe. [CPU-Budgets und Messwerte](decoder-cpu.md).

`start_all.sh` liest dieselbe `backend.conf`: Ein Decoder startet nur, wenn
Hauptschalter, Einzelschalter, zugehöriger RX und sein Portpaar eingeschaltet
sind. Abgeschaltete Decoder fehlen auch in der Neustartüberwachung; ihre
Programme müssen nicht vorhanden und ihre Ports nicht frei sein. Die
entsprechenden UDP-/MM-HTTP-Verbindungen der Webbridge werden ebenfalls
abgeschaltet. `webbridge.conf` muss dafür nicht bearbeitet werden. Bereits
separat gestartete Decoderprozesse verwaltet dieser Schalter nicht.

Für den Lastvergleich auf demselben Raspi eignen sich diese Schritte:

1. Alles auf `1`: kompletter Betrieb.
2. Nur `external_decoders_enabled = 0`: fünf RX ohne externe Decoder.
3. Zusätzlich `rx2_enabled` bis `rx5_enabled` auf `0`: RX1 mit gemeinsamer FFT
   und TX-Verarbeitung. Ein zusätzlicher TX-Anteil bleibt also in dieser Messung.
4. Einzelne RX beziehungsweise Decoder wieder einschalten, um ihren Anteil
   getrennt zu bestimmen. Nach jeder Änderung den vollständigen Stack neu starten.

Den virtuellen RX2/TX2-Audioadapter kann man zusätzlich mit dem bereits
vorhandenen `./start_all.sh --no-digital-audio` auslassen. Er ist kein Decoder
und wird vom Decoder-Hauptschalter nicht abgeschaltet.

Für vergleichbare Messungen denselben Betriebszustand (PTT aus, gleiche NR,
Browser/Decoderzustände) verwenden und pro Fall einige Zeit einlaufen lassen.
Dabei sowohl die gesamte CPU-Zeit aller Prozesse als auch die einzelnen Threads
in `htop` betrachten. Ein weiteres Backend darf nicht gleichzeitig auf denselben
Pluto zugreifen.

`rx_enable_switches` prüft reale PCM-Ausgabe und Threadanzahl bei gemischten
RX-Schaltern, weiterlaufende Wasserfälle sowie Abstimm-/Settings-Antworten.
`load_switches_supervisor` prüft Haupt-/Einzelschalter, die automatische
RX-Abhängigkeit, fehlende abgeschaltete Programme, belegte abgeschaltete Ports,
WebGUI-Bereitschaft, Neustart aktiver Decoder und vollständiges Aufräumen.

## Übergaben und Steuerung

`RxWorkers` verteilt denselben unveränderten, einmal dekodierten IQ-Block auf
die benötigten dauerhaften Worker. Eine vollständig abgeschaltete RX-Gruppe
erzeugt keinen Thread. Jeder aktive Empfänger behält eigene Mischer-, Dezimator-,
Resampler-, Filter- und NR-Zustände. Während die Worker rechnen, berechnet der
Hauptthread die FFT. Anschließend wartet er auf alle drei Worker, sendet deren
getrennt gepufferte PCM-Pakete und gibt erst dann den IQ-Puffer zur nächsten
Befüllung frei. Auch eine Ausnahme im Hauptthread oder Worker wartet alle
laufenden RX-Aufträge ab. Es wird kein zusätzlicher Eingangsblock gesammelt.

Der TX-Worker startet nach der GUI-Steuerung mit kopierten Routing-/Aktivitäts-
Flags und bereitet anhand der Pluto-FIFO-Rückmeldung höchstens vier TX-Blöcke
vor. Währenddessen kann die RX-Verarbeitung laufen. Vor weiteren Befehlen,
TX-Zustandsabfragen, Hardware-/LO-Übernahmen und Neustarts wartet der Hauptthread
auch TX ab. Nur der Hauptthread greift auf UDP-Sockets, Teilnehmer, Epochen,
Frequenzmetadaten und Fehlerzähler zu. Es gibt pro Worker nur einen Auftrag und
keine wachsende Warteschlange.

`DspWorker` reicht Ausnahmen an den Hauptthread weiter und beendet seinen
Thread erst nach Abschluss des letzten Auftrags. SIGINT/SIGTERM dürfen jeden
Thread erreichen: Die Abbruchanforderung verwendet deshalb ein garantiert
lockfreies Atomic. Beim Pluto-Neustart werden Pfadzeiger/Argumente vor `fork()`
aufgelöst; der Kindprozess verwendet danach bis `execv()`/`_exit()` keine
C++-Bibliotheksoperationen. Der bestehende Schutz gegen verwaiste Treiber bleibt.

## Messung auf dem Entwicklungsrechner

`taskset -c 0,1,3,6 ./build/bin/benchmark_dsp` beschränkt den Test auf vier
physische x86-Kerne. Verglichen wurden fünf RX einschließlich NR sowie die
gemeinsame 24.000-Punkte-FFT, mit identischen Eingangsdaten, Fragmenten,
Abstimm-/Filter-/NR-Änderungen und Ausgangsprüfsummen. Die Werte sind Mediane
von sieben Läufen mit jeweils vier Sekunden Signaldaten; ohne Pluto, UDP,
Decodierung externer Digitalprogramme oder GUI.

| RX + FFT pro Sekunde Signaldaten | Serielle Blöcke | Drei RX-Worker |
| --- | ---: | ---: |
| Verstrichene Zeit | 98,34 ms | 42,32 ms |
| Gesamte CPU-Zeit | 98,33 ms | 108,88 ms |
| CPU-Zeit Hauptthread | 98,33 ms | 8,13 ms |

Die Verarbeitung erreicht damit etwa **2,32-fachen Durchsatz**. Die gesamte
CPU-Arbeit steigt wegen Synchronisation um etwa **11 %**; dies ist keine
entsprechende Senkung der Gesamtlast. Die bisherigen Einsparungen durch
FFT-Zusammenlegung und Blockmischer bleiben erhalten. Raspi-Durchsatz,
maximale Kernlast und Scheduling mit Pluto-Treiber und Decodern müssen auf
dem Zielgerät gemessen werden.

## Funktionsprüfung und Einordnung des Live-Tests

Die vollständige CTest-Suite für die manuellen Lastschalter bestand mit
**47/47 Tests**, einschließlich gemischter RX-/Decoder-Schalter und der
simulierten Hardware-/TX-Tests in einem isolierten Netzwerk-Namespace.
Es wurde kein weiterer Live-Zugriff auf den vom Anwender verwendeten Pluto
vorgenommen. Protokolle stehen in `/tmp/qo100-load-switches/ctest.log`.
Der vollständige Browser-Test bestand bei der vorangehenden Thread-Umstellung.

- `dsp_worker_equivalence`: fünf PCM-Streams und drei Wasserfälle exakt gleich
  zur seriellen Blockverarbeitung, auch bei Abstimmung, Filtern, NR, Resets und
  unregelmäßigen Blöcken; unveränderte Eingangsdaten, Ausnahmen und Shutdown.
- `dsp_worker_shutdown`: SIGINT und SIGTERM gezielt an jeden der fünf Threads
  des laufenden Backends; ordnungsgemäßes Beenden und Aufräumen.
- Integrationstests prüfen weiterhin RX-Steuerung, TX1/TX2, PTT, Testschleifen,
  Hardware-Neustarts, Webbridge und Protokolle mit simuliertem Pluto.
- Vollständiger Browser-Test bestanden, einschließlich Audio, RX2/FT8,
  Einstellungen, PTT-Bedienung, Neustart und Trennung der Verbindung.

Der Live-Test am Pluto `ip:192.168.10.25` am 10.11.2026 zeigte zeitweise nur
300 statt 600 kSamples/s und scheiterte deshalb an der Sampleratenprüfung.
**Der Anwender hat die Ursache inzwischen geklärt:** Auf einem anderen Raspberry
Pi lief noch ein weiteres Backend mit Zugriff auf denselben Pluto. Dieser
zweite Rechner war vom Entwicklungsrechner aus nicht sichtbar.
Die damalige Schlussfolgerung, dass der vorhandene Treiber die Datenrate
begrenzt, war damit nicht belegt und ist korrigiert. Die beobachteten I/O-
Wartezeiten entstanden während des konkurrierenden Gerätezugriffs; daraus
wird keine allgemeine Treiberbegrenzung abgeleitet. Die Treiberverarbeitung
wurde nicht verändert; PTT blieb bei allen damaligen Live-Diagnosen aus.
Der Anwender bestätigt anschließend funktionierenden Betrieb und eine gute
Lastverteilung auf alle vier Raspi-Kerne.

Eine abschließende Live-Diagnose bestätigte die Aktivität aller fünf Threads
und die Ausgabe von 120 Fullband-Zeilen, 118 Zoom-Zeilen sowie je 297 RX1-/RX2-
Audiopaketen bei der eingeschränkten Treiberrate. Software-Sampleindizes waren
fortlaufend, FFT-Pakete vollständig und das Aufräumen des Treibers erfolgreich.
Die gemessenen Threadlasten auf dem Entwicklungsrechner waren etwa 1,27 %
(Hauptthread), 1,45 % (TX), 1,81 % (RX1), 2,89 % (RX2/RX3) und 3,07 % (RX4/RX5)
eines CPU-Kerns. Das sind keine Raspi-Werte und kein Vollraten-Leistungsnachweis.

Zusätzliche Live-Diagnosen protokollieren die tatsächliche Rate, Ausgabeformate,
PCM-Sampleindizes und Threadlast, ersetzen aber keinen bestandenen Vollratentest.
Ein fairer Live-Geschwindigkeitsvergleich ist bei wechselnder Eingangsrate nicht
möglich. Der strenge Projekttest wurde nicht gelockert. ThreadSanitizer konnte
auf diesem Host wegen `unexpected memory mapping` nicht starten und liefert
hier keine zusätzliche Prüfung.

Messprotokolle und temporäre Diagnoseprogramme dieser Sitzung liegen unter
`/tmp/qo100-four-core/` (insbesondere `benchmark.log`, `ctest-final.log`,
`browser.log`, `live.log`, `check-live.log` und `diagnostic/`).
