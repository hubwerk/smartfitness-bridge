#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECK=false
HMI=true
WEB=true
for option in "$@"; do
    case "$option" in
        --check) CHECK=true ;;
        --hmi) WEB=false ;;
        --web) HMI=false ;;
        *) echo "Usage: $0 [--check] [--hmi|--web]" >&2; exit 2 ;;
    esac
done
if ! "$HMI" && ! "$WEB"; then
    echo "Choose --hmi or --web, not both" >&2
    exit 2
fi
TEMP="$(mktemp -d)"
trap 'rm -rf "$TEMP"' EXIT
if "$HMI"; then
    make -C "$ROOT" -j"${JOBS:-2}" hmi-screenshots \
        LVGL_DIR="${LVGL_DIR:-$ROOT/.cache/lvgl}" \
        SCREENSHOT_DIR="$TEMP/hmi"
fi
if "$WEB"; then
    python3 "$ROOT/scripts/web_screenshots.py" --output "$TEMP/web"
fi
for interface in hmi web; do
    test -d "$TEMP/$interface" || continue
    if test -n "${SCREENSHOT_DIR:-}"; then
        mkdir -p "$SCREENSHOT_DIR/$interface"
        cp "$TEMP/$interface/"*.png "$SCREENSHOT_DIR/$interface/"
    fi
    if "$CHECK"; then
        if ! diff -qr "$ROOT/docs/hmi/$interface" "$TEMP/$interface"; then
            if test "$interface" = hmi; then
                echo "::error::HMI screenshots are outdated; run scripts/update_hmi_docs.sh --hmi"
                exit 1
            fi
            # Browser/OS rasterization can differ despite a pinned browser.
            echo "::warning::Web screenshots differ; review artifacts and regenerate the gallery"
        fi
    else
        mkdir -p "$ROOT/docs/hmi/$interface"
        find "$ROOT/docs/hmi/$interface" -maxdepth 1 -type f -name '*.png' -delete
        cp "$TEMP/$interface/"*.png "$ROOT/docs/hmi/$interface/"
    fi
done
