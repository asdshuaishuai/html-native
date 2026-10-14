# html-native 路线图：从玩具到 OS-PWA 级框架

> 版本: 1.1 · 原始日期: 2026-09-17 · **校准: 2026-10-13**(§1 全部数字按当日代码
> 实测重取, 命令写在各表脚注; §3 起为路线图, 已完成项在原条目上标注) · 状态: 规划中
>
> 注: 2026-10-09 c49cfbd "全量 C99" 之后渲染只剩自研 C99 一条路径 —— 现为
> 引擎(Sources/CHtmlNative) + 公共运行时(rt/hn_rt.c) + 平台层(platform/hnp_*.c,
> 含 GPU 原语表 hnp_gpu_host) + 应用层(app/hn_app.c), 全部 C99, 见 README 代码地图。
>
> 本文档是 html-native 从当前原型演进到生产可用框架的
> 深度规划。包含差距分析、竞品对标、分阶段路线图与架构演进。

---

## 1. 当前状态（诚实审计, 2026-10-13 实测)

### 1.1 有什么（已验证）

> 行数口径(`wc -l`, 2026-10-13 工作树): 引擎 = `Sources/CHtmlNative/*.c *.h
> include/hn.h`(20,637 行); 平台层 = `platform/*`(3,469 行); 运行时 = `rt/*`
> (2,581 行); 应用层 = `app/*`(1,351 行); 工具与探针 = `tools/*.c tools/*.sh`
> (20,145 行)。断言数 = 各探针当日实跑输出计数(明细见 1.1 表"测试"行)。

