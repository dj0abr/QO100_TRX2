#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "$project_dir/build/bin/qo100_backend" --config "$project_dir/config/backend.conf" "$@"
