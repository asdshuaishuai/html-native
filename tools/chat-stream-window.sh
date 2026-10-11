#!/usr/bin/env bash
# tools/chat-stream-window.sh — 流式会话演示·真窗口版(五步推进, 肉眼验收)
# ============================================================================
# 与 tools/chat-stream.sh 同构的五步流式, 但 open 用 --window —— daemon
# spawn 平台壳开**真桌面窗**, `hn update` 热推送把新内容原子写回源文件,
# 子进程 hn_app 的 mtime watch(~300ms 轮询)整文档重载, 窗口里实时流式
# 推进。**需要桌面会话**(macOS 本机 + 窗口站); headless 截图留档版见
# chat-stream.sh。
#
# 与 headless 版的三处不同:
#   - `hn shot`/`hn dump` 对 window 应用不可用(文档在子进程里, daemon 无
#     句柄, 回 unsupported for window apps) —— 不留三帧截图, 每步之间
#     sleep, 靠肉眼验收; 想留档跑 headless 版即可。
#   - open 的源文件在 /tmp(daemon 对它 spawn 子进程, 之后所有 update 都
#     原子写回这一个文件); 每帧内容先写到 .src 再 `hn update`(daemon 读
#     .src、写源文件, 双重原子, 子进程永远读到完整文档)。
#   - 脚本结束时**窗口保持打开**(人还在看), 不自动 close、不杀 daemon;
#     看完手动 `hn close chat` 收口。
set -euo pipefail
cd "$(dirname "$0")/.."

ID="chat"
HN="dist/hn"
DAEMON="dist/hn-daemon"
TMP="/tmp/hn-chat-window-$$"          # 窗口源文件(spawn 目标, update 写回处)
SRC="$TMP.src"                        # 每帧推送内容的落点(daemon 读它)
DAEMON_PID=""
DAEMON_OWNED=0

cleanup() {
  # 窗口与 daemon 都保持运行(肉眼验收还没完); 只清掉当帧的 .src 中转
  rm -f "$SRC"
  if [ "$DAEMON_OWNED" = 1 ] && [ -n "$DAEMON_PID" ]; then
    echo "[daemon] 自起的 hn-daemon 保持运行(pid=$DAEMON_PID), 收口: kill $DAEMON_PID"
  fi
  echo "[done] 窗口保持打开, 看完: $HN close $ID"
}
trap cleanup EXIT INT TERM

# ---------- 0. daemon: 已起则复用, 否则后台启动并记住 PID ----------
if "$HN" ping >/dev/null 2>&1; then
  echo "[daemon] 复用已运行的 hn-daemon"
else
  "$DAEMON" >/dev/null 2>&1 &
  DAEMON_PID=$!
  DAEMON_OWNED=1
  for _ in $(seq 1 50); do
    "$HN" ping >/dev/null 2>&1 && break
    sleep 0.1
  done
  "$HN" ping >/dev/null 2>&1 || { echo "[daemon] 启动失败"; exit 1; }
  echo "[daemon] 已启动 pid=$DAEMON_PID(cwd=仓库根)"
fi

# ---------- 1. 开真窗口: 源文件 = /tmp 的 $TMP, update 之后都写回它 ----------
# 占位文档与后续各帧声明同一 hn-window 尺寸: 窗口一次开成, 重载只换内容
# (macOS 壳的 hnp_window_resize 只调内容表面不动 NSWindow 外框, 尺寸在
#  open 时定死最稳 —— 见 app/hn_app.c reload 注释)
"$HN" close "$ID" >/dev/null 2>&1 || true
cat > "$TMP" <<'HTML'
<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="hn-surface" content="window">
<meta name="hn-window" content="720x860"><meta name="hn-title" content="DeepAgent · 流式会话(真窗口)">
</head><body><h1>DeepAgent</h1><p id="frame">正在打开会话窗…</p></body></html>
HTML
R=$("$HN" open "$ID" "$TMP" --window)
echo "$R" | grep -q '"window":true' && echo "[window] pid=$(printf '%s' "$R" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p') —— 桌面真窗已弹出" \
  || { echo "[window] open 失败($R)"; exit 1; }
sleep 1   # 等子进程起窗 + 首次 blit

