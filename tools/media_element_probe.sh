#!/usr/bin/env bash
# tools/media_element_probe.sh — 媒体元素探针(原 media_element_probe.py 的 bash 版)。
# <video>/<audio> 的 DOM 解析 / 状态机 / paint 指令。
#
# 用法:
#     bash tools/media_element_probe.sh [hncore 路径]
#
# 数据源是 hncore(纯 C 引擎 CLI + 内置合成宿主, docs/media-design.md §8.1):
# 梯度帧按时间变色(逐字节可断言), 假时钟只认 tick(HN_CLOCK 16ms 分步),
# 因此断言是确定性的 —— 真解码(AVFoundation)的对应面由 tools/media_probe.c
# 在真机另测, 两者互补。
#
# 覆盖(契约 §3/§5/§8.1):
#   DOM 解析   video/audio 属性(src/autoplay/loop/muted/volume/width/height)
#   布局       替换元素尺寸链(样式 > 属性 > 会话帧固有 > 缺省 16:9; audio 高 0)
#   状态机     IDLE/LOADING/READY/PLAYING/PAUSED/ENDED + autoplay/loop/
#              seek 钳制/音量静音声明链
#   paint      BITMAP 几何 = 元素盒, src=64x64, pts≈时钟, 像素=梯度公式;
#              audio 永不发 BITMAP; 无帧画深底占位 RECT

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

hn_find_hn "${1:-}" "dist/hncore-macos-arm64"
D="$(hn_tmpd mediaprobe)"

NPASS=0; NFAIL=0; FAILED_DESCS=()

# ck <名> <0|1> <现场详情> — 与原版同口径: 失败行"期望 断言成立 实际 详情"
ck() {
  if [ "$2" = "1" ]; then
    NPASS=$((NPASS + 1))
  else
    hn_fail "$1" "断言成立" "${3:-}" 34
  fi
}

# run <w> <h> <cmd>; html 从 stdin 读; 环境钮(HN_CLOCK 等)以 VAR=val 前缀传入
run() {
  local w="${1:-400}" h="${2:-300}" cmd="${3:-paint}"
  cat > "$D/case.html"
  "$HN" "$cmd" "$D/case.html" "$w" "$h"
}

# media_lines <输出> → "id state t dur vol muted loop" 行(文档序, 同 id 后行覆盖)
media_lines() {
  printf '%s\n' "$1" | awk '
    /(^|[ \t])MEDIA[ \t]+id=/ {
      id = ""; st = ""; t = ""; dur = ""; vol = ""; mu = ""; lp = ""
      n = split($0, T, /[ \t]+/)
      for (i = 1; i < n; i++) {
        if (T[i] == "MEDIA") {
          v = T[i + 1]; sub(/^id=/, "", v); id = v
          v = T[i + 2]; sub(/^state=/, "", v); st = v
          v = T[i + 3]; sub(/^t=/, "", v); t = v
          v = T[i + 4]; sub(/^dur=/, "", v); dur = v
          v = T[i + 5]; sub(/^vol=/, "", v); vol = v
          v = T[i + 6]; sub(/^muted=/, "", v); mu = v
          v = T[i + 7]; sub(/^loop=/, "", v); lp = v
          break
        }
      }
      if (id != "") M[id] = id" "st" "t" "dur" "vol" "mu" "lp
    }
    END { for (k in M) print M[k] }'
}

# mget <media 行> <id> <字段 2..7> → 值(缺失 → "")
mget() {
  printf '%s\n' "$1" | awk -v id="$2" -v f="$3" \
    '$1 == id { print $f; ok = 1 } END { if (!ok) print "" }' | head -n 1
}

# bitmap_lines <输出> → "x y w h sw sh pts p00 pNN" 行(文档序)
bitmap_lines() {
  printf '%s\n' "$1" | awk '
    /(^|[ \t])BITMAP[ \t]+[0-9]/ {
      x = $2; y = $3; w = $4; h = $5; sw = ""; sh = ""; pts = ""; p0 = ""; pn = ""
      n = split($0, T, /[ \t]+/)
      for (i = 6; i <= n; i++) {
        if (T[i] ~ /^src=/) { v = substr(T[i], 5); split(v, S, "x"); sw = S[1]; sh = S[2] }
        else if (T[i] ~ /^pts=/) pts = substr(T[i], 5)
        else if (T[i] ~ /^p00=/) p0 = substr(T[i], 5)
        else if (T[i] ~ /^pNN=/) pn = substr(T[i], 5)
      }
      print x, y, w, h, sw, sh, pts, p0, pn
    }'
}

