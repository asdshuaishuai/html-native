# html-native 上手指南(人类开发者版)

> 你只需要会 HTML 和 CSS,就能在 macOS 上做出**原生渲染**的小应用。
> 没有浏览器、没有 WebView、没有 Node、没有构建步骤、没有包管理器。
> 一个文本编辑器 + `hn` 命令,就是全部工具链。

## 它是什么

html-native 是一个系统级小程序引擎(定位同 RN / Flutter,但 UI 语言是 HTML):

- **写 HTML/CSS → 得到原生窗口**。引擎(C99 核心)把标记渲染成真正的
  `NSWindow` / 浮层弹窗,不是网页。
- **应用即文件**。一个小应用就是目录里的 `app.html`(+ 可选 `app.css`),
  双击不存在的概念——`hn` 命令秒开、秒销毁。
- **秒级反馈**。开发模式下保存文件,窗口立刻热更新,不用重启任何东西。
- **系统能力开箱即用**。读 CPU/内存/磁盘/运行时长/主机名、本地持久化、剪贴板、
  系统通知、打开 URL,全部是一行 `sys://` 地址,零网络、零依赖
  (电池与全量系统卡见下文校准注)。

AI agent 可以生成这些 HTML 直接推给引擎成窗;人类也可以直接手写——
这份指南就是写给你(人类)的。

## 5 分钟上手

```bash
cd html-native
ZIG=zig bash tools/build-multiplatform.sh   # 一次性构建(只需 zig cc, 无 Xcode 工程)
bash tools/build-daemon-cli.sh              # 产出 dist/hn-daemon(宿主) + dist/hn(CLI)

dist/hn-daemon &                            # 常驻宿主
mkdir myapp && cd myapp                     # 应用即文件: 手写两个文件即可
dist/hn open myapp app.html --css app.css
```

内容已渲染(无头槽位)。现在做三件事,感受开发循环
(要让窗口真出现在桌面上, 见下一条):

1. **改文字**:把 `app.html` 里 `<h1>myapp</h1>` 改成任意标题,⌘S 保存——
   `hn update` 后界面同步变化(窗口应用则由壳的 mtime watch 自动重载)。
2. **改样式**:把 `app.css` 里 `--accent: #4f7cff` 换成 `#ff6b6b`,保存——
   保存按钮连同主题色一起换(CSS 变量贯穿整个界面)。
3. **用存储**:在"你的名字"输入框打字,点保存,关掉窗口重新 `hn open`——
   值还在(本地 JSON 持久化,存在 `~/.html-native/store/`)。

上面是 **daemon + 无头槽位**的开法。要让窗口真的出现在桌面(原生窗, 每窗一进程),
给 `open` 加 `--window`:

```bash
dist/hn open myapp app.html --window        # daemon spawn 平台壳, 回 {"pid":N,"window":true}
dist/hn update myapp app.html               # 热推送新内容到子进程(spawn 的源文件原子写回)
dist/hn close myapp                         # SIGTERM 回收, 关窗即退
```

窗口应用上 `eval`/`shot`/`dump` 不可用(文档在子进程里, daemon 无句柄)——
**headless 是 agent 的渲染/截图管线, window 是给人看的桌面窗**。真窗口下的
热重载由壳自带的 mtime watch 完成(源文件保存 ~300ms 内界面更新, 进程与窗口不动)。

`hn dev` 的本质:引擎 watch 源文件 mtime(`hn_app.c` `HN_WATCH_INTERVAL_MS=300`,
即 ~300ms 轮询),变了就以同 id 热推送新编码,窗口对象与进程不动,只有内容重排。
注: `hn dev`/`hn new` 两个 CLI 子命令**尚未包装**(路线图; 现用 `hn update`
或 `--window` 开窗即等价热开发循环, 详见 README「已知简化与路线图」)。

## 应用解剖

一个最小应用就是两个文件(`hn new` 脚手架在路线图上):

