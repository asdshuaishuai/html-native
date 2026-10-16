# ISSUES-1 — 参赛录屏准备轮遇到的问题

> 记录: 2026-10-16 · 触发场景: 为 OSCC 2026 参赛视频重录 `hn-mesh` 网格动画分片
> （去掉宠物模块演示 → 把网格变形改作"一种动画行为"演示 → 合并新合集入库）
> 改动落点: `Sources/CHtmlNative/hn_layout.c`、`Sources/CHtmlNative/hnsoft.c`、`tools/layout_probe.sh`、`tools/render_probe.c`
> 口径同 `ISSUES.md`: **P0** 会咬人 / **P1** 功能缺陷 / **P2** 工具链与文档
> 状态: ✅ 已修+探针锚定 · ⚠️ 已绕开未修 · ⏳ 待修 · ℹ️ 环境/工具限制

---

## 一、引擎缺陷（已修，探针已锚定）

### P1-A 脱离流嵌套 → 同一句文字被画两遍

**现象**
`position: absolute` 的子元素嵌在一个**定位父**里时，只有**文字**被画两遍 —— 第二份整体偏移
`(left, top)`；背景 `RECT` 仍只有一份。把同一个子元素直接挂 `body` 下、或把子元素改成
`position: static`，都**不复现**。

**根因**（`hn_layout.c`）
`layout_out_of_flow()` 在 `layout_box(ch, 0, 0, …)` 之后**又手动递归了一次** `layout_out_of_flow(ch)`；
而 `layout_box` 尾部（"容器尺寸已定 → 摆放脱离流的子节点"，现 `hn_layout.c:1657`）**已经**调过
同一个递归。于是孙级脱离流子树被整棵重排两遍。

盒坐标因 `translate_subtree` 平移量幂等（第二遍先归零再平移回同一处）而**看不出异常**，
但 `run_push` 只追加不清空（`hn_layout.c:88`），文本 run 直接翻倍 → 显示列表里同一句话两条。

`absolute` 挂在 body 下不炸：那一层的 `ch` 没有脱离流后代，多余递归是空转。所以这个 bug
只在"**absolute 套 absolute**"（或 fixed 套 absolute）时才现形 —— 也是最常见的一类浮层写法。

**判据**（钉死口径的证据）
`240x120` 画布，`.stage{position:absolute}` 内放 `.chip{position:absolute;left:22;top:20}`：

```
修复前  hncore text  →  6 段 run
  x=57.0  "hn-mesh"   ← 多出来的，整体 +22/+20
  x=113.2 "·"
  x=130.0 "网格变形"
  x=35.0  "hn-mesh"   ← 正确的三份
  x=91.2  "·"
  x=108.0 "网格变形"
修复后  hncore text  →  3 段 run（只剩 35.0 / 91.2 / 108.0）
```

**修法**
删掉 `layout_out_of_flow()` 里那次冗余递归（原 `hn_layout.c:830-831`），并在原位留注释说明
"上面那次 `layout_box(ch)` 走到尾部已调过一次"。`layout_box` 的早退分支只有输入控件
（`hn_layout.c:1620` 附近），输入控件不会有脱离流后代，删除安全。

**探针**
`tools/layout_probe.sh:195` 新增「脱离流嵌套」段：同一句文字在三种挂法下（直接挂 body /
嵌 absolute 父 / 嵌 relative 父）的 `hncore paint` TEXT 指令条数**必须相等**。
不硬编码条数 —— 中文在字体回退下的切段数随 FreeType 有无而变，**相等性**才是与字体无关的不变量。

- 旧二进制：`通过 37 / 失败 1`（`✘ absolute 嵌 absolute: 文字不得翻倍 期望 1 实际 2`）
- 修复后：`通过 38 / 失败 0`

**影响面**：所有"定位父里放绝对定位子元素"的页面 —— 浮层、气泡、角标、`.card` 里的绝对定位标签
都会重影。属于会咬人的一类。

---

### P1-B 圆角矩形四边各缩 r/2（`border-radius:50%` 只画出半个圆）

**现象**
`border-radius > 0` 的形状整体比声明的盒子**小一圈**，四边各缩 `r/2`。白底 `100x100` 红块实测：