# ---------- 页面骨架: PRE / POST 之间夹累积缓冲 STREAM ----------
# 与 chat-stream.sh 同一份五帧内容(headless 版的三帧截图即来自它);
# update 是整文档替换, 每步推送 = 完整累积后的文档。
read -r -d '' PAGE_A <<'HTML' || true
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="hn-surface" content="window">
<meta name="hn-window" content="720x860">
<meta name="hn-title" content="DeepAgent · 流式会话">
<meta name="hn-theme" content="dark">
<link rel="stylesheet" href="__UIKIT_HREF__">
<style>
  .app { display: flex; flex-direction: column; height: 100%; box-sizing: border-box; }
  .appbar { display: flex; align-items: center; gap: 10; padding: 14 18 12;
            border-bottom: 1 solid var(--line); }
  .dot-status { width: 9; height: 9; border-radius: 5;
                background: var(--accent); box-shadow: 0 0 0 3 rgba(79,124,255,0.16); }
  .dot-status.done { background: #3ecf6f; box-shadow: 0 0 0 3 rgba(62,207,111,0.16); }
  .ab-text { display: flex; flex-direction: column; gap: 1; }
  .ab-title { font-size: 14; font-weight: 700; color: var(--ink); }
  .ab-sub { font-size: 11; color: var(--dim); }
  .ab-badge { margin-left: auto; font-size: 10; font-weight: 700; color: var(--accent);
              border: 1 solid rgba(79,124,255,0.35); border-radius: 999; padding: 2 9; }
  .ab-badge.done { color: #3ecf6f; border-color: rgba(62,207,111,0.35); }
  #stream { position: relative; overflow: hidden; flex: 1;
            background: #12161e; border: 1 solid var(--line);
            border-radius: 12; margin: 12 14 0; }
  #transcript { position: absolute; left: 12; right: 12; bottom: 12;
                display: flex; flex-direction: column; gap: 12; }
  .chat-ai h3 { font-size: 14; margin: 2 0 6; }
  .chat-ai code { font-family: monospace; font-size: 11.5; color: #9db9ff;
                  background: rgba(79,124,255,0.12); border-radius: 4; padding: 1 5; }
  .chat-ai blockquote { margin: 6 0; padding: 2 0 2 10; border-left: 2 solid var(--line, #262e3d);
                        color: var(--dim, #8a93a8); }
  .msg .code-card { max-width: 78%; }
  .cmt { color: var(--code-dim, #7d8aa3); }
  .typing { display: flex; align-items: center; gap: 5; }
  .tdot { width: 6; height: 6; border-radius: 3; background: var(--dim, #8a93a8); }
  .typing-txt { font-size: 12; color: var(--dim, #8a93a8); margin-left: 4; }
  .done-row { display: flex; }
  .done-tag { font-size: 10; font-weight: 700; color: #3ecf6f;
              background: rgba(62,207,111,0.12); border: 1 solid rgba(62,207,111,0.35);
              border-radius: 999; padding: 3 10; }
  .composer { display: flex; gap: 8; align-items: center; padding: 12 14 14; }
  .composer input { flex: 1; }
  .composer .btn { padding: 8 18; }
</style>
</head>
<body>
<div class="app">
  <div class="appbar">
HTML

read -r -d '' PAGE_TEXT <<'HTML' || true
    <div class="ab-text">
      <div class="ab-title">DeepAgent</div>
      <div class="ab-sub">html-native · 真窗口流式</div>
    </div>
HTML

read -r -d '' PAGE_B <<'HTML' || true
  </div>
  <div id="stream" hn-stream>
    <div id="transcript">
      <div class="msg end"><div class="chat-user">帮我在真窗口里把回答流式打出来,我要亲眼看着它长出来。</div></div>
HTML

read -r -d '' PAGE_POST <<'HTML' || true
    </div>
  </div>
  <div class="composer">
    <input id="ask" placeholder="随时插话或追加要求…">
    <div class="btn">发送</div>
  </div>
</div>
</body>
</html>
HTML

PAGE_A="${PAGE_A//__UIKIT_HREF__/$(pwd)/examples/ui-kit-head.html}"

push() {  # $1=dot cls $2=badge cls $3=badge text — 累积成文 → .src → daemon 原子写回源文件
  { printf '%s\n' "$PAGE_A"
    printf '    <div class="dot-status %s" id="status-dot"></div>\n' "$1"
    printf '%s\n' "$PAGE_TEXT"
    printf '    <div class="ab-badge %s" id="status-badge">%s</div>\n' "$2" "$3"
    printf '%s\n' "$PAGE_B"
    printf '%s\n' "$STREAM"
    printf '%s\n' "$PAGE_POST"
  } > "$SRC"
  # daemon 读 SRC、原子写回 TMP(窗口源文件); 子进程 mtime watch ~300ms 内重载
  "$HN" update "$ID" "$SRC" >/dev/null
  echo "    [stream] 累积缓冲 ${#STREAM} chars → 窗口重载"
}

STREAM=""

# ---------- 2. 五步流式追加(每步 sleep, 肉眼看窗口推进) ----------
# 步 1: 打字中占位
STREAM='<div class="msg"><div class="chat-ai typing"><span class="tdot"></span><span class="tdot"></span><span class="tdot"></span><span class="typing-txt">正在组织回答…</span></div></div>'
push dot-status ab-badge "流式输出中"
echo "[step 1/5] 打字中占位 —— 看窗口: 输入条上方出现三个跳动的点"
sleep 1.6

# 步 2: markdown 正文(替换掉占位)
read -r -d '' STEP2 <<'HTML' || true
<div class="msg"><div class="chat-ai"><div hn-md>
### 真窗口流式

这是**桌面原生窗**, 不是 webview —— 每窗一进程, update 热推送 = 原子写
源文件 + 子进程 mtime 重载。

- **窗口表面不动**: 重载只换文档, 进程/窗不闪不退
- **watch 轮询 300ms**: 推完这一步, 窗口里最多 300ms 后换内容
- **肉眼验收**: 每步之间 sleep, 内容一级一级长出来
</div></div></div>
HTML
STREAM="$STEP2"
push dot-status ab-badge "流式输出中"
echo "[step 2/5] markdown 正文 —— 看窗口: 占位换成一段带标题/列表的回答"
sleep 1.6

# 步 3: 工具调用卡
STREAM="$STREAM
<div class=\"tool-card\">
  <div class=\"tool-head\">
    <span class=\"tool-name\">window.hot-push</span>
    <span class=\"tool-arg\">target=\"chat\"</span>
    <span class=\"tool-state ok\">完成 · 0.3s</span>
  </div>
  <div class=\"tool-body\">update → 原子写回 /tmp 源文件 → 子进程 mtime 重载, 窗口进程保持</div>
</div>"
push dot-status ab-badge "流式输出中"
echo "[step 3/5] 工具调用卡 —— 看窗口: 回答下方追加一张工具卡"
sleep 1.6

# 步 4: 代码块回答
read -r -d '' STEP4 <<'HTML' || true
<div class="msg"><div class="chat-ai"><div hn-md>
接入只要三行 —— 起宿主, 开真窗, 推流:

- **宿主**: 常驻 `hn-daemon`, Unix socket 上跑 JSON-lines
- **开窗**: `hn open <id> <file> --window` spawn 平台壳子进程
- **推流**: `hn update` 原子覆写源文件, 窗口自行重载
</div></div></div>
<div class="msg"><div class="code-card">
  <div class="code-head">
    <span class="code-lang">BASH</span>
    <span class="code-file">真窗口会话驱动</span>
  </div>
  <pre>dist/hn-daemon &amp;
dist/hn open chat /tmp/page.html --window
dist/hn update chat /tmp/next.html   <span class="cmt"># 窗口 300ms 内重载</span></pre>
</div></div>
HTML
STREAM="$STREAM
$STEP4"
push dot-status ab-badge "流式输出中"
echo "[step 4/5] 代码块回答 —— 看窗口: 追加说明气泡 + 代码卡"
sleep 1.6

# 步 5: 完成态 tag + 页头状态切换(绿)
STREAM="$STREAM
<div class=\"done-row\"><span class=\"done-tag\">本轮回答完成 · 真窗口五帧推进</span></div>"
push dot-status done "已完成"
echo "[step 5/5] 完成态 —— 看窗口: 状态点/徽章变绿, 追加完成标签"

echo "[done] 五帧已在真窗口逐级推进; 窗口保持打开, 看完 '$HN close $ID'"
