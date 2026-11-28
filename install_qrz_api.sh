#!/usr/bin/env bash
set -euo pipefail

if (( $# > 1 )); then
    printf 'Aufruf %s [QRZ-Logbook-API-Key]\n' "$0" >&2
    exit 2
fi
qrz_key=${1-DEIN_QRZ_LOGBOOK_API_KEY}
if [[ -z $qrz_key || $qrz_key == *$'\n'* || $qrz_key == *$'\r'* ]]; then
    printf '%s\n' 'Der QRZ-API-Key muss eine nicht leere Zeile sein.' >&2
    exit 2
fi

qrz_dir="${HOME:?HOME muss auf das eigene Benutzerverzeichnis zeigen}/.config/qo100-trx2"
qrz_env="$qrz_dir/qrz.env"
umask 077
mkdir -p -- "$qrz_dir"

if [[ ( -e $qrz_env || -L $qrz_env ) && ! -f $qrz_env ]]; then
    printf 'Unter %s wird eine reguläre Datei erwartet.\n' "$qrz_env" >&2
    exit 1
fi

if (( $# == 1 )) || [[ ! -e $qrz_env ]]; then
    temporary=$(mktemp -- "$qrz_dir/.qrz.env.XXXXXX")
    trap 'rm -f -- "$temporary"' EXIT

    if [[ -f $qrz_env ]]; then
        awk '!/^[[:space:]]*(export[[:space:]]+)?QO100_QRZ_API_KEY=/' "$qrz_env" > "$temporary"
    fi
    quoted_key=${qrz_key//\'/\'\\\'\'}
    printf "export QO100_QRZ_API_KEY='%s'\n" "$quoted_key" >> "$temporary"

    if (( $# == 1 )); then
        mv -fT -- "$temporary" "$qrz_env"
    elif ! ln -T -- "$temporary" "$qrz_env" 2>/dev/null && [[ ! -f $qrz_env ]]; then
        printf 'Die Datei %s konnte nicht angelegt werden.\n' "$qrz_env" >&2
        exit 1
    fi
fi

chmod 600 -- "$qrz_env"
printf 'QRZ-Konfiguration liegt in %s\n' "$qrz_env"
