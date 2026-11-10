#!/usr/bin/env bash
# Foreground supervisor: one terminal and Ctrl+C for the complete transceiver.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bin_dir="$project_dir/build/bin"
backend_config="$project_dir/config/backend.conf"
webbridge_config="$project_dir/config/webbridge.conf"
source=pluto
digital_audio=1
open_browser=0
mm_data_dir="$project_dir/data/mm-beacon"

usage() {
    echo "Aufruf: ./start_all.sh [--source pluto|synthetic]"
    echo "                      [--backend-config DATEI] [--webbridge-config DATEI]"
    echo "                      [--no-digital-audio] [--open-browser]"
    echo "                      [--mm-data-dir VERZEICHNIS]"
    echo "Laeuft im Vordergrund; Strg+C beendet alle gestarteten Programme."
}
while (($#)); do
    case "$1" in
        --help) usage; exit 0 ;;
        --no-digital-audio) digital_audio=0; shift ;;
        --open-browser) open_browser=1; shift ;;
        --source|--backend-config|--webbridge-config|--mm-data-dir)
            if (($# < 2)); then usage >&2; exit 2; fi
            case "$1" in
                --source) source="$2" ;;
                --backend-config) backend_config="$2" ;;
                --webbridge-config) webbridge_config="$2" ;;
                --mm-data-dir) mm_data_dir="$2" ;;
            esac
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ $source != pluto && $source != synthetic ]]; then usage >&2; exit 2; fi
for program in qo100_backend pluto qo100_webbridge; do
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
    def switch(key):
        value = backend.get(key, '1')
        if value not in ('0', '1'):
            raise ValueError(f'{key} muss 0 oder 1 sein.')
        return value == '1'

    rx_enabled = {rx: switch(f'rx{rx}_enabled') for rx in range(2, 6)}
    decoder_enabled = {rx: switch(f'rx{rx}_decoder_enabled') for rx in range(3, 6)}
    external = switch('external_decoders_enabled')
    digital_auto_idle = switch('digital_auto_idle')
    def cpu_budget(key):
        value = backend.get(key, '10')
        if not value.isascii() or not value.isdecimal() or not 1 <= int(value) <= 10:
            raise ValueError(f'{key} muss 1..10 Prozent eines Kerns sein.')
        return int(value)
    ft8_cpu = cpu_budget('rx3_decoder_cpu_percent')
    beacon_cpu = cpu_budget('rx4_decoder_cpu_percent')
    def decoder_ports(rx, audio_key, text_key):
        # CLI overrides also disable the webbridge links, not just processes.
        audio, text = (int(bridge.get(key, 0)) for key in (audio_key, text_key))
        if not all(0 <= p <= 65535 for p in (audio, text)):
            raise ValueError(f'RX{rx}-Decoder-Ports muessen 0..65535 sein.')
        if not (external and rx_enabled[rx] and decoder_enabled[rx]):
            return 0, 0
        if bool(audio) != bool(text):
            raise ValueError(f'Beide RX{rx}-Decoder-Ports muessen zusammen aktiviert werden.')
        return audio, text

    audio3, text3 = decoder_ports(3, 'rx3_decoder_audio_port', 'rx3_decoder_text_port')
    audio4, text4 = decoder_ports(4, 'rx4_decoder_audio_port', 'rx4_decoder_text_port')
    mm_audio, mm_http = decoder_ports(5, 'rx5_decoder_audio_port', 'mm_http_port')
    digital, digital_bridge = (int(bridge.get(k, 0)) for k in ('digital_audio_port', 'digital_bridge_port'))
    if bool(digital) != bool(digital_bridge) or not all(0 <= p <= 65535 for p in (digital, digital_bridge)):
        raise ValueError('Beide Digitalaudio-Ports muessen zusammen aktiviert werden (1..65535).')
    listen, http = bridge.get('listen', '0.0.0.0'), int(bridge.get('port', 8080))
    if not all(1 <= p <= 65535 for p in (gui_port, http)):
        raise ValueError('GUI- und HTTP-Ports muessen 1..65535 sein.')
    udp_ports = [p for p in (gui_port, audio3, text3, audio4, text4, digital, digital_bridge, mm_audio) if p]
    if len(set(udp_ports)) != len(udp_ports):
        raise ValueError('Alle aktivierten UDP-Ports muessen verschieden sein.')
    if mm_http and mm_http == http:
        raise ValueError('MM-HTTP und WebGUI muessen verschiedene TCP-Ports verwenden.')
    addresses = [(socket.SOCK_DGRAM, gui_bind, gui_port),
                 (socket.SOCK_STREAM, listen, http)]
    addresses += [(socket.SOCK_DGRAM, '127.0.0.1', p) for p in (audio3, text3, audio4, text4, mm_audio) if p]
    if mm_http:
        addresses += [(socket.SOCK_STREAM, '127.0.0.1', mm_http)]
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
    print('127.0.0.1' if listen == '0.0.0.0' else listen, http, audio3, text3, audio4, text4, digital, digital_bridge, mm_audio, mm_http, ft8_cpu, beacon_cpu, int(digital_auto_idle))
