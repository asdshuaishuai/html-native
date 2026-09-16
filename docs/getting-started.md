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
swift build                 # 一次性构建(需要 Xcode 命令行工具)
alias hn=.build/debug/Hn    # 建议把 Hn 链到 PATH

hn new myapp                # 生成最小应用骨架
cd myapp
hn dev app.html --css app.css
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

脚手架生成的文件:

```
myapp/
  app.html     界面 + 清单(meta 声明表面/尺寸/标题)
  app.css      样式(CSS 变量做主题)
  head.html    被 <include> 的片段(演示零构建组件)
  README.md    该应用自己的说明
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
- 自定义后端:运行时可注入任意 transport(进程内闭包或 URLSession),
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

## 需要时, 让系统 WebKit 兜底

引擎实现了日常所需的大部分 CSS, 但有些构造还没做(`grid`、`position: absolute`、
`canvas`/`svg`/`video`)。这类页面可以显式声明走系统 WebKit:

```html
<meta name="hn-renderer" content="webkit">
```

- 默认是 `native`(自研引擎原生渲染), **必须显式声明**才切到兜底 ——
  否则同一份文件在不同环境下长得不一样。
- 两条路径**窗口行为完全一致**: 秒开、`--ttl` 自动销毁、`hn update` 热更新、
  `hn dev` 保存即刷新、`hn persist` 离线保存、`sys://` 系统数据、`hx-*` 交互、
  `hn-theme` 主题、本地 KV —— 全都不用改。
- 免单位写法(`padding: 16`)在兜底路径会自动补成 `padding: 16px`,
  所以同一份 CSS 两边都好看。
- 兜底路径自动注入 `charset=utf-8` 与中文系统字体栈(苹方), 中文不会变宋体、
  也不会乱码; 你的 `code/pre` 自动用等宽字体栈。

参考 `examples/webkit-fallback.html`。

## 组件复用(include)

没有打包器。文件即组件:

```html
<include src="head.html">        <!-- 引入样式/结构片段 -->
<include src="parts/row.html">   <!-- 可嵌套(上限 6 层,自动检测循环) -->
```

展开发生在引擎装载前(IncludeExpander),所以片段里可以有 `<style>`、
清单 meta、任意结构。共享一套 UI?做一个目录放片段,到处 include。

## 应用生命周期

```bash
hn open myapp app.html --css app.css    # 生成(表面由 meta 决定)
hn list                                # 在运行的应用
hn update myapp app.html               # 热更新(窗口/状态保持)
hn persist myapp                       # 持久化 → ~/.html-native/apps/myapp.hnapp
hn restore myapp                       # 离线恢复
hn close myapp                         # 销毁

hn open msg.html --ttl 8               # 消息卡:8 秒后自动热销毁
hn syscard                             # 常驻系统信息卡(2s 自刷新)
```

消息卡专属属性:`hn-drag`(该元素成为拖拽手柄)、
`hn-dismiss`(点击弹窗外即关闭)。

所有命令走常驻宿主(`~/.html-native/hn.sock`,JSON-lines 协议),
首次调用自动拉起,之后每个应用都是宿主里的轻量对象。

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
hn-shot examples/showcase.html examples/showcase.css out.png 920 620
hn-shot app.html app.png 480 620 --live            # css 可省略; --live 执行 sys:// 拉取
hn-shot … hover.png 920 620 --hover nav-team     # 注入悬停态
hn-shot … scroll.png 920 620 --scroll frames,220 # 注入滚动
.build/debug/RenderTest                            # 46 项回归断言
```

HnShot 用真实视图自绘(@2x),所见即窗口所得,不需要屏幕录制权限。

## 边界(诚实清单)

- 无 JavaScript——交互全部声明式(hx-*)或由宿主注入的 transport 承担。
- CSS 是实用子集,不是全量(无 grid、无 nth-child,在路线图上)。
- 图片支持 `<img>`;字体用系统字体栈。
- 目前运行时是 macOS;引擎核心 C99 无关性,Linux/Windows 运行时在路线图。

遇到问题先跑 `RenderTest`(全绿说明引擎层健康),
再用 `hn-shot` 单独渲染你的文件定位是内容还是引擎的问题。