| `border-radius` | 中线实心范围 | 应为 |
|---|---|---|
| 0 | 20..119（100 宽） | ✅ |
| 10 | 25..114（**90** 宽） | 100 宽 |
| 25 | 33..106（**74** 宽） | 100 宽 |
| 50 | 45..94（**50** 宽） | 100 宽（直径 100 的圆） |

因为症状是"**收缩**"而不是溢出，单看截图极容易被当成"设计如此"（一张卡片小两像素谁能看出来），
**只有量中线上的实心范围才会露馅**。半径越大越离谱：`border-radius:50%` 的圆形元素
（头像、状态点、圆形按钮）实际只画出**一半直径**。

**根因**（`hnsoft.c:316 sd_rounded`）
内部项写成 `fmaxf(qx, qy)`，标准圆角盒 SDF 是 `min(fmaxf(qx, qy), 0)`。
少了 `min(..., 0)` 之后，"落在矩形内、但在圆角安全区（内缩 r 的那块）之外"的**四条直边带**
被判成外部 —— 直边被从两端各削掉 `r/2`。

```c
/* 错误 */ return sqrtf(ax*ax + ay*ay) + fmaxf(qx, qy) - r;
/* 正确 */ return sqrtf(ax*ax + ay*ay) + fminf(fmaxf(qx, qy), 0.0f) - r;
```

**演进史（值得记一笔，同一行被改错了两次）**

1. 最早：`(qx > qy ? qx : (qy > qx ? qy : 0))` —— `qx == qy` 时落到 `0`。
   正方形矩形的中心恰好 `qx == qy`，于是"内部距离"整项丢失、`d` 塌成 0、覆盖率 `cov = 0.5`，
   正中心那格只有半透明（不透明色变成 50% alpha），45° 对角线全受影响。
2. 改成 `fmaxf(qx, qy)` 修掉了中心 —— 但**同时引入了本文这条直边内缩**。
3. `min(fmaxf(qx, qy), 0)` 两者都对：中心处 `qx == qy < 0` 取 `qx`；直边带 `qx > 0, qy < 0` 取 `0`
   （不改半径项）。这也才是 IQ 圆角盒 SDF 的标准式。

**连带修复**：`sd_rounded` 同时喂给 `CLIP+`（`hnsoft.c:292`，圆角裁剪）与阴影环
（`hnsoft.c:561`），所以圆角裁剪也一并修正。实心快路（`hnsoft.c:506`）用的是
`qx <= -0.5f && qy <= -0.5f` 的保守"必定实心"谓词，在修正后的公式下仍成立
（此时 `cov ≥ 1 + r` 会被钳到 1），**无需改动且与通用路径逐位一致**。

**探针**
`tools/render_probe.c:572` 新增 `test_rounded()`：r = 0/10/25/50 四档，
断言水平/垂直中线的实心范围**都必须是 20..119**（圆角只削四角，直边必须顶满盒），
外加"r ≥ 25 时左上角必须被削掉"（防止矫枉过正成"圆角不生效"）与"盒中心必须实心"。

- 旧引擎：`结果: 71 通过, 12 失败`
- 修复后：`结果: 83 通过, 0 失败`（`render_probe` 总断言数 61 → 83）

**影响面（需要跟进的存量资产）**
- 全仓**每一个**圆角元素此前都画小了，圆角越大越明显。
- `docs/screenshots/` 下 6 张截图（`dashboard.png` / `dev-app.png` / `inputs-focus.png` /
  `message-card.png` / `showcase.png` / `webpage.png`）都是**旧几何**拍的，被
  `README.md` / `docs/index.html` / `docs/threejs-support.md` 引用 → **口径已漂**。
- `docs/oscc-2026/recordings2/` 里 `01-holodeck` / `03-threejs` / `04-agent-stream` / `05-fmt`
  四个分片同样是旧引擎录的；只有新录的 `02-mesh-anim` 是新几何。

---

## 二、引擎缺陷

### P1-C 透明真窗 + Lottie：只渲染动画的一角 ✅ 无法在干净路径复现 — 判定为观察污染（C2/资产路径）

**原始现象（2026-10-16 录屏轮）**

| 路径 | 结果 |
|---|---|
| `hn-transparent=1` 真窗 | **只画出 6 个气泡里的 1 个**（其余不出现） |
| `hn-transparent=0` 真窗 | 正常，5/6 个气泡出现（第 6 个被窗边裁掉，合理） |
| `hncore render` 离屏（952x784 与 440x362 都试过） | **完全正常** |

