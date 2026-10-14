#!/usr/bin/env bash
# fetch-cubism-core.sh — 拉取官方 Live2D Cubism Core(moc3 运行库, C API)
# ======================================================================
# 许可口径(与 live2d-py 等社区项目同一姿势): Core 二进制**不入 git 仓库**
# —— 使用方自行运行本脚本从 Live2D 官方发布包提取, 遵守 Live2D Proprietary
# Software License(个人/小规模事業者免费授权线内使用, 发布应用时附许可
# 与 NOTICE 声明)。见 README "Live2D (Cubism moc3) 授权说明"。
#
# 提取物: vendor/cubism/include/Live2DCubismCore.h   (C 头文件)
#         vendor/cubism/lib/macos-arm64/libLive2DCubismCore.a (静态库)
# 两者均在 .gitignore —— 仓库只保存本脚本与文档。
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:-5-r.5}"
URL="https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-${VER}.zip"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "· 下载 $URL"
curl -sL --max-time 300 -o "$TMP/sdk.zip" "$URL"
[ -s "$TMP/sdk.zip" ] || { echo "FAIL 下载为空"; exit 1; }

echo "· 解包"
unzip -o -q "$TMP/sdk.zip" -d "$TMP/sdk"
SDK=$(find "$TMP/sdk" -maxdepth 1 -type d -name "CubismSdkForNative-*" | head -1)
[ -n "$SDK" ] || { echo "FAIL 包内无 SDK 目录"; exit 1; }

LIB=$(find "$SDK/Core/lib/macos" -name "libLive2DCubismCore.a" -path "*arm64*" | head -1)
HDR=$(find "$SDK/Core/include" -name "Live2DCubismCore.h" | head -1)
[ -n "$LIB" ] && [ -n "$HDR" ] || { echo "FAIL 包内无 macos-arm64 Core"; exit 1; }

mkdir -p vendor/cubism/lib/macos-arm64 vendor/cubism/include
cp "$LIB" vendor/cubism/lib/macos-arm64/libLive2DCubismCore.a
cp "$HDR" vendor/cubism/include/Live2DCubismCore.h
echo "OK: vendor/cubism/ 就绪(不入库)。"
"$([ -x /usr/bin/file ] && echo /usr/bin/file)" -b vendor/cubism/lib/macos-arm64/libLive2DCubismCore.a | cut -c1-60
