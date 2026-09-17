# html-native

[![release](https://img.shields.io/github/v/release/asdshuaishuai/html-native?color=4f7cff)](https://github.com/asdshuaishuai/html-native/releases)
[![site](https://img.shields.io/badge/site-asdshuaishuai.github.io%2Fhtml--native-4f7cff)](https://asdshuaishuai.github.io/html-native/)
[![license](https://img.shields.io/badge/license-MIT-3ecf6f)](LICENSE)

> 与 RN / Flutter 同级的原生 UI 框架——UI 描述语言是 HTML。
> 形态是一个**系统级 PWA / 小程序引擎**：随时生成、销毁、持久化一个应用。
>
> 渲染以自研 C99 引擎为主(原生、无 WebView), 可显式声明 WebKit 兜底。
> **人类和 AI 都能直接开发**：会 HTML/CSS 就能出原生窗口，无构建、无
> 依赖、无 JavaScript；目标是把应用开发的技术成本压到最低。
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
   的 `hx-*` 属性由运行时解释, transport 可注入(进程内 / URLSession /
   `sys://` 系统桥, 不绑定 HTTP 服务)。
2. **应用是易变对象**。像 PWA/小程序一样: 生成(秒开一个表面)、
   销毁(close)、持久化(persist → `.hnapp` 包, 离线可再唤起)、
   热更新(同 id 推送新 hn 编码, 窗口与状态保持)。
3. **HTML 是最低成本的 UI 语言**。人类手写或 AI 生成是两条等价入口:
   人类走 `hn new → hn dev` 热重载循环(保存即生效, 无构建链);
   agent 现场生成 hn 编码推给常驻宿主, 系统里即刻出现信息窗口——
   不拉浏览器, 不装应用。

## 快速开始

```bash
git clone https://github.com/asdshuaishuai/html-native && cd html-native
swift build
alias hn=.build/debug/Hn

hn new myapp && cd myapp      # 生成骨架: app.html / app.css / head.html
hn dev app.html --css app.css # 开发模式: 保存即热更新
```

只需要会 HTML 和 CSS。无需 Node、npm、打包器、JavaScript。

**不想用命令行**：把这套能力给 agent 用 —— 项目自带 MCP server
(13 个工具) 与 skill (`SKILL.md`)。除了开窗口与内省，还能**驱动**界面：

| 能力 | 工具 | 说明 |
|---|---|---|
| 创建 / 热更新 / 销毁 | `hn_open` `hn_update` `hn_close` | 传 HTML 内容或文件路径 |
| 感知 | `hn_dom` `hn_text` `hn_dump` `hn_list` | DOM 树 / 某元素文本 / 绘制指令 / 应用列表 |
| **驱动** | `hn_event` `hn_eval` | 注入点击键盘等事件；在该应用 JS 上下文里求值 |
| 离线 / 系统 / 截图 | `hn_persist` `hn_restore` `hn_sys` `hn_shot` | `.hnapp` 离线包 / `sys://` 数据 / 渲染 PNG |

`hn_event` 回传三态，agent 能区分"已处理""已派发但无人监听""目标未找到"；
`hn_eval` 在 native 与 webkit 两个渲染器上都可用（native 走系统自带的
JavaScriptCore），因此自动化不必关心用了哪个渲染器。

**软件光栅化器(hnsoft)**：引擎的绘制后端接缝支持多后端 —— macOS 用 CoreGraphics
(原生质量)，跨平台/无 GUI 场景用 `hnsoft`(纯 C 软件光栅化器 + FreeType 文本)。
`hncore render app.html out.png 520 692` 在任何有 C 编译器的平台产出 PNG，
适合服务器端渲染截图、CI 视觉回归。渲染后端选择与评估见
[ADR-001](docs/adr-001-renderer.md)。

**三平台引擎二进制**：`ZIG=zig ./tools/build-multiplatform.sh` 一条命令产出
macOS/Linux/Windows 五个目标产物（`hncore` 引擎 CLI，用于验证与集成测试）。
详见 [release](https://github.com/asdshuaishuai/html-native/releases)。

## 架构与代码地图

```
Sources/CHtmlNative/  引擎核心(C99, ~2200 行, 零依赖)
  hn_html.c    hn 编码解析 → DOM(arena)
  hn_css.c     样式表解析 → 规则(选择器展开/特异性)
  hn_style.c   级联 + 继承 + UA 样式表
  hn_layout.c  block 流 + flexbox + 文本换行(CJK 硬拆)
  hn_paint.c   DOM → 绘制指令列表(display list)
  hn_json.c    极简 JSON 解析(零依赖; Lottie 等公开格式需要)
  hn_png.c     PNG 解码(含自带 inflate; 让软件光栅后端真能显示图片)
  hn_lottie.c  Lottie 求值 → 矢量指令(多边形/图片/矩形)
  hn_mesh.c    网格变形贴图 → MESH 指令(Live2D 类效果的原语)
  hn_context.c 会话: 布局编排/命中/hot-update/清单解析
Sources/HtmlNative/   macOS 运行时 + 应用模型
  TextShaper.swift     CoreText 文本后端(测量回调)
  HNPainter.swift      display list → CoreGraphics
  HNLayerCompositor.swift 网格变形合成(三角形仿射纹理映射)
  ImageStore.swift     图片加载/缓存; AssetStore.swift 外部资产(Lottie JSON)
  HtmlNativeView.swift 渲染视图 + htmx(hx-*)执行 + sys:// 应用路由
  HNEngine.swift       应用注册表: open/update/close/persist/restore
                       表面物化: window(NSWindow)/popup(NSPanel 浮层)
  SystemBridge.swift   sys:// 只读系统桥(cpu/内存/磁盘/电池/uptime/host)
  IncludeExpander.swift <include> 展开(零构建组件) + HNStore 本地 KV
  SysCard.swift        内置系统信息卡
Sources/HnDaemon/      常驻宿主: accessory 进程 + Unix socket + JSON-lines
                       DevWatcher(开发模式 mtime 热推送)
Sources/Hn/            hn 命令行(open/update/dev/new/…, 自动拉起宿主)
                       Scaffold.swift: hn new 项目脚手架
Sources/DemoApp/       嵌入式使用示例(引擎作为库嵌进一个应用)
Sources/RenderTest/    离屏验收(断言 + 任意文件出 PNG, 即窗口所见)
Sources/HnShot/        真实视图截图(cacheDisplay 自绘, @2x, 可注入 hover/scroll 态)
screenshots/           引擎效果截图(showcase / 行内 / 输入 / 交互 / 主题 / 系统卡 /
                       消息卡 / 脚手架 dev-app / 桌面仪表盘 dashboard / 网页习惯 webpage)
```

## 使用

### 命令行(agent 的系统入口)

```bash
cd html-native && swift build
D=.build/debug

# 生成: 秒开一个弹窗(表面类型由 hn 编码里的 meta 声明)
$D/Hn open agent-card examples/agent-card.html --css examples/agent-card.css

$D/Hn list                 # 运行中的应用
$D/Hn update agent-card examples/agent-card.html   # 热更新(窗口保持)
$D/Hn persist agent-card   # 持久化 → ~/.html-native/apps/agent-card.hnapp
$D/Hn close agent-card     # 销毁
$D/Hn restore agent-card   # 从包恢复

cat foo.html | $D/Hn open - --surface popup    # 管道: agent 生成 → 成窗
```

### 开发工作流(人类: 无构建, 保存即热更新)

```bash
$D/Hn new myapp && cd myapp    # 脚手架: app.html/app.css/head.html/README
$D/Hn dev app.html --css app.css
# 编辑器里改 HTML/CSS → ⌘S → 窗口半秒内热更新(mtime watch, 窗口与状态保持)
```

配套的人类友好能力: `<include src=…>` 文件级组件(嵌套/循环检测)、
`sys://store` 本地 KV 持久化(`~/.html-native/store/<id>.json`)、
`sys://clipboard|notify|open`。完整上手: [docs/getting-started.md](docs/getting-started.md)

### 双渲染器: 自研引擎为主, WebKit 显式兜底

```html
<meta name="hn-renderer" content="native">   <!-- 默认: C99 引擎原生渲染 -->
<meta name="hn-renderer" content="webkit">   <!-- 显式兜底: 系统 WKWebView -->
```

- **native(默认)**: 我们的 C99 引擎解析 → 布局 → 绘制指令 → CoreGraphics 落屏。
  零 WebKit 渲染进程, 这是主力路径。
- **webkit(显式声明)**: 交给系统 WKWebView, 用于引擎暂不支持的构造
  (`grid` / `position:absolute` / `canvas` / `svg` / `video` / 复杂选择器)。
- **绝不静默切换**: 必须页面显式声明。否则同一文件在不同环境下渲染出不同结果,
  那是灾难而非兜底。
- **两条路径共用一切上层语义**: 窗口生命周期(秒开/TTL/热更新/持久化/`hn dev` 热重载)、
  `sys://` 系统桥、`hx-*` 交互(click/load/`every Ns`/回车提交/表单参数)、
  `hn-theme` 设计令牌、本地 KV —— 换的只是渲染层, 应用模型完全一致。
- **CSS 归一化**: 引擎接受免单位数值(`padding: 16`), 标准 CSS 会忽略。
  兜底前自动补齐单位(长度属性补 `px`, 倍数/百分比/函数式值/关键字不动),
  保证同一份文件在两条路径下长相一致。
- **文本编码与中文字体**: 兜底路径自动注入 `charset=utf-8`(缺失时)与
  **中文系统字体栈**(`-apple-system` + PingFang SC + Hiragino Sans GB),
  避免 WebKit 默认 Times 导致中文 fallback 成宋体、两条路径字形不一致;
  `code/pre` 等宽栈与 native UA 对齐。中文解析/布局/表单编码全程 UTF-8 无损。
- 参考 `examples/webkit-fallback.html`(用 3 列 grid 演示兜底能力)。

### hn 编码里的表面声明(清单)

```html
<meta name="hn-surface" content="window|popup|layer">
<meta name="hn-window"  content="360x520">        <!-- 或 360x520@100,80 -->
<meta name="hn-title"   content="agent · 仓库分析">
<meta name="hn-theme"   content="dark">            <!-- 或 light: 内置设计令牌基底 -->
```

引擎只解析声明(C: `hn_doc_manifest`), 由宿主物化成对应表面。

### 宿主协议(Unix socket, ~/.html-native/hn.sock, JSON-lines)

```json
{"op":"open","id":"card","html":"<h1>hi</h1>","surface":"popup","w":360,"h":520}
{"op":"update","id":"card","html":"..."}
{"op":"close","id":"card"} / {"op":"list"} / {"op":"persist","id":"card"} / {"op":"restore","id":"card"}
```

### 截图(真实视图自绘, 无需屏幕权限)

```bash
$D/HnShot examples/showcase.html examples/showcase.css shot.png 920 620
$D/HnShot app.html app.png 480 620 --live        # css 可省略; --live 执行 sys:// 拉取
$D/HnShot examples/showcase.html examples/showcase.css hover.png 920 620 --hover nav-team
$D/HnShot examples/showcase.html examples/showcase.css scroll.png 920 620 --scroll frames,220
```

### 嵌入式使用(像 RN 一样嵌进应用)

```swift
import HtmlNative

let view = HtmlNativeView(html: html, css: css)
view.hxTransport = { action, done in   // htmx 网络能力: 注入任意 transport
    done(PanelFragments.generate(route: action.urlString))
}
// 或经应用模型:
HNEngine.shared.open(id: "panel", html: html, surface: .popup)
```

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
  **linear-gradient 渐变**、
  **box-shadow 阴影**、**overflow(裁剪+滚动)**、**cursor**、颜色背景、
  字体(size/weight/style/line-height/letter-spacing)、text-align、
  opacity; 颜色 **#hex(3/4/6/8 位)/rgb(a)/**`hsl(a)`/transparent/36+ 命名色
  (函数式颜色支持含空格写法); 单位 **px/pt/%/em/rem/vw/vh**;
  `display:inline-flex`(外层原子行内盒); `border-radius:50%`(百分比圆角);
  `text-decoration(underline/line-through/overline)`; `list-style(none/square/circle)`;
  **排版精细化**:`margin:0 auto` 居中 / flex `margin-left:auto` 推右(标准优先级压 justify)、
  `min/max-width/height` 约束、**完整 margin 简写正确展开 TRBL**(修复历史越界 bug)、
  `font-family(monospace/serif 系统设计, code 等默认等宽)**、默认行高 1.45、
  **窗口标题栏无缝**(透明标题条 + 底色跟随 body 背景 + 可拖拽空白区)
- **图片**: `<img>`(尺寸回退链: 样式 > 属性 > 固有尺寸), 图片后端注入
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
- **3D 变换**: `transform: rotate/rotateX/rotateY + perspective`;
  绕三轴旋转矩阵 + 透视投影(近大远小) → 四边形光栅化(新绘制指令 QUAD,
  CoreGraphics 与软件光栅双后端实现)。可做卡片翻转、3D 倾斜面板
- **Lottie 矢量动画**: `<img src="a.json" hn-lottie>` 直接播 Lottie/bodymovin
  文件 —— 引擎解析 JSON、按时间轴求值, 产出**多边形绘制指令(POLYGON)**,
  所以不必为每个平台写一遍播放器, 也不需要 WebView。支持形状层
  (组/矩形/椭圆/贝塞尔路径) / 填充 / 描边 / 纯色层 / 图片层, 以及
  锚点·位置·缩放·旋转·不透明度与**路径变形**关键帧。
  可选 `hn-lottie-speed`(倍速)、`hn-lottie-fit`(contain/cover/fill/none)。
  **明确不支持**(跳过而非报错): 预合成层、文本层、表达式、特效、
  trim path、遮罩与轨道遮罩、repeater
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
  ease-in-out / cubic-bezier(a,b,c,d)`, 贝塞尔用牛顿迭代反解, 与 CSS 同法)。
  全部由引擎插值, 页面只声明; 帧循环与 vsync 对齐
- **原生 JavaScript(JavaScriptCore)**: `<script>` 内的 JS 走**系统自带的 
  JavaScriptCore**(非 WebKit, 引擎仍是纯 C 与其隔离)。作为 HTML 
  **交互业务能力的补充**: 计算/状态机/条件逻辑用 JS, 结构与样式仍用 HTML/CSS。
  提供 `document.getElementById/querySelector/querySelectorAll/createElement`、
  元素上的 `textContent`/`innerHTML`/`classList`/`style`/`addEventListener`、
  以及高层 `hn.request`(复用 hx 语义)/`hn.get|set`(本地 KV)/`hn.log`。
  JS 的 DOM 变更走**同一条布局绘制管线** —— 与 HTML 写的完全等价。
  `hn-js="off"` 可关闭; 无 `<script>` 时零开销(不创建 JSContext)
- **流式视图(agent 轨迹/日志/对话)**: 声明 `hn-stream` 即获得"自动跟随底部"语义
  (运行时提供, 页面零脚本): 内容增长时平滑追上, 用户上翻时让出滚动条;
  `hn-stream-loop="14"` 环形缓冲只保留最近 N 条 —— 内容可无限追加而内存与视觉收敛。
  `sys://agent/step` 每次调用返回一条轨迹条目(思考/输出/工具调用/图片)演示循环滚动。
  见 `examples/agent-stream.html`
- **动画与呈现**: 帧循环用 `CADisplayLink`/`CVDisplayLink` **与 vsync 对齐**
  (不用 Timer —— 它与刷新率不同相, 60Hz 定时器配 120Hz 屏只会隔帧更新);
  缓动是**帧率无关的真指数逼近** `step = gap·(1−e^(−dt/τ))`, 无最小步长、
  中途步长取整像素(文字不因亚像素相位变化而发虚)。回归断言量化了这些性质:
  步长单调递减、60Hz 与 120Hz 收敛耗时相差 <35%、中途零小数帧、静止后零开销。
  实测滚动+重绘 0.02ms/帧(预算 16.6ms), 全量重排 2.44ms
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
  **input 回车提交**(自身 → 祖先 → 最近容器子树内第一个 hx-post/get 载体)
- **系统集成**: `hx-get="sys://…"` 由本地桥直接应答(零网络):
  `info`(全量卡) / `cpu`(双采样占用) / `memory` / `disk` / `battery` /
  `uptime` / `host`, 返回自带样式的 HTML 片段, 换入即用。
- **消息卡片**: `hn open <id> file.html --ttl 4` 秒开消息弹窗、到期热销毁;
  `hn-drag` 拖拽、`hn-dismiss` 点外关闭; `hn syscard` 打开常驻系统信息卡
  (sys:// 每 2s 轮询)。daemon 路径自动执行 `hx-trigger="load/every"`。
- **人类开发闭环**: `hn new` 脚手架 → `hn dev` 热重载(mtime watch);
  `<include src=…>` 零构建组件(嵌套上限 6, 循环检测);
  `sys://store/get|set` 应用级 KV 持久化、`sys://clipboard/get|set`、
  `sys://notify`、`sys://open` —— 全部本地, 无网络无服务进程。
- **交互**: 命中测试(冒泡找 id)、`hx-get/post/target/swap`、
  `hx-trigger`(`click` | `load` | **`every Ns`** 轮询)、
  hx 请求自动携带表单参数(GET 拼 query / POST 发 body)、
  **过渡动画**(`transition`, 引擎持有当前值 + 运行时帧循环插值)、
  热更新三通道(render 整体 / swap 片段 / set_text)

## 已知简化与路线图

- 片段热更新走 arena, 旧节点不回收; class/id 选择器统一小写
- 近期: select/checkbox、grid 布局、position:absolute、多屏/多表面组合
- 弹窗原生交互: `hn-drag`(元素即拖拽手柄)、`hn-dismiss`(点击弹窗外自动关闭)
- 中期: Rust 运行时(winit + skia) 覆盖 Windows/Linux、宿主协议鉴权、
  `.hnapp` 包签名与资源(字体/图片)打包、动画
- 远期: 常驻宿主的发现机制(Bonjour/命名空间)、组件生态层(在引擎之上,
  不进引擎)
