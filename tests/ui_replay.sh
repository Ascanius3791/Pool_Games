#!/bin/bash
# Headless browser check of the replay UI (Windows Chrome from WSL). Run after ./build.sh.
# usage: tests/ui_replay.sh [replay|resign]   (resign: pause, resign and bot concession)
set -e
cd "$(dirname "$0")/.."
T="${1:-replay}"
T="$T" python3 - <<'PY'
import os
t = os.environ['T']
h = open('build/harness.html').read()
h = h.replace('https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js', 'npm/node_modules/three/build/three.min.js')
h = h.replace('</body>', '<script>' + open(f'tests/ui_{t}_inject.js').read() + '</script></body>')
open(f'build/ui_{t}.html', 'w').write(h)
PY
mkdir -p "${CHROME_PROFILE:-/tmp/fc-chrome}"
CHROME="${CHROME:-/mnt/c/Program Files/Google/Chrome/Application/chrome.exe}"
"$CHROME" --headless=new --user-data-dir="$(wslpath -w "${CHROME_PROFILE:-/tmp/fc-chrome}")" --no-first-run --enable-unsafe-swiftshader --use-angle=swiftshader --window-size=1280,800 --virtual-time-budget=60000 \
  --dump-dom "file:$(wslpath -w build/ui_$T.html | sed 's#\\#/#g')" 2>/dev/null | python3 -c "import sys,re,html; m=re.search(r'<pre id=\"tlog\"[^>]*>(.*?)</pre>', sys.stdin.read(), re.S); print(html.unescape(m.group(1)) if m else 'no log')"
