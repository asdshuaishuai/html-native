#!/usr/bin/env bash
# tools/sysbridge_probe.sh — sys:// 解析层用例薄封装。
#
# sysbridge.c 只编 Windows 目标, 其三个**无平台依赖**纯函数(query 解析/
# JSON 取值/HTML 转义)的回归由 sysbridge_probe.c 在 macOS 本机编译运行
# (main() 末尾自带"复制版 vs 源文件特征行"防漂移比对)。此前无运行器,
# 本脚本补齐(纯单文件编译, 无引擎依赖)。
#
# 用法: bash tools/sysbridge_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."

LABEL="sysbridge_probe.sh"
OUT="${TMPDIR:-/tmp}/hn_sysbridge_probe_bin"
echo "$LABEL: compile"
cc -std=c99 -O2 -Wall -I Sources/CHtmlNative/include \
    tools/sysbridge_probe.c -o "$OUT"

echo "$LABEL: run"
"$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"
