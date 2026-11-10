# Embedded Pluto driver

Driver sources imported from [dj0abr/Pluto](https://github.com/dj0abr/Pluto)
at commit `8a65dd0a3cc33b11a675b2e27d2aa479d9e81310` (2022-04-04).
Original copyright notices and [LICENSE](LICENSE) are retained.

The transceiver builds this driver from source through the root CMake project:

```sh
# Run from the QO100_TRX2 project root.
./build_all.sh
```

The backend starts `build/bin/pluto` and creates its private configuration from
`config/backend.conf` and the saved settings. The driver uses the project's
pinned liquid-dsp header and static library. Its UDP links are local to the
backend computer.

The old standalone executable, build/install/start helpers, example configuration
and unused liquid-dsp 1.3.2 header are omitted from this integrated source copy.
See [integration notes](../../docs/pluto-upstream.md) for protocol details and
the history of local changes. Normal installation and operation are described
in the [project README](../../README.md).
