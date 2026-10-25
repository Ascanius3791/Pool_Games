#!/bin/bash
# Build: C++ engine -> WASM -> embedded into the single-file web page
set -e
# requires: clang++ (wasm32 target), wasm-ld, python3, node and build/npm with three@0.128.0 + binaryen@116.0.0 (scripts/setup.sh)
cd "$(dirname "$0")"
clang++ --target=wasm32 -O3 -std=c++17 -nostdlib -ffreestanding -fno-exceptions -fno-rtti -ffp-contract=off \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=2097152 -o build/engine.wasm engine/wasm_api.cpp
node build/npm/node_modules/binaryen/bin/wasm2js build/engine.wasm -O2 -o build/engine.wasm.js
BUILD_ID="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet HEAD -- engine train/out 2>/dev/null || echo +)" python3 - <<'PY'
import base64, re, os
js = open('build/engine.wasm.js').read()
js = re.sub(r'^export var .*$', '', js, flags=re.M)
t = open('web/template.html').read()
w = base64.b64encode(open('build/engine.wasm','rb').read()).decode()
open('web/index.html','w').write(t.replace('__WASM_B64__', w).replace('/*__JS_FALLBACK__*/', js).replace('__NN_STD__', os.environ.get('NN_STD','true')).replace('__NN_NOISY__', os.environ.get('NN_NOISY','true')).replace('__BUILD_ID__', os.environ.get('BUILD_ID','unknown')))
# local test harness (adds the skeleton the artifact host provides)
open('build/harness.html','w').write('<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><style>body{margin:0;font:14px system-ui}[hidden]{display:none!important}</style></head><body>' + open('web/index.html').read() + '</body></html>')
PY
echo "built web/index.html ($(wc -c < web/index.html) bytes)"