except (OSError, ValueError) as error:
    sys.exit(f'Start abgebrochen: {error}')
PY
)"
read -r http_host http_port ft8_port ft8_text_port beacon_port beacon_text_port digital_port digital_bridge_port mm_port mm_http_port ft8_cpu beacon_cpu digital_auto_idle <<< "$connection"
decoder_programs=()
if ((ft8_port)); then decoder_programs+=(qo100_ft8_decoder ft8-decoder); fi
if ((beacon_port)); then decoder_programs+=(qo100_beacon_decoder); fi
for program in "${decoder_programs[@]}"; do
    if [[ ! -x "$bin_dir/$program" ]]; then
        echo "Fehlt: $bin_dir/$program. Bitte zuerst ./build_all.sh ausfuehren." >&2
        exit 1
    fi
done
if ((digital_port == 0)); then digital_audio=0; fi
if ((digital_audio)) && [[ ! -x "$bin_dir/qo100_digital_audio" ]]; then
    echo "Fehlt: qo100_digital_audio. Bitte ./build_all.sh ausfuehren." >&2
    exit 1
fi
if ((mm_port)) && [[ ! -x "$bin_dir/qo100_mm_decoder" || ! -x "$bin_dir/qo100_mm_probe" ]]; then
    echo "Fehlt: RX5 MM-Decoder. Bitte ./build_all.sh ausfuehren." >&2
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
    for name in backend decoder-ft8 decoder-beacon webbridge decoder-mm; do
        [[ -f "$log_dir/$name.log" ]] || continue
        echo "$name ($log_dir/$name.log):" >&2
        tail -n 12 "$log_dir/$name.log" >&2
    done
}

