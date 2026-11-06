# Herkunft

Kopiert aus `/Daten/funk/kwWebRXpp/src/ft8`, einschließlich der vendorten
WSJT-X-Quellen, GPL-3.0-Lizenz, Prüfsummen und Original-Testaufnahme.
Es werden keine Dateien aus kwWebRXpp zur Build- oder Laufzeit benötigt.
Der Inhalt von `vendor/wsjtx` bleibt unverändert; `tools/verify_vendor.py`
prüft dessen Herkunft und die Testaufnahme.

`decoder/FT8DecoderProcess.{h,cpp}` stammt aus demselben kwWebRXpp-Verzeichnis.
Lokale Anpassungen: 64-Bit-HF-Frequenzen und Generationskennung zum Verwerfen
überholter Ergebnisse. Der IQ-Empfänger wird nicht übernommen: RX2 liefert
bereits demoduliertes 12-kHz-Audio. Die UDP-/UTC-Slot-Anbindung ist projektspezifisch.

SHA-256 der kopierten Integrationsvorlagen vor Anpassung:

```json
{
  "FT8DecoderProcess.cpp": "a7b194ba18be2f48860016a62d3c81ba1159ffc8a127b3af3d33c12968e3cb08",
  "FT8DecoderProcess.h": "5aebe1b29d7b846d3748cadcac3ff36e18b6a6b3143d10931c72be8f3800d636",
  "ft8/CMakeLists.txt": "85b684568c20c2faa0d69b4b69de0040d8687fd7c20afe060532c3a7fc643623"
}
```
