# html-native

[![release](https://img.shields.io/github/v/release/asdshuaishuai/html-native?color=4f7cff)](https://github.com/asdshuaishuai/html-native/releases)
[![site](https://img.shields.io/badge/site-asdshuaishuai.github.io%2Fhtml--native-4f7cff)](https://asdshuaishuai.github.io/html-native/)
[![license](https://img.shields.io/badge/license-MIT-3ecf6f)](LICENSE)

> 与 RN / Flutter 同级的原生 UI 框架——UI 描述语言是 HTML。
> 形态是一个**系统级 PWA / 小程序引擎**：随时生成、销毁、持久化一个应用。
>
> 渲染全部走自研 C99 引擎(原生、无 WebView、无 Swift): 平台差异收敛在
> `platform/hn_platform.h` 的薄胶水层 —— 每平台一个 `hnp_*.c`, 运行期
> dlopen 系统 API, 编译期零 SDK 依赖。
> **人类和 AI 都能直接开发**：会 HTML/CSS 就能出原生窗口，无构建、无
> 依赖；目标是把应用开发的技术成本压到最低。
> 人类开发者上手 → [docs/getting-started.md](docs/getting-started.md)。

## 定位

```
┌──────────────────────────────────────────────────────┐
│ 上层(不属于引擎): 组件库 / UX 规范 / 交互模式 / 工具链   │
├──────────────────────────────────────────────────────┤
│ hn 引擎(核心)                                          │
│   把 hn 编码(HTML 语法)渲染成系统级 UI:                 │
│   图层 · 窗口 · 弹窗 · 应用                             │
│   渲染核心 C99 无关性: 不含平台/UI/UX/网络               │
├──────────────────────────────────────────────────────┤
│ 平台运行时(每系统一个, 薄): 物化表面 + 事件 + 文本后端   │
│ 宿主(常驻): 应用生命周期 + Unix socket 会话协议         │
└──────────────────────────────────────────────────────┘
```

三个原则:

1. **引擎具备无关性**。它只对 hn 编码做一件事——渲染成系统级表面
   (图层/窗口/弹窗/应用)。上层的 UI/UX 实现(组件、设计系统、交互
   模式)不在引擎里, 由使用方在其上构建。引擎也不含网络: htmx 风格
   的 `hx-*` 属性由运行时解释, transport 可注入(进程内 /
   `sys://` 系统桥, 不绑定 HTTP 服务)。
2. **应用是易变对象**。像 PWA/小程序一样: 生成(秒开一个表面)、
   销毁(close)、持久化(persist → `.hnapp` 胶囊, 离线可再唤起)、
   热更新(同 id 推送新 hn 编码, 窗口与状态保持)。
3. **HTML 是最低成本的 UI 语言**。人类手写或 AI 生成是两条等价入口:
   人类写完 `app.html` 一条命令开窗(无构建链); agent 现场生成 hn 编码
   推给常驻宿主, 系统里即刻出现信息窗口——
   不拉浏览器, 不装应用。

### 与 WebView 的边界(宣言)

把 HTML 画上屏的东西, 必然有解析/级联/布局/光栅/动画时钟——这一层与
浏览器趋同是物理事实, 不是要回避的。分界线画在**产品契约**上:

| 契约维度 | WebView | 本项目 |
|---|---|---|
| 兼容目标 | 任意网页, 与 Chrome 一致 | 自定义 hn 子集, 探针钉死(约百条断言即语言规范) |
| 体积/依赖 | 数千万行, 运行时 100MB+ | ~1.5MB 单二进制, 零外部依赖 |
| 确定性 | 不承诺跨版本一致 | 跨架构布局逐字节一致(构建门禁守着) |
| 脚本面 | 完整 DOM/Web API | 小桥(hnSetText / hnGet2D / hnCanvas2D…) |
| 网络 | 完整栈 + 安全沙箱 | 无(hx-* 注入 transport, sys:// 本地) |
| 窗口语义 | 平台默认窗 | 一等公民: popup/layer/桌宠/鼠标穿透/幽灵模式/秒开秒毁 |

由此两条纪律:

1. **子集冻结**: "支持的子集" + 探针 = 语言规范。新能力只以 `hn-*` 扩展
   身份进入, 且必须服务"agent 生成 UI"; 服务"渲染真实网页"的特性一律
   视为范围蔓延 —— 走 `sys://open` 桥出, 不塞进引擎。
2. **押注 WebView 做不好或不肯做的事**: 无头确定性(agent 视觉回归/CI 闭环)、
   窗口即易变对象(秒开/胶囊/轻应用/桌宠/幽灵模式)、单二进制零安装、
   面向 agent 的协议面(能力图 + 胶囊内操作指令)。渲染器只是地基,
   这些才是产品。

## 快速开始

```bash
git clone https://github.com/asdshuaishuai/html-native && cd html-native
ZIG=zig bash tools/build-multiplatform.sh        # 产出 dist/ 全套 C99 二进制

dist/hnapp-macos-arm64 examples/agent-card.html   # 秒开一个原生窗口
dist/hncore-macos-arm64 render app.html out.png 460 560   # 无头渲染出 PNG
```

只需要会 HTML 和 CSS。构建链只有 bash + zig cc —— 无 Node、npm、
打包器、Swift、Xcode 工程。

**agent 的入口: C99 常驻宿主 + CLI**(Unix socket JSON-lines, 协议即文档):

```bash
bash tools/build-daemon-cli.sh     # 产出 dist/hn-daemon(宿主) + dist/hn(客户端)
dist/hn-daemon &                   # 常驻; 默认 ~/.html-native/hn-daemon.sock
dist/hn open card examples/agent-card.html --css examples/agent-card.css
dist/hn list / dump card / event card --kind click --target btn / shot card out.png
```

宿主与壳的分工: **开窗走平台壳**(`hnapp-*`/`hnweb-*`, 事件循环在壳里),
**无头驱动走 daemon**(CI/服务器上 open/update/eval/dom/shot 全链路不触 GUI)。
任何语言的 socket 客户端都能直接说这套 JSON-lines 协议, 不依赖 hn CLI。

**绘制后端：一个跨平台框架，不是每个平台各写一份**。引擎产出的是纯数据的显示
列表(8 类指令)，绘制层把它翻译成像素。这个接缝上有两个可互换实现，**签名完全
相同**(`hnsoft_render` / `hncairo_render`)，换后端只需换链接目标，调用方一行不改：

| 后端 | 用于 | 特点 |
|---|---|---|
| `hnsoft.c` | 默认光栅器(全部平台) | 纯 C 软件光栅, 零外部依赖 |
| `hn_cairo.c` | 可选: 生产级阴影/裁剪/压缩 | **一份实现喂所有平台** |

`hn_cairo.c` 是"别再为每个平台手写一份绘制"的答案。此前 Windows 与无头场景只能
靠 `hnsoft` 手写光栅，而它自己承认几处妥协：阴影是"6 层扩边"近似(无高斯模糊)、
圆角裁剪被简化为轴对齐矩形、PNG 用存储型 deflate(无压缩)。cairo 把这三件事换成
生产级实现，同时给 macOS/Linux/Windows/iOS/Android 同一份代码。

对照显示列表 8 类指令逐项实测过(`tools/hncairo_probe.c`, 35 条断言全通过)：
圆角矩形+实色/渐变填充、**任意路径裁剪(含圆角)**、even-odd/非零环绕多边形、
四边形、UV 贴图、真实覆盖率抗锯齿、**真模糊阴影**、逐像素透明背板。

```
hncore render  app.html out.png 460 560   # hnsoft 后端
hncore renderc app.html out.png 460 560   # cairo 后端(同一份显示列表)
```

构建时带 `-DHN_USE_CAIRO` 即切换(需链接 cairo + FreeType)。文本仍走 FreeType
光栅化而不用 cairo 的字体后端 —— 因为**测量与绘制必须同源**：布局期用 FreeType
的字形 advance 排布，绘制若换另一套字体后端，字形就会落出排好的行盒。

一个实测差距：同一份文档 cairo 输出的 PNG 是 31 KB，hnsoft 是 1007 KB ——
后者用存储型 deflate(不压缩)，前者走真 zlib。

**三平台二进制**：`ZIG=zig ./tools/build-multiplatform.sh` 一条命令产出
macOS/Linux/Windows 产物 —— 除五个 `hncore` 引擎 CLI（验证与集成测试）外，
还有**跨平台运行时** `hnweb-*`：Linux（x86_64/aarch64, X11 运行期 dlopen,
零编译期依赖）与 Windows（x86_64, Win32）各一套, 以及新分层架构的
`hnapp-*`(见下"架构与代码地图")。
详见 [release](https://github.com/asdshuaishuai/html-native/releases)。

## 架构与代码地图

```
Sources/CHtmlNative/  引擎核心(C99, ~10900 行, 零依赖, 不含平台/UI/网络)
  hn_html.c    hn 编码解析 → DOM(arena)
  hn_css.c     样式表解析 → 规则(选择器展开/特异性)
  hn_style.c   级联 + 继承 + UA 样式表
  hn_layout.c  block 流 + flexbox + 文本换行(CJK 硬拆)
  hn_paint.c   DOM → 绘制指令列表(display list)
  hn_theme.c   hn-theme 设计令牌基底(暗/亮)
  hn_json.c    极简 JSON 解析(零依赖; Lottie 等公开格式需要)
  hn_png.c     PNG 解码(含自带 inflate; 让软件光栅后端真能显示图片)
  hn_gif.c     GIF87a/89a 动图解码(LZW/色表/帧间 disposal/透明索引/循环次数)
  hn_lottie.c  Lottie 求值 → 矢量指令(多边形/图片/矩形/渐变/裁剪)
  hn_mesh.c    网格变形贴图 → MESH 指令(Live2D 类效果的原语)
  hn_context.c 会话: 布局编排/命中/hot-update/清单解析
  hn_cairo.c   **可选**绘制后端: 显示列表 → cairo(一份实现喂所有平台)
  include/hn.h 引擎公共 API(单头)
rt/hn_rt.c             公共运行时: 文档生命周期/htmx 执行/sys:// 桥/本地 KV
                       + QuickJS 求值(HN_HAVE_QUICKJS, 静态链接可选)
platform/hn_platform.h 平台抽象 API(~15 个函数: init/开窗/blit/事件/时钟/光标)
  hnp_headless.c       无头平台(CI/服务器/agent 无显示环境)
  hnp_macos.c          macOS: 运行期 dlopen AppKit + objc_msgSend(零 .m/.swift)
  hnp_linux.c          Linux: 运行期 dlopen X11
app/hn_app.c           唯一主循环(~120 行): 链接不同 hnp_*.c 即该平台的应用
tools/hncore.c         引擎 CLI(render/renderc: hnsoft 与 cairo 同一显示列表)
tools/hnweb_macos.c    macOS 窗口壳(M1+M2: dlopen AppKit/CoreGraphics)
tools/hnweb_linux.c    Linux 窗口壳(M1+M2: X11 dlopen + hnsoft, 事件管道+帧循环)
tools/hnwin.c          Windows 壳(Win32)
tools/hn_daemon.c      C99 常驻宿主(Unix socket JSON-lines, agent 入口)
tools/hn_cli.c         C99 命令行客户端(与 daemon 配对)
tools/build-multiplatform.sh  交叉构建(zig cc, 三平台产物 + 跨架构确定性门禁)
tools/build-daemon-cli.sh     daemon/CLI 本机构建
tools/*_probe.c/.py    探针(render/alpha/cairo/mesh/sysbridge/rt + 布局/CSS/HTML/Lottie/GIF)
screenshots/           引擎效果截图
```

## 使用

### 命令行(agent 的系统入口)

```bash
bash tools/build-daemon-cli.sh
D=dist

$D/hn-daemon &                            # 常驻宿主(默认 ~/.html-native/hn-daemon.sock)

$D/hn open card examples/agent-card.html --css examples/agent-card.css
$D/hn list                                # 运行中的应用
$D/hn dump card                           # 绘制指令数(管线健康自检)
$D/hn text card --element title           # 元素文本(感知)
$D/hn event card --kind click --target btn  # 注入事件(驱动)
$D/hn shot card out.png                   # 渲染 PNG
$D/hn update card examples/agent-card.html  # 热更新(同 id 推新文档)
$D/hn close card                          # 销毁

cat foo.html | $D/hn open -               # 管道: agent 现场生成 → 成窗
```

daemon 是**无头优先**的: open/update/eval/dom/dump/shot 全链路不触 GUI,
CI/服务器同样可用; 真窗口由平台壳开(见上"快速开始"的 `hnapp-*`)。
持久化(`persist`/`restore`)与轻应用槽位(`applets`)走 socket op ——
任何语言的 socket 客户端都可直接对接, CLI 只是其中一个客户端。

### 渲染运行时: 全部 C99, 无 WebKit、无 Swift

- **没有 WKWebView / WebView2 / WebKitGTK**。历史上走过"系统 webview 优先"
  与"Swift 运行时"两条路, 都已删除 —— 现在只有自研 C99 引擎这一条渲染路径,
  平台差异收敛为 `platform/hn_platform.h` 的 ~15 个函数(开窗/blit/事件/时钟/
  光标), 每平台一个 `hnp_*.c`, 全部**运行期 dlopen 系统 API, 编译期零 SDK 依赖**。
- **新三层(主推)**: `app/hn_app.c` 是唯一主循环(~120 行) + `rt/hn_rt.c`
  公共运行时(文档/htmx/sys:// 桥/QuickJS) + `platform/hnp_{headless,macos,linux}.c`。
  链接哪个 hnp_*.c 就是哪个平台的应用: `hnapp-macos-arm64`(真窗口)、
  `hnapp-macos-headless`(无 GUI 自检/CI)、`hnapp-linux-x86_64`(dlopen X11)。
  `hn_app file.html` 开窗, `--probe` 管线自检, `--shot out.png` 渲染落盘。
- **M1+M2 窗口壳(保留)**: `tools/hnweb_macos.c`(dlopen AppKit/CoreGraphics)、
  `tools/hnweb_linux.c`(X11 运行期 dlopen + hnsoft)与 `tools/hnwin.c`(Win32)。
  M2 能力: **事件派发管道**(点击/键盘/滚轮/hover → 目标解析 → 带 id 冒泡 →
  `hx-trigger` 消费; click/mousedown/mouseup/mouseenter/mouseleave/focus/
  blur/keydown/keyup/scroll 全走同一管道)与**动画帧循环**(dt 推进
  `hn_context_anim_tick` —— 过渡/@keyframes 与 Lottie/网格变形的独立时钟
  共用; select 超时取"16ms 帧 / 最近 hx 轮询 / 无限阻塞"最小者, 空闲零唤醒)。
  Linux 壳零外部依赖、musl 静态, 交叉产物 `hnweb-linux-*`(x86_64/aarch64);
  无头 CI 走 `--probe`/`--shot` 自检。
- **无头与确定性**: `hncore render` / daemon 无头模式 / `hnp_headless` 服务
  服务器端截图、CI 视觉回归与确定性渲染 —— 同一份文档跨架构逐字节相同的
  布局输出(见"支持的子集"的确定性门禁)。
- **上层语义与渲染解耦**: 窗口生命周期(秒开/热更新/持久化)、`sys://` 系统桥、
  `hx-*` 交互(click/load/`every Ns`/回车提交/表单参数)、`hn-theme` 设计令牌、
  本地 KV 都在引擎/运行时层 —— 换壳不换语义。
- **CSS 归一化**: 引擎接受免单位数值(`padding: 16`), 标准 CSS 会忽略;
  引擎侧自动补齐单位(长度属性补 `px`, 倍数/百分比/函数式值/关键字不动),
  所以按浏览器习惯写的文件无需改动。
- **文本编码与中文字体**: `charset=utf-8` 缺失时自动注入; 默认样式带
  **中文系统字体栈**(-apple-system + PingFang SC + Hiragino Sans GB),
  不会 fallback 成宋体; `code/pre` 等宽栈。中文解析/布局/表单编码全程
  UTF-8 无损。

### hn 编码里的表面声明(清单)

```html
<meta name="hn-surface" content="window|popup|layer">
<meta name="hn-window"  content="360x520">        <!-- 或 360x520@100,80 -->
<meta name="hn-title"   content="agent · 仓库分析">
<meta name="hn-theme"   content="dark">            <!-- 或 light: 内置设计令牌基底 -->
<meta name="hn-applet"  content="clock">          <!-- 轻应用槽位名, 见下 -->
```

引擎只解析声明(C: `hn_doc_manifest`), 由宿主物化成对应表面。

### 宿主协议(Unix socket, ~/.html-native/hn-daemon.sock, JSON-lines)

```json
{"op":"ping"}
{"op":"open","id":"card","html":"<h1>hi</h1>","css":"...","w":360,"h":520,"headless":0}
{"op":"update","id":"card","html":"..."}
{"op":"close","id":"card"} / {"op":"list"}
{"op":"eval","id":"card","js":"..."}   → {"value":"..."}   (QuickJS)
{"op":"dom","id":"card"} / {"op":"dump","id":"card"} / {"op":"text","id":"card","element":"x"}
{"op":"event","id":"card","kind":"click","target":"go","x":..,"y":..}
{"op":"shot","id":"card","path":"out.png"}
{"op":"applets"} / {"op":"applet-remove","name":"clock"}
{"op":"persist","id":"card","path":"~/x.hnapp","instructions":"怎么驱动它"}   # 打成胶囊
{"op":"restore","path":"~/x.hnapp"}                                          # 拆胶囊
```

### 截图(离屏渲染, 无需屏幕权限)

```bash
dist/hnapp-macos-arm64 app.html --shot out.png 480 620    # 应用壳渲染落盘
dist/hncore-macos-arm64 render  app.html out.png 460 560  # hnsoft 后端
dist/hncore-macos-arm64 renderc app.html out.png 460 560  # cairo 后端(同一份显示列表)
dist/hn shot card out.png                                 # daemon 里的无头应用
```

### 嵌入式使用(引擎作为库链接)

引擎是普通 C99 库: 编译期只要 `include/hn.h` 单头, 运行期只需宿主喂事件与像素。
最小嵌入 = 三件套链接:

- 引擎核心 `Sources/CHtmlNative/*.c`(解析/级联/布局/绘制指令)
- 运行时 `rt/hn_rt.c`(htmx 执行 / `sys://` 桥 / QuickJS, 均可裁剪)
- 任一 `platform/hnp_*.c`(或照 `hn_platform.h` 自己写一个, ~15 个函数)

`app/hn_app.c`(~120 行)就是这份契约的参考实现 —— 换掉它的表面物化,
就是你自己的应用容器; `tools/hncore.c` 演示零 GUI 嵌入(离屏渲染出 PNG)。

## 支持的子集(v1)

- **hn 编码**: HTML 语法——元素/属性/void/自闭合、DOCTYPE/注释、`<style>` 内联、
  **`<link rel="stylesheet">` 本地外链样式(相对路径, 预处理层内联)**、
  实体(命名/十进制/**十六进制**/`&nbsp;` 不折叠)、空白折叠、`<meta name="hn-*">` 清单
- **CSS(按网页习惯书写即可)**: 选择器(tag/.class/#id/*/复合/后代/组/
  **`>` 子代 / `+` 相邻 / `~` 通用兄弟组合器**/`:hover`/`:active`/`:focus`
  /`:nth-child(odd|even|N|2n|2n+1)`/**`:first-child`/`:last-child`**, 伪类按标准计入类级特异性) +
  级联特异性 + 继承 + 行内 style + **自定义属性**(`--x` 声明收集/继承/覆盖,
  `var(--x[, fallback])` 代换, 递归深度 4) + **`@media (min/max-width)`**
  (级联按视口宽度过滤, 窗口缩放即刻重排); 属性: display(block/flex/none/inline/inline-block)、flex 系
  (direction/justify/align/gap/grow/**shrink**/`flex:n`⇒basis0/basis)、
  盒模型(margin/padding/border/radius/box-sizing)、**transition 过渡**、
  **linear-gradient 渐变**(轴长按 CSS 口径: 渐变线长 |W·sinθ|+|H·cosθ|,
  端点恰落在 0/1 —— hnsoft 与 cairo 同式; 旧口径多除一个 2, 两端只到
  25%/75%)、
  **box-shadow 阴影**、**overflow(裁剪+滚动)**、**cursor**、颜色背景、
  字体(size/weight/style/line-height/letter-spacing)、text-align、
  opacity; 颜色 **#hex(3/4/6/8 位)/rgb(a)/**`hsl(a)`/transparent/36+ 命名色
  (函数式颜色支持含空格写法); 单位 **px/pt/%/em/rem/vw/vh**;
  `display:inline-flex`(外层原子行内盒); `border-radius:50%`(百分比圆角);
  `text-decoration(underline/line-through/overline)`; `list-style(none/square/circle)`;
  **Lottie**: 引擎求值(不是 WebView 播放器) → 平台无关指令, 三个后端同等支持。
  形状: 组(嵌套+自身变换)/矩形/椭圆/路径(**静态与变形关键帧都支持**)、
  填充/描边、**多 stop 线性渐变填充(gf)与渐变描边(gs)**(扁平
  `[pos,r,g,b,...]` 与新式 `[{t,s}]` 两种编码、0..1 与 0..100 两种量纲都吃下,
  在形状局部空间切成色带后走既有多边形指令 —— 后端零改动; 径向 t=2 退化为
  中位色, 如实写明)、**trim path**(线/进度环/加载动画的核心)、**repeater**、
  **预合成 precomp**(真实导出文件几乎必带)、图层混合、纯色层/图片层。
  **mask 最小版**: `masksProperties` 的单条加法蒙版(不带 inv)按其路径外接
  矩形做 CLIP 裁剪, 支持蒙版路径关键帧 —— 矩形蒙版精确, 曲线蒙版是外接盒
  近似; 反相/相减/相交等多蒙版语义不做。
  **明确不支持**: 文本层/表达式/特效/轨道遮罩/merge paths/时间重映射/
  径向渐变的形状化/多蒙版布尔组合。
  **变换引擎**:**非等比缩放**(`scale(x,y)` / `scaleX()` / `scaleY()` ——
  之前只取第一个参数, `scale(2,1)` 被当成等比 2, 图形纵向也被拉高一倍;
  `scale(2)` 等比形式只装等比一路, 与 `scale(2,2)` 同值 —— 旧实现两路
  相乘曾得 4 倍)、
  **`transform-origin`**(px 与 %; 之前完全没有, 所有 rotate/scale 都以盒中心
  为原点, 想绕左上角转只能靠 translate 硬凑而且角度是错的)、
  **3D 变换**(`rotateX/Y/Z` + 透视投影 + `translateZ` 深度缩放)。
  **`translateZ()` 与 `rotate3d(x,y,z,angle)` 已实现投影**(深度位移按透视
  缩放、任意轴按 Rodrigues 公式旋转后与 X/Y/Z 复合)。
  **`perspective` 视距与消失点**(元素自身声明视距, 否则取最近的祖先声明 ——
  消失点跟随**声明者**的 `perspective-origin`, 缺省声明者盒中心 50% 50%;
  曾错标成子盒自身中心, 探针以 translateZ(±200) 的角点收放钉死口径)。
  **`transform-style: preserve-3d` 层级矩阵栈**(父子 transform 逐级复合成
  4×4 矩阵, "容器旋转 × 子面 rotateY/X + translateZ"的真立方体/卡片环直接
  可写; 父级 2D scale/rotate 同样折叠进子级 3D 坐标系 —— scale(2) 祖先下
  子面片投影随之放大, 不再视而不见)、
  **flat 摊平升级单应**(flat(缺省)元素复位矩阵, 并把父面片的投影以
  **单应**摊平给子级: 盒四角 ↔ 投影四角解 8×8 线性方程组, 透视四边形
  8 自由度仿射表达不了第 4 角 —— 旧三点仿射拟合在远边误差达数十 px;
  退化(四角共线)时回退三点仿射)、
  **QUAD 面片的像素渐变/边框/文字**(渐变画在元素平面上随面片一起投影:
  hnsoft 逐像素按双三角仿射采样、cairo 单路径 + 单 pattern 一次填充 ——
  拆双三角各自 clip 会沿对角缝漏底色(实测 ±40/255 振荡), 单次填充天然
  无缝; 两后端与 RECT 共用同一条 CSS 轴长口径。边框按投影四边形出四条
  梯形, 不再按未变换矩形画歪; 子树文字/盒尺寸以**锚点处局部线尺度**
  取档 —— 同一张转动面片内近大远小(透视字号), 缺省档 sqrt|det|;
  字形本身轴对齐, 不做逐像素透视畸变)、
  **deco/li 随变换**(text-decoration 线与列表标记坐标经同一变换、
  宽厚/尺寸随缩放档 —— 此前标记直用布局坐标, scale(2) 祖先下与已变换
  的正文脱节)、
  **flex/absolute/inline-block 子树的 3D**(与普通块流同一绘制路径,
  不再静默丢弃 —— 3D 卡可直接做 flex 子项)。
  **选择器引擎强化**:**属性选择器**(`[a]` `[a=v]` `[a^=]` `[a$=]` `[a*=]` `[a~=]`)、
  **`:not()`**(否定一层简单复合, 实参按标准计入特异性)、
  **`:nth-of-type()` / `:first-of-type` / `:last-of-type`**(只数同标签兄弟 ——
  与 nth-child 的差别正是表格/列表隔行错位的根源)、
  **`calc()`**(`calc(100% - 40px)` 等; % 走包含块基准, 纯绝对值就地求值, 负宽 clamp 到 0)、
  **`text-shadow`**(cairo 走降采样放大近似高斯模糊, hnsoft 偏移无模糊;
  两个绘制后端同一语义)。
  这一批的共同点是**之前声明了不生效也不报错** —— 探针先行把它们全部翻了出来。
  **文字特效**:`word-spacing`(每个"词间隔"附加宽度, 继承; 折叠模式作用于
  被压成的那一枚空格, pre/pre-wrap 作用于串内每个空白)、
  **`line-clamp` / `-webkit-line-clamp`**(最多显示的行盒数, 超出以省略号收尾
  —— 此前 ellipsis 只能配 nowrap 做单行截断, 多行卡片摘要无法表达)。
  **滤镜与裁剪**:**`filter: brightness() contrast() saturate()`**(绘制期
  对填充色调色, `120%` 与 `1.2` 等价, `none` 复位; 复合声明按
  saturate → contrast → brightness 次序)、
  **`clip-path: circle() / inset()`**(circle 支持 `closest-side` 与百分比半径;
  inset 四边可各带 px/%, `round R` 圆角; ellipse/polygon 不支持, 声明被忽略)。
  **排版精细化**:`margin:0 auto` 居中 / flex `margin-left:auto` 推右(标准优先级压 justify)、
  `min/max-width/height` 约束、**完整 margin 简写正确展开 TRBL**(修复历史越界 bug)、
  `font-family(monospace/serif 系统设计, code 等默认等宽)**、默认行高 1.45、
  **窗口标题栏无缝**(透明标题条 + 底色跟随 body 背景 + 可拖拽空白区)
- **图片**: `<img>`(尺寸回退链: 样式 > 属性 > 固有尺寸), 图片后端注入;
  **`object-fit: contain/cover/fill/none`**(替换元素的内容适配 —— 之前声明了
  不生效也不报错, 所有图一律被拉伸)。固有尺寸优先问图片后端, 未注入时引擎
  直接**嗅探文件头**(PNG IHDR / GIF 逻辑屏 / BMP biWidth-biHeight), 不引入
  解码依赖; 未识别格式退回拉伸(旧口径)。
- **GIF 动图**: `<img src="x.gif">`(src 以 .gif 结尾自动生效, 或 `hn-gif`
  显式声明; `hn-gif-speed` 倍速)。解码器是引擎自带(hn_gif.c): GIF87a/89a、
  **LZW**(标准增长规则, 与 PIL/giflib 逐像素对照过)、全局/局部色表、
  **帧间 disposal 0/1/2/3**、透明色索引、NETSCAPE 循环次数、交织。产物是
  **合成好的整幅 RGBA 帧序列** + 每帧 delay; 当前帧 = 时钟的**纯函数**(按
  delay 累计取模, 有限循环播完停末帧)——与 Lottie/网格变形**共用同一帧时钟**
  (节点 ext_clock, hn_context_anim_tick 推进), 所以真窗连续播放、HN_CLOCK
  离屏取帧、循环回绕是同一套口径; 播放中节点标记活跃驱动帧循环。
  单帧 GIF 走同一条路(等价静态图); 解码失败退到常规 img 路径(显式暴露而非静默空白)。
  见 `examples/image-lab.html`(口径由 `tools/gif_probe.c` 61 断言钉死)。
- **弱化存在感 / 小程序 · 插件形态**: 引擎默认"不像一个 App"。
  `hn-presence=ghost`(默认于 popup/layer)声明**不占系统身份** —— 不进 Dock、
  无图标、不进 Cmd+Tab 的应用切换(引擎解析声明并下发给壳, 物化语义由
  各平台壳实现 —— C99 壳对浮层级别/空间对齐的完整对齐在壳层路线图上)。
  只有 `hn-surface=window`(或显式 `hn-presence=app`)才驻留 Dock。
  `hn-lifecycle="launch show hide destroy"` 声明关心哪些生命周期事件,
  未声明的页面一次都不打扰; 事件走**同一条统一事件管道**(JS 处理器与 hx 共用)。
- **桌宠形态(无背景 + 悬浮桌面)**: 上一条是"不像一个 App", 这一条是"
  **几乎不以窗口存在**"—— 没有标题栏、没有底色、没有投影, 画面上只剩角色与
  气泡, 其余是真实桌面。一组 meta 声明即整个形态(macOS 已物化, 其余平台优雅忽略):
  `hn-transparent="1"`(逐像素 alpha)、`hn-frameless="1"`(无标题栏无边框, 整窗
  即内容)、`hn-level="floating|desktop|normal"`(悬浮于普通窗口之上 / 垫到桌面级
  —— 值取 `NSFloatingWindowLevel=3` 与 `kCGDesktopWindowLevel`, 逐值抄 SDK 头)、
  `hn-spaces="all"`(跟随所有空间, 含全屏空间旁)、`hn-shadow="0"`、`hn-click-through="1"`
  (初始鼠标穿透)。与 `popup`/`layer` 的区别: 那两者是"存在感低的窗口"(仍有窗口
  矩形、层级与标题条语义), 桌宠把窗口本体也抹掉 —— 无边框 + 逐像素透明 +
  `hn-presence` 不占系统身份, 观感是"桌面上住着一个小东西"而非"开了一个窗口"。
  无边框后内容就是唯一拖动面(壳层覆写 `mouseDownCanMoveWindow`, 立绘/空白均可拖);
  **运行期穿透切换**走 `hnp_window_set_click_through(w, on)`(桌宠"幽灵模式":
  穿透时点击落到桌面, 再点回来)—— 初始态由 `hn-click-through` 声明,
  验收脚本可用环境钮 `HN_CLICK_THROUGH=0|1` 覆盖。
  见 `examples/pet.html`(hn-mesh 摆动立绘 + keyframes 错相气泡轮播 + 状态 dot),
  离屏取帧 `screenshots/pet.png`, 口径由 `tools/pet_probe.sh` 钉死(四角 alpha=0/
  headless 收下新字段/真窗口 3 秒冒烟)。
- **透明背景层**: 引擎剥离 `html/body` 底色 + 绘制后端的真逐像素 alpha
  (cairo 直通、hnsoft 非预乘 source-over) + 平台窗口透明背板。光栅层上的历史 bug 都在
  `tools/hnsoft_alpha_probe.c` 与 css 探针的端到端断言里守着:
  底色 alpha 曾被硬编码 255、`blend()` 只写 RGB 不写 alpha、
  `sd_rounded` 在 `qx==qy` 时内部距离塌成 0。实测 `examples/transparent.html`
  81.7% 全透明 + 10.6% 抗锯齿过渡 + 7.7% 内容。
- **JS 运行时选型(QuickJS, 已探针验证)**: 选型落地 **QuickJS**(Zlib 协议,
  ~2.6MB 静态链接, C99 友好), 集成在 `rt/hn_rt.c` —— `<script>` 求值与
  daemon `eval` op 的执行体。(历史上曾为"webview 兜底"验证过 litehtml +
  QuickJS 组合, 兜底路线删除后 QuickJS 保留为 JS 运行时。)
  集成级约束已实测并记录: ①JS 异常必须 `JS_GetException` 消费; ②该
  quickjs 构建的 `JS_FreeRuntime` 在有 JS 函数定义时会断言 —— runtime 与
  进程同生命周期即可规避(daemon 本就常驻)。
- **胶囊持久化(一个文件装下全部)**: `.hnapp` 是单个可携带文件, 参照 Capsule
  "documents that run like apps" 的形态 —— 不再把应用散落在运行时目录里:
  - **文档** html / css
  - **数据** 页面自己的 KV(原 `~/.html-native/store/<id>.json`)
  - **几何** 轻应用槽位位置(原 `~/.html-native/applets/<名>.json`)
  - **agent 操作指令** + **能力图**(元素 id / 触发器 / 轮询 / 生命周期)
    + **操作历史**(open/update/event/lifecycle 的可回放轨迹)
  纯 JSON 文本: 人能直接打开读、能 diff、任何编辑器都能看。实测一个时钟轻应用
  打出来 5.4 KB。`hn persist <id> <文件>` 写、`hn restore <文件>` 从任意位置拆
  (数据与几何一并装回), 换个 id 打开同一份页面照常还原。
  **持久化应用必须带 agent 指令**: 这些应用绝大多数是 agent 造的, 下一个 agent
  拿到文件唯一需要的就是"这是什么、怎么驱动它"。不传 `--instructions` 时运行时
  会按能力图自动导出一段(写成可直接照抄的 `hn event …` 命令形态), 并在响应里标
  `instructionsSource: "derived"` 提示补一段更好的 —— 但没有指令的胶囊不存在,
  因为没有指令的持久化应用对下一个 agent 等于一张截图。
  v1 老格式(只有 id/html/css/surface/title/w/h)照常读取, 缺的段按空处理。
- **轻应用槽位(半固化)**: 声明 `hn-applet="clock"` 的表面成为**具名桌面轻应用**
  (KDE Plasmoid 那类形态, 不是 PWA 也不是页面栈): 位置按槽位名记在
  `~/.html-native/applets/<名>.json`, 销毁后再打开回到原处。特征: 槽位管**半固化**、
  槽位名本身管**具名**、`hn-presence` 管**系统级弱存在感**(随用随消的
  `--ttl` 在壳层路线图上)。要点:
  - 槽位绑定的是**名字不是 app id**: 换个 id 打开同一份页面, 仍回到原处
  - 优先级: 调用方显式 `--x/--y` > 槽位记忆 > 页面声明 > 屏幕居中。
    显式坐标**不落盘**, agent 临时摆位不会污染记忆
  - 固定尺寸表面(popup/layer)只记位置, 宽高仍以页面声明为准 —— 否则作者改了
    `hn-window` 看不到变化; `window` 表面则连 resize 后的尺寸一起记
  - 存**绝对屏幕坐标**, 与窗口停在哪块屏无关; 副屏位置原样还原
  - agent: `hn applet list` / `hn applet remove <名>`(`{"op":"applets"}`)
- **网格变形(Live2D 类原语)**: `<img src="char.png" hn-mesh="12x10" hn-mesh-sway="8"
  hn-mesh-speed="1.4" hn-mesh-anchor="bottom">` —— 贴图映射到可变形网格, 顶点由
  正弦摆动驱动(**anchor 六模式**: bottom/top/left/right/center-radial, 固定端位移
  恒为 0), 也可由脚本 `hn_node_set_mesh_verts()` 逐帧写入顶点(rig 逻辑属应用层,
  与 Live2D 把 SDK 与建模工具分层的做法一致)。`.moc3` 是专有格式需 Cubism SDK
  商业授权, 无法自研解码, 但底层技术以本原语形式交付。UV 在变形下仍是均匀网格
  (贴图跟着网格走)。`tools/mesh_probe.c` 19 条断言覆盖网格解析/六种 anchor/
  UV 正确性/脚本驱动。
- **跨平台确定性**: 引擎是纯 C99, `-ffp-contract=off` 关掉 FMA 融合(arm64 有
  而 x86_64 基线没有, 默认融合会让同一文档算出不同 px 值)。构建脚本带**跨架构
  确定性门禁**: 同 HN_NO_TEXT 口径下比对各架构产物的布局输出, 不一致即构建失败。
  实测这个门禁抓到过一个真实的未定义行为(`padding` 简写的单位数组未初始化,
  zig cc -O2 arm64 下读出垃圾值, 同一份源码 clang 却正常)
- **HTML 解析**: 实体解码 · 注释剔除 · void 元素 · 自闭合标签 · 标签/属性名
  大小写不敏感 · 未闭合容错 · 500 层嵌套与 2 万字符文本无退化。
  **DOM 保留原始空白**, 空白折叠按 `white-space` 在布局期决定(与浏览器
  分工一致) —— `pre` / `pre-wrap` 因此能正确保留连续空格与换行
- **默认样式(UA)**: 网页语义开箱即用——`<ul>/<ol>` 列表标记(disc/空心/方块/
  十进制序号)、`<a>` 蓝色下划线、`<u>/<s>` 装饰线、`<hr>`、`<strong>/<em>`、
  h1-h6/p 边距; 未知标签按块级处理(语义标签可直接用)
- **布局**: block 常规流、flexbox 行/列(grow/shrink/stretch)、
  **行内格式化上下文(IFC)**: 文本与 `display:inline`/`inline-block` 元素共享
  行盒、按词贪心换行 + CJK 码点硬拆、跨节点/跨行基线对齐、
  overflow 裁剪与滚动(带滚动指示条)
- **flexbox**: 行/列 · `grow/shrink/basis` · `justify-content`(六种, 含
  space-around/evenly) · `align-items` / **`align-self`** · **`order`** ·
  **`flex-wrap`**(换行/换列, 与 gap、align、justify 正确叠加) · `gap`
- **排版单位**: `line-height` 的三种语义分开处理 —— 无单位是**倍数**、
  `px` 是绝对像素、`%`/`em` 相对字号; `margin`/`padding` 百分比按
  **包含块宽度**解析(CSS 规定四个边都一样)
- **关键帧动画(@keyframes)**: 多段动画定义 + `animation: <名> <时长> <缓动> 
  infinite alternate` 简写; 时间轴逐帧采样, 支持 iteration/方向/fill。
  适合"持续旋转/呼吸/脉冲/颜色循环"等无法用二态 transition 表达的效果
- **3D 变换**: `transform: rotate/rotateX/rotateY/rotate3d + translateZ +
  perspective + transform-style: preserve-3d`; 4×4 层级矩阵栈 + 透视投影
  (近大远小) → 投影四边形光栅化(绘制指令 QUAD, hnsoft 与 cairo 双后端
  同语义)。preserve-3d 父子矩阵逐级复合(真立方体/卡片环两级变换),
  父级 2D scale/rotate 折叠进子级 3D 坐标系; flat 摊平按**单应**精确
  落位(不再三点仿射近似第 4 角)。QUAD 面片带**像素渐变**/边框/文字
  (渐变随面片投影 —— hnsoft 双三角仿射逐像素采样、cairo 单路径无缝
  填充; 轴长与 RECT 同一条 CSS 口径; 边框按投影四边形出梯形; 子树
  文字/盒尺寸按锚点处局部线尺度取档, 面片内近大远小; deco 线与 li
  标记随同一变换), flex/absolute/inline-block 子树与普通块流同一绘制
  路径。可做卡片翻转、3D 倾斜面板、真立方体、卡片环 ——
  见 `examples/threejs-lab.html`(口径由 `tools/quad3d_probe.c` 钉死)与
  `examples/threejs-ui.html`(three.js 风格 3D 数据大屏:透视舞台/网格地板/
  全息面板/六面立方体/环形仪表, 支持度矩阵见 `docs/threejs-support.md`)
- **Canvas 2D(`<canvas>` + QuickJS)**: `<canvas>` 替换元素 + `hnGet2D(id)`
  返回标准形态的 2D context —— fillStyle/strokeStyle(`#hex/rgb()/rgba()`+
  线性渐变)、save/restore、CTM(translate/scale/rotate/setTransform)、
  完整路径命令(moveTo/lineTo/arc/closePath, fill/stroke/clip)、
  fillRect/strokeRect/clearRect、fillText/strokeText、drawImage。
  **状态机在 JS 层**(与 CanvasKit htmlcanvas 同构的语义参照), 坐标经 CTM
  变换后以已解析原语落账本, 绘制期翻译成同一条显示列表(RECT 渐变/POLYGON/
  QUAD/TEXT/IMAGE/CLIP)—— 无 Skia、无 WebGL。画智能 UI 仪表盘/折线图/
  圆弧仪表开箱即用, 见 `examples/canvas-demo.html`
  (口径由 `tools/canvas_probe.c` 61 断言钉死)
- **Lottie 矢量动画**: `<img src="a.json" hn-lottie>` 直接播 Lottie/bodymovin
  文件 —— 引擎解析 JSON、按时间轴求值, 产出**多边形绘制指令(POLYGON)**,
  所以不必为每个平台写一遍播放器, 也不需要 WebView。支持形状层
  (组/矩形/椭圆/贝塞尔路径) / 填充 / 描边 / **多 stop 线性渐变填充 gf 与
  渐变描边 gs**(两种 stop 编码、0..1 与 0..100 两种量纲; 形状局部空间切
  色带, 后端零改动)/ 纯色层 / 图片层, 以及
  锚点·位置·缩放·旋转·不透明度与**路径变形**关键帧; **mask 最小版**
  (masksProperties 单条加法蒙版 → 路径外接矩形 CLIP 裁剪, 支持蒙版路径
  关键帧)。
  可选 `hn-lottie-speed`(倍速)、`hn-lottie-fit`(contain/cover/fill/none)。
  **明确不支持**(跳过而非报错): 预合成层(会展开)、文本层、表达式、特效、
  轨道遮罩、merge paths、时间重映射、径向渐变的形状化、多蒙版布尔组合
- **网格变形贴图(Live2D 类效果的原语)**: `<img src="c.png" hn-mesh="12x10"
  hn-mesh-sway="6" hn-mesh-anchor="bottom">` —— 把贴图映射到可变形网格,
  顶点按正弦形变(固定端不动, 形成波浪)。这与 Live2D 让立绘"活起来"是
  **同一套底层原理**, 产物是 MESH 绘制指令(三角形仿射纹理映射,
  CoreGraphics 与软件光栅双后端实现)。更复杂的装配(骨骼/物理/口型同步)
  可由脚本逐帧调用 `hn_node_set_mesh_verts()` 写入顶点 —— 引擎只做
  "网格 + 贴图"的合成, rig 逻辑属于应用层。
  ⚠️ **Live2D 的 `.moc3` 是专有格式**, 解码需要其 Cubism SDK(商业授权),
  本项目不自研解码器; 交付的是同技术的开放原语
- **透明背板**: `<meta name="hn-transparent" content="1">` 让窗口**无底色**
  (逐像素 alpha, 桌面/下层窗口从透明处透出), 配合 `hn-shadow`(投影开关)、
  `hn-draggable`(空白区拖动开关)。引擎在每次样式重算后都重新剥离 html/body
  底色, 所以 resize 与热更新都不会把底色装回来。适合圆形/HUD/悬浮控件窗口。
  见 `examples/transparent.html`、`examples/lottie.html`
- **动画系统(引擎级, 不是滚动专属)**: `transition` 过渡 + 
  **`animation: up|down|left|fade|scale <时长>` 入场预设**(新内容平滑浮现而非硬闪) +
  **`translate` / `scale` 几何动画**(位移并入滚动偏移、缩放按盒中心换算, 
  子元素盒与文字一起变换) + **缓动可配**(`linear / ease / ease-in / ease-out / 
  ease-in-out / cubic-bezier(a,b,c,d)`, 贝塞尔用牛顿迭代反解, 与 CSS 同法;
  **弹性族闭式解**(无逐帧积分状态, 与引擎"按 t 求值"同口径): `spring`
  衰减振荡 / `ease-out-bounce` 落地弹跳 / `ease-out-elastic` 橡皮筋 /
  `ease-out-back` 回拉越过, 以及 **`steps(n[, start|end])`** 分段;
  简写里缓动名先于动画名识别, 否则 `spring` 会被当成 keyframes 名吞掉)。
  **按属性独立过渡**(`transition: opacity .3s, transform .5s` 顶层逗号分段、
  槽位对位; longhand `transition-property/duration/delay/timing-function`
  齐备, 未单独声明的属性槽不过渡 —— 与 CSS 一致, 无属性段时退回旧简写行为) +
  **`transition-delay` / `animation-delay`**(负进度实现: 延迟未耗尽停在起始态,
  负值直接从中途开始; stagger 用 `animation-delay: calc(var(--i) * 0.1s)`)。
  全部由引擎插值, 页面只声明; 帧循环与 vsync 对齐
- **嵌入式 JavaScript(QuickJS)**: `<script>` 内的 JS 走**内嵌 QuickJS**
  (Zlib 协议, 静态链接进 hn_rt; 构建期探测到才启用 —— 引擎本体仍是零依赖纯 C,
  没有 QuickJS 时 JS 段整体编译出局)。定位是 HTML **交互业务能力的补充**:
  计算/状态机/条件逻辑用 JS, 结构与样式仍用 HTML/CSS。
  桥接函数 `hnSetText(选择器, 文本)` / `hnSetValue(选择器, 值)` 直改 DOM,
  走**同一条布局绘制管线** —— 与 HTML 写的完全等价; daemon 的 `eval` op
  即由此求值。runtime 与进程同生命周期(daemon 本就常驻, 规避 GC 断言)。
  无 `<script>` 且无 eval 时零开销(不创建 JS 上下文)
- **媒体元素(`<video>`/`<audio>`, 解码委托平台)**: 元素解析照常, 语义是仿
  HTMLMediaElement 的**六态状态机**(IDLE/LOADING/READY/PLAYING/PAUSED/ENDED),
  声明属性即语义(`src`/`autoplay`/`loop`/`muted`/`volume`, seek 钳制到
  [0, duration])。**引擎不认识解码器**: 解码/出声委托平台媒体框架 —— macOS 走
  **AVFoundation**(运行期 dlopen + objc_msgSend, 与引擎零 SDK 依赖同一手法),
  **非 FFmpeg 路线**(不打包、不链接、不 dlopen); 其他平台 ABI 留位, 无宿主时
  元素照常解析只是不播。平台差异收敛在 `hn_media_host` 回调表(与图片后端同一
  依赖倒置), `currentTime` 唯一真源是宿主 `position()`。绘制新增
  `HN_CMD_BITMAP` 指令(hnsoft 与 cairo 双后端), `<audio>` 永不发位图。控制
  入口两层: C 端 `hn_media_play/pause/seek/…`(元素寻址)与 QuickJS 桥
  `hnMediaPlay/Pause/Seek/Time/Duration/Volume`(daemon `eval` op 即可驱动)。
  测试双轨: hncore **合成宿主**(梯度帧+假时钟, 确定性)供
  `media_element_probe.py` 40 断言全绿; 真机 AVFoundation 由 `media_probe.py`
  实测(播放推进/seek 落位/pause 恒定/纯音频无帧/系统样例抽真帧,
  18 ok 0 fail)。**真窗口播放已接线**: `app/hn_media_platform.c` 把平台
  ABI(`hnp_media_*`)适配成 `hn_media_host` 交 `hn_rt_set_media` ——
  hn_app = AVFoundation 平台宿主(视频解码进 `HN_CMD_BITMAP`、音频经
  AVPlayer 直出), headless/Linux 目标与 hncore 探针 = 同语义合成宿主
  (梯度帧假时钟); 平台宿主缺席或单文件 open 失败时降级为 poster/深底占位
  (hn_paint 占位 RECT, 不静默空白)。窗口内 `m` 键切媒体静音;
  `HN_MEDIA_TRACE=1` 打会话状态行, 验收见 `tools/media_window_probe.sh`。
- **WASM 运行时(wasm3, 纯计算导出)**: 内嵌 **wasm3**(MIT, vendor 快照最小
  11 源 + LICENSE, `-std=c99` 零警告零外部依赖, 三平台无条件编入)。引擎封装
  `hn_wasm_load/call/free` + 实例表 `hn_wasm_install/by_id/unload`(id 1..32);
  定位是**纯计算导出函数**(值按 int32 进出, 引擎按模块导出签名转换
  i32/f32/f64), 不做 WASI、不用 WASM 解码媒体。QuickJS 桥
  `hnWasmLoad(bytesOrPath)` → id、`hnWasmCall(id, fn, ...args)` → 结果
  (失败 NaN, 不抛异常)。demo 模块 `examples/wasm/add.c`(zig cc
  wasm32-freestanding, `add.wasm` 已入库); `wasm_probe.py` 19 断言(装载/
  调用/签名转换/错误路径)全绿。
- **媒体/WASM 端到端示例**: `examples/media-demo.html` —— `<video>` 播放区
  (真窗口里 AVFoundation 真播, 自绘控制条 + 静音钮; 引擎不做浏览器控件 UI,
  media-design §9)+ `hnWasmLoad`/`hnWasmCall` 计算卡 + `sys://` 系统卡,
  四段各走一条后端; 样片 `examples/assets/demo.mp4`
  (320x180 H.264+AAC 4s)入库。真窗验证:
  `cd examples && HN_MEDIA_TRACE=1 ../dist/hnapp-macos-arm64 media-demo.html`
  看 `[media] … state=PLAYING t=…` 前进(m 键切静音);
  无头验证: `hncore paint` 输出 `BITMAP/MEDIA` 行, daemon `eval` op 驱动
  JS 桥, `tools/media_window_probe.sh` 17 断言钉死整条链路。
- **Markdown 直染(agent 母语)**: `<div hn-md>…markdown 源…</div>` 装载即替换成
  普通 DOM 子树 —— 标题/列表(嵌套 2 层)/任务清单/围栏代码块(data-lang)/引用/
  管道表格(对齐)/分隔线; 行内 **粗** *斜* `码` ~~删除~~ [链接](url) 与嵌套、
  转义; 图片 `![]()` 走既有 img 管线; 围栏代码块**三色语法高亮**
  (注释/字符串/关键字, 逐行口径)。之后样式/布局/绘制/交互零特例。
  **链接点击导航**: `#锚` 滚进可视区(引擎内), 外部链接置 pending 由宿主
  消费 —— 窗口壳路径直接系统打开(hn_rt_button → sys://open 同一出口),
  daemon `event` op 回传 `"link":"…"` 由 agent 决定。明确不做:
  HTML 内嵌/脚注/缩进式代码块/跨行块注释延续。
  见 `examples/md-demo.html`(agent 报告窗); 口径由 `tools/md_probe.c`
  107 断言钉死
- **JSON / CSV 直染(结构化工具输出)**: 与 markdown 同一模式、同一装载挂点
  (`hn_context_set_doc` / `hn_context_render` / `hn_doc_swap` 三入口的
  hn_md 批次内), 源文本替换成普通 DOM 子树, 之后零特例。
  `<div hn-json>…</div>` 走既有 `hn_json.c` 解析 → 摘要条 `.json-head`
  (`{…} N keys · N KB`)+ 逐行缩进树 `.json-row`/`.json-kids`(内联
  padding-left), 令牌 `tok-k` 键 / `tok-s` 字符串(转义与中文原样) /
  `tok-n` 数字 / `tok-b` 布尔与 null / `.json-brace` 括号;
  `<div hn-csv>`(可选 `hn-csv-sep=";"`)→ 既有 table 语义
  (table/tr/th/td, md 表格同款皮肤), 首行表头 `.csv-head`,
  引号包围/引号内逗号与换行/`""` 转义(RFC4180 子集),
  **逐列嗅探数字列(>70% 可解析)整列右对齐**。
  **失败优雅回落**: 半截 JSON / 畸形 CSV / 空 → 顶部错误条或直接原样
  代码块, 不抛错不中断装载。**截断口径**(agent 巨输出不炸布局):
  JSON 深度 6(超出显示 `…`)/ 数组 50 / 对象 100 / 行预算 400;
  CSV 200 行 / 12 列, 截断处补 `… more N items` / `… 共 N 行 · 共 N 列`。
  见 `examples/fmt-demo.html`(agent 工具输出窗); 口径由
  `tools/fmt_probe.c` 118 断言钉死
- **流式视图(agent 轨迹/日志/对话)**: 声明 `hn-stream` 的 overflow 容器即获得
  "自动跟随底部"语义(引擎实现, 页面零脚本): 内容增长时**平滑追上**
  (帧率无关指数逼近 τ=80ms, 实时帧驱动; 离屏单帧路径直接贴底),
  用户上翻(离底 >48px)让出不追(动画即断), 滚回底部附近自动恢复;
  **`hn-stream-loop="N"` 环形缓冲**只保留最近 N 个子元素, 内容无限追加
  而内存与视觉收敛。整文档替换式流式推送的最简姿势:
  脚本持有累积缓冲 + daemon `update` 整推 —— 完整演示 `tools/chat-stream.sh`
  五步推流 + 三帧留档, 见 `examples/agent-chat.html` 与
  `screenshots/chat-stream-{1,2,3}.png`
- **动画与呈现**: 引擎按 t 求值 —— 过渡/@keyframes/Lottie/网格变形由
  `hn_context_anim_tick(dt)` 单一时钟推进, 壳的帧循环只负责喂 dt:
  `hn_rt_frame` 返回唤醒节拍(动画→16ms / 最近 hx 轮询→期限 / 空闲→阻塞),
  hn_app/hnweb 消费它 —— **空闲零空转**(静态文档真窗 CPU 0.0%);
  `#锚`导航与 hn-stream 同款**指数逼近平滑动画**(实时帧驱动, 离屏/shot
  直接落位, 用户滚轮即中断); rt_render/shot 在 relayout 后**零步进重采样**
  —— 过渡/关键帧的中间态真正落到像素(此前真窗只见终态)。
  缓动曲线全部**帧率无关**, 弹性族是闭式解。这些性质由 anim_probe(31)、
  stream_probe(30)与各探针的回归断言守着
- **设计令牌基底(`hn-theme`)**: `dark`/`light` 一行声明即得完整设计系统——
  色彩令牌(`--accent/--bg/--card/--line/--ink/--dim`)、字阶与行距、卡片/按钮/输入/骨架
  居中、sys:// 片段自动皮肤化、卡片投影与 hover 反馈。作者 css 排在其后, 任意令牌可覆盖。
  **AI agent 不写一行 CSS 也能产出精致界面** —— 这是"可靠 + 符合审美"的框架层保证
- **可靠性**: 携带 hx-* 但缺 id 的元素在装载时自动分配 id(`hx-auto-N`),
  load/轮询/点击的目标定位不再因漏写 id 而静默失效 —— agent 生成代码的容错层
- **网页习惯端到端**: `examples/webpage.html` —— DOCTYPE + link 外链 CSS +
  语义标签 + hex3/hsl/rgba 前导点 + rem/vw + 子选择器/兄弟选择器 + inline-flex
  chip + 50% 圆头像, 一份"按浏览器写法"的文件零改动直接渲染
- **输入控件**: `<input>`/`<textarea>` 值状态、caret(DOM 记录 byte 偏移 + 闪烁)、
  点击/`Tab` 聚焦(在控件间轮换)、原生键盘编辑(退格/删除/UTF-8 插入)、
  `:focus` 伪类、`formEncoded()` 表单 URL 编码;
  **textarea 多行**(值按行布局, caret 跟随所在行)、
  **input 回车提交**(自身 → 祖先 → 最近容器子树内第一个 hx-post/get 载体);
  **控件 type 语义**(`type=text/password/checkbox/radio` + textarea 各归其位
  —— 此前 checkbox/radio 会被当成可编辑文本: password 绘制掩码但值保持原文,
  checkbox/radio 绘制方框对勾/圆点、不可输入, 观感缺省强调色填充、作者
  background/border 优先; `:focus` 缺省视觉为强调色 2px 外扩描边, 与主题
  作者的 `:focus` 规则并存不冲突)、
  **状态伪类**(`:disabled`/`:enabled` 按 disabled 属性、`:checked` 按
  checkbox/radio 的 checked 属性, 值 "false"/"0" 视为未选/启用;
  禁用控件拒绝聚焦)、
  **表单提交**(`name=value` 收集 + `application/x-www-form-urlencoded` 编码;
  checkbox/radio 仅在选中时提交, 值取 value 属性、缺省 `"on"`; disabled 不参与)
- **系统集成**: `hx-get="sys://…"` 由本地桥直接应答(零网络):
  `info`(全量卡) / `cpu`(双采样占用) / `memory` / `disk` / `battery` /
  `uptime` / `host`, 返回自带样式的 HTML 片段, 换入即用。
- **消息卡片**: popup 表面即消息卡 —— `hn open` 秒开、`hn close` 销毁;
  daemon 路径自动执行 `hx-trigger="load/every"`。`--ttl` 到期自毁、
  `hn-drag` 拖拽手柄、`hn-dismiss` 点外关闭在壳层路线图上。
- **人类开发闭环**: 写 `app.html` → `hn_app`/`hn open` 即开(daemon `update`
  热推送新文档, 窗口状态保持); `<include src=…>` 零构建组件(装载期文本级
  拼接, src 相对**包含它的文件**, 嵌套上限 6, 环检测/缺文件容错占位 ——
  口径由 `tools/include_probe.c` 23 断言钉死); `sys://store/get|set` 应用级 KV 持久化、`sys://clipboard/get|set`、
  `sys://notify`、`sys://open` —— 全部本地, 无网络无服务进程。
  `hn new` 脚手架在路线图上(mtime 热重载已随 `hn_app --watch` 落地:
  源文件保存 ~300ms 内真窗口热更新, daemon update 对窗口应用即写源触发)。
- **daemon 接管开窗(窗口即 UI)**: agent 一条命令桌面弹原生窗 ——
  `hn open <id> <file.html> [--w N --h N] --window`, daemon 不建文档
  (不占 headless 槽位), 而是 `posix_spawn` 平台壳子进程
  (`hnapp-macos-arm64 <path> [W H]`, 路径/资产解析口径与 `hn_app` 直开
  完全一致), 回 `{"ok":true,"pid":N,"window":true}`。进程模型:
  **每窗一进程, 关窗即退** —— `hn close <id>` SIGTERM+waitpid 回收,
  daemon 退出(SIGINT/SIGTERM/SIGHUP)对存活窗口统一收口, 不留孤儿窗、
  不留 stale socket; 非 macOS 或平台壳二进制缺失自动回退 headless
  (响应标注 `"fallback":"headless"`)。`hn list` 里窗口应用以
  `kind:"window"`(带 pid/w/h)与 headless 应用并列。**update 热推送对
  真窗口生效**: `hn update <id> <file>` 把新内容(html + 可选 css 以
  `<style>` 并入)**原子写回** spawn 的源文件(同目录 tmp + rename, 子
  进程永远读到完整文档), 子进程 `hn_app` 的 mtime watch(~300ms 轮询,
  `--watch 0` 可关)检测到变化即整文档重载 —— 窗口表面/进程不动, 内容
  替换 + 重绘, 不闪不退(装载失败保留旧文档, 下个轮询自然重试)。
  分工: **headless 是 agent 的渲染/截图管线, window 是给人看的桌面窗**
  —— `eval`/`shot`/`dump`/`text` 对 window 应用回
  `"error":"unsupported for window apps (headless only)"`(文档在子进程
  里, daemon 无句柄)。无安装、无 Dock 常驻应用形态, 窗口即 UI。生命
  周期与热推送由 `tools/window_probe.sh` 断言(open→list→update×2→
  分工错误→close→退出收口, 25 断言); 真窗口流式演示见
  `tools/chat-stream-window.sh`(肉眼版)。
- **交互**: 命中测试(冒泡找 id)、`hx-get/post/target/swap`、
  `hx-trigger`(`click` | `load` | **`every Ns`** 轮询)、
  hx 请求自动携带表单参数(GET 拼 query / POST 发 body)、
  **过渡动画**(`transition`, 引擎持有当前值 + 运行时帧循环插值)、
  热更新三通道(render 整体 / swap 片段 / set_text)
- **平台窗口壳**: `tools/hnweb_linux.c` —— X11 窗口(运行期
  `dlopen("libX11.so.6")`, 编译期零依赖) + **hnsoft 软件光栅**;
  Windows 侧 `tools/hnwin.c`(Win32), macOS 侧 `tools/hnweb_macos.c`
  (dlopen AppKit/CoreGraphics)。三者跑同一份 C99 引擎管线, 换的只是
  窗口/事件/像素搬运层; Linux 壳零外部依赖、musl 静态,
  `ZIG=zig ./tools/build-multiplatform.sh` 交叉编译产物 `hnweb-linux-*`
  (x86_64 / aarch64, 无头 CI 走 `--probe`/`--shot` 自检)。
  **M2 已落地**: 事件派发管道(点击/键盘/滚轮/hover, 三壳同构)
  与动画帧循环(过渡/@keyframes/Lottie/网格变形共用, 空闲零唤醒);
  新三层(`platform/hn_platform.h` + `app/hn_app.c`)是这些壳经验的收敛版。

## 智能 UI 组件预设(上层, include 即得)

这是 README 定位图里"上层: 组件库 / UX 规范"的第一块砖 —— 纯上层资产,
零引擎改动。agent 现场生成 UI 时不必每次从零写样式: **include 一次, 写类名即可**。

```html
<meta name="hn-theme" content="dark">                       <!-- 或 light, 皮肤自动切换 -->
<link rel="stylesheet" href="examples/ui-kit-head.html">    <!-- 当前装载路径 -->
<include src="examples/ui-kit-head.html">                   <!-- 片段引入等价写法 -->
```

设计原则一句话: **只依赖 hn-theme 令牌(--accent/--bg/--card/--line/--ink/--dim),
主题自动皮肤化; agent 生成时 include 一次, 写类名即可** —— 不写死任何主题色
(唯一的扩展面是 ok/warn/err 状态色与代码底色, 以 `var(--ok, 缺省)` 形式提供,
主题可覆盖)。组件清单(每个组件头部带"用途 + 最小 HTML 用法"注释, 可直接照抄):

| 类名 | 用途 |
|---|---|
| `.chat` `.msg` `.msg.end` + `.chat-ai` / `.chat-user` | 对话气泡: AI 左侧强调色描边卡, 用户右侧实底渐变卡 |
| `.tool-card`(`.folded` 折叠) + `.tool-head/.tool-name/.tool-body/.tool-state.ok/run/err` | agent 工具调用卡: 标题行 + 状态 chip + 可折叠体 |
| `.code-card` + `.code-head/.code-lang/.code-file` + `<pre>` | 代码卡: 深底等宽 + 顶部语言标签 |
| `.stats` + `.stat-card`(`.stat-num/.stat-label/.stat-delta.up/.down`) | 统计卡: 大数字 + 标签 + 环比小字, 三列网格 |
| `.progress` + `.progress-fill` | 细进度条, 强调色渐变填充 |
| `.bar-mini` + `<b>`(行内高度, `.hi` 高亮) | 迷你条形组(趋势/分布) |
| `.tag` `.tag-ok` `.tag-warn` `.tag-err` | 小标签(中性 + 三档语义色) |
| `.skeleton` | 骨架条(呼吸动画, 观感同主题 `.sk` 但只依赖 `--line`) |
| `.divider-label` + 两根 `<i>` | 带文字的分隔线 |
| `.kbd` | 键帽(等宽 + 细边 + 底部投影) |
| `.dot` `.dot-ok/.dot-warn/.dot-err/.dot-run` | 状态圆点(运行中自带呼吸) |

活文档: `examples/ui-kit.html`(920x900, 一段模拟的 agent 会话把上述组件全部
真实排布一遍) —— 渲染命令与双路径出图见该页头注释。

## 已知简化与路线图

- 片段热更新走 arena, 旧节点不回收; class/id 选择器统一小写
- 近期: select、grid 布局、多屏/多表面组合、`hn new` 脚手架;
  **mtime 热重载已落地**(`hn_app --watch`, 缺省开 —— update 热推送
  真窗口即靠它; `hn dev` CLI 脚手架仍路线图)
- 已完成: **daemon 接管开窗 op** —— open 带 `--window`(`"window":1`)
  由 daemon spawn 平台壳子进程开真窗, 每窗一进程, close/list/退出收口
  统一管理(见"daemon 接管开窗(窗口即 UI)"); 顺手修掉 `hn list` 一开
  应用就把 daemon 打崩的 FORTIFY 越界与 SIGHUP 无 handler 留 stale
  socket 两个旧账
- **媒体/WASM 收尾**: 引擎侧已落地(契约见 `docs/media-design.md`, 逐项实现
  状态标注在该文末) —— `<video>`/`<audio>` 六态状态机 + hncore 合成宿主 +
  `hnMedia*`/`hnWasm*` JS 桥、wasm3 内嵌 + `hnWasmLoad`/`hnWasmCall`、
  三套探针全绿与 `examples/media-demo.html`。**窗口壳媒体宿主已接线**
  (`app/hn_media_platform.c`: hnp_media_* → hn_media_host →
  hn_rt_set_media; hnapp-macos-arm64 = AVFoundation 真解码/真出声,
  hnapp-headless/linux = 合成替身), `tools/media_window_probe.sh` 17 断言
  (真窗存活/PLAYING 且 t 前进/音频 dur 真解析/两帧像素不同/headless 降级/
  open 失败降 IDLE 占位)全绿。剩: 窗口内 `<script>` 求值入口仍是
  daemon `eval` op("点击→JS"未接线, 真窗静音走 m 键)
- 弹窗原生交互: `hn-drag`(元素即拖拽手柄)、`hn-dismiss`(点击弹窗外自动关闭)
- 中期: Windows 壳已有 `tools/hnwin.c`(Win32 + hnsoft, 纯 C99);
  **原生 Linux 运行时已落地**(`tools/hnweb_linux.c`: M1 窗口/渲染 +
  M2 事件派发管道与动画帧循环); 继续: 宿主协议鉴权、
  `.hnapp` 包签名与资源(字体/图片)打包
- 远期: 常驻宿主的发现机制(Bonjour/命名空间)、组件生态层(在引擎之上,
  不进引擎)

<!-- deepgit:begin progress -->
## 项目进度

> 本区域由 **deepGit** 自动维护（浅更新）· 更新于 2026-10-01 00:05
> 追踪 1 个分支 · 2 处未提交改动

### 工程脉搏

- 提交构成：`docs` ×3 · `other` ×27

### `main`（默认分支 · 当前）

- **状态**：闲置 · 最近提交 11 天前（`a6b37000` mesh 驱动路径: 竖向 anchor 权重写反了 + 新增 19…）
- **摘要**：最近 30 个提交：更新×15、文档×4、新增×3
- **近期进展**
  - 新增：“mesh 驱动路径: 竖向 anchor 权重写反了 + 新增 19 条断…
  - 更新：“Lottie 四个真实缺陷 + translateZ/rotate3d 投影”
  - 更新：“变换引擎: 非等比缩放 + transform-origin(又是三个静默…
  - 修复：“透明背景层: 修掉三个让透明背板退化成实色的 bug”
  - 修复：“选择器引擎与 calc: 修掉一批"声明了不生效也不报错"的…
- 本次记录 30 个提交
<!-- deepgit:end progress -->