```
myapp/
  app.html     界面 + 清单(meta 声明表面/尺寸/标题)
  app.css      样式(CSS 变量做主题)
  head.html    可选: 被 <include> 的片段(演示零构建组件)
```

`app.html` 骨架:

```html
<html><head>
<meta name="hn-surface" content="window">      <!-- window | popup | layer -->
<meta name="hn-window"  content="420x520">      <!-- 宽x高,可加 @x,y -->
<meta name="hn-title"   content="myapp">
<include src="head.html">                       <!-- 引入片段(可嵌套) -->
</head><body>
  <div class="card">…</div>
</body></html>
```

三种表面(`hn-surface` 由引擎解析, 物化取决于壳; 桌宠另有一组 meta, 见下):

| surface | 物化成 | 适合 |
|---|---|---|
| `window` | 常规原生窗(NSWindow / X11 / Win32) | 正式小应用 |
| `popup` | 低存在感窗口(不抢焦点语义; `hn-presence=ghost` 缺省) | 消息卡、agent 输出 |
| `layer` | 弱存在感小组件(`hn-level="floating"` 悬浮于普通窗口上) | 常驻小组件 |

注: 当前 C99 平台层(`hn_app` + `hnp_*.c`)的开窗能力聚焦"桌宠一组 meta":
`hn-transparent`(逐像素透明)/`hn-frameless`(无框)/`hn-level=floating|desktop`/
`hn-spaces=all`/`hn-shadow=0`/`hn-click-through` —— macOS 已物化, Linux 逐字段
对齐(`tools/linux_parity_check.sh` 编译级对账, 行为级需真机), Windows/headless
优雅忽略。`popup`/`layer` 的精细物化(不抢焦点浮层等)在壳层路线图上。

## 交互(htmx 风格)

不需要 JavaScript。声明式属性说清"点谁 → 拿什么 → 换到哪":

```html
<div hx-get="sys://cpu" hx-target="cpu-out" hx-trigger="click">刷新</div>
<div id="cpu-out">…</div>

<span hx-get="sys://uptime" hx-trigger="load every 2s">…</span>  <!-- 自动轮询 -->

<div hx-post="sys://store/set?key=name"
     hx-target="name-out">保存</div>   <!-- POST 自动携带表单字段 -->
```

- `hx-trigger`:`click`(默认)/ `load`(载入即发)/ `every Ns`(轮询)。
- GET 请求自动把页面上 input 的值拼进 query;POST 放 body。
- `hx-target` 指向元素 id,响应片段原位换入(`hx-swap` 语义为 outerHTML)。
- 自定义后端:运行时可注入任意 transport(进程内回调或 `sys://` 桥),
  所以 `hx-*` 不绑定 HTTP——`sys://` 就是纯本地应答。

## 系统能力速查(sys://)

全部本地、零网络,响应自带样式片段,换入即用
(下表为 C99 运行时 `rt/hn_rt.c` 的 `hn_rt_sys_fragment` 实测路由; 2026-10-13 逐条实跑验证):

| 地址 | 作用 |
|---|---|
| `sys://cpu` / `sys://memory` / `sys://disk` / `sys://uptime` / `sys://host` | 系统单项数据(CPU 采样占用 / 内存 / 磁盘 / 运行时长 / 主机名) |
| `sys://store/get?key=k` | 读本地 KV(该应用持久化值) |
| `hx-post="sys://store/set?key=k"` | 写本地 KV(表单 `value` 字段) |
| `sys://clipboard/get` / `hx-post="sys://clipboard/set"` | 剪贴板读写 |
| `hx-post="sys://notify"` | 系统通知(`text` 字段) |
| `sys://open?url=…` | 用系统默认方式打开 URL/文件 |

> 校准注: `sys://info`(全量卡)与 `sys://battery` **不在** C99 运行时路由表里
> (只在 Windows 的 `tools/sysbridge.c` 桥里; Linux 壳的 `sys_fragment` 是待移植
> 占位)。全量卡可由页面自己拼 `cpu`/`memory`/… 片段得到; 电池是路线图。

## 样式能力(CSS 子集)