# bget <bitmap 行> <序号 1..> [字段 1..9; 缺省整行]
bget() {
  printf '%s\n' "$1" | awk -v n="$2" -v f="${3:-0}" 'NR == n { if (f == 0) print $0; else print $f }'
}

bcount() {
  if [ -z "$1" ]; then echo 0; else printf '%s\n' "$1" | awk 'END { print NR }'; fi
}

# qnear <名> <got "x y w h"|MISSING> <x> <y> <w> <h> <tol> — 盒几何近似
qnear() {
  if hn_quad_near "$2" "$3" "$4" "$5" "$6" "$7"; then
    NPASS=$((NPASS + 1))
  else
    hn_fail "$1" "断言成立" "got $2 want ($3, $4, $5, $6)±$7" 34
  fi
}

# bnear <名> <bitmap 行> <序号> <x> <y> <w> <h> <tol> — BITMAP 几何近似
bnear() {
  local got
  got="$(printf '%s\n' "$2" | awk -v n="$3" 'NR == n { print $1, $2, $3, $4 }')"
  if [ -z "$got" ]; then got="MISSING"; fi
  qnear "$1" "$got" "$4" "$5" "$6" "$7" "$8"
}

# feq <名> <got> <want> <tol> — 单浮点近似;  feq0 — 精确
feq() {
  if awk -v a="$2" -v b="$3" -v t="$4" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t + 1e-9) }'; then
    NPASS=$((NPASS + 1))
  else
    hn_fail "$1" "断言成立" "got $2 want $3±$4" 34
  fi
}

# ============ 1. DOM 解析(属性一个不少) ============
html='<html><body>
<video id="v" src="clip.mp4" autoplay loop muted width="320" height="180"
 data-duration="2"></video>
<audio id="a" src="tone.m4a" volume="0.5" data-duration="1.5"></audio>
</body></html>'
dom="$(printf '%s' "$html" | run 400 300 parse)"
ok=1
for frag in 'id="v"' 'src="clip.mp4"' 'autoplay=""' 'loop=""' 'muted=""' 'width="320"' 'height="180"'; do
  printf '%s' "$dom" | grep -qF -- "$frag" || ok=0
done
ck "解析: video 属性齐全" "$ok" "$dom"
ok=1
for frag in 'id="a"' 'src="tone.m4a"' 'volume="0.5"'; do
  printf '%s' "$dom" | grep -qF -- "$frag" || ok=0
done
ck "解析: audio 属性齐全" "$ok" "$dom"
# 无 id 的媒体元素也在(display:block 缺省 inline-block 由 UA CSS 给)
v_n="$(printf '%s' "$dom" | grep -o '<video' | awk 'END { print NR }')"
a_n="$(printf '%s' "$dom" | grep -o '<audio' | awk 'END { print NR }')"
ck "解析: video/audio 是普通元素(非 void, 带闭合语义)" \
  "$([ "${v_n:-0}" = 1 ] && [ "${a_n:-0}" = 1 ] && echo 1 || echo 0)" "$dom"

# ============ 2. 布局: 替换元素尺寸链 ============
bx="$(printf '%s' '<html><head><style>html,body{margin:0}video,audio{display:block}</style>
</head><body><video id="v" src="c.mp4" width="320" height="180"></video>
<video id="v2" src="c.mp4" width="160"></video>
<audio id="a" src="t.m4a"></audio></body></html>' | run 400 300 boxes | hn_boxes_lines)"
qnear "布局: video 走 width/height 属性" "$(hn_box_of "$bx" v)" 0 0 320 180 0.6
if [ "$(hn_box_of "$bx" v2)" != "MISSING" ]; then
  # 只有 width 属性: 高 = 宽 * 9/16(缺省 16:9)
  qnear "布局: video 缺省 16:9(160x90)" "$(hn_box_of "$bx" v2)" 0 180 160 90 0.6
else
  ck "布局: video 缺省 16:9 存在" 0 "v2 MISSING"
