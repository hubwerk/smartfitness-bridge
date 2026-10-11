#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMP="$(mktemp -d)"
trap 'rm -rf "$TEMP"' EXIT

fonttools varLib.instancer "$ROOT/assets/fonts/Antonio-Variable.ttf" wght=400 \
    -o "$TEMP/Antonio-Regular.ttf"
pyftsubset "$TEMP/Antonio-Regular.ttf" --unicodes=U+0020-007E,U+00A0-00FF \
    --flavor=woff2 --output-file="$ROOT/assets/fonts/Antonio-Regular.woff2"
for size in 16 20; do
    npx --yes lv_font_conv@1.5.3 --font "$TEMP/Antonio-Regular.ttf" \
        --range 0x20-0x7e,0xa0-0xff --size "$size" --bpp 4 --format lvgl \
        --no-compress --lv-include lvgl.h --lv-fallback lv_font_montserrat_14 \
        --lv-font-name "sf_antonio_$size" \
        -o "$ROOT/src/variants/hmi/fonts/sf_antonio_$size.c"
done
npx --yes lv_font_conv@1.5.3 --font "$TEMP/Antonio-Regular.ttf" \
    --symbols '0123456789.,-+' --size 48 --bpp 4 --format lvgl \
    --no-compress --lv-include lvgl.h --lv-fallback lv_font_montserrat_14 \
    --lv-font-name sf_antonio_48 -o "$ROOT/src/variants/hmi/fonts/sf_antonio_48.c"
python3 - "$ROOT" <<'PY'
import base64
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
html = root / "src/variants/web/assets/index.html"
font = base64.b64encode((root / "assets/fonts/Antonio-Regular.woff2").read_bytes()).decode()
text, count = re.subn(r"data:font/woff2;base64,[A-Za-z0-9+/=]+",
                      "data:font/woff2;base64," + font, html.read_text())
if count != 1:
    raise SystemExit("Expected exactly one embedded Antonio font")
html.write_text(text)
PY
