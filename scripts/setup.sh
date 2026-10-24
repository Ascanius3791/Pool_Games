#!/usr/bin/env bash
# scripts/setup.sh — install what the build and the training need (Debian/Ubuntu).
set -e
cd "$(dirname "$0")/.."
need() { command -v "$1" > /dev/null 2>&1; }
if ! need g++ || ! need clang++ || ! need wasm-ld || ! need python3 || ! need node; then
  echo "installing compilers (needs sudo/apt)…"
  sudo apt-get update -y && sudo apt-get install -y g++ clang lld python3 nodejs npm make
fi
mkdir -p build/npm
( cd build/npm && [ -f package.json ] || npm init -y > /dev/null; npm install three@0.128.0 binaryen@116.0.0 )
make -s tools
echo "ok: tools built in train/bin, web build deps in build/npm"
