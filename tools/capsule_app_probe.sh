#!/usr/bin/env bash
# tools/capsule_app_probe.sh — hn build .app 外壳 + --capsule 直启回归
#
# 口径: examples/project-demo(真项目) → hncore build --target macos →
# .app 结构/Info.plist plist 校验/运行时可执行/胶囊 PK+unzip 双校验 →
# hn_app --capsule 直启真解包(apps/<id>/ 入口页 + 嵌套资产目录)。
#
# 用法: bash tools/capsule_app_probe.sh

set -u
cd "$(dirname "$0")/.."
NPASS=0; NFAIL=0
ck() { if [ "$1" = 1 ]; then NPASS=$((NPASS+1)); else NFAIL=$((NFAIL+1)); echo "  ✘ $2"; fi; }

rm -rf /tmp/cbp-build "$HOME/.html-native/apps/com.hn.project-demo"
mkdir -p /tmp/cbp-apps
./dist/hncore-macos-arm64 build examples/project-demo --out /tmp/cbp-build --runtime dist/hnapp-macos-arm64 >/dev/null 2>&1
ck "$([ -f /tmp/cbp-build/project-demo.hnapp ] && echo 1 || echo 0)" ".hnapp 胶囊产出"
ck "$([ -d /tmp/cbp-build/project-demo.app ] && echo 1 || echo 0)" ".app 外壳目录"
ck "$([ -f /tmp/cbp-build/project-demo.app/Contents/Info.plist ] && echo 1 || echo 0)" "Info.plist 存在"
plutil -lint /tmp/cbp-build/project-demo.app/Contents/Info.plist >/dev/null 2>&1
ck "$([ $? = 0 ] && echo 1 || echo 0)" "Info.plist plist 校验通过"
ck "$([ -x /tmp/cbp-build/project-demo.app/Contents/MacOS/hnapp ] && echo 1 || echo 0)" "运行时二进制可执行"
ck "$([ "$(head -c 2 /tmp/cbp-build/project-demo.app/Contents/Resources/app.hnapp)" = "PK" ] && echo 1 || echo 0)" "Resources/app.hnapp 是 ZIP 胶囊(PK)"
unzip -t /tmp/cbp-build/project-demo.app/Contents/Resources/app.hnapp >/dev/null 2>&1
ck "$([ $? = 0 ] && echo 1 || echo 0)" "胶囊过系统 unzip 校验"
grep -q "CFBundleIdentifier" /tmp/cbp-build/project-demo.app/Contents/Info.plist
ck "$([ $? = 0 ] && echo 1 || echo 0)" "Info.plist 含 bundle id"

# --capsule 直启: 解包到 apps/<id>/ 后窗口开跑 —— 轮询到解包完成即收割
# (项目模式 + 真窗会常驻, 探针只验证解包与资产落盘; 渲染正确性归 project_probe)
./dist/hnapp-macos-arm64 --capsule /tmp/cbp-build/project-demo.app/Contents/Resources/app.hnapp >/dev/null 2>&1 &
CAP_PID=$!
for i in $(seq 1 50); do
  [ -f "$HOME/.html-native/apps/com.hn.project-demo/index.html" ] && break
  sleep 0.2
done
kill $CAP_PID 2>/dev/null
wait $CAP_PID 2>/dev/null
ck "$([ -f "$HOME/.html-native/apps/com.hn.project-demo/index.html" ] && echo 1 || echo 0)" "--capsule 解包入口页"
ck "$([ -d "$HOME/.html-native/apps/com.hn.project-demo/pages" ] && echo 1 || echo 0)" "嵌套资产目录保留(pages/)"
echo "结果: $NPASS 通过, $NFAIL 失败"
[ "$NFAIL" = 0 ]
