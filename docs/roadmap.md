# html-native 路线图：从玩具到 OS-PWA 级框架

> 版本: 1.1 · 原始日期: 2026-09-17 · **校准: 2026-10-15**(§1 行数与计数按当日代码
> 实测重取, 命令写在各表脚注; 10-13 首校, 两天内即漂移 —— 故行数取约数并标注日期;
> §3 起为路线图, 已完成项在原条目上标注) · 状态: 规划中
>
> 注: 2026-10-09 c49cfbd "全量 C99" 之后渲染只剩自研 C99 一条路径 —— 现为
> 引擎(Sources/CHtmlNative) + 公共运行时(rt/hn_rt.c) + 平台层(platform/hnp_*.c,
> 含 GPU 原语表 hnp_gpu_host) + 应用层(app/hn_app.c), 全部 C99, 见 README 代码地图。
>
> 本文档是 html-native 从当前原型演进到生产可用框架的
> 深度规划。包含差距分析、竞品对标、分阶段路线图与架构演进。

---

## 1. 当前状态（诚实审计, 2026-10-15 实测)

### 1.1 有什么（已验证）

> 行数口径(`wc -l`, 2026-10-15 工作树, 活跃开发期按日漂移故取约数):
> 引擎 = `Sources/CHtmlNative/*.c *.h include/hn.h`(约 2.5 万行); 平台层 =
> `platform/*`(约 5.2k 行); 运行时 = `rt/*`(约 3.6k 行); 应用层 = `app/*`
> (约 1.5k 行); 工具与探针 = `tools/*.c *.h *.sh`(约 2.3 万行)。
> 断言数 = 各探针最近一次实跑输出计数(2026-10-13 轮, 明细见 1.1 表"测试"行;
> events_probe 58 条来自同轮 §7 记录)。