**2026-10-15 复核（修复轮）——两代二进制 A/B + 受控实验，症状不复现**

- 当前构建（7e7d2bf + sd_rounded 修复）：`hn-transparent=1` 真窗（daemon spawn，
  ffmpeg 抓副屏第 2 秒帧）——**6 个气泡全部实心在场**（打字/感叹/齿轮/扳手/身体部件）。
- 旧构建（2119239，parent 骨架链修复**之前**，worktree 重建）：同页面同开法——
  **同样全部气泡在场**。即症状在两代引擎上都无法重现。
- 期间两次"复现出棋盘格/残缺"的抓帧，事后查明是**测试脚手架自己的错**：
  把页面复制到 /tmp 却没带 `assets/rec-chatbot.json`，lottie 加载失败 → 空页
  （窗口只剩清屏色 + 加载失败的点阵占位）。与"transparent=1 才坏"无关。

**根因推断**
原始观察发生在 nohup 守护进程被反复回收的排查环境里（本文件 C2 自证：
"连续三轮改了没变化……根本没有窗口开着"）。"1/6 气泡"最可能是**陈旧守护进程
spawn 的旧壳 + 相对资产路径解析差异**（daemon CWD ≠ 页面 CWD 时
`src="assets/rec-chatbot.json"` 落空）叠加的假象，而非引擎缺陷。

**回归锚**
`tools/layout_probe.sh`「百分比高度链」两条断言（body height:100% → img 100%
铺满视口 / html,body 100% + flex 居中）钉住该页面骨架的布局不变量
（38 → 40 条）。真窗抓帧不进自动门禁（依赖副屏与桌面环境），复核口径保留在上文。

**教训（比结论更重要）**
1. 页面复制到别处 = 资产路径必断；"换个目录跑就坏"先查相对路径。
2. 真窗类缺陷的复核必须固定：同一个受管守护进程、同一条 spawn 命令、
   同一资产布局——三个变量任何一个漂了，现象就不可信。
3. `hn shot` 不支持真窗（headless only）；副屏真窗取证走 ffmpeg avfoundation
   （C3/C4 口径），窗口自身取证走 `screencapture -l <CGWindowNumber>`。

---

## 三、工具链/流程陷阱（ℹ️ 非引擎缺陷，但会直接误导判断）

### C1 探针缺省二进制是 `/tmp/hncore`，可能是**陈旧副本** ℹ️

`tools/_engine_sources.sh:375 hn_find_hn "${1:-}" "/tmp/hncore"` —— 不传参时走 `/tmp/hncore`。
本机那个文件是 **2026-10-12** 的旧构建。于是"改完引擎跑探针"会拿到**旧二进制的结果**，
看起来像"修复没生效"。

`lottie_probe.sh:90` 的注释其实已经提醒过（"`/tmp/hncore` 可能是旧构建"），但默认路径仍指向它。

- **踩坑实况**：`bash tools/layout_probe.sh`（不传参）在修复后仍报
  `✘ absolute 嵌 absolute: 文字不得翻倍 期望 1 实际 2`，一度以为是修复无效；
  换成 `bash tools/layout_probe.sh ./dist/hncore-macos-arm64` 立刻 `38 / 0`。
- **建议**：跑探针一律显式传 `./dist/hncore-macos-arm64`；或把默认值改掉，
  或在 `hn_find_hn` 里对 `/tmp/hncore` 打印一行"用的是 /tmp 副本，mtime=…"的醒目提示。

### C2 daemon 不能用 `(... &)` / `nohup` 起 ℹ️

`(HN_ANIM_FPS=60 ./dist/hn-daemon &)` 起来的守护进程会被回收 → `hn ping` 报
`daemon 未响应`、`hn open --window` 后根本看不到窗口。**必须用受管后台任务**（有 task id、
能被追踪与回收的那种）启动。这条与 P1-C 的"三轮无效改动"直接相关。

### C3 `screencapture` 与 `ffmpeg avfoundation` 的屏幕编号口径不一致 ℹ️

- `screencapture -x`：只拿到 **3440x1440** 主屏，副屏不在其中。
- `ffmpeg -f avfoundation -i "1:none"`：抓的是 **1920x1080 副屏**（`0:none` = 主屏）。

