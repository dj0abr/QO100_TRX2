#!/usr/bin/env bash
# Foreground supervisor: one terminal and Ctrl+C for the complete transceiver.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bin_dir="$project_dir/build/bin"
backend_config="$project_dir/config/backend.conf"
webbridge_config="$project_dir/config/webbridge.conf"
source=pluto
digital_audio=1

usage() {
    echo "Aufruf: ./start_all.sh [--source pluto|synthetic]"
    echo "                      [--backend-config DATEI] [--webbridge-config DATEI]"
    echo "                      [--no-digital-audio]"
    echo "Laeuft im Vordergrund; Strg+C beendet alle gestarteten Programme."
}
while (($#)); do
    case "$1" in
        --help) usage; exit 0 ;;
        --no-digital-audio) digital_audio=0; shift ;;
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
for program in qo100_backend pluto qo100_webbridge qo100_ft8_decoder ft8-decoder qo100_beacon_decoder; do
    if [[ ! -x "$bin_dir/$program" ]]; then
        echo "Fehlt: $bin_dir/$program. Bitte zuerst ./build_all.sh ausfuehren." >&2
        exit 1
    fi
done

# Validate the local links before starting anything, including occupied ports.
# Python uses only its standard library; no GUI/audio packages are needed here.
connection="$(python3 - "$backend_config" "$webbridge_config" "$source" "$digital_audio" <<'PY'
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
    audio3, text3, audio4, text4 = (int(bridge.get(k, 0)) for k in
        ('rx3_decoder_audio_port', 'rx3_decoder_text_port', 'rx4_decoder_audio_port', 'rx4_decoder_text_port'))
    digital, digital_bridge = (int(bridge.get(k, 0)) for k in ('digital_audio_port', 'digital_bridge_port'))
    if bool(digital) != bool(digital_bridge) or not all(0 <= p <= 65535 for p in (digital, digital_bridge)):
        raise ValueError('Beide Digitalaudio-Ports muessen zusammen aktiviert werden (1..65535).')
    if digital and len({gui_port, audio3, text3, audio4, text4, digital, digital_bridge}) != 7:
        raise ValueError('Alle UDP-Ports muessen verschieden sein.')
    listen, http = bridge.get('listen', '0.0.0.0'), int(bridge.get('port', 8080))
    if not all(1 <= p <= 65535 for p in (gui_port, audio3, text3, audio4, text4, http)):
        raise ValueError('GUI-, HTTP- und alle vier Decoder-Ports muessen 1..65535 sein.')
    if len({gui_port, audio3, text3, audio4, text4}) != 5:
        raise ValueError('GUI- und Decoder-Ports muessen verschieden sein.')
    addresses = [(socket.SOCK_DGRAM, gui_bind, gui_port),
                 (socket.SOCK_DGRAM, '127.0.0.1', audio3),
                 (socket.SOCK_DGRAM, '127.0.0.1', text3),
                 (socket.SOCK_DGRAM, '127.0.0.1', audio4),
                 (socket.SOCK_DGRAM, '127.0.0.1', text4),
                 (socket.SOCK_STREAM, listen, http)]
    if sys.argv[3] == 'pluto':
        addresses += [(socket.SOCK_DGRAM, '127.0.0.1', p) for p in (40808, 40809, 40810)]
    if digital:
        addresses += [(socket.SOCK_DGRAM, '127.0.0.1', digital_bridge)]
        if sys.argv[4] == '1':
            addresses += [(socket.SOCK_DGRAM, '127.0.0.1', digital)]
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
    print('127.0.0.1' if listen == '0.0.0.0' else listen, http, audio3, audio4, digital, digital_bridge)
except (OSError, ValueError) as error:
    sys.exit(f'Start abgebrochen: {error}')
PY
)"
read -r http_host http_port ft8_port beacon_port digital_port digital_bridge_port <<< "$connection"
if ((digital_port == 0)); then digital_audio=0; fi
if ((digital_audio)) && [[ ! -x "$bin_dir/qo100_digital_audio" ]]; then
    echo "Fehlt: qo100_digital_audio. Bitte ./build_all.sh ausfuehren." >&2
    exit 1
fi

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
    for name in backend decoder-ft8 decoder-beacon webbridge; do
        echo "$name ($log_dir/$name.log):" >&2
        tail -n 12 "$log_dir/$name.log" >&2
    done
}

launch backend "$bin_dir/qo100_backend" --config "$backend_config" --source "$source"
launch decoder-ft8 "$bin_dir/qo100_ft8_decoder" --port "$ft8_port"
launch decoder-beacon "$bin_dir/qo100_beacon_decoder" --port "$beacon_port"
launch webbridge "$bin_dir/qo100_webbridge" --config "$webbridge_config"
if ((digital_audio)); then launch digital-audio "$bin_dir/qo100_digital_audio" --port "$digital_port" --bridge-port "$digital_bridge_port"; fi
echo "Starte QO100; Logs: $log_dir"

deadline=$((SECONDS + 15))
while :; do
    for index in 0 1 2 3; do
        pid="${pids[$index]}"
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Ein Dienst wurde beim Start beendet." >&2
            show_logs
            exit 1
        fi
    done
    if grep -q '^Source=' "$log_dir/backend.log" && \
       grep -q '^FT8 receiver: RX3' "$log_dir/decoder-ft8.log" && \
       grep -q '^PSK beacon receiver: RX4' "$log_dir/decoder-beacon.log" && \
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

echo "Backend, WebSocket-Adapter, RX3 FT8 und RX4 PSK laufen."
if ((digital_audio)); then echo "RX2/TX2 Audioschnittstelle: QO100_RX2 / QO100_TX2 (Status: $log_dir/digital-audio.log)."; fi
echo "Browser auf diesem Rechner: http://$http_host:$http_port/"
echo "Im LAN: http://<IP-dieses-Rechners>:$http_port/ oder die OPNsense-HTTPS-Adresse."
echo "Dieses Terminal offen lassen. Strg+C beendet alle Programme samt Pluto-Treiber."

# A failed monitor must not interrupt the QSO or the other decoder.
# The two-second poll also bounds restart frequency for a repeatedly failing decoder.
while :; do
    for index in 0 3; do
        if ! kill -0 "${pids[$index]}" 2>/dev/null; then
            echo "Backend oder Webadapter wurde beendet. Logs: $log_dir" >&2
            show_logs
            exit 1
        fi
    done
    restart_indices=(1 2)
    if ((digital_audio)); then restart_indices+=(4); fi
    for index in "${restart_indices[@]}"; do
        if ! kill -0 "${pids[$index]}" 2>/dev/null; then
            wait "${pids[$index]}" 2>/dev/null || true
            if ((index == 1)); then
                name=decoder-ft8; program=qo100_ft8_decoder; monitor_port="$ft8_port"
            elif ((index == 2)); then
                name=decoder-beacon; program=qo100_beacon_decoder; monitor_port="$beacon_port"
            else
                name=digital-audio; program=qo100_digital_audio; monitor_port="$digital_port"
            fi
            echo "$name wird neu gestartet. Logs: $log_dir/$name.log" >&2
            extra_args=()
            if ((index == 4)); then extra_args=(--bridge-port "$digital_bridge_port"); fi
            "$bin_dir/$program" --port "$monitor_port" "${extra_args[@]}" >>"$log_dir/$name.log" 2>&1 &
            pids[$index]=$!
        fi
    done
    sleep 2
done
