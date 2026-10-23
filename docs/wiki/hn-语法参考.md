# hn 语法参考

> hn 编码 = HTML 语法子集。**探针断言即语言规范**(README「支持的
> 子集(v1)」为完整矩阵); 本页是写作时的速查。CSS 免单位数值合法。

## 表面声明(`<meta>`, 宿主消费)

```html
<meta name="hn-surface"   content="window|popup|layer">
<meta name="hn-window"    content="360x520">          <!-- 或 360x520@100,80 -->
<meta name="hn-title"     content="agent · 仓库分析">
<meta name="hn-theme"     content="dark|light">       <!-- 设计令牌基底 -->
<meta name="hn-applet"    content="clock">            <!-- 轻应用槽位 -->
<meta name="hn-frameless" content="0">                <!-- 缺省 1 无标题栏 -->
<meta name="hn-reveal"    content="paint|fade|none">  <!-- 呈现动画 -->
```

## 布局

- `display: block | flex | inline | inline-block | none`(flex 子集:
  主轴六种对齐/grow/shrink/wrap/gap; 列方向)
- 定位: `position: static | relative | absolute | fixed` +
  `top/right/bottom/left` + `z-index`(脱离流参照最近 positioned 祖先)
- 尺寸: `width/height/min-*`/`max-*`(px/%/em/rem/auto) +
  `box-sizing: border-box` + `aspect-ratio`
- 盒模型: `margin`(含相邻折叠)/`padding`/`border`(逐侧)/`border-radius`/
  `overflow`(裁剪, 圆角羽化)

## 文本

`font-size/weight/style(italic)/family(system|monospace|serif)` ·
`color/background` · `text-align` · `text-overflow: ellipsis` ·
`line-clamp`(多行截断) · `white-space`(nowrap/pre/pre-wrap) ·
`letter-spacing`/`word-spacing` · `text-transform` · `line-height` ·
`text-shadow`。中文默认字体栈(PingFang SC 系), `code/pre` 等宽。

## 视觉

`linear-gradient/radial-gradient` · `box-shadow`(真高斯) · `opacity` ·
`visibility`(子级可复显) · 伪元素 `::before/::after`(content + 声明) ·
`transform`(rotate/rotateX/rotateY/perspective + 3D 深度排序) ·
`transition`/`@keyframes`(缓动族/时间轴采样) · `filter`(canvas 侧)。
物理弹簧: `transition: opacity .5s spring(180, 10)` —— spring(刚度, 阻尼[, 初速度])
功能式激活物理积分(欠阻尼过冲/重阻尼平滑收敛, settle 精确落位); 裸名 `spring` 仍为闭式弹性缓动。
跨元素时间轴: 容器 `hn-timeline="loop=2|infinite alternate reversed interval=ms"` 编排后代入场
步骤(拍点 = 各自 animation 的 delay), 轮完成重播/反向, 退役派 `hn-timeline:complete`。
JS 动画: `hn.animate(targets, {opacity: [0.2, 1], duration, ease, stagger, onComplete})` ——
anime.js 形状薄桥, 插值由引擎自有过渡完成。

## 交互控件

`button`(按标签内容收缩) · `input`(text/checkbox/radio/range/button)
· `select`(原生弹层, 键盘导航) · `details/summary`(折叠) · `progress`
· `form`(回车提交, urlencoded 收集) · 伪类
`:hover :active :focus :disabled :checked :root :not() :nth-of-type
:nth-child :first/last/only-child`。

## 富媒体与表现型标签

| 写法 | 效果 |
|---|---|
| `hn-lottie="x.json"` | Lottie / dotLottie(.lottie ZIP) 矢量动画; `hn-lottie-speed` |
| `hn-mesh` + `hn-mesh-anchor/sway/look/breath/idle/phase-y/speed` | 网格变形贴图(立绘呼吸/摆动/视线跟随) |
| `hn-three` + `hn-three-src/geo/tex/camera-orbit/spin/msaa/color/azimuth` | three.js 场景(3D 大屏/舞台) |
| `hn-cubism="model3.json"` | Live2D 官方 Core 绑定(遮罩/反转载剪) |
| `hn-gif="x.gif"` | GIF 动图(±`hn-gif-speed`) |
| `hn-md` `hn-json` `hn-csv`(+`hn-csv-sep`) `hn-fmt` | Markdown/JSON/CSV/格式化直染 |
| `hn-live` `hn-stream`(±`hn-stream-loop=N`) | 流式追加(对话/日志; 环形缓冲) |
| `<img>` `<video>` `<audio>` `<canvas>` | 图片/媒体(AVPlayerLayer·Xv 图层直通)/Canvas 2D |
| `hn-transparent` `hn-level` `hn-shadow` `hn-draggable` `hn-click-through` `hn-presence` `hn-spaces` | 桌宠/窗口形态语义 |
| `<include src="ui-kit.html">` | 组件预设复用(写类名即得) |

## 窗口形态属性(元素级)

`hn-window`(meta)之外的窗口语义放在元素上: 透明背板 `hn-transparent`、
层级 `hn-level`、阴影 `hn-shadow`、拖拽 `hn-draggable`、鼠标穿透
`hn-click-through`、在场 `hn-presence`、跨桌面 `hn-spaces`。

## 确定性

同一份 hn 编码在 macOS/Linux/Windows 产物上布局输出**逐字节一致**
(构建门禁守着); 动画时钟 1/64ms 栅格量化, 跨帧率重放一致。
不支持特性(无 WebView 兜底)以 README 子集表与 `css/html/dom` 探针
为准; `hn pack` 可对既有网页做**兼容性体检**逐页报告。
