#!/usr/bin/env bash
# Start the complete transceiver and open the desktop's default browser.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "$project_dir/start_all.sh" --open-browser "$@"