结果是用 `screencapture` 自检"窗口有没有出现在副屏上"时**截错了屏**，看不到自己的窗口。
自检副屏一律走 ffmpeg 抽帧（或 `-D` 指定 display）。

另：副屏的**全局坐标**是 `x ∈ [-1920, 0]`，窗口 `@x,y` 用的是全局坐标，
换算到 ffmpeg 抓的副屏图里要 **+1920**。本轮窗口 `@-1200,330` → 画面内 `(712,322)`，
实测 bbox `(712,322)-(1206,756)` 与换算一致。

### C4 `ffmpeg -t 1 … -y out.png`（image2 封装）会挂死 ℹ️

想"抓一帧"而输出 `.png` 时进程会一直不返回（本机实测被外层超时杀掉，退出码 137）。
输出 mp4（`-c:v libx264`）再抽帧则正常。另：本机 ffmpeg **没有 `drawtext` 滤镜**，
抽帧叠编号的写法不可用，改用 PIL 拼胶片。

### C5 `concat` demuxer 会破坏屏幕录制的时长元数据 ℹ️

用 concat demuxer 合并 5 段共 74s 的屏幕录制，`ffprobe` 报出的时长是 4804s
（`avg_frame_rate` 塌成 925/2002）；加 `-r 30` 反而更糟（95MB / 4811s）。
必须走 `filter_complex "...concat=n=N:v=1:a=0"`（本轮新合集即用此法，74s 正确）。

### C6 `hncore text` 与 `hncore paint` 对最后一段 run 的打印不一致 ℹ️

同一份页面，`hncore text` 把末尾 run 打印成 `"引擎逐帧重算网格"`（截断），
`hncore paint` 打印成 `"引擎逐帧重算网格顶点"`（完整）。显示列表里是完整的，
看着像文字被吞了两个字，排查时会带偏方向。

---

## 四、既存失败（复核：均为陈旧二进制伪故障，已消除）

2026-10-15 用重建的 `dist/hncore-macos-arm64` 复核：

| 探针 | 录制轮结果 | 复核结果 | 结论 |
|---|---|---|---|
| `tools/lottie_probe.sh` | `通过 15 / 失败 1`（`C 级 zip 探针失败`） | **通过 16 / 失败 0** | 旧 `/tmp/hncore` 缺 zip 探针新增断言所致（C1 陷阱本体） |
| `tools/media_probe.sh` | `17 ok, 1 fail` | **18 ok, 0 fail** | 同上，样例候选数随系统媒体文件变化 |
| `timer_probe` 8c | 2 断言红 | 已由 GIF fill-alpha 根因修复治愈（`7e7d2bf`），timer 59/0 | 见 `ISSUES.md` |

三条都指向同一个教训：**探针红先看二进制 mtime，再谈引擎**（即 C1）。

---

## 五、本轮改动后的全量探针复核

全部通过（`dist/hncore-macos-arm64`，2026-10-16 重建）：

| 探针 | 结果 |
|---|---|
| `layout_probe` | 38 / 0（含新增 3 条脱离流断言） |
| `render_probe` | 83 / 0（含新增 22 条圆角几何断言） |
| `css_probe` | 96 / 0 |
| `anim_probe` | 31 / 0 |
| `gif_probe` | 61 / 0 |
| `md_probe` | 107 / 0 |
| `fmt_probe` | 118 / 0 |
| `include_probe` | 23 / 0 |
| `events_probe` | 58 / 0 |
| `stream_probe` | 30 / 0 |
| `canvas_probe` | 178 / 0 |
| `quad3d_probe` | 33 / 0 |
| `hidpi_probe` | 全部通过 |
| `http_probe` / `sysroute_probe` | PASS |

`build-multiplatform.sh` 自带门禁亦全绿：布局不变量检查、**跨架构确定性**
（dashboard / showcase / webpage / lottie / transparent 5 个文档，同 `HN_NO_TEXT` 口径逐字节一致）。

---

## 六、待办（本轮暴露，未做）

1. **重拍截图**：`docs/screenshots/` 6 张是旧圆角几何，口径已漂 → 用新引擎重出并核对
   `README.md` / `docs/index.html` / `docs/threejs-support.md` 的引用。
2. **重录存量分片**：`recordings2/01-holodeck` / `03-threejs` / `04-agent-stream` / `05-fmt`
   四个分片是旧引擎录的；若要合集内部几何一致，需按新引擎重录。
