#!/usr/bin/env bash
# build-wk-macos.sh — 构建 macOS WebView 画板壳(纯 C99, dlopen WebKit/AppKit, 零链接框架)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dist
cc -std=c99 -O2 -Wall -ffp-contract=off tools/hnweb_wk_macos.c -o dist/hnweb-wk-macos-arm64
echo "✓ dist/hnweb-wk-macos-arm64"