fi
if [ "$(hn_box_of "$bx" a)" != "MISSING" ]; then
  qnear "布局: audio 兜底高 0" "$(hn_box_of "$bx" a)" 0 270 300 0 0.6
else
  ck "布局: audio 盒存在" 0 "a MISSING"
fi

# ============ 3. 状态机 ============
# 3.1 无 autoplay、无时钟: 会话惰性建立但停在 IDLE + 占位 RECT(非静默空白)
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" width="120" height="68"></video></body></html>' | run 400 300 paint)"
ml="$(media_lines "$out")"
ck "状态机: 无 autoplay 停在 IDLE" "$([ "$(mget "$ml" v 2)" = "IDLE" ] && echo 1 || echo 0)" "$ml"
ck "状态机: IDLE 时长未知(-1)" \
  "$(awk -v a="$(mget "$ml" v 4)" 'BEGIN { exit !(a + 0 == -1.0) }' && echo 1 || echo 0)" "$ml"
if printf '%s' "$out" | grep -q 'fill=14171EFF' && ! printf '%s' "$out" | grep -q 'BITMAP'; then
  ck "绘制: IDLE 画占位 RECT(深底)" 1
else
  ck "绘制: IDLE 画占位 RECT(深底)" 0 "$out"
fi

# 3.2 autoplay、无时钟: open 了但元数据未就绪 → LOADING
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" autoplay></video></body></html>' | run 400 300 paint)"
ml="$(media_lines "$out")"
ck "状态机: autoplay 后 LOADING(时钟未推进)" \
  "$([ "$(mget "$ml" v 2)" = "LOADING" ] && echo 1 || echo 0)" "$ml"

# 3.3 autoplay + 时钟推进: LOADING→READY→PLAYING, cur_time 随假时钟
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" autoplay data-duration="2"></video></body></html>' | HN_CLOCK=416 run 400 300 paint)"
ml="$(media_lines "$out")"
ck "状态机: tick 后 PLAYING" "$([ "$(mget "$ml" v 2)" = "PLAYING" ] && echo 1 || echo 0)" "$ml"
ck "状态机: 时长来自 data-duration" \
  "$(awk -v a="$(mget "$ml" v 4)" 'BEGIN { exit !(a + 0 == 2.0) }' && echo 1 || echo 0)" "$ml"
# 首个 tick 被 LOADING→READY 消耗(就绪后才起播), 25 个播放 tick = 400ms
feq "状态机: cur_time≈假时钟" "$(mget "$ml" v 3)" 0.400 0.017

# 3.4 播完(非循环)→ ENDED, 时间冻结在时长
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" autoplay data-duration="0.1"></video></body></html>' | HN_CLOCK=416 run 400 300 paint)"
ml="$(media_lines "$out")"
ck "状态机: 播完 ENDED" "$([ "$(mget "$ml" v 2)" = "ENDED" ] && echo 1 || echo 0)" "$ml"
feq "状态机: ENDED 时间=时长" "$(mget "$ml" v 3)" 0.100 0.001

# 3.5 loop: 回绕后仍 PLAYING, 时间落在时长内
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" autoplay loop data-duration="0.1"></video></body></html>' | HN_CLOCK=416 run 400 300 paint)"
ml="$(media_lines "$out")"
ck "状态机: loop 回绕后仍 PLAYING" "$([ "$(mget "$ml" v 2)" = "PLAYING" ] && echo 1 || echo 0)" "$ml"
if awk -v a="$(mget "$ml" v 3)" 'BEGIN { exit !(a + 0 >= 0 && a + 0 < 0.1) }'; then
  ck "状态机: loop 时间落在时长内" 1
else
  ck "状态机: loop 时间落在时长内" 0 "$ml"
fi

# 3.6 seek 钳制(环境钮驱动引擎 API, 与 JS hnMediaSeek 同一入口)
base='<html><body><video id="v" src="c.mp4" autoplay data-duration="0.5"></video></body></html>'
out="$(printf '%s' "$base" | HN_CLOCK=160 HN_MEDIA_SEEK=99 run 400 300 paint)"
feq "seek: 越界钳制到时长" "$(mget "$(media_lines "$out")" v 3)" 0.5 0.001
out="$(printf '%s' "$base" | HN_CLOCK=160 HN_MEDIA_SEEK=-3 run 400 300 paint)"
feq "seek: 负值钳制到 0" "$(mget "$(media_lines "$out")" v 3)" 0.0 0.001

