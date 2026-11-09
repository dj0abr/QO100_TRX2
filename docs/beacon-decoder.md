# PSK-Bake über RX4

Bauen: `./build_all.sh --install-deps` (installiert auch NumPy/SciPy).
Dann den bisherigen Stack beenden und starten:

```sh
./start_all.sh
```

RX3/FT8 und RX4/PSK laufen gleichzeitig. Unter **Setup → Monitoring-Empfänger**
steht **RX4 · PSK (Hz)** standardmäßig auf **10489748821 Hz**. Die Bake bei
10489,750 MHz liegt damit bei etwa 1179 Hz im USB-Audio. Änderungen dort speichern.
Die Frequenz ist unabhängig von RX1/RX2 und wird dauerhaft gespeichert.

Oben in der Zoom-Kachel **PSK-Monitor** auswählen; der Text nutzt die gesamte Kachel.
Die ersten vollständigen Texte erscheinen nach etwa 20–40 Sekunden. Beide
Textquellen werden auch bei ausgeblendeter Anzeige gleichzeitig empfangen.

Einzelstart: `./build/bin/qo100_beacon_decoder --port 40912`.
Der separate Python-Decoder empfängt RX4-Audio (QTR2-Stream **6**, 12 kHz Mono)
auf **127.0.0.1:40912** und antwortet mit UTF-8-Text an den Adapter-Port **40913**.
Der Browser empfängt `decoder_text` mit `stream:4` (logische Empfängernummer).

Überlappende Fenster umfassen höchstens 40 Sekunden. Bei Paketverlust oder
Abstimmung werden angefangene Blöcke verworfen; nur CRC-/RS-geprüfte Texte
werden ausgegeben. Kleine Fehler der Abtastfrequenz werden mitgesucht.

## Verifizierte Protokollparameter

- BPSK, differenziell/Manchester: 400 Bit/s, 800 Chips/s.
- Uncodiert: Sync `39 15 ED 30`, 512 Datenbytes, CRC-16 `0x1021`,
  Start `0xffff`, MSB-first, keine Reflexion/kein finaler XOR, CRC High-Byte zuerst.
  CRC über Daten und CRC ergibt null; Sync zählt nicht mit.
- AO-40 FEC: 5200 Bits, 65 verteilte Syncbits an Position `80*j`.
  Sync: `11111110000111011110010110010010000001000100110001011101011011000`.
  Deinterleaver `d[j]=b[80*(j%65)+j//65]`, Nutzbereich `d[65:5197]`;
  drei unbenutzte Bits am Ende.
- Viterbi: Rate 1/2, K=7, Polynome `[79,-109]` (Register nach links,
  neues Bit am LSB, zweiter Ausgang invertiert), Start/Endzustand 0,
  sechs Tailbits. 320 Bytes nach Entfernen der Tailbits.
- CCSDS-Descrambler: `x^8+x^7+x^5+x^3+1`, Start `0xff` pro Rahmen,
  Beginn `FF 48 0E C0 9A 0D 70 BC`.
- RS(160,128), zwei verschachtelte Wörter (gerade/ungerade Bytes),
  verkürztes RS(255,223) mit 95 Nullbytes, GF-Polynom `0x187`,
  fcr=112, prim=11, 32 Paritätssymbole, konventionelle Polynomialbasis.
  Bis 16 Bytefehler je Wort. Ergebnis 256 Datenbytes, keine zusätzliche CRC.

Die Parameter wurden mit der beigefügten Aufnahme und der gr-satellites-
Referenz geprüft; die unterschiedliche Schreibweise der Faltungspolynome
in historischen Dokumenten beruht auf der Register-/Bitreihenfolge.

Quellen/Lizenz: [vendorte Referenz und Herkunft](../external/qo100-beacon-decoder/ORIGIN.md),
[AMSAT P3](https://amsat-dl.org/wp-content/uploads/2019/01/tlmspec.txt),
[AO-40 FEC](https://www.ka9q.net/papers/ao40tlm.html).