日常够用的部分都已实现:

- 选择器:tag / `.class` / `#id` / 复合 / 后代 / **`>` 子代 / `+` 相邻 / `~` 兄弟** /
  分组 / `:hover` `:active` `:focus` / `:nth-child(odd|even|N)` /
  `:first-child` `:last-child`(条纹列表一行 CSS 就够);
  级联特异性 + 继承 + 行内 `style`。
- 布局:`display: block / flex / inline / inline-block / inline-flex / none`,
  flexbox 全家(direction/justify/align/gap/grow/shrink/basis),
  行内排版与 CJK 换行,overflow 裁剪+滚动(带指示条);
  `margin: 0 auto` 水平居中、flex 里 `margin-left: auto` 把元素推到最右,
  `min/max-width/height` 约束齐备——网页排版习惯原样可用。
- 视觉:盒模型、圆角、`linear-gradient` 渐变、`box-shadow`、`opacity`、
  `transition` 过渡动画、`cursor`。
- 主题:`--var` 自定义属性(继承/覆盖)+ `var(--x, fallback)` 代换。
- 响应式:`@media (min-width/max-width)`,窗口缩放即刻重排。
- 颜色 `#hex(含 #fff 三位)/ rgb(a) / hsl(a) / transparent` / 36+ 命名色;
  单位 **px / pt / % / em / rem / vw / vh**;
  `border-radius: 50%` 画圆;`text-decoration: underline` 装饰线;
  `font-family: monospace / serif`(系统等宽/衬线设计,`<code>` 默认等宽)。

## 一行主题, 免写样式

head 里声明主题, 引擎注入一整套设计令牌基底(暗/亮):色彩系统、字阶行距、
卡片/按钮/输入框/骨架屏默认样式、sys:// 片段皮肤、卡片投影与 hover:

```html
<meta name="hn-theme" content="dark">
```

你可以直接用这些类:`.card` `.btn` `.cap` `.sub` `.row` `.grid` `.sk`(骨架条),
以及变量 `var(--accent)` `var(--card)` `var(--ink)` 等; 自己的 css 写在后面即可覆盖任何令牌。
`examples/dashboard.html` 就是零视觉样式(只写布局)用主题做出来的。

## 网页习惯直接用

你可以完全按给浏览器写页面的习惯来写:

- 文件开头 `<!DOCTYPE html>`、注释 `<!-- -->` 都没问题;
- 样式用 **`<link rel="stylesheet" href="app.css">`** 外链(相对路径),
  不必再走 `--css` 参数;
- 语义标签 `<header> <nav> <article> <footer> <section>` 按块级渲染,
  `<ul>/<ol>` 自带列表标记,`<a>` 蓝色下划线,`<hr>` 分隔线,`<strong>/<em>` 加粗斜体;
- CSS 里 `hsl(215, 80%, 55%)`、`rgba(0,0,0,.5)`、`#fff`、`1.25rem`、`50vw`
  这些写法全部支持。

参考 `examples/webpage.html` —— 一份零迁就的"网页式"文件。

## 能力边界(无 WebKit 兜底)

引擎不走系统 WebKit —— 渲染只有自研 C99 这一条路。CSS 是实用子集:
`grid`、`svg`、`::before/::after`、`@font-face`、`word-break` 还未实现
(3D 变换/网格变形 MESH 指令已覆盖一部分立体与形变需求)。
**已落地**(不再是"进行中"): `<canvas>` 2D API(QuickJS 驱动, 状态机在 JS 层)、
音频/视频(`<video>`/`<audio>`, 解码委托平台媒体框架 —— macOS AVFoundation,
非 FFmpeg 路线; 真窗可走 AVPlayerLayer 图层直通)、WASM 运行时缝(wasm3)、
JS 定时器与 RAF、Markdown/JSON/CSV 直染、GIF/dotLottie/Live2D motion 驱动、
桌宠窗口形态、HiDPI。
契约见 [docs/media-design.md](media-design.md)、平台矩阵见
[docs/platforms.md](platforms.md)。完整能力清单见 [README](../README.md)「支持的子集」。

