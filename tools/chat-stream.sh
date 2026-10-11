#!/usr/bin/env bash
# tools/chat-stream.sh — agent 流式会话演示(脚本化, 可重复执行)
# ============================================================================
# 场景: agent 在原生窗里流式"打字"输出 markdown 回答, 中途插入工具调用卡,
# 三帧截图留档(半个回答 / 工具卡+代码块 / 完成态)。
#
# 重复执行安全: 先 `hn ping`, daemon 已起则复用(不动别人的 daemon);
# 本脚本自己启动的 daemon 记住 PID, 退出时 trap 清理。应用 id 撞车时先
# close 再 open。临时 HTML 放 /tmp, 结束清理。
#
# 口径(以 dist/hn 实际支持为准, `dist/hn` 不带参可看用法):
#   - daemon 默认 headless=1, CLI open 无 --headless 参数(无需传);
#   - open/update 推的是**整文档**: 脚本侧持有累积缓冲, 每步把"完整累积
#     后的 HTML"写入 /tmp 临时文件再 `hn update chat <file>`;
#   - shot 的相对路径按 daemon 的 cwd 解析 → 本脚本先 cd 到仓库根再起 daemon,
#     截图直落 screenshots/。
set -euo pipefail
cd "$(dirname "$0")/.."

ID="chat"
HN="dist/hn"
DAEMON="dist/hn-daemon"
TMP="/tmp/hn-chat-stream-$$.html"
DAEMON_PID=""
DAEMON_OWNED=0

cleanup() {
  "$HN" close "$ID" >/dev/null 2>&1 || true
  if [ "$DAEMON_OWNED" = 1 ] && [ -n "$DAEMON_PID" ]; then
    kill "$DAEMON_PID" 2>/dev/null || true
  fi
  rm -f "$TMP"
}
trap cleanup EXIT INT TERM

# ---------- 0. daemon: 已起则复用, 否则后台启动并记住 PID ----------
if "$HN" ping >/dev/null 2>&1; then
  echo "[daemon] 复用已运行的 hn-daemon"
else
  "$DAEMON" &
  DAEMON_PID=$!
  DAEMON_OWNED=1
  for _ in $(seq 1 50); do
    "$HN" ping >/dev/null 2>&1 && break
    sleep 0.1
  done
  "$HN" ping >/dev/null 2>&1 || { echo "[daemon] 启动失败"; exit 1; }
  echo "[daemon] 已启动 pid=$DAEMON_PID(headless, cwd=仓库根)"
fi

# ---------- 1. 打开会话窗(重复执行安全: 先 close 残留同 id 应用) ----------
"$HN" close "$ID" >/dev/null 2>&1 || true
"$HN" open "$ID" examples/agent-chat.html

# ---------- 页面骨架: PRE / POST 之间夹累积缓冲 STREAM ----------
# update 是整文档替换, 所以每步推送 = PRE + STREAM + POST 的完整累积结果。
# 状态条(appbar)两个变体: 输出中(accent) / 已完成(green)。
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
  /* 应用壳样式(页面自有, 非 ui-kit 组件; ui-kit 子集一律走上面的 link ——
     daemon 装载已走 hn_rt 路径, link 内联与 hn_app 同效, 不再内联兜底) */
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
      <div class="ab-sub">html-native · 会话留档中</div>
    </div>
HTML

read -r -d '' PAGE_B <<'HTML' || true
  </div>
  <div id="stream" hn-stream>
    <div id="transcript">
      <div class="msg end"><div class="chat-user">帮我把 agent 会话窗跑起来,顺便检查一下构建脚本还在不在。</div></div>
      <div class="msg"><div class="chat-ai"><div hn-md>
### 任务理解

收到,分三步走:

- **定位入口**: `dist/hn` CLI + `dist/hn-daemon` 常驻宿主
- **渲染链路**: `hn-md` 直染回答、`hn-stream` 跟随底部
- **留档**: 三个关键帧落 `screenshots/`

> update 是整文档替换, 脚本会持有累积缓冲、每步整体推送。
</div></div></div>
      <div class="tool-card">
        <div class="tool-head">
          <span class="tool-name">fs.read_tree</span>
          <span class="tool-arg">path="html-native"</span>
          <span class="tool-state ok">完成 · 0.4s</span>
        </div>
        <div class="tool-body">rt/ · Sources/CHtmlNative/ · dist/ · examples/ · tools/  —  5 dirs, 38 files</div>
      </div>
      <div class="msg end"><div class="chat-user">好,那把构建也跑一遍,回答直接流式打出来。</div></div>
HTML

# STREAM 追加点之后的收尾(流区/输入条/文档闭合)
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

# 推送页落在 /tmp: daemon 按文档目录解析 <link>, href 需要绝对路径。
# (examples/agent-chat.html 与页面同目录, 用同目录相对引用即可。)
PAGE_A="${PAGE_A//__UIKIT_HREF__/$(pwd)/examples/ui-kit-head.html}"