| 层 | 代码量 | 能力 |
|---|---|---|
| C99 引擎 | ~2.5 万行 | HTML 解析 · CSS 级联(含属性选择器/`:not`/`nth-of-type`/`calc`) · flex/行内/absolute 布局 · 绘制指令(14 种: RECT/TEXT/IMAGE/CLIP_PUSH/CLIP_POP/CLIP_PATH_PUSH/QUAD/POLYGON/MESH/BITMAP/CLEAR_RECT/PDATA/PATTERN/TRIS, 见 hn.h 的 HN_CMD_*) · 动画插值(过渡/@keyframes/缓动族) · 3D 变换(4×4 矩阵栈 + preserve-3d + **深度排序**) · Lottie/dotLottie 求值 · Live2D motion3 驱动 rig · 网格变形 · GIF · PNG · Markdown/JSON/CSV **直染** · 媒体元素状态机 · Canvas 2D API · WASM(wasm3) · 结构自检 |
| 平台层 `platform/` | ~5.2k 行 | `hn_platform.h` 契约(窗口/媒体/视频图层/**GPU 原语表**) · `hnp_macos.c`(dlopen AppKit/CoreGraphics/QuartzCore + AVPlayerLayer 图层直通) · `hnp_media_macos.c`(AVFoundation) · `hnp_linux.c`(X11 + **Xv 视频图层原语**) · `hnp_headless.c` · `hnp_gpu_metal.c`(**Metal v1**, 862 行) |
| 公共运行时 `rt/` | ~3.6k 行 | 文档生命周期/htmx 执行/sys:// 桥/本地 KV/持久化 · QuickJS 求值(HN_HAVE_QUICKJS) · **JS 定时器与 RAF**(帧驱动) · **JS 事件监听**(addEventListener 命中链冒泡) · **HiDPI scale** · **GPU 宿主槽**(`hn_rt_set_gpu_host`, 缺省 hnsoft 逐帧回退) |
| 应用层 `app/` | ~1.5k 行 | `hn_app.c` 唯一主循环(开窗/mtime 热重载/**几何记忆**/--shot/--probe) · `hn_media_platform.c`(平台媒体宿主装配) · `hn_media_synth.c`(合成宿主单一来源) |
| ~~macOS 运行时(Swift)~~ 已删 | — | Swift 全层于 c49cfbd 删除; 现 C99 层接管: `tools/hnweb_macos.c` + `platform/hnp_macos.c`(dlopen AppKit/CoreGraphics, 纯 C99) |
| Linux 运行时 | `tools/hnweb_linux.c` ~2000 行 | M1 窗口/渲染/expose/resize/点击命中 + M2 **事件派发管道**(点击/键盘/滚轮/hover → 目标解析 → 带 id 冒泡 → hx-trigger 消费)与**动画帧循环**(dt 推进, 空闲零唤醒); 新架构 `hnapp-linux-x86_64`(`platform/hnp_linux.c`, 桌宠字段全对齐 + Xv 视频图层原语)。仍缺: 旧壳 sys:// 桥 transport(hnweb_linux.c:789 sys_fragment 恒返 NULL, 新架构 hnapp-linux 走 rt/hn_rt.c 真桥)、XIM 键入、交叉口径文本 |
| Windows 运行时 | `tools/hnwin.c` 816 行 | Win32 窗口 + hnsoft + `sysbridge.c`(sys:// 桥); 新架构 `hnp_win32.c` 未写(平台矩阵 Windows 列在路线图) |
| 软件光栅器 | hnsoft 1,943 行 + cairo 后端 1,031 行 | SDF 圆角矩形 · 线性/径向渐变 · 多边形扫描线 · 网格纹理映射 · **PNG 解码(自带 inflate)** · FreeType 文本 · PNG 编码; cairo 后端同签名可换(`hn_cairo.c`, 真模糊阴影/任意路径裁剪/真 zlib) |
| 宿主与工具 | daemon 1,317 行 + CLI 292 行 | daemon 开窗 spawn(`--window`)/热推送/常驻(`resident`)/胶囊(`persist`/`restore`)/开窗几何记忆; CLI 9 个子命令(ping/open/update/close/list/dump/text/event/shot) |
| 测试 | 探针 28 个(`tools/*_probe.sh`, 以 `find tools -name '*_probe.sh'` 为准，含 perf_probe CPU 硬线)+ `linux_parity_check.sh` | **约 1,200 项断言/检查**(逐探针最近实跑计数: render 61 · css 96 · layout 33 · html 37 · canvas 95 · timer 59 · fmt 118 · md 107 · gif 61 · anim 31 · stream 30 · include 23 · wasm 19 · mesh 36 · live2d 44 · lottie 37 · quad3d 33 · gpu 7 · pet 27 · resident 16 · window 25 · media_element 40 · media_window 30 · media 18 · hidpi 8 · sysbridge 17 · hnsoft_alpha 7 · cairo 30 · events 58; 另有 `linux_parity_check.sh`(编译级对账)与 `perf_probe.sh`(CPU 硬线 1 条)全过) |
| 示例 | 50+ 个 HTML(+6 CSS) | 仪表盘 / 网页写法 / 消息卡 / 输入控件 / Lottie / dotLottie / 透明背板 / 桌宠 / live2d-demo / live2d-miku / canvas-demo / image-lab / md-demo / fmt-demo / media-demo / threejs-lab / threejs-ui / threejs-sphere / threejs-plane / threejs-pet / threejs-demo / ui-kit / anim-lab / agent-chat / agent-stream … |
| 产物 | 16 个(`dist/`, 除 SHA256SUMS/pdb) | `hncore-*` 6 + `hncore-cairo-*` 1 + `hnweb-*` 4 + `hnwin-*` 1 + `hnapp-*` 3 + daemon/CLI; 实测大小 344 KB(hncore-macos-arm64)→ 1.4 MB(hnapp-macos-arm64, 含 QuickJS + WASM); 一条 `ZIG=zig bash tools/build-multiplatform.sh` 出全套(2026-10-13 实跑全绿) |

### 1.2 没有什么（差距，按严重度排列）

#### 🔴 阻断级（缺了就无法构建真实应用）

| 缺口 | 影响 | 涉及层 |
|---|---|---|
| ~~`position: absolute/fixed`~~ ✅ | 已实现: 解析 + 脱离流布局 + positioned 祖先参照（examples/overlay.html 在用） | — |
| ~~`z-index`~~ ✅ | 已实现: 绘制期层叠排序 | — |
| ~~`top/right/bottom/left`~~ ✅ | 已实现: 配合 position 使用 | — |
| **增量渲染 / 惰性布局** | 每次变化触发全量重排(2000 卡片文档全量 layout 实测约 0.6ms/次, 见 1.3; 大文档仍全量) | 引擎(C) |
| ~~**真实事件系统**~~ ✅ | 已完成(v0.2): 统一派发管道 + 冒泡 + 键盘/焦点/输入(见 3d 的完成注) | — |
| **组件模型** | 无可复用/可组合的组件、无生命周期钩子 | 引擎(C) + JS |
| ~~**网络层**~~ ✅ | 运行时直接绑定系统 HTTP transport：hx-get/hx-post 支持 http(s)，QuickJS 提供 `hn.fetch` Promise、主机白名单与错误状态；引擎核心仍保持零网络 | 运行时 |
| **表单** | 表单编码 + 回车提交已实现；仍无 form 元素 / validation | 引擎(C) + 运行时 |
| ~~`text-overflow: ellipsis`~~ ✅ | 已实现(含 `line-clamp` 多行截断) | — |
| ~~**逐侧边框** (border-top/left/...)~~ ✅ | 已实现: border_w4/border_c4 逐侧解析 + 绘制 | — |

#### 🟡 重要级（没有则严重受限）

| 缺口 | 影响 |
|---|---|
| `::before` / `::after` 伪元素 | 装饰性 UI 的标配(当前无实现) |
| `word-break` / `overflow-wrap`（`white-space` 已实现） | 文本布局精确控制(当前无实现) |
| CSS Grid | 现代布局（可延后到 flex 覆盖大部分场景后; `hn-theme` 的 `.grid` 只是 flex 类名, 非布局算法） |
| ~~`transform` (rotate/matrix)~~ ✅ | 已完成(v0.2): rotate/rotateX/rotateY/perspective + 四边形光栅化 |
| 路由 / 多视图导航 | 多页面应用 |
| 状态管理 | 响应式数据绑定 |
| 背景图片 (`background-image: url()`) | 视觉设计(当前无实现; 用 `<img>` 替代) |
| ~~`:not()` / 属性选择器 `[attr=v]`~~ ✅ | 已实现(另含 `nth-of-type`/`calc`/`text-shadow`) |
| 无障碍 (ARIA) | 生产必备(当前无实现) |
| 开发者工具 | 调试效率(已有 `hn dump`/`hn text`/`hncore paint` 三层取证, 无窗口式 DevTools) |

#### 🟢 加分级（有更好）

| 缺口 | 场景 |
|---|---|
| 虚拟滚动 / IntersectionObserver | 长列表性能(已有 `hn-stream-loop` 环形缓冲, 非虚拟滚动) |
| Web Workers 等价 | 后台计算(已有 wasm3 纯计算导出, 无 JS Worker) |
| ~~启动持续动画~~ ✅ | 已完成(v0.2): @keyframes 多段动画 + 时间轴采样 |
| ~~Lottie 矢量动画~~ ✅ | 已完成(v0.3): JSON 解析 + 关键帧求值 → POLYGON 指令(三后端同源) |
| ~~网格变形贴图(Live2D 类)~~ ✅ | 已完成(v0.3): MESH 指令 + 三角形仿射纹理映射; rig 由脚本驱动 |
| Live2D 官方 `.moc3` | ✅ 已实现(2026-10): **官方 Cubism Core 绑定**(`hn_cubism.c`, `hn-cubism="model3.json"`); Core 闭源二进制不入 git, `tools/fetch-cubism-core.sh` 自取, 授权口径见 README "Live2D (Cubism moc3) 授权说明"; 遮罩/混合观感精修待办(见 1.4 注) |
| CSS `contain` / `will-change` | 性能提示 |
| 自定义字体 (`@font-face`) | 品牌字体(当前无实现) |
| 触摸事件 | 移动端 |

### 1.3 性能基线（2026-10-13 实测）

> 口径: `cn> dist/hncore-macos-arm64 <子命令> …` 本机 arm64 计时(`time`),
> 2000 卡片合成文档 196 KB(`/tmp/perf2000.html`); CPU 治理值来自
> `bash tools/perf_probe.sh`(真窗 ps 采样); 内存来自 `ps -o rss=` 实测。

| 指标 | 数值 | 评价 |
|---|---|---|
| 极小文档 parse+启动 | ~1.8ms/次(200 次 0.365s, 含进程启动) | ✅ 冷启动基线 |
| 2000 卡片文档 parse | ~5.8ms/次(200 次 1.165s, 含进程启动; 扣启动基线约 4ms 为纯解析) | ✅ |
| 2000 卡片文档 layout | ~13.8ms/次(200 次 2.761s, 含进程启动; 扣本机启动基线 ~2ms 后纯布局/级联约 11.7ms) | ⚠️ 满足 16.6ms 帧预算但不宽裕 —— 增量布局的价值所在 |
| 2000 卡片文档 paint | ~17.6ms/次(200 次 3.514s, 含进程启动; 扣基线约 15.5ms) | ⚠️ 视文档规模可超帧预算 |
| 真窗空转 CPU(仅 250ms 定时器) | 0.0%(治理前 ~60fps 空转两位数) | ✅ 空闲零空转 |
| 真窗 CPU(桌宠 ambient 33ms) | 9.3%(6s 均值; GPU 分担后视频窗更低) | ✅ |
| 内存(daemon idle) | 1.8 MB | ✅ |
| 内存(daemon + 1 应用/agent-chat) | 11.7 MB | ✅ |
| 内存(headless 桌宠页) | ~8–10 MB(4 次采样) | ✅ 有 arena 压缩 |
| 二进制 | 344 KB(hncore)→ 1.4 MB(hnapp 含 QuickJS+WASM) | ✅ 极小(vs Electron 200MB) |
| GPU 对照(信息值) | metal 0.076ms/帧 vs hnsoft 0.021ms/帧(120 帧 CPU 时间; GPU 版含 readback 回拷, 真窗直呈是后续) | ⚠️ 当前为离屏 readback 口径 |

### 1.4 明确的边界：Live2D `.moc3` 的口径(2026-10-14 更新)

v0.3 加入了 **Lottie 矢量动画**与**网格变形贴图**，两者都作为**引擎原语**
（产出平台无关的 POLYGON / MESH 绘制指令），因此三个后端自动同等支持。
这里要把边界讲清楚，避免误解：

| 项 | 状态 | 说明 |
|---|---|---|
| Lottie (`.json`) | ✅ 已实现 | `bodymovin` 是公开格式；引擎自解析 + 自求值，无第三方依赖; **`.lottie`(dotLottie ZIP 容器)直读**已落地 |
| 网格变形绘制 | ✅ 已实现 | 三角形仿射纹理映射（hnsoft / cairo / Metal GPU 三后端同语义） |
| 网格动画声明 | ✅ 已实现 | `hn-mesh` 正弦形变(含 breath/anchor 三带/phase/look/idle) + `hn_node_set_mesh_verts()` 脚本逐帧驱动 |
| Live2D 公开数据 | ✅ 已实现 | model3/cdi3/physics3/motion3.json 解析 + motion 曲线采样驱动自有 rig(`hn-live2d*`); `.moc3` 网格变形已由官方 Core 绑定承担(`hn-cubism*`, 见 1.4) |
| 骨骼 / 物理 / 口型同步 | ⚪ 应用层 | 引擎只做"网格 + 贴图"合成；rig 装配逻辑不属于引擎(phong 物理仅解析计数, v1 不做仿真) |
| Live2D `.moc3` 解码 | ✅ 已实现(2026-10) | **官方 Core 绑定**落地(`hn_cubism.c`: csmReviveMocInPlace/csmUpdateModel, 形变数学不重实现); Core 为闭源运行库, 二进制 `tools/fetch-cubism-core.sh` 自取不入 git, 商业授权与协议约束归使用方(README 授权说明); 待办: 遮罩/混合观感精修(对照官方 Web runtime, 见 `hn_cubism.c` 头注"已知差距") |
| Live2D `motion3.json` 等 | ✅ 已实现 | 公开数据已接入: 段类型 0/1/2/3 求值器 + Loop 回绕(见 `tools/live2d_probe.sh` 44 断言) |

**结论**：自有原语路径交付的是 Live2D 视觉效果背后的**技术原理**（网格变形 + UV 贴图
+ 逐帧顶点驱动），"立绘呼吸/摆动/口型"这类效果用 `hn-mesh` 或脚本驱动顶点即可。
直接加载市面上的 `.moc3` 模型已由**官方 Cubism Core 绑定**落地(2026-10,
`hn_cubism.c` + `hn-cubism` 属性): Core 闭源二进制不入 git, 由
`tools/fetch-cubism-core.sh` 自取, 商业授权走 Live2D 官方口径(README 授权说明);
遮罩/混合观感仍有精修待办(见上表)。

---

## 2. 竞品对标（html-native 的独特定位）

| 维度 | Electron | Flutter | React Native | Tauri | **html-native (目标)** |
|---|---|---|---|---|---|
| 创作语言 | HTML/CSS/JS | Dart | JS + 平台组件 | HTML/CSS/JS(Rust 后端) | **HTML/CSS(+可选 JS)** |
| 渲染方式 | Chromium(Blink) | Skia 自绘 | 平台原生组件 | 系统 WebView | **自研 C99 引擎** |
| 二进制大小 | ~200 MB | ~30 MB | ~15 MB | ~10 MB | **~1 MB (目标)** |
| 内存占用 | 150-500 MB | 80-150 MB | 60-120 MB | 80-150 MB | **30-80 MB (目标)** |
| 启动时间 | 2-5 s | 0.5-1 s | 0.3-1 s | 0.5-2 s | **< 100 ms (目标)** |
| CSS 支持 | 完整 | 无(自有 Widget) | 无(StyleSheet) | 完整(WebView) | **子集(持续扩展)** |
| JS 支持 | 完整(V8) | 无(Dart) | 完整(JSC/Hermes) | 完整(WebView) | **内嵌 QuickJS 子集(桥面: hnSetText/hnGet2D/hnCanvas2D/hnMedia*/hnWasm*/hnMeshParam + 定时器)** |
| 跨平台 | ✅ | ✅ | ✅(非 Web) | ✅ | **已落地: hncore 5 目标 + hnweb Linux/Windows/macOS + hnapp 三平台(见 3.x 完成项)** |

**html-native 的独特定位：**
> **最轻量的原生 UI 框架，但用 Web 的创作语言。**
> 用 HTML/CSS 写界面（比 Flutter/Dart 学习曲线低），渲染成真正的原生像素
> （比 Electron/Tauri 轻 100 倍），没有浏览器也没有运行时依赖。

**Trade-off：** 功能完整度 vs 体积/性能。目前偏"太少功能"，路线图要
在不放弃体积/性能优势的前提下补齐关键能力。

---

## 3. 分阶段路线图

### Phase 1: 可用工具（v0.2）—— "能构建简单但真实的桌面应用"

**目标：** 一个人用 HTML/CSS + hx-* + 少量 JS，能在 1 天内做出一个
有真实功能的桌面工具（如：系统监控面板、笔记应用、API 调试器）。

#### 1a. 定位系统（引擎 + 绘制 + 布局）✅ 已完成
```
position: relative | absolute | fixed
top / right / bottom / left
z-index
```
- 引擎: layout 阶段增加 absolute 定位计算（脱离常规流，参照最近 positioned 祖先）
- 绘制: paint_walk 按 z-index 排序子节点
- 状态: 已落地(`hn_position` 四档 + 绘制期层叠排序; examples/overlay.html 在用)

#### 1b. 文本增强 ✅ 已完成(部分)
```
text-overflow: ellipsis            ✅(另有 line-clamp 多行截断)
white-space: nowrap | pre | pre-wrap   ✅
word-break: break-all | break-word ⬜ 仍未实现(1.2 重要级)
```

#### 1c. 逐侧边框 ✅ 已完成
```
border-top: 2 solid #4f7cff;
border-left-color: #333;
```
- 引擎: style 拆分 border 为四边独立值；paint 分四条路径绘制
- 状态: 已落地(border_w4/border_c4 逐侧解析 + 绘制)

#### 1d. 事件系统（运行时层）✅ 已完成(以现口径重述)

**现状口径(2026-10-15 重核)**: 引擎提供 `hn_event` 数据与冒泡路径
(`hn_event_path_at`/`hn_event_path_len`, 只收集带 id 元素), 平台壳(M2 管道:
`hnweb_macos.c`/`hnweb_linux.c`/`hnwin.c`)做目标解析 → 带 id 冒泡 → `hx-trigger`
消费; 事件种类见 `hn.h`(mouse 系/keydown-up/focus-blur/scroll/input/change/
submit/hover + launch/show/hide/destroy 生命周期)。daemon 侧有
`hn_rt_synthetic`(合成 click 驱动 hx 的执行入口)。
**历史口径勘误(①已过时)**: ①Swift 版的 JS `addEventListener` 桥随
c49cfbd 全量 C99 删除后, 曾在 C99 层缺位 —— **现已重新落地**(见下条 ✅,
rt/hn_rt.c:1907 起: `document/window/element.addEventListener` 注册 → 命中链
冒泡 → JS 回调 + 标准事件对象含 `preventDefault()/stopPropagation()`), 本条
早先"当前 C99 层没有 JS 事件监听面/events.html 会报 TypeError"的表述作废;
②"M2 全壳同构"的完整度是 Linux 壳口径, macOS
M1 壳仍走自己的 hover/轮询路径。事件类型表(原规划的 mousedown/dblclick 等)
为当时目标, 现引擎枚举为准。
```
-- 事件类型(hn.h: hn_event_kind): mouse 系 / keydown-keyup / focus-blur /
--            scroll / input-change / submit / hover / 生命周期四件
-- 冒泡路径 + hx-trigger 消费(click/load/every Ns/hover 等)
```
- ~~后续(可选): JS 侧 `addEventListener` 面在 C99 运行时重新落地~~ ✅ 已完成
  (2026-10, rt 层 glue: `document/window/element.addEventListener` 注册 →
  命中链冒泡 → JS 回调 + 标准事件对象; `tools/events_probe.sh` 58 断言全绿)

#### 1e. 网络层（运行时层）✅

运行时通过系统网络 transport 提供 `hx-get`/`hx-post` 的 HTTP(S) 请求，并在 QuickJS 中提供 `hn.fetch(url, options)` Promise API。请求支持超时、重定向上限、响应体上限与 `HN_FETCH_ALLOW` 主机白名单；4xx/5xx 以带状态码的错误 reject。引擎核心仍不直接做网络 I/O，`sys://` 保持本地系统桥语义。

- macOS/Linux：运行期绑定 libcurl；Windows：运行期绑定 WinHTTP，编译期不依赖 SDK 网络库。
- 回归：`tools/http_probe.sh` 覆盖 hx 换入、404 保持目标不动、白名单拒绝、`hn.fetch` JSON resolve/reject。

#### 1f. 表单处理 ⚠️ 部分
```
<form> 元素收集 name→value    ⬜ 无 form 元素
name=value 收集 + urlencoded   ✅(hn_doc_form_encode; checkbox/radio 仅选中提交)
input 回车提交                 ✅(自身 → 祖先 → 最近容器 hx 载体)
validation(required 等)        ⬜
```

#### 1g. 增量布局 ⬜ 仍未实现
- 当前: 任何内容变化仍走 `hn_context_layout` 全量重排(2000 卡片文档 layout+paint
  实测各 ~14–18ms/次含进程启动, 见 1.3 —— 16.6ms 预算下大文档已有压力)
- 优化: 标记 dirty 子树，只重排受影响的分支
- 触发场景: hx-swap 只改变一个容器 → 只有该容器需要重新布局
- 工作量: ~2 周（需要仔细的不变量维护）
- 注: 已落地的相邻优化 = 绘制期**轻路径**(content 尺寸未变时免 relayout 只重建
  显示列表)与 `hn-stream-loop` 环形缓冲; 真正的增量布局尚未做

**Phase 1 总工作量: ~10 周**

---

### Phase 2: 真实应用框架（v0.3）—— "能构建多视图、有状态的生产应用"

> 2026-10-15 注: 本阶段 2a/2b/2c 已由 **§8"下一阶段核心"** 收编并给出 A2UI
> 协议口径(组件 API 形状按受信目录设计); 下文保留原始规划作对照。

**目标：** 小团队能用 html-native 构建有 3-5 个视图、有状态管理、
调用后端 API 的内部工具/面板类应用。

#### 2a. 组件模型
```html
<define-element name="user-card">
  <template>
    <div class="card">
      <img class="avatar" src="{$attr.avatar}">
      <div class="name">{$attr.name}</div>
      <slot></slot>
    </div>
  </template>
  <script>
    // 生命周期
    hn.on('mount', (el) => { ... });
    hn.on('attr', (el, name, old, val) => { ... });
    hn.on('unmount', (el) => { ... });
  </script>
</define-element>

<!-- 使用 -->
<user-card avatar="a.png" name="kelthas">管理员</user-card>
```
- 运行时: 自定义元素注册表 + 模板实例化 + 属性观察 + 生命周期
- 依赖: JS 运行时(可选, 纯 HTML 组件用模板语法)
- 工作量: ~3 周

#### 2b. 状态管理
```js
// 响应式 store: 数据变化自动更新绑定的 DOM
hn.store.define({ count: 0, items: [] });
hn.store.bind('#count-display', 'count');       // 文本绑定
hn.store.bind('#item-list', 'items', 'list');   // 列表绑定
hn.store.set('count', 42);                       // → DOM 自动更新
```
- 运行时: 观察者模式 + 脏标记 + 批量更新(避免抖动)
- 依赖: 增量渲染(Phase 1g)
- 工作量: ~2 周

#### 2c. 路由
```html
<view name="home" path="/"><dashboard /></view>
<view name="settings" path="/settings"><settings-panel /></view>
```
```js
hn.router.navigate('/settings');
hn.router.on('change', (view, params) => { ... });
```
- 运行时: 视图注册表 + 切换动画 + 历史栈
- 工作量: ~1.5 周

#### 2d. `::before` / `::after` 伪元素
```css
.badge::after { content: "NEW"; font-size: 9; color: var(--accent); }
```
- 引擎: style 计算阶段为有伪元素的节点生成虚拟子节点
- 工作量: ~2 周

#### 2e. CSS Grid (基础)
```css
.grid { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 10; }
```
- 引擎: layout 阶段增加 grid 布局算法(先支持等分/固定/比例列)
- 工作量: ~3 周
- 注意: 如果 flex + width% 已够用, 可延后

**Phase 2 总工作量: ~8-12 周**

---

### Phase 3: 生产可用（v0.5）—— "安全、高性能、可调试"

**目标：** 框架可以在生产环境运行，有调试工具、性能保障和安全边界。

#### 3a. 合成层 / GPU 加速 ⚠️ GPU 契约 v1 已落地, 直呈与合成层仍在路上
- 现状(2026-10-13): **GPU 原语契约 + Metal v1 后端已落地** ——
  `platform/hn_platform.h` 尾部 `hnp_gpu_host`(设备/纹理/四边形/裁剪/readback
  原语表), 引擎侧 `hn_gpu.c` 做显示列表 → 四边形批翻译, `hnp_gpu_metal.c`
  提供 Metal 实现(运行期 dlopen + MSL 运行期编译); 呼出方按 `supported_cmds`
  位集逐帧判定, 覆盖不了的指令整帧回退 hnsoft(逐帧、静默)。口径:
  与 hnsoft 同页逐像素色差 ≤16/255(探针实测 ≤1/255), 探针 `tools/gpu_probe.sh`。
- 已接线: `<video>` 真窗走 **AVPlayerLayer 原生图层直通**(像素不过 CPU;
  `HN_MEDIA_LAYER=0` 退回 CPU 逐帧), Linux 同契约 Xv 原语(libXv dlopen)。
  开关 `HN_GPU=metal`(缺省关, 零漂移)。
- 仍缺: **GPU 直呈**(当前经 readback 回拷, 真窗仍未把 GPU 帧直接上屏)、
  合成层缓存(`will-change: transform` → 离屏层)、圆角/任意路径裁剪的 GPU 化。
- 工作量: 直呈 ~2 周; 合成层 ~4 周

#### 3b. 虚拟滚动 ⬜ 未实现
```html
<list-view items="10000" item-height="44">
  <template><div class="row">{$item.name}</div></template>
</list-view>
```
- 只渲染可见区域 ±buffer; 滚动时回收/复用 DOM 节点
- 现状: 已有 `hn-stream-loop="N"` 环形缓冲(对话/日志场景的近似手段), 非通用虚拟滚动
- 工作量: ~2 周

#### 3c. 开发者工具 ⬜ 未实现(已有三层取证 CLI)
- **Inspector**: 元素树 + 盒模型可视化(类似浏览器 DevTools)
- **Console**: JS 交互式终端
- **Network**: hx-* 请求日志
- **Performance**: 帧时间线(layout/paint/animation 各阶段耗时)
- 形态: 独立窗口 + Unix socket 通信(与守护进程同架构)
- 现状: `hn dump`(指令数)/`hn text`/`hncore paint|boxes|layout|verify` 已能在
  命令行逐层定位; 窗口式工具未做
- 工作量: ~4 周

#### 3d. 无障碍 (ARIA) ⬜ 未实现
- `role` / `aria-label` / `aria-expanded` 属性解析
- macOS: NSAccessibility 协议桥接
- 工作量: ~2 周

#### 3e. 安全模型 ⚠️ 部分(能力本身即边界)
- per-app 沙箱(文件/网络/系统调用白名单) —— 现状: 无网络、`sys://` 只读系统
  数据/本地 KV/剪贴板/通知/打开 URL, 天然小面; 无显式沙箱
- CSP 等价(限制可加载的资源) ⬜
- JS 权限分级(读-only / 读写 / 系统调用) ⬜
- 工作量: ~3 周

**Phase 3 总工作量: ~15 周(其中 GPU 契约 v1 已完成, 3a 剩余部分重估)**

---

### Phase 4: OS-PWA（v1.0）—— "像 PWA 一样开发，像原生应用一样运行"

**目标：** 开发体验 = PWA(HTML/CSS/JS + 离线缓存 + 安装),
运行体验 = 原生(轻量、快速、系统集成)。

- 后台任务(定时器、推送) —— 定时器 ✅(setTimeout/setInterval/RAF 帧驱动);
  推送 ⬜
- 文件系统访问(沙箱化的读写) ⚠️ 图片/资产按文档目录相对解析; 任意 FS API ⬜
- 系统传感器(位置、摄像头 —— 平台 API 桥接) ⬜
- 多窗口协调(窗口间通信) ⚠️ daemon `resident` 账本可重开多窗, 窗间通信 ⬜
- 离线数据同步 ⬜
- 应用间通信(URL scheme / socket) ⚠️ `sys://open` 单向有; daemon socket 已有
- 打包分发(.hnapp 格式 + 安装器) ⚠️ **`.hnapp` 胶囊已落地**(persist/restore,
  agent 指令与能力图内嵌); 安装器/签名 ⬜(见 README "已知简化与路线图")
- 国际化框架 ⬜
- 触摸/手势支持 ⬜

**工作量: 持续迭代**

---

## 4. 架构演进路径

### 当前架构（2026-10-13 实况）
```
HTML/CSS → C99 引擎(解析→级联→布局→绘制指令) → 平台层 → 屏幕
             hn_rt(文档生命周期/htmx/sys:///KV/媒体/GPU 宿主槽)   ↑ 事件与帧循环
                                                    ↑ JS(内嵌 QuickJS, 可选)
```
产物分层: `hncore`(无平台 CLI) · `hnapp`(app/hn_app.c + 平台 hnp_*.c) ·
`hnweb-*`/`hnwin`(旧窗口壳, 保留) · daemon/CLI(常驻宿主 + 客户端)。

### 已实现的三平台路径(替代原 v0.2 图)
```
显示列表(14 种指令，依据 `Sources/CHtmlNative/include/hn.h` 的 `HN_CMD_*` 枚举；
CLIP_PUSH/POP/CLIP_PATH_PUSH 分立计数) → 后端选择:
  ├ hnsoft 软件光栅(全平台默认)
  ├ cairo(可选编译 -DHN_USE_CAIRO, 一份实现喂所有平台)
  └ hnp_gpu_host 原语表 → Metal(macOS, HN_GPU=metal) / GL(路线图) / D3D11(路线图)
媒体图层(不经显示列表): AVPlayerLayer(macOS) / Xv(Linux, 行为级待验)
```

### v0.3+ 目标架构
```
应用定义(.hnapp 包)      ⚠️ 胶囊已落地(persist/restore), 组件/状态/路由未做
  ├ 页面(HTML/CSS/JS)
  ├ 组件(自定义元素)      ⬜
  ├ 状态(hn.store)        ⬜
  └ 路由(视图注册)        ⬜
       ↓
引擎管线(增量布局 + 脏区域追踪)  ⬜
       ↓
合成层管理(静态层缓存 + 动画层独立)  ⬜(GPU 契约 v1 与媒体图层直通是第一步)
       ↓
后端(hnsoft / cairo / Metal / GL / D3D11)
```

### 关键架构决策

| 决策 | 选项 | 结果/倾向 |
|---|---|---|
| 组件模型 | Web Components 式 vs 自有系统 | 自有系统(更轻, 不需要 Shadow DOM 的完整性) |
| 状态管理 | 响应式Proxy vs 显式绑定 | 显式绑定(更可控) |
| JS 引擎 | JavaScriptCore vs QuickJS vs 无 | **已定: 内嵌 QuickJS**(JSC 随 Swift 层删除; 见 README "JS 运行时选型") |
| 增量布局 | 脏子树标记 vs 布局树缓存 | 脏子树标记(与 CSS containment 对齐; 未实施) |
| 绘制后端 | hnsoft vs cairo vs Skia vs 平台原生 | **已定: hnsoft 默认 + cairo 可选 + GPU 原语契约(hnp_gpu_host)**; Skia 未采用(ADR 001) |

---

## 5. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 增量布局引入回归 | 高 | 高 | 每步全量回归断言 + 结构自检 |
| absolute 定位与 flex 交互复杂 | 中 | 高 | ✅ 已落地(overlay 等实用场景验证过), 风险降级 |
| JS 桥性能(每操作跨语言调用) | 中 | 中 | 批量操作 API + 命令缓冲 |
| 组件模型设计过度 | 中 | 中 | 先支持最小生命周期, 按需增加 |
| FreeType 交叉编译 | 中 | 低 | HN_NO_TEXT 降级路径已实现 |
| GPU 后端与 hnsoft 像素等价漂移 | 中 | 中 | gpu_probe 逐像素对照(≤16/255 收紧线) + 不支持指令整帧回退 |
| Linux/Windows 只到编译级对账(无行为回归环境) | 高 | 中 | linux_parity_check 符号级对账 + 需真机验证的项在 platforms.md 逐格标注 |
| 单人维护 bandwidth | 高 | 高 | 分阶段发布, 每阶段独立可用 |

---

## 6. 里程碑总结（2026-10-13 校准）

| 版本 | 主题 | 周期 | 交付 |
|---|---|---|---|
| v0.1 ✅ | 引擎原型 | 已完成 | 14 种指令渲染 · hx-* · sys:// · 流式 · 动画 · JS · 三平台 |
| v0.2 ✅ | 可用工具 | 已完成(定位/省略号/逐侧边框/事件/@keyframes/3D; 网络层随 Swift 层删除) | 14 种指令渲染 · 事件管道 · 3D 变换 |
| v0.3 ✅ | 富媒体 | 已完成(Lottie/网格变形/透明背板) | Lottie 矢量动画 · 网格变形贴图 · 逐像素透明窗口 |
| **v0.3.5** ✅ | **标准格式与形态** | 已完成(2026-10) | GIF / dotLottie(.lottie) / Live2D 运行时包 / JSON-CSV-Markdown 直染 / 桌宠窗口形态 / 媒体元素 + AVPlayerLayer 图层直通 / HiDPI / daemon 开窗与常驻 / WASM(wasm3) / Canvas 2D / GPU 契约 + Metal v1 |
| **v0.4** | **应用框架 + Smart-UX** | ~12 周 | §9 需求路线: 组件 · 状态 · 路由 · 伪元素 · Grid(五件套, 门槛) · A2UI 收口(§8) · 原生绘制动效语言(ChatGPT 级流畅感) · CSS 模块化(原子化 utility, 先评审) |
| **v0.5** | **生产可用** | GPU 契约 v1 已完成, 余 ~15 周 | ~~合成层~~(GPU 契约 v1 ✅, 直呈/图层缓存待做) · 虚拟滚动 · DevTools · 无障碍 · 安全 |
| **v1.0** | **OS-PWA** | 持续 | 后台任务 · 文件 · 传感器 · 多窗口 · 打包分发(胶囊 ✅) |

## 7. 下阶段候选清单（2026-10-13 定序, 全部未启动或有前缀条件; 核心主线已单列为 §8, 本表是候选池)

| 候选 | 动机 | 前置/口径 |
|---|---|---|
| **Metal 直呈**(GPU 帧不经 readback 直接上屏) | 现 GPU 路径仍是离屏 readback, 真窗收益受限; 契约里 `end_readback` 已注明"直呈模式(后续)" | `hnp_gpu_host` 契约已留位 |
| **GL 后端(Linux)** | 平台矩阵 Linux GPU 列为路线图 | 同一原语表; 需 Linux+X11 主机验证 |
| **D3D11 后端(Windows)** | 同上 | 需 Windows 主机 |
| **Linux 行为级验证** | 桌宠字段/Xv 视频图层/弹窗目前只到"编译级 + 符号级"对账(`linux_parity_check.sh` 自述) | 需 Linux+X11 真桌面 |
| **增量渲染 / 惰性布局** | 1.2 阻断级缺口里最重的一项; 大文档全量 layout 每帧有压力 | Phase 1g 设计可复用 |
| **组件生态**(上层, 不进引擎) | README "已知简化与路线图"远期项; 现有 ui-kit 预设是雏形 | 需先把 include/主题之外的组合能力补齐 |
| 弹窗原生交互(`hn-drag`/`hn-dismiss`) | README 路线图 | 壳层 |
| `hn dev` CLI 脚手架 | README/greeting 在指路, 未落地(`hn new` 同理) | CLI |
| ~~JS `addEventListener` 面(C99 重落地)~~ ✅ 已完成(2026-10) | rt 层 glue: 注册 → 命中链冒泡 → JS 回调 + 标准事件对象; `tools/events_probe.sh` 58 断言全绿 | examples/events.html 可用 |

## 8. 下一阶段核心：完全适配 A2UI 协议 · 组件系统 · 内部路由与状态管理

> 定稿: 2026-10-15。本节把 §3 Phase 2 的 2a(组件)/2b(状态)/2c(路由)收编为一个
> 有北极星的方向: **Neb 成为 A2UI 协议的一等客户端渲染器**。A2UI(Agent-to-User
> Interface, Google 发起的开放协议, Apache 2.0; 当前生产版 v0.9.1, v1.0 为候选)
> 让 agent 发送**声明式 JSON**(surface + 平铺组件列表 + ID 引用 + 数据绑定),
> 客户端用**本地受信组件目录**渲染 —— 与本仓库"agent 生成 hn 编码 → C99 引擎直
> 渲、无代码执行"的定位同构, 差别只在进料格式。

### 8.1 目标(可验收口径)

1. **A2UI 消息面**: daemon 新增 `a2ui` socket op(复用 JSON-lines 常驻通道,
   transport 无关), 接收 A2UI `createSurface` / `surfaceUpdate` 消息流 ——
   平铺组件列表按邻接表模型组装成组件树, 逐 surface 映射为一个 hn 文档/
   文档片段; 支持增量更新(按 ID 引用改子树, 走既有 `hn_doc_swap`/热更新
   路径)与流式渐进渲染(agent 分批推送, 用户实时看到界面生长)。
2. **组件目录(catalog)**: 建立"受信组件目录"注册表 —— 每个 A2UI 标准组件
   (Text/Image/Button/Row/Column/TextField/Card…)映射为一个模板 + 参数表,
   实例化产出普通 DOM 子树; 开放注册模式允许把自定义组件(含 ui-kit 预设与
   hn-mesh/hn-live2d/hn-three 等"表现型"扩展)注册进目录, 按 A2UI custom
   catalog 规范暴露。**agent 只能请求目录内组件** —— 未注册组件落占位卡 +
   stderr 明细(与 `hn-cubism` 未链 Core 的降级同一纪律), 不静默、不执行。
3. **动作回传**: 用户交互(点击/提交/输入)经现有命中管道(§1d)转成 A2UI
   action 消息(v1.0 的 actionResponse/action IDs)回传 agent —— 复用
   `hn event`/daemon socket 上行, 窗口内不引入新的 JS 面。
4. **内部路由与状态管理**: 视图注册表 + 历史栈(多视图/多 surface 切换,
   含切换动画挂钩)与响应式 store(显式绑定: `hn.store.define/bind/set`,
   数据变化 → 绑定 DOM 批量更新)—— 二者是 A2UI `data binding` 与
   multi-surface 语义的本地承载, 也是纯 hn 编码应用(不经 A2UI)直接可用的
   框架能力。

### 8.2 边界(明确不做)

- **不成为浏览器**: A2UI 组件目录只按 Neb 已支持的渲染能力注册; 不为对齐
  Web 组件生态引入 WebView/Shadow DOM/CSS 全量。组件树落到引擎的显示列表,
  三后端(hnsoft/cairo/Metal)自动同等。
- **不执行 agent 代码**: A2UI 的安全哲学(声明式数据、目录受信、无 UI 注入)
  与 Neb"无 web 运行时"承诺天然一致 —— agent 侧永远只发 JSON/数据; QuickJS
  仍是**本地作者**的可选交互面, 不对 A2UI 消息开放任意求值。
- **hn 编码不废弃、不降级为次等公民**: A2UI 是**进料方言**(JSON → 组件树 →
  hn 编码/直接 DOM 装配), 不是替代; 人类手写 `app.html` 与 agent 推 A2UI
  消息是两条等价入口(对齐 README"两条等价入口"原则)。反向(hn → A2UI
  导出)不做承诺。
- **协议版本纪律**: 按 v0.9.1(当前生产版)实现适配层, v1.0 候选期只跟踪不
  押注; 消息 schema 校验失败 = 显式错误响应, 不做宽容解析(协议演进期
  静默兼容是伪功能)。
- **transport 收口在 daemon**: A2A/HTTP/AG-UI 等传输网关属上层(宿主/代理
  侧), 引擎与运行时只认 socket op; 不在网络层破"无网络"承诺(sys:// 之外
  依旧零 fetch)。

### 8.3 与现有 hn-* 属性体系的关系

hn-* 体系不动、不改名、不被覆盖 —— A2UI 适配层是**挂在引擎公共 API 之上的
进料与编目层**, 分工如下:

| A2UI 概念 | Neb 现有对应 | 关系 |
|---|---|---|
| surface(createSurface/surfaceId) | daemon `open` 的 id + `<meta name="hn-surface" content="window\|popup\|layer">`/`hn-window` 几何 | surfaceId 即 daemon 应用 id; surface 呈现形态沿用 hn-surface 清单, 一个 surface = 一个已开文档 |
| 组件目录 + 邻接表组件列表 | 引擎元素 + `hn-*` 声明属性(模板参数直通: `hn-transparent`/`hn-level`/`hn-mesh-*`…) | 每条目录项 = "模板 hn 片段 + 属性映射表"; 标准目录覆盖 A2UI 规范组件, Neb 扩展组件按 custom catalog 注册(hn-* 属性作为模板参数原样透传) |
| data binding(路径绑定/数据模型) | 规划中的 `hn.store`(§Phase 2b, 显式绑定) + 已有 `sys://store` KV | 绑定表达式在 store 层解析, 变更 → 脏标记 → 批量更新绑定子树; sys://store 保持为应用级持久化 KV, 不与响应式 store 混用 |
| user action(client→agent) | `hx-get/post/trigger` 载体 + 命中管道(§1d) | A2UI action 挂在既有触发点上: 触发时除 hx 语义外再发一条 action 上行; `hx-*` 行为不变 |
| 渐进渲染(流式 surfaceUpdate) | daemon `update` 整推 + `hn_doc_swap` 片段交换 + hn-stream 追底 | surfaceUpdate 消息流直接复用这条装载/换入管线, 无新渲染路径 |

一句话: **hn-* 管"这个表面/元素长什么样、怎么表现", A2UI 适配层管"agent 的
意图怎么变成这些声明"** —— 后者编译到前者, 不并行造第二套描述语言。

### 8.4 优先级与依赖序(全部未启动)

| 序 | 交付 | 动机/依赖 | 估量 |
|---|---|---|---|
| P0 | **组件系统最小面**: 注册表 + 模板实例化 + 属性/子槽 + mount/unmount 生命周期(§Phase 2a 收编, 但 API 形状按目录项设计) | 是 A2UI 目录映射与后续一切的地基; 纯 C 模板 + 可选 QuickJS 钩子 | ~3 周 |
| P1 | **状态管理**(§Phase 2b 收编): define/bind/set 显式绑定 + 批量更新 | A2UI data binding 的承载; 依赖 P0(绑定目标是组件实例) | ~2 周 |
| P1.5 | **内部路由**(§Phase 2c 收编): 视图注册表 + 历史栈 + 切换动画挂钩 | 多视图/多 surface 切换; 依赖 P0, 与 P1 可并行 | ~1.5 周 |
| P2 | **A2UI 消息面收口**: `a2ui` socket op + createSurface/surfaceUpdate 解析 + 目录映射 + action 回传; 探针(a2ui_probe)钉住消息级回归 | 依赖 P0/P1/P1.5 全部; 协议对齐 v0.9.1, 用 A2UI Composer 产物做 fixture | ~3 周 |

总计 ~10 周(与 §6 v0.4 估量一致 —— 本章是 v0.4 的具体化, 不是新增一档)。
验收门禁: 双构建零 error + a2ui_probe(消息解析/目录映射/增量更新/降级占位)
+ 既有探针零回归; A2UI 端到端演示 = 同一份 agent 消息流在 hnapp 真窗与
headless shot 产出一致渲染。

---

## 9. v0.4 需求路线(2026-10-16 定稿): 从"能渲染"到"能构建应用"

> 北极星: **生态复用, 而不是引擎强度**。引擎再强, agent 每次都得从零写完整
> HTML, 形成不了复用与生态 —— **生态才是"现象级"的定义**。
> `examples/ui-kit.html` 的"include 一次写类名即可"是对的方向, 说明这层
> 已经被看到了; 但它是 CSS 预设, 不是组件模型。v0.4 要把它变成真的。

### 9.1 应用框架五件套(阻断级, 此前全部未动)

组件 / 状态 / 路由 / 伪元素 / Grid —— 这是从"能渲染"跨到"能构建应用"的
门槛, 五项一个都不能少:

| # | 需求 | 验收口径(需求级描述) |
|---|---|---|
| F1 | **组件模型** | `<define-element>`/受信目录: 定义一次, agent 与人类**写一个标签+属性**即得整块 UI; 属性/插槽/生命周期可组合; 与 ui-kit 的差别 = 组件承载**结构+行为+默认样式**, 不是散的 CSS 类 |
| F2 | **状态管理** | 声明式绑定(数据变 → 绑定的 UI 自动更新, 批量不抖动); 与 sys://store 持久化不混用 |
| F3 | **内部路由** | 多视图注册 + 切换(含过渡动画挂钩); 桌宠/面板类应用的"页面"概念 |
| F4 | **伪元素** `::before/::after` | 装饰性 UI 的标配(content/定位/继承父样式); 组件内部实现装饰不再需要额外节点 |
| F5 | **CSS Grid 基础** | `grid-template-columns` 等分/固定/比例 + gap; 与 flex 互补, agent 生成布局的面大幅拓宽 |

组件/状态/路由的 API 形状沿用 §8(A2UI 一等客户端方向)—— §9 是 §8 的
**需求侧定稿与扩展**(补 F4/F5 与下述两线)。

### 9.2 CSS 模块化: 不能让 AI 重复写 CSS

方向: **引入原子化 CSS(utility-first, 对 —— 就是 Tailwind 那个思路)**,
但要**先考虑清楚再动**:

- 问题: agent 每次生成应用都在重写 `.card{...}` 这类重复 CSS; 没有复用层,
  生成的样式质量不稳定、体积虚胖。
- 候选口径(需先做设计评审再实施):
  1. **引擎内置原子类**: 把 `hn-theme` 令牌体系扩为受信 utility 集
     (`flex-1`/`gap-2`/`text-sm` 风格), 引擎级解析, 零运行时成本;
  2. **hn-theme 主题 + utility 双层**: 主题管令牌/组件皮肤, utility 管
     布局微调 —— 与 F1 组件的"默认样式可被 utility 覆盖"衔接;
  3. 纯构建期方案(把原子 CSS 当预处理)不采: 违反"无构建步骤"定位。
- 评审要回答: 类名集多大、与组件默认样式谁赢、对现有探针断言的冲击面。

### 9.3 Smart-UX: 桌面上的原生绘制动画感

现状的诚实问题: **呈现总是"忽然出现、忽然消失"** —— 元素进出没有过渡,
帧率有余而动效语言缺位。v0.4 的体验目标是 **ChatGPT 智能 UI 那种流畅感**:
每个状态变化都有物理感的过渡, UI 是"真的画出来的", 不是 DOM 翻页。

三条线:

| 线 | 需求 |
|---|---|
| **进场/退场动效语言** | 元素挂载/卸载默认带 fade/up/scale 过渡(可关); 列表增删有交错动画; 对话流(agent-stream)逐条滑入 —— 覆盖"忽然出现/消失"的全部高频场景 |
| **高帧率设计** | 交互类动画全路径 60fps 已有底子(帧合并/ambient 分级/双 paint 合并 2026-10-16 落地); 补: GPU 直呈帧下的动画管线(HN_GPU_PRESENT 模式不丢帧)、`prefers-reduced-motion` 尊重、掉帧可观测(帧耗时分位) |
| **Canvas/Three 精度与精美感** | Canvas: ity/CG 后端能力面收齐(文字/渐变/混合不再回退错画); Three: 贴图采样质量(双线性/透明边缘)、光照一致性(GPU/软件同观感)、hn-three 与 CSS 动画同一时钟的编排 |

验收口径(需求级): 同一份组件化 demo 页, 在 hnapp 真窗连续操作 30s,
肉眼无"闪现/跳变", 帧率采样 p95 ≤ 20ms(60fps 档), 录屏与 ChatGPT 桌面端
动效语言同级(渐入渐出、有惯性、无硬切)。

### 9.4 交付顺序(需求定稿, 估量沿 §8 并入 F4/F5)

P0 组件 → P1 状态 + P1.5 路由(并行) → **P1.7 伪元素/Grid(新增, 与 P1 并行)** →
P2 A2UI 收口 → **P3 Smart-UX 动效语言(贯穿, 组件默认样式即带)** →
P4 CSS 模块化设计评审(先评审后实施)。

---

### 规划梳理(2026-10-19): 三段线路 + 两条路线

**第一段 · 眼前**: 演示与提交(按"personal agent 的 Smart-UX"叙事)。

**第二段 · 核心补全**(三主题逐个收口):
1. **多 HTML 项目模式** —— 项目 = 目录一级实体。
   **已落地**(2026-10-19): `hn_app` 接受目录路径(project.json 声明入口,
   缺省 index.html 隐式项目); 引擎内页面导航(`a href` 指向项目内文件 →
   同窗换装 hn_rt_navigate/back, 换 DOM/重布局/滚动归零/窗口不动, 页面栈
   上限 64, 外部地址仍交系统打开; 内联文档(daemon 推流)导航关闭口径不变);
   目录递归指纹热重载(FNV-1a 折 (路径,mtime_ns,size), 4096 项/深度 8 上限,
   任何被 include 片段/共享 css 变化都触发重装载; 几何记忆键 = 项目目录
   basename)。示例 `examples/project-demo/`, 断言 `tools/project_probe.sh`
   14 项。**余下**: `hn new` 脚手架、project.json 的 pages/app 字段消费
   (hn build 用)。原文:
2. **持久化 v2** —— `.hnapp` 从 JSON 升级 ZIP 容器。
   **已落地**(2026-10-19): 引擎级 `hn_zip.h/.c`(读 = EOCD→中央目录→按需
   取条目, 解压复用 hn_png 的 hn_inflate; 写 = 全存储 + CRC32; .lottie
   直读改单源同用); daemon persist v2(ZIP: manifest.json + index.html +
   DOM 遍历收集的本地资产, 原子写 tmp+rename)与 restore v2(PK 魔数识别,
   资产解到 ~/.html-native/apps/<id>/ 后以项目模式打开 —— 导航/相对引用
   照常; 同 id 替换语义; **restore 移出统一 app 守卫**, 无需先 open);
   v1 JSON 胶囊照常可读。端到端: open→persist→restore→text 全通, 产出
   包过系统 unzip -t 与 python zipfile 双外部校验。断言
   `tools/capsule_probe.sh` 15 项。**余下**: 胶囊签名(ed25519)、
   字体/音频类资产、JS scope 回收、`applet-remove` op。原文:
3. **agent 生态对接** —— 界面交付协议封装成主流 personal agent 框架
   的工具插件(function-call 一发即成窗)。
4. **`hn pack`: 既有 H5 网页 → .hnapp 轻应用**(依赖持久化 v2 的 ZIP
   胶囊与阶段 1 的项目目录):
   - 输入: 本地 H5 文件/目录, 或 URL(经运行时网络层拉取)
   - **兼容性体检**: 页面落进引擎无头渲染一遍(--shot/--probe), 对照
     hn 子集报告不支持/被忽略的特性 —— "符合标准的 H5"这个前提从
     口头承诺变成逐页可机检; 边界宣言不变(不承诺任意网页)
   - **改装**: 注入 hn-* meta(窗口尺寸/无边框缺省/hn-theme/hn-applet
     具名槽位), 原样式零改动
   - **打包**: 收集引用资产(图片/字体/css/js)进 assets/ 并改写引用,
     产出 项目目录(阶段 1 口径) 与 .hnapp 胶囊 v2 —— restore 即得
     桌面轻应用, 亦可直接 `hn build` 出安装形态
   - 定位: 把"网上已有的 H5 内容"变成 personal agent 桌面上的一等
     轻应用 —— 内容不动, 形态升级

**第三段 · 特殊路线 `hn build`(给人类的 Flutter 式编译出口) + 平台深化**:
- **定位纪律**: 主线(AI 层面) = daemon + socket + 秒开秒毁 + 热推送,
  无构建、窗口即易变对象 —— 产品本体, 架构决策为它服务; 特殊路线
  (`hn build`) = 把定稿项目编译成各平台传统可分发形态, 给人类开发者
  与正式分发场景。两条路共享同一引擎/同一页面代码/同一胶囊格式,
  build 不引入新运行时语义(同一个 hn_app 壳, 资源来源从 daemon 换成
  内嵌胶囊)。
- **机制**(全是既有件组装): 输入项目目录 → ① 项目打 .hnapp 胶囊 v2
  → ② 选对应平台 hnapp 二进制 → ③ 套外壳: macOS `.app`(Info.plist +
  icns) / Windows `.exe`(图标+资源) / Linux AppImage。应用名/图标/版本
  取 project.json 的 app 字段。`hn build --target macos|windows|linux`。
- **平台深化**(并行): Linux 透明窗 32 位 visual、Windows GPU 路径、
  胶囊签名(ed25519)与跨设备迁移。
- **远期**: 宿主发现机制、轻应用商店形态、ui-kit 组件生态层。

### neb3 原生纹理直入合成器(2026-10-22 第一步落地)

neb3(wgpu 后端)的帧此前每帧 GPU→CPU 回读 → 引擎再上传纹理 —— 一个
完整的 GPU 往返。第一步已落地**跨设备互操作桥**:

- wgpu 把 resolve 结果**直接渲进 IOSurface 背书的 MTLTexture**
  (MtlDevice.newTextureWithDescriptor:iosurface:plane:, objc2-metal
  绑定, 无链接期 IOSurface 符号依赖 —— 现代 macOS 的 IOSurface C 函数
  不再从框架导出, ObjC 类 alloc/initWithProperties 走运行时解析);
- 合成器侧在同一 IOSurface 上包自己的 MTLTexture 消费(IOSurface 是
  Apple 跨 MTLDevice 共享的标准机制); 同步纪律 = render 返回前
  `device.poll(Wait)`, 宿主同帧在其后消费(MTLSharedEvent 为升级项);
- 新 C ABI: `neb3_render_native` / `neb3_native_info` /
  `neb3_native_read_rgba`(读回助手, 锁 IOSurface 去行距拷出);
- **探针 `tools/neb3_surface_probe.sh`(18 断言)**: 原生路径与 CPU 回读
  路径对同一确定性场景**逐位一致**(互操作无语义漂移) + generation 递增
  + NULL 健壮性; c_abi 回归(1)与 neb3_fallback/three 探针全绿。
- ✅ **消费接线已落地**(同日第二轮): `hn_cmd.native_surface` 字段
  (BITMAP 携带 IOSurface, 字节可空) + 原语表 `tex_from_native`
  (hnp_gpu_metal 实现: newTextureWithDescriptor:iosurface:plane:, 零
  上传) + `hn_gpu_native_surface_ok()` 路由判定 + hn_three 原路
  (可选符号 dlsym, 旧后端/无 GPU host 自动回落读回)。hn_gpu 的位图
  缓存按 surface 指针命中即零操作(生产者已写入同表面)。**端到端探针
  `neb3_gpu_probe.sh`**: 同页双上下文(GPU=原生路由 / hnsoft=读回)对照
  —— 原生路由触发断言(防失败静默回落假绿) + 盒区内容双路一致
  (内容采样 84=84) + 框外恒背景。GPU 帧的三维内容零 CPU 往返达成。
  语义要点: 原生 IOSurface 内容为**预乘域**(管线预乘变体 +
  LoadOp::Clear 预乘清屏), 与 Metal 合成器口径一致; 不透明片元的
  预乘(a=1)≡直读(MSAA resolve 逐位同值, surface 探针逐位锚)。
  v1 边界: 整帧回退 hnsoft 的罕见场景下原生框跳过一帧(roadmap 已档)。

### 已落地(2026-10-15 渲染与分发批次)

- **`hn build --target macos`**(第三段核心落地): 项目目录 → .hnapp 胶囊 v2
  → `<Name>.app` 外壳(Info.plist plutil 校验 + Contents/MacOS/hnapp 运行时 +
  Resources/app.hnapp; 应用名/id/版本取 project.json app 字段)。
  **运行时入口**: `hn_app --capsule <x.hnapp>` —— 与 daemon restore 同口径解包
  到 apps/<id>/ 后走项目模式(隐式 index.html), W/H 取 manifest。
  断言 `tools/capsule_app_probe.sh` 10 项(结构/plutil/PK/unzip/直启解包)。
- **渲染质量**: box-shadow 真 3-pass 高斯(Kovesi)+ 渐变 bayer 去色带 +
  three.js Blinn-Phong 双灯(高光/半球环境) —— 绘制画质整体升级。
- **脱流命中修复**: absolute 元素在无流内容页曾永远命不中(hit 按祖先盒
  剪枝)—— 根链覆盖画布 + 脱流子级优先命中(浏览器语义)。
- **visibility 子级复显**: visibility 入继承集, hidden 父下 visible 子级
  正常绘制(顺带修 goto 越过初始化的深层 bug)。
- **select 弹层**: 开合/选项行/外点关闭全交互。

### 已落地(2026-10 中旬批次)

- **窗口默认无标题栏**(窗口即画布; `hn-frameless="0"` 显式要系统标题栏;
  hnwin 补认 frameless + 背景拖拽) —— 三平台形态口径一致
- **呈现动画**: 画刷铺色(逐像素透明窗专属, `hn-reveal`) + 全窗入场淡入/
  关闭淡出; `HN_REVEAL`/`HN_REVEAL_MS`/`HN_WINDOW_POS` 观测与钉位口径
- **渲染质量**: 边缘 4-tap 抗锯齿(填充/描边环/圆角裁剪), 描边环改 CSS
  border 内带语义(修边框幽灵化), clearRect 分数覆盖
- **收尾清理**: 死代码/嵌套注释/匿名参数清零, render/mesh/sysbridge 三
  探针补运行器(37 族), fb_clip_cov 直角 bbox 回归修