## 组件复用(include)

没有打包器。文件即组件:

```html
<include src="head.html">        <!-- 引入样式/结构片段 -->
<include src="parts/row.html">   <!-- 可嵌套(上限 6 层,自动检测循环) -->
```

展开发生在引擎装载前(预处理层),所以片段里可以有 `<style>`、
清单 meta、任意结构。共享一套 UI?做一个目录放片段,到处 include。

## 应用生命周期

```bash
hn open myapp app.html --css app.css    # 生成(无头槽位; 表面由 meta 决定)
hn open myapp app.html --window         # 真桌面窗(daemon spawn 平台壳, 每窗一进程)
hn list                                 # 在运行的应用(窗口应用带 kind:window/pid)
hn update myapp app.html                # 热更新(无头保持文档生命周期; 窗口应用原子写回源文件)
hn shot myapp out.png                   # 渲染 PNG(无头应用)
hn close myapp                          # 销毁(窗口应用 = SIGTERM 回收子进程)
```

持久化与轻应用槽位走宿主 socket op(`{"op":"persist",…}` /
`{"op":"restore",…}` / `{"op":"applets"}` / `{"op":"resident",…}` 常驻回归),
CLI 尚未包这一层 —— 任何 socket 客户端都能直接调。
(CLI 现有 9 个子命令: ping/open/update/close/list/dump/text/event/shot。)

所有命令走常驻宿主(`~/.html-native/hn-daemon.sock`, JSON-lines 协议):
先 `dist/hn-daemon &` 起宿主, 之后每个应用都是宿主里的轻量对象
(窗口应用是宿主 spawn 的独立子进程)。

## 输入控件

`<input>` / `<textarea>` 开箱可用:点击聚焦(`:focus` 生效)、
Tab 在控件间轮换、原生键盘编辑(含中文输入与光标移动)、闪烁 caret。
`type=text/password/checkbox/radio` 语义齐备(password 掩码、
checkbox/radio 绘制方框对勾/圆点、`:disabled`/`:checked` 伪类)。
表单值经 `hx-post` 自动编码发送。两个顺手的表单语义:

- input 里按**回车** = 提交:自动定位最近的提交载体
  (自身 → 祖先 → 最近容器内第一个 `hx-post`/`hx-get` 元素)再触发;
- textarea 支持多行:值按换行逐行布局,光标跟随所在行。

## 新能力速览(校准补记, 全部已落地)

| 能力 | 一条命令/声明 |
|---|---|
| **桌宠形态**(透明/无框/悬浮/穿透) | `examples/pet.html`: `hn-transparent`+`hn-frameless`+`hn-level`+`hn-click-through`; 真窗 `dist/hnapp-macos-arm64 examples/pet.html` |
| **媒体元素**(`<video>`/`<audio>`) | `examples/media-demo.html`; 真窗 AVFoundation 解码出声, `video` 可走 AVPlayerLayer 图层直通(`HN_MEDIA_LAYER=0` 退回 CPU) |
| **直染**(agent 输出零脚本渲染) | `<div hn-md>…markdown…</div>` / `hn-json` / `hn-csv`; 见 `examples/md-demo.html`、`fmt-demo.html` |
| **HiDPI**(Retina 2x 真窗) | 缺省按平台 backing scale; `HN_RENDER_SCALE=1\|2` 可覆盖(离屏恒 1x) |
| **GPU 契约 + Metal 直呈** | `HN_GPU=metal` 离屏加速; `HN_GPU=metal HN_GPU_PRESENT=1` 不透明窗 **CAMetalLayer 直呈**(drawable 直出, 零 readback/零 CPU 位图; 退出打 `presented/readback` 统计) |
| **网络层**(2026-10-16) | `hx-get`/`hx-post` 指向 `http(s)://` 走真请求(GET 自动拼 query, POST 带 urlencoded 头); JS `hn.fetch(url,{method,headers,body})` → Promise, 响应 `ok/status/headers/text()/json()`, 4xx/5xx reject 带 `e.status`; 白名单 `HN_FETCH_ALLOW=host1,host2`(重定向逐跳重校验), 超时 `HN_FETCH_TIMEOUT_MS`; 示例 `examples/network.html` |
| **Canvas 后端** | `HN_CANVAS_BACKEND=ity\|cg\|system` 显式切换(canvas_ity 可移植 / CoreGraphics 系统质量; 缺省账本重放, 能力不足自动整帧回退) |
| **常驻/几何记忆** | socket op `{"op":"resident",...}` 登记后 daemon 重启自动重开; 开窗位置自动记忆 |
| **持久化胶囊** | `hn persist <id> x.hnapp` / `restore`(socket op; 胶囊内嵌 agent 指令与能力图) |