3. ~~**P1-C** 待用户侧修完后回归~~ → ✅ 已复核结案（见第二节：干净路径不可复现，
   判定观察污染；布局不变量已进 `layout_probe`，40/0）。
4. ~~**C1** 探针缺省二进制~~ → ✅ 已裁决落地：`hn_find_hn` 不传参时优先
   `dist/hncore-<os>-<arch>`，落到 `/tmp` 副本时打带 mtime 的醒目告警。

---

## 七、2026-10-15 H5 标准扩展轮（后续追加）

本文件前六节记录录屏准备轮；本轮（dev 分支）在其基础上做了 H5 标准扩展与
canvas 兼容增强，顺带解决两处"探针会骗人"的工具陷阱：

**根因修复（引擎）**
- `arcTo` 弧向判定重写：旧判据（v1×v2 叉积 → ccw）在 y-down 屏幕坐标下
  不成立 —— "向右走向上转"与"向左走向下转"叉积同号但短弧方向相反，
  直角会画出 270° 长弧（`roundRect` 全坏、一切用 arcTo 的页面弧线画错）。
  新判据：短弧候选 + 中点方向必须朝顶点（弧向顶点鼓出）。此前**没有任何
  探针覆盖 arcTo** —— 现由 `dom_probe` roundRect 像素断言钉住。
- canvas `globalAlpha` 下送列色是负 int32（位运算产物），JSON 负数到引擎
  侧丢失 → alpha 混合完全不落像素。`>>>0` 归正。
- EVENT/CANVAS glue 内 JS 正则的 `\s`/`\w` 被 C 字符串转义吃掉
  （`split(/\s+/)` 变成按字母 s 分割）—— classList 全套失效且无报错。
  glue 装载失败现在会打 `[canvas-glue]` 一行 stderr（此前静默吞）。

**标准面新增**
- `<template>` 内容不再渲染（UA `display:none`）；`<button>`/`<input type=
  button|submit|reset>` 按钮控件（收缩包裹 + 居中标签 + 缺省皮肤, 不可编辑/
  不进 Tab 焦点链）；语义行内/块级标签 UA 皮肤补全（mark/blockquote/
  figure/summary/details/center/dl 族等）。
- CSS：`text-transform`（ASCII, 度量绘制同源）/ `aspect-ratio`（一维定
  另一维 auto 补齐）/ `overflow-x`（单轴模型别名, 如实声明）/
  `:root` / `:only-child`。
- JS DOM：textContent get、getAttribute/setAttribute/removeAttribute/
  hasAttribute、classList（add/remove/toggle/contains）、dataset（camel↔data-dash
  写透）、style 代理（setProperty/cssText）、checked/disabled/hidden、
  querySelector/querySelectorAll（引擎新增 `hn_doc_query_selector` ——
  选择器语法与样式表同一套, `hn_css_parse_selgroup` + `hn_style_selgroup_match`）、
  createElement/appendChild（移动语义）/remove、children/parentElement/
  firstElementChild/lastElementChild、focus、document.body/documentElement、
  getElementById null 语义（游离/不存在 → null）。命中元素无 id 时现场补
  唯一 `el-N`（桥按 id 寻址的必然要求）。
- Canvas：rect/roundRect/getTransform/createImageData/isPointInPath（像素
  近似, 如实声明）/toDataURL（`hn_canvas_get_image_data` → PNG → base64,
  新增 `__hnCanvasPng` 桥）/globalAlpha（统一乘点 rgbaGA）/lineJoin/miterLimit。
- 根背景画布传播（浏览器标准）：html/body 实色背景 → 画布清屏色
  （`hn_doc_canvas_background`；rt_raster / daemon shot / hncore render 三口
  同源; 透明窗不传播 —— 逐像素 alpha 契约优先）。

**探针**
- 新增 `tools/dom_probe.{c,sh}`（38 断言：DOM 标准面 28 + Canvas 扩展 10,
  关键处 C 侧交叉验证"写透"）。
- `layout_probe` 40→43（按钮收缩/百分比高度链）；`css_probe` 96→102
  （aspect-ratio/:root/:only-child）；`html_probe` 37→43（template/
  text-transform/mark）。
- 本轮教训入档：探针红先核对**被测二进制 mtime**（C1 的第三次变体——
  css_probe 的 dist 陈旧又骗了一轮）。
