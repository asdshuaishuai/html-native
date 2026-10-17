#!/bin/bash
# _secondary_screen.sh — 副屏摆位助手(测试/验收窗口不占主屏)
#
# 用法: POS=$(bash tools/_secondary_screen.sh [W H])   → "X,Y" 或空
#   W H 缺省 360x360(只影响居中算位)。输出直接喂 hnapp 的 HN_WINDOW_POS
#   环境钮:   export HN_WINDOW_POS="$(bash tools/_secondary_screen.sh 360 360)"
#   坐标口径同 manifest hn-window @x,y(左上原点, y 向下)。
#
# 多屏: 取 NSScreen.screens[1](第一块副屏), 把 WxH 摆其中心;
#   y 翻转 = 主屏高 − y_appkit − h(与 app/hn_app.c·hnp_macos.c 同口径)。
# 单屏/探测失败: 输出空 —— 调用方回落缺省(主屏居中), 行为不变。
set -u
W="${1:-360}"; H="${2:-360}"
osascript -l JavaScript - "$W" "$H" <<'JXA' 2>/dev/null
ObjC.import("AppKit");
function run(argv) {
    const w = parseFloat(argv[0]), h = parseFloat(argv[1]);
    const ss = $.NSScreen.screens;
    if (ss.count < 2) return "";
    const pri = ss.objectAtIndex(0).frame;   // 主屏(菜单栏所在; AppKit 左下原点)
    const f = ss.objectAtIndex(1).frame;     // 第一块副屏
    const x = f.origin.x + (f.size.width - w) / 2;
    const yAk = f.origin.y + (f.size.height - h) / 2;
    const y = pri.size.height - yAk - h;     // 左下原点 → 左上原点
    return Math.round(x) + "," + Math.round(y);
}
JXA
