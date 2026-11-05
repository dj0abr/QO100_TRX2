# Embedded Pluto driver

- Repository: https://github.com/dj0abr/Pluto
- Branch at import: `main`
- Commit: `8a65dd0a3cc33b11a675b2e27d2aa479d9e81310`
- Commit date: 2022-04-04
- Original sources and LICENSE are retained in `external/Pluto/`.
- The upstream executable is retained as part of the copy but is never executed
  by the backend or used by the CMake build.

Local integration changes are recorded in `pluto-integration.patch`:

1. Allow CMake to override UDP ports and buffer sizes via preprocessor defines.
   QO100 build: input 40808, output 40809, status 40810; 24000-sample buffers.
2. Add `RX_ONLY` configuration. When enabled, skip TX setup/buffers, UDP TX input,
   interpolation and the FIFO status timer.
   No TX samples or marker are generated in this mode.
3. Enforce the local-only UDP link in the QO100 build: loopback bind, sender check
   and `127.0.0.1` destination check. Correct the socket-option integer type.
4. Fail on missing channels/buffers/rate configuration or RX refill errors;
   never forward stale data after a failed refill. Forward actual refill length.
5. Flush driver stdout by line for runtime diagnostic logs.
6. Align the copied example config with QO100_TRX2: RX 739.750 MHz,
   TX 1290.250 MHz, CROSSBANDREPEATER 0 and RX_ONLY 1.

The existing RX sample conversion and factor-two sample selection are unchanged.
The original TX processing remains available when `TX_RAW_IQ` is absent or zero.
The backend now explicitly selects `TX_RAW_IQ 1`: raw hardware-rate I16/Q16,
with PTXI block headers, no interpolation, marker or scaling. The raw-mode FIFO
has eight usable 20 ms slots; PTXS feedback is sent after every hardware push.
Empty FIFO produces zero samples and push errors fail the driver. The UDP
receive buffer was increased to accommodate bursts of several TX blocks.
Generated TX bandwidth is 600 kHz. Backend attenuation is mapped to the original
driver's `TX_GAIN - 10` hardwaregain convention. No PTT command is provided by
upstream; the GUI gates audio, while backend and driver continue zero IQ.

Both backend and driver now compile against the bundled liquid-dsp 1.8.3
header and link its local static archive. The copied upstream 1.3.2 header is
retained but unused. Legacy TX complex sample access uses std::complex getters
and setters, matching the shared C++ API of the new header.

Copyright/license notices in the imported files remain unchanged, including
the differing GPL notices already present in upstream helper files.
