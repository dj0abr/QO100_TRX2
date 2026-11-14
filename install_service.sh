#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
if [[ "$project_dir" == *$'\n'* || "$project_dir" == *$'\r'* ]]; then
    echo "The checkout path cannot contain line breaks." >&2
    exit 1
fi

service_user="$(id -un)"
elevate=()
if ((EUID == 0)); then
    service_user="${SUDO_USER:-$service_user}"
else
    elevate=(sudo)
fi

unit_dir=/etc/systemd/system
template=qo100-trx2@.service
unit="$(systemd-escape --template="$template" -- "$service_user")"
drop_in="$(mktemp)"
trap 'rm -f -- "$drop_in"' EXIT
# WorkingDirectory expands systemd specifiers, so escape literal percent signs.
printf '[Service]\nWorkingDirectory=%s/\n' "${project_dir//%/%%}" > "$drop_in"

"${elevate[@]}" install -m 0644 "$project_dir/systemd/$template" "$unit_dir/$template"
"${elevate[@]}" install -d -m 0755 "$unit_dir/$unit.d"
"${elevate[@]}" install -m 0644 "$drop_in" "$unit_dir/$unit.d/10-checkout.conf"
"${elevate[@]}" systemctl daemon-reload
"${elevate[@]}" systemctl enable --now "$unit"
printf 'Logs: journalctl -u %q -b\n' "$unit"
"${elevate[@]}" systemctl --no-pager status "$unit"