## 调试与验收

```bash
dist/hnapp-macos-arm64 app.html --shot out.png 480 620   # 壳渲染落盘
dist/hncore-macos-arm64 render app.html out.png 460 560  # 引擎 CLI(hnsoft 后端)
dist/hncore-cairo-macos-arm64 renderc app.html out.png 460 560  # cairo 后端(同一显示列表)
dist/hn shot myapp out.png                               # daemon 无头渲染

# C99 离屏验收(61 断言; 位图即窗口所见, 光栅器就是生产用的 hnsoft)
cc -O2 -I Sources/CHtmlNative/include -I Sources/CHtmlNative \
   -I /opt/homebrew/include/freetype2 tools/render_probe.c \
   Sources/CHtmlNative/hn_{arena,html,css,style,layout,paint,context,theme}.c \
   Sources/CHtmlNative/hn_{json,lottie,mesh,png,media}.c \
   Sources/CHtmlNative/hn_{md,fmt,canvas,gif}.c \
   Sources/CHtmlNative/hnsoft.c -L/opt/homebrew/lib -lfreetype -lm \
   -o /tmp/render_probe && /tmp/render_probe

# 探针任选(全绿 = 该链路健康, 数字为 2026-10-13 实测断言数):
bash tools/canvas_probe.sh   # 95    bash tools/md_probe.sh      # 107
bash tools/fmt_probe.sh      # 118   bash tools/gif_probe.sh     # 61
bash tools/live2d_probe.sh   # 44    bash tools/lottie_probe.sh  # 37
bash tools/quad3d_probe.sh   # 33    bash tools/pet_probe.sh     # 27
bash tools/gpu_probe.sh      # 7     bash tools/timer_probe.sh   # 59
```

## 边界(诚实清单)

- JavaScript 走内嵌 QuickJS(可选): `<script>` 求值 + 桥函数
  `hnSetText`/`hnSetValue`/`hnCanvas2D`/`hnMeshParam`/`hnMedia*`/`hnWasm*`;
  **`addEventListener` 已落地**(命中链冒泡, 事件对象含 key/clientX/preventDefault 等);
  **`hn.fetch` 已落地**(异步 Promise, 系统网络栈 transport; 见上表网络层)。
  没有的是 DOM 全量 API / `require` / `localStorage`(用 `hn.get/set`)。
- CSS 是实用子集,不是全量(无 grid/svg/伪元素/`@font-face`/`word-break`;
  `nth-child` 已支持,其余在路线图上)。
- 图片支持 `<img>`(含 GIF 动图、`object-fit`);字体走 FreeType + 系统字体栈。
- 平台壳全部纯 C99: macOS `hnp_macos.c`/`hnweb_macos.c`(运行期 dlopen
  AppKit/CoreGraphics/QuartzCore/Metal), Linux `hnp_linux.c`/`hnweb_linux.c`
  (dlopen X11/libXext/libXv), Windows `tools/hnwin.c`(Win32);
  无头走 `hnp_headless.c`/daemon。没有 Swift、没有 WebKit、没有 .m 文件。

遇到问题先跑 `render_probe`(全绿说明引擎层健康),
再单独渲染你的文件定位是内容还是引擎的问题。