| 层 | 代码量 | 能力 |
|---|---|---|
| C99 引擎 | 20,637 行 | HTML 解析 · CSS 级联(含属性选择器/`:not`/`nth-of-type`/`calc`) · flex/行内/absolute 布局 · 绘制指令(10 类: RECT/TEXT/IMAGE/CLIP±/QUAD/POLYGON/MESH/BITMAP/CLEAR_RECT) · 动画插值(过渡/@keyframes/缓动族) · 3D 变换(4×4 矩阵栈 + preserve-3d + **深度排序**) · Lottie/dotLottie 求值 · Live2D motion3 驱动 rig · 网格变形 · GIF · PNG · Markdown/JSON/CSV **直染** · 媒体元素状态机 · Canvas 2D API · WASM(wasm3) · 结构自检 |
| 平台层 `platform/` | 3,469 行 | `hn_platform.h` 契约(窗口/媒体/视频图层/**GPU 原语表**) · `hnp_macos.c`(dlopen AppKit/CoreGraphics/QuartzCore + AVPlayerLayer 图层直通) · `hnp_media_macos.c`(AVFoundation) · `hnp_linux.c`(X11 + **Xv 视频图层原语**) · `hnp_headless.c` · `hnp_gpu_metal.c`(**Metal v1**, 682 行) |
| 公共运行时 `rt/` | 2,581 行 | 文档生命周期/htmx 执行/sys:// 桥/本地 KV/持久化 · QuickJS 求值(HN_HAVE_QUICKJS) · **JS 定时器与 RAF**(帧驱动) · **HiDPI scale** · **GPU 宿主槽**(`hn_rt_set_gpu_host`, 缺省 hnsoft 逐帧回退) |
| 应用层 `app/` | 1,351 行 | `hn_app.c` 唯一主循环(开窗/mtime 热重载/**几何记忆**/--shot/--probe) · `hn_media_platform.c`(平台媒体宿主装配) · `hn_media_synth.c`(合成宿主单一来源) |
| ~~macOS 运行时(Swift)~~ 已删 | — | Swift 全层于 c49cfbd 删除; 现 C99 层接管: `tools/hnweb_macos.c` + `platform/hnp_macos.c`(dlopen AppKit/CoreGraphics, 纯 C99) |
| Linux 运行时 | `tools/hnweb_linux.c` ~2000 行 | M1 窗口/渲染/expose/resize/点击命中 + M2 **事件派发管道**(点击/键盘/滚轮/hover → 目标解析 → 带 id 冒泡 → hx-trigger 消费)与**动画帧循环**(dt 推进, 空闲零唤醒); 新架构 `hnapp-linux-x86_64`(`platform/hnp_linux.c`, 桌宠字段全对齐 + Xv 视频图层原语)。仍缺: sys:// 桥(壳内 transport 未移植, 点击轨迹照常)、XIM 键入、交叉口径文本 |
| Windows 运行时 | `tools/hnwin.c` 803 行 | Win32 窗口 + hnsoft + `sysbridge.c`(sys:// 桥); 新架构 `hnp_win32.c` 未写(平台矩阵 Windows 列在路线图) |
| 软件光栅器 | 1,135 行 + cairo 后端 898 行 | SDF 圆角矩形 · 线性/径向渐变 · 多边形扫描线 · 网格纹理映射 · **PNG 解码(自带 inflate)** · FreeType 文本 · PNG 编码; cairo 后端同签名可换(`hn_cairo.c`, 真模糊阴影/任意路径裁剪/真 zlib) |
| 宿主与工具 | daemon 1,306 行 + CLI 292 行 | daemon 开窗 spawn(`--window`)/热推送/常驻(`resident`)/胶囊(`persist`/`restore`)/开窗几何记忆; CLI 9 个子命令(ping/open/update/close/list/dump/text/event/shot) |
| 测试 | 探针 24 个(`tools/*_probe.sh`)+ 门禁 2 个 | **约 1145 项断言/检查**(逐探针实跑计数: render 61 · css 96 · layout 33 · html 37 · canvas 95 · timer 59 · fmt 118 · md 107 · gif 61 · anim 31 · stream 30 · include 23 · wasm 19 · mesh 36 · live2d 44 · lottie 37 · quad3d 33 · gpu 7 · pet 27 · resident 16 · window 25 · media_element 40 · media_window 30 · media 18 · hidpi 8 · sysbridge 17 · hnsoft_alpha 7 · cairo 30; 另有 `linux_parity_check.sh`(编译级对账)与 `perf_probe.sh`(CPU 硬线 1 条)全过) |
| 示例 | 36 个 HTML(+6 CSS) | 仪表盘 / 网页写法 / 消息卡 / 输入控件 / Lottie / dotLottie / 透明背板 / 桌宠 / live2d-demo / live2d-miku / canvas-demo / image-lab / md-demo / fmt-demo / media-demo / threejs-lab / threejs-ui / ui-kit / anim-lab / agent-chat / agent-stream … |
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
| ~~**网络层**~~ ⚠️ | Swift 版 `hn.fetch`(Promise + 主机白名单)随 c49cfbd 全量 C99 删除, 当前 C99 层**没有**网络实现(examples/network.html 用 hn.fetch 会报 TypeError); 重新落地是候选 | 运行时 |
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

** trade-off：** 功能完整度 vs 体积/性能。目前偏"太少功能"，路线图要
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

**现状口径(2026-10-13 重核)**: 引擎提供 `hn_event` 数据与冒泡路径
(`hn_event_path_at`/`hn_event_path_len`, 只收集带 id 元素), 平台壳(M2 管道:
`hnweb_macos.c`/`hnweb_linux.c`/`hnwin.c`)做目标解析 → 带 id 冒泡 → `hx-trigger`
消费; 事件种类见 `hn.h`(mouse 系/keydown-up/focus-blur/scroll/input/change/
submit/hover + launch/show/hide/destroy 生命周期)。daemon 侧有
`hn_rt_synthetic`(合成 click 驱动 hx 的执行入口)。
**如实修正**: ①Swift 版的 JS `addEventListener` 桥与 `preventDefault()` 随
c49cfbd 全量 C99 删除, 当前 C99 层**没有** JS 事件监听面(examples/events.html
的 `<script>` 会报 TypeError); ②"M2 全壳同构"的完整度是 Linux 壳口径, macOS
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

#### 1e. 网络层（运行时层）⚠️ 曾被标记完成, 现不存在

**校准**: Swift 时代(V0.2)确实做过 `hn.fetch`(URLSession + 主机白名单 +
Promise 包装, 165 断言)。**c49cfbd 全量 C99 时随 Swift 全层删除**, 当前
`Sources/rt/tools/app/platform` 全部 C 文件里 grep 不到任何 fetch/网络实现;
README 与 platforms.md 的网络口径是"无网络: hx-* transport 可注入(sys:// 本地)"。
examples/network.html 仍在, 但 `hn.fetch` 调用会抛 TypeError(daemon 日志可见)。
- 若要恢复: C99 侧需自实现 HTTP 客户端 + 白名单(或明确保持"无网络"承诺, 走
  `sys://open` 桥外置) —— 这是候选而非完成项。

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
显示列表(10 类指令) → 后端选择:
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
| v0.1 ✅ | 引擎原型 | 已完成 | 5 指令渲染 · hx-* · sys:// · 流式 · 动画 · JS · 三平台 |
| v0.2 ✅ | 可用工具 | 已完成(定位/省略号/逐侧边框/事件/@keyframes/3D; 网络层随 Swift 层删除) | 8 指令渲染 · 事件管道 · 3D 变换 |
| v0.3 ✅ | 富媒体 | 已完成(Lottie/网格变形/透明背板) | Lottie 矢量动画 · 网格变形贴图 · 逐像素透明窗口 |
| **v0.3.5** ✅ | **标准格式与形态** | 已完成(2026-10) | GIF / dotLottie(.lottie) / Live2D 运行时包 / JSON-CSV-Markdown 直染 / 桌宠窗口形态 / 媒体元素 + AVPlayerLayer 图层直通 / HiDPI / daemon 开窗与常驻 / WASM(wasm3) / Canvas 2D / GPU 契约 + Metal v1 |
| **v0.4** | **应用框架** | ~10 周 | 组件 · 状态 · 路由 · 伪元素 · Grid(全部未动) |
| **v0.5** | **生产可用** | GPU 契约 v1 已完成, 余 ~15 周 | ~~合成层~~(GPU 契约 v1 ✅, 直呈/图层缓存待做) · 虚拟滚动 · DevTools · 无障碍 · 安全 |
| **v1.0** | **OS-PWA** | 持续 | 后台任务 · 文件 · 传感器 · 多窗口 · 打包分发(胶囊 ✅) |

## 7. 下阶段候选清单（2026-10-13 定序, 全部未启动或有前缀条件)

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