# 3.7 pause → PAUSED(时间停在暂停时刻)
out="$(printf '%s' "$base" | HN_CLOCK=160 HN_MEDIA_PAUSE=1 run 400 300 paint)"
ml="$(media_lines "$out")"
ck "状态机: pause → PAUSED" "$([ "$(mget "$ml" v 2)" = "PAUSED" ] && echo 1 || echo 0)" "$ml"
feq "状态机: PAUSED 时间冻结" "$(mget "$ml" v 3)" 0.144 0.017

# 3.8 音量/静音声明链: 元素属性 → 引擎钳制 → 宿主落位(回读)
out="$(printf '%s' '<html><body><audio id="a" src="t.m4a" autoplay volume="0.5" muted
 data-duration="2"></audio></body></html>' | HN_CLOCK=160 run 400 300 paint)"
ml="$(media_lines "$out")"
ck "音量: volume=0.5 落到宿主" \
  "$(awk -v a="$(mget "$ml" a 5)" 'BEGIN { exit !(a + 0 == 0.5) }' && echo 1 || echo 0)" "$ml"
ck "音量: muted 属性生效" "$([ "$(mget "$ml" a 6)" = "1" ] && echo 1 || echo 0)" "$ml"
# 环境钮设音量(验引擎 0..1 钳制路径)
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" autoplay data-duration="2"></video></body></html>' | HN_CLOCK=160 HN_MEDIA_VOLUME=0.25 run 400 300 paint)"
ck "音量: 运行期设 0.25" \
  "$(awk -v a="$(mget "$(media_lines "$out")" v 5)" 'BEGIN { exit !(a + 0 == 0.25) }' && echo 1 || echo 0)" "$(media_lines "$out")"
out="$(printf '%s' '<html><body><video id="v" src="c.mp4" autoplay data-duration="2"></video></body></html>' | HN_CLOCK=160 HN_MEDIA_VOLUME=7 run 400 300 paint)"
ck "音量: 越界钳制到 1" \
  "$(awk -v a="$(mget "$(media_lines "$out")" v 5)" 'BEGIN { exit !(a + 0 == 1.0) }' && echo 1 || echo 0)" "$(media_lines "$out")"
# 时长环境钮覆盖(真宿主读元数据的确定性替身)
out="$(printf '%s' "$base" | HN_CLOCK=160 HN_MEDIA_DURATION=1.5 run 400 300 paint)"
ck "时长: HN_MEDIA_DURATION 覆盖" \
  "$(awk -v a="$(mget "$(media_lines "$out")" v 4)" 'BEGIN { exit !(a + 0 == 1.5) }' && echo 1 || echo 0)" "$(media_lines "$out")"

# ============ 4. paint 指令 ============
blk='<html><head><style>html,body{margin:0}video{display:block}</style></head>
<body><video id="v" src="c.mp4" autoplay data-duration="2"
 width="200" height="150"></video></body></html>'
out="$(printf '%s' "$blk" | HN_CLOCK=416 run 400 300 paint)"
bl="$(bitmap_lines "$out")"
ck "paint: 发出 BITMAP" "$([ "$(bcount "$bl")" = 1 ] && echo 1 || echo 0)" "$bl"
if [ "$(bcount "$bl")" = 1 ]; then
  bnear "paint: BITMAP 几何 = 元素盒" "$bl" 1 0 0 200 150 0.6
  ck "paint: 帧源尺寸 64x64(合成宿主)" \
    "$([ "$(bget "$bl" 1 5)" = 64 ] && [ "$(bget "$bl" 1 6)" = 64 ] && echo 1 || echo 0)" \
    "$(bget "$bl" 1 5)x$(bget "$bl" 1 6)"
  feq "paint: BITMAP pts ≈ 时钟" "$(bget "$bl" 1 7)" 0.400 0.017
  # 梯度帧逐字节: R=x*255/64, G=y*255/64, B=(int(pts*10)*37)%256
  # pts=0.400 → 桶 4 → B=148
  ck "paint: p00 逐字节(0,0,148,255)" "$([ "$(bget "$bl" 1 8)" = "0,0,148,255" ] && echo 1 || echo 0)" "$(bget "$bl" 1 8)"
  ck "paint: pNN 逐字节(251,251,148,255)" "$([ "$(bget "$bl" 1 9)" = "251,251,148,255" ] && echo 1 || echo 0)" "$(bget "$bl" 1 9)"
