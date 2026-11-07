#!/usr/bin/env bash
# Foreground supervisor: one terminal and Ctrl+C for the complete transceiver.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bin_dir="$project_dir/build/bin"
backend_config="$project_dir/config/backend.conf"
webbridge_config="$project_dir/config/webbridge.conf"
source=pluto

usage() {
    echo "Aufruf: ./start_all.sh [--source pluto|synthetic]"
    echo "                      [--backend-config DATEI] [--webbridge-config DATEI]"
    echo "Laeuft im Vordergrund; Strg+C beendet alle gestarteten Programme."
}
while (($#)); do
    case "$1" in
        --help) usage; exit 0 ;;
        --source|--backend-config|--webbridge-config)
            if (($# < 2)); then usage >&2; exit 2; fi
            case "$1" in
                --source) source="$2" ;;
                --backend-config) backend_config="$2" ;;
                --webbridge-config) webbridge_config="$2" ;;
            esac
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ $source != pluto && $source != synthetic ]]; then usage >&2; exit 2; fi
for program in qo100_backend pluto qo100_webbridge qo100_ft8_decoder ft8-decoder; do
    if [[ ! -x "$bin_dir/$program" ]]; then
        echo "Fehlt: $bin_dir/$program. Bitte zuerst ./build_all.sh ausfuehren." >&2
        exit 1
    fi
done

# Validate the local links before starting anything, including occupied ports.
# Python uses only its standard library; no GUI/audio packages are needed here.
connection="$(python3 - "$backend_config" "$webbridge_config" "$source" <<'PY'
from pathlib import Path
import socket
import sys

def config(filename):
    values = {}
    for raw in Path(filename).read_text().splitlines():
        line = raw.partition('#')[0].strip()
        if not line:
            continue
        key, sep, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if not sep or not key or key in values:
            raise ValueError(f'Ungueltige/doppelte Einstellung in {filename}: {line}')
        values[key] = value
    return values

try:
    backend, bridge = map(config, sys.argv[1:3])
    gui_port = int(backend.get('gui_port', 40900))
    if gui_port != int(bridge.get('backend_port', 40900)):
        raise ValueError('gui_port und backend_port muessen uebereinstimmen.')
    gui_bind = backend.get('gui_bind', '0.0.0.0')
    if gui_bind not in ('0.0.0.0', '127.0.0.1'):
        raise ValueError('gui_bind muss fuer den lokalen Webadapter 0.0.0.0 oder 127.0.0.1 sein.')
    audio, text = (int(bridge.get(k, 0)) for k in ('decoder_audio_port', 'decoder_text_port'))
    listen, http = bridge.get('listen', '0.0.0.0'), int(bridge.get('port', 8080))
    if not all(1 <= p <= 65535 for p in (gui_port, audio, text, http)):
        raise ValueError('GUI-, HTTP- und beide Decoder-Ports muessen 1..65535 sein.')
    if len({gui_port, audio, text}) != 3:
        raise ValueError('GUI- und Decoder-Ports muessen verschieden sein.')
    addresses = [(socket.SOCK_DGRAM, gui_bind, gui_port),
                 (socket.SOCK_DGRAM, '127.0.0.1', audio),
                 (socket.SOCK_DGRAM, '127.0.0.1', text),
                 (socket.SOCK_STREAM, listen, http)]
    if sys.argv[3] == 'pluto':
        addresses += [(socket.SOCK_DGRAM, '127.0.0.1', p) for p in (40808, 40809, 40810)]
    sockets = []
    for kind, host, port in addresses:
        s = socket.socket(socket.AF_INET, kind)
        sockets.append(s)
        if kind == socket.SOCK_STREAM:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
        except OSError as error:
            protocol = 'TCP' if kind == socket.SOCK_STREAM else 'UDP'
            raise ValueError(f'{protocol} {host}:{port} nicht verfuegbar: {error}. '
                             'Bereits laufende Dienste zuerst beenden.') from error
    print('127.0.0.1' if listen == '0.0.0.0' else listen, http, audio)
except (OSError, ValueError) as error:
    sys.exit(f'Start abgebrochen: {error}')
PY
)"
read -r http_host http_port decoder_port <<< "$connection"

log_dir="$project_dir/build/run/stack-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p -- "$log_dir"
pids=()
cleanup() {
    trap - EXIT INT TERM HUP
    if ((${#pids[@]})); then
        echo "Stoppe Webadapter, Decoder und Backend ..." >&2
        kill -TERM "${pids[@]}" 2>/dev/null || true
        local deadline=$((SECONDS + 8)) alive pid
        while ((SECONDS < deadline)); do
            alive=0
            for pid in "${pids[@]}"; do
                if kill -0 "$pid" 2>/dev/null; then alive=1; fi
            done
            if ((alive == 0)); then break; fi
            sleep 0.1
        done
        for pid in "${pids[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid" 2>/dev/null || true; fi
            wait "$pid" 2>/dev/null || true
        done
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
launch() {
    local name="$1"; shift
    "$@" >"$log_dir/$name.log" 2>&1 &
    pids+=("$!")
}
show_logs() {
    local name
    for name in backend decoder webbridge; do
        echo "$name ($log_dir/$name.log):" >&2
        tail -n 12 "$log_dir/$name.log" >&2
    done
}

launch backend "$bin_dir/qo100_backend" --config "$backend_config" --source "$source"
launch decoder "$bin_dir/qo100_ft8_decoder" --port "$decoder_port"
launch webbridge "$bin_dir/qo100_webbridge" --config "$webbridge_config"
echo "Starte QO100; Logs: $log_dir"

deadline=$((SECONDS + 15))
while :; do
    for pid in "${pids[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Ein Dienst wurde beim Start beendet." >&2
            show_logs
            exit 1
        fi
    done
    if grep -q '^Source=' "$log_dir/backend.log" && \
       grep -q '^FT8 receiver: RX2' "$log_dir/decoder.log" && \
       grep -q '^HTTP/WS' "$log_dir/webbridge.log"; then
        if python3 - "$http_host" "$http_port" <<'PY'
import json
import sys
import urllib.request
try:
    client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with client.open(f'http://{sys.argv[1]}:{sys.argv[2]}/health', timeout=.5) as reply:
        assert json.load(reply).get('service') == 'qo100_webbridge'
except Exception:
    sys.exit(1)
PY
        then break; fi
    fi
    if ((SECONDS >= deadline)); then
        echo "Start nach 15 Sekunden nicht bestaetigt." >&2
        show_logs
        exit 1
    fi
    sleep 0.1
done

echo "Backend, WebSocket-Adapter und FT8-Decoder laufen."
echo "Browser auf diesem Rechner: http://$http_host:$http_port/"
echo "Im LAN: http://<IP-dieses-Rechners>:$http_port/ oder die OPNsense-HTTPS-Adresse."
echo "Dieses Terminal offen lassen. Strg+C beendet alle drei Programme samt Pluto-Treiber."

# If one service exits, stop the others instead of leaving a partial stack.
if wait -n "${pids[@]}"; then status=1; else status=$?; fi
echo "Ein Dienst wurde beendet (Status $status). Logs: $log_dir" >&2
show_logs
exit "$status"