backend_index=${#pids[@]}
launch backend "$bin_dir/qo100_backend" --config "$backend_config" --source "$source"
ft8_index=-1; beacon_index=-1
if ((ft8_port)); then
    ft8_index=${#pids[@]}
    launch decoder-ft8 "$bin_dir/qo100_ft8_decoder" --port "$ft8_port" --cpu-percent "$ft8_cpu"
fi
if ((beacon_port)); then
    beacon_index=${#pids[@]}
    launch decoder-beacon "$bin_dir/qo100_beacon_decoder" --port "$beacon_port" --cpu-percent "$beacon_cpu"
fi
webbridge_index=${#pids[@]}
launch webbridge "$bin_dir/qo100_webbridge" --config "$webbridge_config" \
    --rx3-decoder-audio-port "$ft8_port" --rx3-decoder-text-port "$ft8_text_port" \
    --rx4-decoder-audio-port "$beacon_port" --rx4-decoder-text-port "$beacon_text_port" \
    --rx5-decoder-audio-port "$mm_port" --mm-http-port "$mm_http_port"
digital_index=-1; mm_index=-1
if ((digital_audio)); then
    digital_index=${#pids[@]}
    audio_args=();if ((digital_auto_idle == 0)); then audio_args+=(--always-active); fi
    launch digital-audio "$bin_dir/qo100_digital_audio" --port "$digital_port" --bridge-port "$digital_bridge_port" "${audio_args[@]}"
fi
if ((mm_port)); then
    mm_index=${#pids[@]}
    launch decoder-mm "$bin_dir/qo100_mm_decoder" --port "$mm_port" --http-port "$mm_http_port" --data-dir "$mm_data_dir"
fi
echo "Starte QO100; Logs: $log_dir"

deadline=$((SECONDS + 15))
while :; do
    startup_indices=("$backend_index" "$webbridge_index")
    if ((ft8_index >= 0)); then startup_indices+=("$ft8_index"); fi
    if ((beacon_index >= 0)); then startup_indices+=("$beacon_index"); fi
    if ((mm_index >= 0)); then startup_indices+=("$mm_index"); fi
    for index in "${startup_indices[@]}"; do
        pid="${pids[$index]}"
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Ein Dienst wurde beim Start beendet." >&2
            show_logs
            exit 1
        fi
    done
    if grep -q '^Source=' "$log_dir/backend.log" && \
       { ((ft8_port == 0)) || grep -q '^FT8 receiver: RX3' "$log_dir/decoder-ft8.log"; } && \
       { ((beacon_port == 0)) || grep -q '^PSK beacon receiver: RX4' "$log_dir/decoder-beacon.log"; } && \
       grep -q '^HTTP/WS' "$log_dir/webbridge.log" && \
       { ((mm_port == 0)) || grep -q '^MM beacon receiver: RX5' "$log_dir/decoder-mm.log"; }; then
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

if ((ft8_port && beacon_port)); then
    echo "Backend, WebSocket-Adapter, RX3 FT8 und RX4 PSK laufen."
else
    echo "Backend und WebSocket-Adapter laufen."
    if ((ft8_port)); then echo "RX3 FT8 laeuft."; else echo "RX3 FT8 ausgeschaltet."; fi
    if ((beacon_port)); then echo "RX4 PSK laeuft."; else echo "RX4 PSK ausgeschaltet."; fi
fi
if ((mm_port == 0)); then echo "RX5 MM-Decoder ausgeschaltet."; fi
if ((mm_port)); then echo "RX5 MM-Bake: DX-Cluster, CW-Skimmer und Bulletin (Daten: $mm_data_dir)."; fi
if ((digital_audio)); then echo "RX2/TX2 Audioschnittstelle: fromQO100 / toQO100 (Status: $log_dir/digital-audio.log)."; fi
echo "Browser auf diesem Rechner: http://$http_host:$http_port/"
echo "Im LAN: http://<IP-dieses-Rechners>:$http_port/ oder die OPNsense-HTTPS-Adresse."
echo "Dieses Terminal offen lassen. Strg+C beendet alle Programme samt Pluto-Treiber."

# Open only after the complete stack passed its startup checks. The browser
# belongs to the desktop session, not to the supervised radio processes.
if ((open_browser)); then
    browser_url="http://$http_host:$http_port/"
    browser_command=()
    if command -v xdg-open >/dev/null 2>&1; then
        browser_command=(xdg-open)
    elif command -v gio >/dev/null 2>&1; then
        browser_command=(gio open)
    fi
    if ((${#browser_command[@]})); then
        echo "Oeffne Standardbrowser: $browser_url"
        (
            trap - EXIT INT TERM HUP
            if ! "${browser_command[@]}" "$browser_url" >"$log_dir/browser.log" 2>&1; then
                echo "Browser konnte nicht geoeffnet werden. Bitte $browser_url manuell oeffnen. Details: $log_dir/browser.log" >&2
            fi
        ) &
    else
        echo "Kein Browserstarter gefunden (xdg-open/gio). Bitte $browser_url manuell oeffnen oder xdg-utils installieren." >&2
    fi
fi

# A failed monitor must not interrupt the QSO or the other decoder.
# The two-second poll also bounds restart frequency for a repeatedly failing decoder.
while :; do
    for index in "$backend_index" "$webbridge_index"; do
        if ! kill -0 "${pids[$index]}" 2>/dev/null; then
            echo "Backend oder Webadapter wurde beendet. Logs: $log_dir" >&2
            show_logs
            exit 1
        fi
    done
    restart_indices=()
    if ((ft8_index >= 0)); then restart_indices+=("$ft8_index"); fi
    if ((beacon_index >= 0)); then restart_indices+=("$beacon_index"); fi
    if ((digital_index >= 0)); then restart_indices+=("$digital_index"); fi
    if ((mm_index >= 0)); then restart_indices+=("$mm_index"); fi
    for index in "${restart_indices[@]}"; do
        if ! kill -0 "${pids[$index]}" 2>/dev/null; then
            wait "${pids[$index]}" 2>/dev/null || true
            extra_args=()
            if ((index == ft8_index)); then
                name=decoder-ft8; program=qo100_ft8_decoder; monitor_port="$ft8_port"
                extra_args=(--cpu-percent "$ft8_cpu")
            elif ((index == beacon_index)); then
                name=decoder-beacon; program=qo100_beacon_decoder; monitor_port="$beacon_port"
                extra_args=(--cpu-percent "$beacon_cpu")
            elif ((index == digital_index)); then
                name=digital-audio; program=qo100_digital_audio; monitor_port="$digital_port"
                extra_args=(--bridge-port "$digital_bridge_port")
                if ((digital_auto_idle == 0)); then extra_args+=(--always-active); fi
            else
                name=decoder-mm; program=qo100_mm_decoder; monitor_port="$mm_port"
                extra_args=(--http-port "$mm_http_port" --data-dir "$mm_data_dir")
            fi
            echo "$name wird neu gestartet. Logs: $log_dir/$name.log" >&2
            "$bin_dir/$program" --port "$monitor_port" "${extra_args[@]}" >>"$log_dir/$name.log" 2>&1 &
            pids[$index]=$!
        fi
    done
    sleep 2
done
