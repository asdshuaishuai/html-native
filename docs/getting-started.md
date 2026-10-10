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
- **系统能力开箱即用**。读 CPU/内存/电池、本地持久化、剪贴板、系统通知,
  全部是一行 `sys://` 地址,零网络、零依赖。

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

窗口已经出现了。现在做三件事,感受开发循环:

1. **改文字**:把 `app.html` 里 `<h1>myapp</h1>` 改成任意标题,⌘S 保存——
   半秒内窗口标题和界面同步变化。
2. **改样式**:把 `app.css` 里 `--accent: #4f7cff` 换成 `#ff6b6b`,保存——
   保存按钮连同主题色一起换(CSS 变量贯穿整个界面)。
3. **用存储**:在"你的名字"输入框打字,点保存,关掉窗口重新 `hn open`——
   值还在(本地 JSON 持久化,存在 `~/.html-native/store/`)。

`hn dev` 的本质:引擎 watch 源文件 mtime(0.5s 轮询),变了就以同 id
热推送新编码,窗口对象与进程不动,只有内容重排。

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

三种表面:

| surface | 物化成 | 适合 |
|---|---|---|
| `window` | 常规 NSWindow | 正式小应用 |
| `popup` | 浮层面板(不抢焦点、悬浮) | 消息卡、agent 输出 |
| `layer` | 状态栏层级 | 常驻小组件 |

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

全部本地、零网络,响应自带样式片段,换入即用:

| 地址 | 作用 |
|---|---|
| `sys://info` | 全量系统卡(主机/CPU/内存/磁盘/电池/运行时间) |
| `sys://cpu` / `sys://memory` / `sys://disk` / `sys://battery` / `sys://uptime` / `sys://host` | 单项数据 |
| `sys://store/get?key=k` | 读本地 KV(该应用持久化值) |
| `hx-post="sys://store/set?key=k"` | 写本地 KV(表单 `value` 字段) |
| `sys://clipboard/get` / `hx-post="sys://clipboard/set"` | 剪贴板读写 |
| `hx-post="sys://notify"` | 系统通知(title/body 表单字段) |
| `sys://open?url=…` | 用系统默认方式打开 URL/文件 |

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
`grid`、`canvas`/`svg` 还未实现(3D 变换/网格变形 MESH 指令已覆盖一部分
立体与形变需求)。**正在进行中**: 音频/视频(`<video>`/`<audio>`, 解码委托
平台媒体框架, 非 FFmpeg 路线)与 WASM 运行时缝(wasm3),
契约见 [docs/media-design.md](media-design.md)。
完整能力清单见 [README](../README.md)「支持的子集」。

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
hn open myapp app.html --css app.css    # 生成(表面由 meta 决定)
hn list                                 # 在运行的应用
hn update myapp app.html                # 热更新(文档生命周期保持)
hn shot myapp out.png                   # 渲染 PNG
hn close myapp                          # 销毁
```

持久化与轻应用槽位走宿主 socket op(`{"op":"persist",…}` /
`{"op":"restore",…}` / `{"op":"applets"}`), CLI 尚未包这一层 ——
任何 socket 客户端都能直接调。

所有命令走常驻宿主(`~/.html-native/hn-daemon.sock`, JSON-lines 协议):
先 `dist/hn-daemon &` 起宿主, 之后每个应用都是宿主里的轻量对象。

## 输入控件

`<input>` / `<textarea>` 开箱可用:点击聚焦(`:focus` 生效)、
Tab 在控件间轮换、原生键盘编辑(含中文输入与光标移动)、闪烁 caret。
表单值经 `hx-post` 自动编码发送。两个顺手的表单语义:

- input 里按**回车** = 提交:自动定位最近的提交载体
  (自身 → 祖先 → 最近容器内第一个 `hx-post`/`hx-get` 元素)再触发;
- textarea 支持多行:值按 `
` 逐行布局,光标跟随所在行。

## 调试与验收

```bash
dist/hnapp-macos-arm64 app.html --shot out.png 480 620   # 壳渲染落盘
dist/hncore-macos-arm64 render app.html out.png 460 560  # 引擎 CLI(hnsoft 后端)
dist/hn shot myapp out.png                               # daemon 无头渲染

# C99 离屏验收(61 断言; 位图即窗口所见, 光栅器就是生产用的 hnsoft)
cc -O2 -I Sources/CHtmlNative/include -I Sources/CHtmlNative \
   -I /opt/homebrew/include/freetype2 tools/render_probe.c \
   Sources/CHtmlNative/hn_{arena,html,css,style,layout,paint,context,theme}.c \
   Sources/CHtmlNative/hn_{json,lottie,mesh,png,media}.c \
   Sources/CHtmlNative/hnsoft.c -L/opt/homebrew/lib -lfreetype -lm \
   -o /tmp/render_probe && /tmp/render_probe
```

## 边界(诚实清单)

- JavaScript 走内嵌 QuickJS(可选): `<script>` 求值 + `hnSetText`/`hnSetValue`
  桥; 交互主体仍是声明式 hx-*。
- CSS 是实用子集,不是全量(无 grid 等;`nth-child` 已支持,其余在路线图上)。
- 图片支持 `<img>`;字体走 FreeType + 系统字体栈。
- 平台壳全部纯 C99: macOS `hnp_macos.c`/`hnweb_macos.c`(运行期 dlopen
  AppKit), Linux `hnp_linux.c`/`hnweb_linux.c`(dlopen X11),
  Windows `tools/hnwin.c`(Win32); 无头走 `hnp_headless.c`/daemon。
  没有 Swift、没有 WebKit、没有 .m 文件。

遇到问题先跑 `render_probe`(全绿说明引擎层健康),
再单独渲染你的文件定位是内容还是引擎的问题。