push() {  # $1=dot cls $2=badge cls $3=badge text — 累积缓冲整体成文并推送
  { printf '%s\n' "$PAGE_A"
    printf '    <div class="dot-status %s" id="status-dot"></div>\n' "$1"
    printf '%s\n' "$PAGE_TEXT"
    printf '    <div class="ab-badge %s" id="status-badge">%s</div>\n' "$2" "$3"
    printf '%s\n' "$PAGE_B"
    printf '%s\n' "$STREAM"
    printf '%s\n' "$PAGE_POST"
  } > "$TMP"
  "$HN" update "$ID" "$TMP" >/dev/null
  echo "    [stream] 累积缓冲 ${#STREAM} chars"   # 脚本侧缓冲的逐级增长
  # 进度读数: dump 回绘制指令数(daemon 回数字, CLI 已兼容解析) ——
  # 每推一步指令数应单调不减, 作为流式推送确实生效的旁证
  "$HN" dump "$ID"
}

STREAM=""

# ---------- 2. 五步流式追加 ----------
# 步 1: 打字中占位
STREAM='<div class="msg"><div class="chat-ai typing"><span class="tdot"></span><span class="tdot"></span><span class="tdot"></span><span class="typing-txt">正在组织回答…</span></div></div>'
push dot-status ab-badge "流式输出中"
echo "[step 1/5] 打字中占位"

sleep 0.6

# 步 2: 一段 markdown 正文(替换掉占位 —— 整文档替换语义下自然实现)
read -r -d '' STEP2 <<'HTML' || true
<div class="msg"><div class="chat-ai"><div hn-md>
### 构建体检结果

两条构建链路都活着, 结论先说: **可以放心接**。

- `tools/build-multiplatform.sh` — 三平台一键出包, 产物齐全
- `tools/build-daemon-cli.sh` — daemon + CLI 单独构建, 迭代快
- 入口口径: `open` / `update` / `shot` 全部收敛在同一份 `dist/hn`

> 下一帧我真的跑一遍构建, 再给接入代码。
</div></div></div>
HTML
STREAM="$STEP2"   # 正文替换掉打字占位(整文档替换语义: 缓冲里不再有占位)
push dot-status ab-badge "流式输出中"
"$HN" shot "$ID" screenshots/chat-stream-1.png
echo "[step 2/5] markdown 正文 → screenshots/chat-stream-1.png"

sleep 0.6

# 步 3: 工具调用卡(运行 build-multiplatform.sh, ok 状态)
STREAM="$STREAM
<div class=\"tool-card\">
  <div class=\"tool-head\">
    <span class=\"tool-name\">bash.build-multiplatform.sh</span>
    <span class=\"tool-arg\">--all</span>
    <span class=\"tool-state ok\">完成 · 6.8s</span>
  </div>
  <div class=\"tool-body\">macos-arm64 · linux-x86_64 · linux-aarch64 · windows-x86_64 — 12 artifacts, 0 warnings</div>
</div>"
push dot-status ab-badge "流式输出中"
echo "[step 3/5] 工具调用卡"

sleep 0.6

# 步 4: 代码块回答(说明气泡 + ui-kit code-card 代码卡, 各自成泡)。
# 代码体用真正的 <pre> —— 引擎已按换行计行测高(flex 容器内也不再被压成一行)。
read -r -d '' STEP4 <<'HTML' || true
<div class="msg"><div class="chat-ai"><div hn-md>
接入只要三行 —— 起宿主, 开窗, 推流, 对应下面这张卡:

- **宿主**: 常驻 `hn-daemon`, Unix socket 上跑 JSON-lines
- **开窗**: `hn open` 一次装载本页(headless 不开窗也全可用)
- **推流**: `hn update` 整文档替换, 平滑接上一步内容
</div></div></div>
<div class="msg"><div class="code-card">
  <div class="code-head">
    <span class="code-lang">BASH</span>
    <span class="code-file">agent 会话驱动</span>
  </div>
  <pre>dist/hn-daemon &amp;                      <span class="cmt"># 常驻宿主(Unix socket JSON-lines)</span>
dist/hn open chat examples/agent-chat.html
dist/hn update chat /tmp/next.html    <span class="cmt"># 整文档替换, 平滑接上一步</span></pre>
</div></div>
HTML
STREAM="$STREAM
$STEP4"
push dot-status ab-badge "流式输出中"
"$HN" shot "$ID" screenshots/chat-stream-2.png
echo "[step 4/5] 代码块回答 → screenshots/chat-stream-2.png"

sleep 0.6

# 步 5: 完成态 tag + 页头状态切换
STREAM="$STREAM
<div class=\"done-row\"><span class=\"done-tag\">本轮回答完成 · 留档 3 帧</span></div>"
push dot-status done "已完成"
"$HN" shot "$ID" screenshots/chat-stream-3.png
echo "[step 5/5] 完成态 → screenshots/chat-stream-3.png"

echo "[done] 三帧递进: 半个回答 / 工具卡+代码块 / 完成态"
