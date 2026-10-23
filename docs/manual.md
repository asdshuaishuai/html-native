# Neb 完整使用手册(html-native)

> 面向**写应用的人与 agent**的完整参考: 清单声明、语法面、交互、
> 系统能力、命令行、守护进程协议、打包分发、环境变量。上手教程见
> [getting-started.md](getting-started.md); 架构见
> [architecture.md](architecture.md); 能力边界以
> [README「支持的子集」](../README.md#支持的子集v1)为准。

---

## 1. 两条等价入口

| 入口 | 流程 | 适合 |
|---|---|---|
| 人类直接开发 | 写 `app.html` → `hnapp app.html` 开窗(无构建链) | 手写应用 |
| agent 生成 | daemon 常驻 → `hn open <id> <file|->` / socket `open` op → 秒开秒毁热推送 | agent 交付界面 |

两者消费**同一份 hn 编码**(HTML 语法子集)与同一引擎, 可混用。

## 2. 表面声明(`<meta>` 清单)

| 声明 | 值 | 语义 |
|---|---|---|
| `hn-surface` | `window` \| `popup` \| `layer` | 表面形态(缺省 window) |
| `hn-window` | `360x520` 或 `360x520@100,80` | 尺寸(与初始位置; 几何记忆覆盖) |
| `hn-title` | 任意文本 | 窗口标题(有标题栏时) |
| `hn-theme` | `dark` \| `light` | 内置设计令牌基底(免写配色) |
| `hn-applet` | 槽位名 | 轻应用具名槽位(daemon applets) |
| `hn-frameless` | `0` | **缺省 1 = 无标题栏**(窗口即画布); 显式 0 要系统标题栏 |
| `hn-reveal` | `paint` \| `fade` \| `none` | 呈现动画; paint = 画刷铺色(逐像素透明窗专属) |

配套环境钮: `HN_WINDOW_POS=X,Y`(钉位, 不写几何记忆)、`HN_REVEAL`/
`HN_REVEAL_MS`(动画覆盖)、`HN_CLICK_THROUGH`、`HN_RENDER_SCALE`。

## 3. 布局与样式(CSS 子集)

- **布局**: block 流 + flexbox(主轴对齐六种/grow/shrink/wrap/gap) +
  行内格式化(按词贪心换行、CJK 硬拆、基线对齐) + absolute/fixed 脱离
  流 + z-index + 百分比/em/rem + min/max + box-sizing + aspect-ratio。
- **视觉**: 圆角/渐变(linear/radial)/box-shadow(真高斯)/描边(逐侧)/
  overflow 裁剪(圆角羽化)/opacity/text-shadow/伪元素 `::before::after`
  (content+声明)。
- **文本**: ellipsis/line-clamp/white-space/word-spacing/letter-spacing/
  text-transform/中文系统字体栈/code 等宽栈; **免单位数值**自动补 px
  (`padding:16` 合法)。
- **交互控件**: button/input(text/checkbox/radio/range/button)/
  select(弹层)/details(summary 折叠)/进度。
- **免写样式**: `hn-theme` 令牌 + `examples/ui-kit.html` 预设
  (`<include>` 一次, 写类名即得组件皮)。
- 完整支持矩阵以 README「支持的子集(v1)」与 `css/html/layout/dom`
  探针断言为准 —— **断言即语言规范**。

## 4. 交互(hx-*)与生命周期

```html
<div hx-get="sys://cpu" hx-target="#out" hx-trigger="click">刷新</div>
<span hx-get="sys://uptime" hx-trigger="load every 2s">…</span>
<form hx-post="sys://notify" hx-target="#tip"><input name="msg"></form>
<div hx-put="https://api/x" hx-confirm="确定提交?">…</div>
```

- **方法词表**: `hx-get`/`hx-post`/`hx-put`/`hx-delete`/`hx-patch`
  (GET 表单拼 query, 其余 urlencoded 体)。
- `hx-target`(`this`/CSS/`closest`/`find`/`next`/`previous`)、
  `hx-swap`(innerHTML/outerHTML/beforeend/afterbegin/afterend/delete/
  none)、`hx-trigger`(click/load/`every Ns`/`change`/hover… +
  `delay:N` 防抖 + `changed`)、`hx-vals`/`hx-headers`(JSON)。
- `hx-include="选择器列表"`: 附加元素值进请求体(逗号分隔 CSS;
  输入控件取当前值, 非控件取 name/value 属性)。
- `hx-select="选择器"`: 响应子树选择 —— 只换入匹配元素(无匹配 = 换空)。
- `hx-confirm="问题"`: 触发 `htmx:confirm` 事件(`e.payload` = 问题),
  JS preventDefault 取消请求; 无 JS 监听 = 放行(对话框形态归平台层)。
- `htmx:beforeRequest`(载荷=HTTP 动词, preventDefault 取消)/
  `htmx:afterRequest`/`htmx:afterSwap` 事件可监听; input 回车沿
  自身→祖先→最近载体提交。
- **列表错档**: 容器 `hn-stagger="每项ms"` —— 子项(运行时插入)入场按
  序错档浮现(首项即入, 后项各延迟一档); 单条追加恒 idx=0 即时浮现。
- 目标地址两类: `sys://` 系统桥(§6)与注入的 HTTP transport
  (运行时 `hn.fetch` 同源, `HN_FETCH_ALLOW` 白名单)。
- 生命周期事件: `launch/show/hide/destroy`(JS `addEventListener`
  或宿主注入)。

## 5. 脚本面(QuickJS, 可选)

- **DOM**: `querySelector(All)` / `getElementById` / `createElement +
  appendChild + remove()` / `textContent` / 属性四件套 / `classList` /
  `dataset` / `style` 代理 / `checked·disabled·hidden` / 树关系 /
  `document.body|documentElement`。
- **事件**: `document/window/element.addEventListener` → 命中链冒泡 →
  标准事件对象(`preventDefault/stopPropagation`)。
- **定时**: `setTimeout/setInterval/requestAnimationFrame`(帧驱动,
  空闲零唤醒)。
- **网络**: `hn.fetch(url, options)` → Promise(超时/重定向/体积上限/
  `HN_FETCH_ALLOW` 主机白名单; 4xx/5xx reject 带状态码)。
- **Canvas 2D**: `hnCanvas2D` 桥(rect/roundRect/arc/arcTo/路径/渐变/
  变换/像素/toDataURL/imageData/globalAlpha/阴影/filter)。
- **内嵌计算**: wasm3(WASM 解释器, 纯计算导出)。
- 桥面小桥: `hnSetText` / `hnGet2D` / `hnCanvas2D` / `hnMedia*` /
  `hnWasm*` / `hnMeshParam`(嵌入宿主可只接这一层, 不带 QuickJS)。

## 6. 系统能力(`sys://`, 零网络零依赖)

| 路由 | 返回/行为 |
|---|---|
| `sys://host` `cpu` `memory|mem` `disk` `battery|power` `uptime` `info` | 系统读取(渲染成 DOM 由 hx-target 换入) |
| `sys://open?url=…` | 交系统打开 URL |
| `sys://notify?…` | 系统通知 |
| `sys://store/get|set` | 应用级持久化 KV(宿主提供命名空间) |
| `sys://clipboard` | 剪贴板 |

`sys://info` 列出全部可用路由。引擎核心不做 I/O —— 这些在 rt 层,
宿主可整体裁剪。

## 7. 富媒体与表现型标签

| 标签/属性 | 说明 |
|---|---|
| `hn-lottie="x.json"` (+`hn-lottie-speed`) | Lottie/dotLottie(.lottie ZIP 直读) 矢量动画 |
| `hn-mesh`(+anchor/sway/look/breath/idle/phase-y/speed) | 网格变形贴图(立绘呼吸/摆动/视线跟随) |
| `hn-three`(+src/geo/tex/camera-orbit/spin/msaa/color/azimuth) | three.js 子集(编译进引擎) |
| `hn-cubism="model3.json"` | Live2D 官方 Cubism Core 绑定(Core 二进制 `tools/fetch-cubism-core.sh` 自取, 授权见 README) |
| `hn-gif`(±`hn-gif-speed`) | GIF 动图(帧缓存带 64MB 预算 LRU) |
| `hn-md` / `hn-json` / `hn-csv`(±sep) / `hn-fmt` | Markdown/JSON/CSV 直染 |
| `hn-live` / `hn-stream`(+`hn-stream-loop=N`) | 流式追加(对话/日志; 环形缓冲) |
| `<video>/<audio>` | 状态机 + 平台图层直通(AVPlayerLayer/Xv); 合成宿主兜底 |
| `hn-transparent` / `hn-level` / `hn-shadow` / `hn-draggable` / `hn-click-through` / `hn-presence` | 窗口形态(透明/层级/阴影/拖拽/穿透/在场) |
| `hn-spaces` / `hn-daemon` / `hn-lifecycle` | 桌面集成 |

3D 支持度细节: [threejs-support.md](threejs-support.md); 媒体契约:
[media-design.md](media-design.md)。

## 8. 命令行参考

**hncore**(引擎离屏 CLI, 零 GUI, 三平台同源逐字节可比):

```bash
dist/hncore-macos-arm64 parse  app.html            # DOM 树
dist/hncore-macos-arm64 layout app.html [W H]      # 盒模型树
dist/hncore-macos-arm64 boxes  app.html [W H]      # 机器可读绝对盒
dist/hncore-macos-arm64 paint  app.html [W H]      # 绘制指令(display list)
dist/hncore-macos-arm64 text   app.html [W H]      # 文本片段(字节区间+位置)
dist/hncore-macos-arm64 render app.html out.png W H    # hnsoft 出 PNG
dist/hncore-macos-arm64 renderc app.html out.png W H   # cairo 后端(同一显示列表)
dist/hncore-macos-arm64 verify app.html [W H]      # 布局不变量自检
```

**hnapp**(应用壳, 链接哪平台 hnp 就是哪平台应用):

```bash
dist/hnapp-macos-arm64 app.html            # 开窗(帧循环/动效/几何记忆)
dist/hnapp-macos-arm64 app.html --probe    # 管线自检(platform=headless…)
dist/hnapp-macos-arm64 app.html --shot out.png [W H] [--advance ms]
dist/hnapp-macos-headless  app.html --probe   # 无 GUI(CI/agent)
dist/hnapp-macos-arm64 --capsule app.hnapp    # 直启胶囊(restore 同口径)
```

**hn**(daemon 客户端): `ping / open <id> <file|-> [--css] [--w] [--h]
[--window] / update / close / list / dump / text --element / event
--kind click --target / shot / eval --js / persist / restore / applets`
—— 全部是 daemon socket op 的薄封装(§9)。

## 9. 守护进程与 socket 协议

```bash
dist/hn-daemon &                 # 常驻; socket: ~/.html-native/hn-daemon.sock
```

JSON-lines 单行请求(任何语言可对接):

| op | 说明 |
|---|---|
| `open` `{id, html, css?, w?, h?, headless?}` | 开应用(秒开; `html:"-"` 不适用 socket —— 直接传字节) |
| `update` `{id, html}` | 热更新(同 id 推新文档, 窗口与状态保持) |
| `close` / `list` / `ping` | 销毁 / 枚举 / 探活 |
| `eval` `{id, js}` → `{value}` | QuickJS 求值 |
| `dom` / `dump` / `text` | DOM 快照 / 指令数 / 元素文本(感知) |
| `event` `{id, kind, target, x?, y?}` | 注入事件(驱动) |
| `shot` `{id, path}` | 无头渲染 PNG |
| `persist` `{id, path, instructions?}` | 打 `.hnapp` 胶囊(manifest+入口页+资产, ZIP v2, 原子写) |
| `restore` `{path}` | 拆胶囊 → 项目模式打开(同 id 替换; 移出统一 app 守卫) |
| `applets` | 轻应用槽位枚举 |

daemon **无头优先**: open/update/eval/dom/dump/shot 全链路不触 GUI。

## 10. 项目、打包与分发

- **项目 = 目录**: `hn_app dir/`(project.json 声明入口, 缺省
  index.html); 引擎内页面导航(`a href` 项目内文件 → 同窗换装,
  页面栈 64 层); 目录递归指纹热重载(任何 include 片段/共享 css
  变化都触发)。示例 `examples/project-demo/`。
- **`.hnapp` 胶囊 v2**: ZIP(manifest.json + index.html + DOM 收集的
  本地资产), 原子写; restore 解到 `~/.html-native/apps/<id>/` 后按
  项目模式打开。`hn pack`: 既有 H5 网页/目录/URL → 兼容性体检(无头
  渲染对照 hn 子集报告不支持特性) → 注入 hn-* meta → 收集资产 →
  产出项目目录与胶囊(原样式零改动)。
- **`hn build --target macos`**(人类分发形态): 项目目录 → 胶囊 →
  `<Name>.app`(Info.plist + 运行时 + Resources/app.hnapp; 名称/id/
  版本取 project.json app 字段)。Windows `.exe` / Linux AppImage
  按 roadmap 排期。

## 11. 环境变量速查

| 组 | 变量 |
|---|---|
| 性能三层 | `HN_FRAGMENT`(=0 关片段) `HN_LAYOUT_SKIP` `HN_MEASURE_CACHE` `HN_STYLE_MEMO`; 各自 `_TRACE=1` 打命中统计 |
| GPU/渲染 | `HN_GPU=metal` `HN_GPU_PRESENT` `HN_GPU_TRACE` `HN_RENDER_SCALE` `HN_RENDER_TRACE` `HN_CANVAS_BACKEND`(ledger/ity/cg/system) |
| 呈现 | `HN_REVEAL=paint\|fade\|none` `HN_REVEAL_MS` `HN_REVEAL_TRACE` `HN_WINDOW_POS=X,Y` `HN_CLICK_THROUGH` |
| 媒体 | `HN_MEDIA_AUTOPLAY/DURATION/FPS/LAYER/MUTE/PAUSE/SEEK/VOLUME/UNMUTE/TRACE`(合成宿主契约) |
| 网络 | `HN_FETCH_ALLOW` `HN_FETCH_TIMEOUT_MS` |
| 其它 | `HN_CLOCK=<ms>`(离屏动画推时刻) `HN_ANIM_FPS` `HN_GIF_CACHE_MB` `HN_CUBISM_TRACE` `HN_VIS_TRACE` `NEB3_LIB` |

## 12. 调试与验收

- 分层取证: `hn dump`(指令数) → `hncore boxes|layout`(几何) →
  `hncore paint`(指令表) → `hncore verify`(不变量) → `--shot`(像素)。
- 探针 42 族 = 回归门禁: `bash tools/<主题>_probe.sh`; 全量绿 = 可交付。
- 修样式不生效? 先确认跑的是新构建(dist 可能陈旧), 再
  `HN_STYLE_MEMO=0 HN_LAYOUT_SKIP=0 HN_FRAGMENT=0` 三关对照定位层级。
