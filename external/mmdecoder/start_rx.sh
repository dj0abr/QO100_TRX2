#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
make
exec /usr/bin/python3 ./rx_monitor.py "$@"
