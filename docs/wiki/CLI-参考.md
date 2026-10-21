# CLI 参考

## hncore — 引擎离屏 CLI(零 GUI, 三平台逐字节可比)

```
hncore parse  <file.html>            解析并打印 DOM 树
hncore layout <file.html> [W H]      级联+布局, 打印盒模型树
hncore boxes  <file.html> [W H]      每元素绝对盒(CSV, 机器可读)
hncore paint  <file.html> [W H]      绘制指令列表(display list)
hncore text   <file.html> [W H]      文本片段(字节区间 + 位置)
hncore render <file.html> out.png W H   hnsoft 后端出 PNG
hncore renderc <file.html> out.png W H  cairo 后端(同一显示列表)
hncore verify <file.html> [W H]      布局不变量自检(重叠/越界/负尺寸)
```

用途: agent 感知(boxes/text)、CI 视觉回归(render)、管线取证
(paint/verify)。`HN_CLOCK=<ms>` 可把动画推到指定时刻再出图。

## hnapp — 应用壳(链接哪平台 hnp 即哪平台应用)

```
hnapp-macos-arm64    app.html              开窗(帧循环/呈现动效/几何记忆)
hnapp-macos-arm64    app.html --probe      管线自检(platform=…)
hnapp-macos-arm64    app.html --shot out.png [W H] [--advance ms]
hnapp-macos-arm64    --capsule app.hnapp   直启胶囊
hnapp-macos-headless app.html --probe      无 GUI(CI/agent; 合成媒体宿主)
hnapp-linux-x86_64   app.html              dlopen X11
```

产物由 `ZIG=zig bash tools/build-multiplatform.sh` 一条命令出全套。

## hn — daemon 客户端(agent 的系统入口)

```
hn ping
hn open <id> <file.html|-> [--css f.css] [--w N] [--h N] [--window]
hn update <id> <file.html>            热更新(同 id, 窗口与状态保持)
hn close <id>       hn list
hn dump <id>                          绘制指令数(管线健康)
hn text <id> --element <id属性>       元素文本(感知)
hn event <id> --kind click --target <id属性>
hn shot <id> out.png                  无头渲染
hn persist <id> out.hnapp [--instructions "…"]
hn restore out.hnapp
hn applets                            轻应用槽位
```

`hn open card -` 从管道读文档 —— agent 现场生成、秒开。

## hn-daemon — 常驻宿主

```bash
dist/hn-daemon &     # socket: ~/.html-native/hn-daemon.sock, JSON-lines
```

协议 op 全集见 [manual.md §9](../manual.md#9-守护进程与-socket-协议)
(ping/open/update/close/list/eval/dom/dump/text/event/shot/persist/
restore/applets)。daemon 无头优先: 不开真窗也能全链路服务 —— CI/
服务器/agent 同构。任何语言的 socket 客户端都可直接对接。