fi
# pts 前进 → B 桶变色(800ms 时钟 → 49 播放 tick = 784ms → 桶 7 → B=3)
out="$(printf '%s' "$blk" | HN_CLOCK=800 run 400 300 paint)"
bl="$(bitmap_lines "$out")"
if [ "$(bcount "$bl")" -ge 1 ]; then
  p2="$(bget "$bl" 1 8 | awk -F, '{print $3}')"
  pts="$(bget "$bl" 1 7)"
  if [ "$p2" = "3" ] && awk -v a="$pts" 'BEGIN { d = a - 0.784; if (d < 0) d = -d; exit !(d <= 0.017 + 1e-9) }'; then
    ck "paint: pts 前进(784ms → 桶 7 → B=3)" 1
  else
    ck "paint: pts 前进(784ms → 桶 7 → B=3)" 0 "$(bget "$bl" 1)"
  fi
else
  ck "paint: pts 前进(BITMAP 存在)" 0 "no BITMAP"
fi

# 4.1 audio 永不发 BITMAP(纯音频会话), 但状态机照常推进
out="$(printf '%s' '<html><body><audio id="a" src="t.m4a" autoplay data-duration="2"></audio></body></html>' | HN_CLOCK=416 run 400 300 paint)"
if printf '%s' "$out" | grep -q 'BITMAP'; then
  ck "audio: 无 BITMAP" 0 "$out"
else
  ck "audio: 无 BITMAP" 1
fi
ck "audio: 状态照常 PLAYING" \
  "$([ "$(mget "$(media_lines "$out")" a 2)" = "PLAYING" ] && echo 1 || echo 0)" "$(media_lines "$out")"

# 4.2 双元素: 两个 BITMAP(几何各随其盒), MEDIA 行一个不少
out="$(printf '%s' '<html><head><style>html,body{margin:0}video{display:block}</style></head><body>
<video id="v1" src="c.mp4" autoplay width="120" height="68"></video>
<video id="v2" src="c.mp4" autoplay width="80" height="45"></video>
</body></html>' | HN_CLOCK=416 run 400 300 paint)"
bl="$(bitmap_lines "$out")"
ck "paint: 两个 video 两条 BITMAP" "$([ "$(bcount "$bl")" = 2 ] && echo 1 || echo 0)" "$bl"
if [ "$(bcount "$bl")" = 2 ]; then
  ck "paint: BITMAP#1 几何 120x68" \
    "$(awk -v w="$(bget "$bl" 1 3)" -v h="$(bget "$bl" 1 4)" 'BEGIN { exit !(w + 0 == 120 && h + 0 == 68) }' && echo 1 || echo 0)" "$(bget "$bl" 1)"
  ck "paint: BITMAP#2 几何 80x45" \
    "$(awk -v w="$(bget "$bl" 2 3)" -v h="$(bget "$bl" 2 4)" 'BEGIN { exit !(w + 0 == 80 && h + 0 == 45) }' && echo 1 || echo 0)" "$(bget "$bl" 2)"
fi

# 4.3 确定性: 同文档同环境钮两次运行输出逐字节一致
o1="$(printf '%s' "$blk" | HN_CLOCK=416 run 400 300 paint)"
o2="$(printf '%s' "$blk" | HN_CLOCK=416 run 400 300 paint)"
ck "确定性: 两次运行逐字节一致" "$([ "$o1" = "$o2" ] && echo 1 || echo 0)"

# 4.4 无媒体元素的文档: 输出零 BITMAP/MEDIA 行(零激活)
out="$(printf '%s' '<html><body><div id="d" style="width:50;height:30"></div></body></html>' | HN_CLOCK=416 run 400 300 paint)"
if printf '%s' "$out" | grep -q 'BITMAP' || printf '%s' "$out" | grep -q 'MEDIA'; then
  ck "无媒体文档: 零 BITMAP/MEDIA 行" 0 "$out"
else
  ck "无媒体文档: 零 BITMAP/MEDIA 行" 1
fi

# ============ 报告 ============
hn_harness_report
