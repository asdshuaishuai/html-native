# ISSUES — 存量技术债与活跃问题

> 更新: 2026-10-15（全量刷新）· 基线: `c76595f`
> 上一版已修复并剔除: P0-2(a.out)、P1-2(webkit-fallback)。
> 本版合并: 上一版存量 + 3D 冲刺工作流 22 条复审发现 + 代码治理 13 条复审发现 + canvas_ity/CG 后端遗留。
>
> **另见 [`ISSUES-1.md`](ISSUES-1.md)** —— 2026-10-16 参赛录屏准备轮新发现的问题
> （含 2 个已修+探针锚定的引擎缺陷: 脱离流嵌套文字重影、圆角矩形四边各缩 r/2）。

**范围**: 只记录当前已落地能力的存量债务与活跃问题。v0.4 规划(A2UI/组件/路由)写在 roadmap 不在此重复。
**严重度**: P0 = 会咬人; P1 = 功能缺陷; P2 = 文档/注释腐化。

---

## P0 — 工程卫生

### P0-1 根目录 `deepStudio` 符号链接已删除

- 原链接 `html-native/deepStudio → ..` 会造成递归工具循环，并存在尾斜杠误删父目录风险。
- 已安全删除链接本身，父目录内容未触碰。

### P0-2 构建: PE/canvas_ity 清单已修复

- `hn_canvas_ity.cpp` 已启用 `CANVAS_ITY_IMPLEMENTATION`；`build-pe-dev.sh` 已恢复 PainterEngine include、core 源清单与链接参数。
- 已验证：`build-pe-dev.sh` 成功生成 `/tmp/hnapp-pe`（1824 KB）。

### P0-3 探针/daemon/multiplatform/daemon-cli 构建清单已统一

- 共享清单已纳入 `hn_three.c`、`neb3_soft.c`，运行时目标纳入 `rt/hn_sys.c`，各显式 runtime probe 与 Cubism/daemon/PE/multiplatform 入口已同步。
- 已验证：daemon/CLI、Cubism、PE 和 `build-multiplatform.sh` 全部成功；Windows x86_64、Linux x86_64/aarch64、macOS arm64/x86_64/headless 产物均生成。

### P0-4 `ity_enabled()` 已接入入口

- `hn_canvas_paint_ity` 现在仅在显式选择 `HN_CANVAS_BACKEND=ity` 时接管画布，否则回落 ledger。
- 已验证：Canvas 后端专项的 default/ledger/invalid/ity/cg/system 变体均通过。

---

## P1 — 功能缺陷（本轮已修复）

### P1-1 neb3 C ABI 结构体 vs 4 float 不兼容

- Rust 导出已使用 `#[repr(C)] Neb3Color`，并按 `neb3_color` 结构体传值，和 C 头保持单一 ABI。
- 已验证：`cargo test --release` 的 C ABI 回归通过。

### P1-2 `neb3_last_error` 空 String 悬垂指针

- 空错误现在返回静态 NUL 字符串，非 dangling 指针；非空错误使用稳定的 CString 存储。
- 已验证：C ABI 回归真实调用 `neb3_last_error` 并通过。

### P1-3 `hn_three.c` LRU 纹理缓存逐出生命周期

- TRIS 显示列表不再引用纹理门槽的可逐出字符串，路径统一拷贝到本帧临时 arena；逐出不会影响已排队命令。
- 已验证：`three_probe.sh all` 738 项断言、normal/ASan 均 0 失败。

### P1-4 CG 后端接线缺口

- `hn_canvas_paint_cg` 已接入显式 `HN_CANVAS_BACKEND=cg/system` 路径，并通过带 context 的工厂提供资产加载、CoreText 与图像分支。
- 已验证：CG 后端专项通过图像方向、CoreText、重放与裁剪测试。

### P1-5 daemon `hx_perform` 系统路由缺失

- `sys://` 路由已抽到 `rt/hn_sys.c` 单一实现；补齐 host/cpu/memory/disk/battery/uptime/clipboard/open/notify/info，并通过 host callbacks 保留 daemon 与 runtime 各自 store 命名空间。
- 已验证：当前源码重建的 daemon 在隔离 HOME 下真实执行 `sys://host`、`sys://battery`，`tools/sysroute_probe.sh` 9/9 通过。

### P1-6 daemon `hx_perform` 缺 stderr 诊断

- daemon 与 runtime 现在统一经 `hn_sys_request` 输出 `[hx] fetch 不支持`（非法 scheme）和 `[hx] 未知 sys:// 路由`，并立即 flush stderr。
- 已验证：`tools/sysroute_probe.sh` 在当前重建 daemon 上 9/9 通过。

### P1-7 ity/cg 后端 `clearRect` 拖影

- ity 与 CoreGraphics 后端在重放前都会清空全幅透明面，并回退/重建基底 save、clip 状态；局部 `clearRect` 也会按矩形透明擦除，避免跨帧状态和像素污染。
- 已验证：Canvas 后端专项 default/ledger/invalid/ity/cg/system 全部 0 failures；既有 `canvas_probe.sh` 178/0 通过。

### P1-8 `hn_three.c` Windows 交叉构建

- `dlfcn.h`、dlopen/dlsym 代码已用 `_WIN32` 门控；Windows 目标走软件路径。
- 已验证：Windows x86_64 C99 `hncore`/`hnwin` 交叉构建成功；Linux parity 与跨平台构建通过。

### P1-9 运行时网络层与 `hn.fetch`

- `hx-get`/`hx-post` 的 `http://` 与 `https://` 已接入运行时系统 transport：macOS/Linux 运行期绑定 libcurl，Windows 运行期绑定 WinHTTP；请求带超时、重定向上限、响应体上限和 `HN_FETCH_ALLOW` 主机白名单。
- QuickJS 已提供 `hn.fetch(url, options)` Promise API，响应支持 `ok/status/statusText/text()/json()`，4xx/5xx reject 并带 `status`；daemon headless 帧驱动会执行 pending jobs。
- 已验证：本地回环 HTTP 的 hx-get DOM 换入通过；QuickJS `hn.fetch` 回环请求解析 JSON 并更新 DOM 为 `resolved:local`；daemon/CLI 重建通过。

---

## P2 — 文档与注释腐化

### 已修/移出活跃

- P2-2 `paint_polygon_triangle`：已删除未接线死代码；现行三角覆盖仅由 `poly_edge_coverage` 负责。
- P2-3 ity 颜色通道注释：已按引擎统一 `0xRRGGBBAA` 口径修正注释与提取逻辑。
- P2-5 threejs-demo 图注：已更正为仅描述已接线的 `poly_edge_coverage`。
- P2-6 hx-test-post 预期：已更正为 `store-value`/所存值回显，不再误称 `copied`。
- P2-7 hx-test-external 预期：已对齐实际 `[hx] fetch 不支持` stderr 诊断。
- P2-10 canvas opacity：已将 `<canvas>` 自身 `opacity` 折入 `paint_canvas` 的 alpha。

## P2 — 活跃

| # | 问题 | 位置 |
|---|---|---|
| P2-1a | README 的显示列表口径已更新为 14 种；但当前树实测引擎 25,095 行，README/roadmap 的其余约数仍随开发漂移 | README.md / docs/roadmap.md |
| P2-1b | 统计口径已改为可复核的 `find` 表达式；当前工作树的精确数量随新增探针变化，不再把旧 36/48 示例、24/26 探针当作现状 | README.md / docs/index.html / docs/oscc-2026/ |
| P2-4 | CG 后端 `premult_copy` 旧注释论据需继续核对；现有实现结论安全，且本轮已有通道/绘制顺序修订 | Sources/CHtmlNative/hn_canvas_cg.cpp |
| P2-8 | media-design.md 仍有多处行号锚漂移，至少包含 `hnp_macos.c:137-145`、`rt/hn_rt.c:455-505` 等需按当前源码复核的引用 | docs/media-design.md |
| P2-9 | 显示列表命令数量已统一为 14 种，需保持以 `hn.h` 的 `HN_CMD_*` 枚举为唯一来源 | docs/roadmap.md / README.md / docs/index.html |

---

## 2026-10-16 系统性修复轮(网络/GPU/Canvas/three/调度/直呈)

### 已修复并验证

- **GPU 内存安全**: 纹理句柄改墓碑槽位(交换删除曾使存活句柄失效); >16384 quad 分块的 IBO 偏移越界改按块重绑顶点(均通过 gpu_probe 全绿)。
- **GPU 正确性**: 渐变 LUT 双重预乘(rgb×a²)修为直写非预乘; 帧事务 `g_failed`(批次/字形/位图/掩码失败整帧回退, 不再静默缺笔); 字形缓存计数先于提交的投毒修复; 位图缓存命中更新失败曾返回陈旧纹理; 命令缓冲执行状态检查; mtl_init 失败路径统一释放(幂等 shutdown)。
- **Canvas**: ity 渐变写错 brush(stroke→fill); ity 能力预检(不支持 op/混合整帧回退账本, 不再静默跳过); CG/ity 后端槽位绑定 context(跨 context 串画/悬垂修复)。
- **three**: 软件路径投影漏用 world 变换(旋转只改光照不改几何, 像素差已验证); neb3_soft 占位帧返回诚实失败(此前吞掉软件输出→空白); 轨道相机改按下拖拽语义(pointer_down 贯穿引擎)。
- **调度/度量**: perf_probe 分母(实际 5s 被除以 6)与 RSS 单位(10GiB 容差→10MB)修复 + HN_GPU 隔离; Linux pump 零超时曾无限阻塞; daemon 帧推进改真实单调时钟(原固定 16ms → 1/3 速率); full 路径双 paint 合并(canvas-demo CPU 8.8%→2.8%, timer-only 0.2%)。
- **macOS Metal 窗口真直呈**(HN_GPU=metal HN_GPU_PRESENT=1, 不透明窗): CAMetalLayer.nextDrawable 渲染 + presentDrawable 出帧; drawable/cmdbuf autorelease 生命周期(retain + 前帧入口同步); 实测 presented=207 / readback 稳定为 2(attach 前初始帧), 退出统计可见; --shot 在直呈模式仍走像素路径; 弱桩保证无 GPU 构建零漂移。

### 已知问题(本轮发现, 已修)

- ~~timer_probe 8c GIF ambient 像素 2 断言红~~ **已修**: 根因是 hn_paint.c GIF 分支发射 BITMAP 时 `cmd.fill` 从未设置(memset 0 → alpha 字节 0 → hnsoft 以 alpha=0 混合)—— **GIF 动图在渲染管线完全不可见**(静态图走 IMAGE 分支不受影响)。修复: fill 低字节写元素 alpha(与 canvas/three 分支同契约)。timer_probe 59/0。
- ~~布局: `html,body{height:100%}` + flex 居中 body 盒错位~~ **已修**(两处根因):
  1. 解析器合成包裹根(tag 同为 "html")参与作者 CSS 匹配 → `html{display:flex}` 把包裹根变 flex 容器、真实 html 反成 flex item(整页错位)。修复: 节点加 `is_doc_wrapper` 标志, compound_matches 对包裹根不匹配作者选择器。
  2. flex pass1 度量时交叉轴未知 → `height:100%` 的 item 按 0 度量、居中偏移 (H-0)/2 整体下移半视口。修复: pass2 定位前对百分比高子项按行交叉可用高重解析。
  3. rem 基准随 1 联动修正: g_root_font 改取真实 html(父=包裹根)的字号。layout 38/0 + css 96/0。

### 2026-10-15 H5 标准第二轮(交互/量测/动态画布, 已修+探针锚定)

- **Canvas 动态改尺寸**(H5 标准): JS 重设 canvas.width/height = 位图变化 +
  **清空画布**。`hn_canvas_sync_element_size` 在 record/get_image_data/paint
  重放三消费口统一同步; 顺带修既有 bug: 空账本 getImageData 返回 null
  (标准应给透明像素 —— 重设尺寸后读像素曾拿 null)。
- **Canvas 标准 API 补充**: ctx.letterSpacing/wordSpacing(字距进度量与绘制,
  op 带 lsp 字段) / ctx.reset()(清画布+状态归缺省)。
- **DOM 量测与交互**: getBoundingClientRect/offset 四件套(新桥 __hnGetRect)、
  scrollTop/scrollLeft 读写 + scrollIntoView、insertBefore/replaceChild
  (引擎 `hn_node_insert_before`, 移动语义)、dispatchEvent + el.click()
  (走平台同一派发入口 rt_js_dispatch, 冒泡链全程)、addEventListener
  {once}、window.innerWidth/innerHeight/devicePixelRatio。
- **CSS**: visibility:hidden(保布局不画; 子级 visible 复显未做——如实声明)、
  pointer-events:none(命中跳过子树)、min()/max()/clamp()(绝对单位; clamp
  按 CSS 规范 MAX<MIN 时 MIN 赢; 含 % 实参不支持——如实声明)。
- **HTML**: 命名实体扩到 ~70 个(数字实体本就全量); `<progress>` 控件绘制
  (UA 200x14 + 轨道/填充, 无 value = 不定态往复动画)。
- 探针: dom_probe 51 断言(+13: rect/offset/viewport/scroll/insert/
  replace/dispatch/once/click/resize 清空/letterSpacing/reset);
  css_probe 96→106(min/max/clamp/visibility 保布局);
  html_probe 43→48(实体解码/数字实体/progress 盒+绘制/visibility 不出文本)。

### 2026-10-15 H5 标准第三轮(交互控件与原生默认动作, 已修+探针锚定)

- **`<details>/<summary>` 折叠**: 点击 summary 切换祖先 details 的 open
  (正文点击不触发; preventDefault 阻止); 无 open → 非 summary 子级
  display:none(级联后布局前的文档遍历, 布局/绘制/命中三管线一致)。
- **checkbox/radio 原生切换**: 点击切换/同名组互斥(组 = 最近 form 祖先,
  无则文档根)—— 此前点击无任何反应(状态只由 JS/属性驱动)。
- **`<input type=range>` 滑条**: HN_IN_RANGE 控件(轨道+填充+滑块绘制,
  UA 160x20 border-box 零 padding); 点击/拖拽线性设值写回 value 属性;
  不可编辑文本/不进焦点链。
- **`<select>` 关闭态渲染**: 盒 + 选中项(option[selected] 优先)文本 + ▾;
  UA border-box 160x26。下拉弹层未做 —— 如实声明。
- **原生默认动作架构**: 激活行为统一收口 `rt_native_click`(rt_js_dispatch
  内, 事件派发后、无 preventDefault 才执行 —— 浏览器同序); 零监听器
  纯声明式页面同样工作; el.click() 合成点击同样触发。
- **visibility 入继承集**: hidden 父下子级继承 hidden(子级显式复显的
  完整语义未做 —— 子树整体跳过, 如实声明)。
- 探针: events_probe 58→66(+8: 真输入通路的 details/checkbox/radio/
  range/preventDefault); html_probe 48→53(details 折叠显隐/select·range 盒)。

### 2026-10-15 H5 标准第四轮(弹层/复显/键盘/绘制高清, 已修+探针锚定)

- **`<select>` 弹层**: 点击开(▾ 翻 ▴) → 选项行列表(紧贴盒下缘, 视口装不下
  整体上翻; 选中行高亮) → 点行 selected 迁移并收起 / 点外仅关闭。绘制 =
  paint_select_popups(paint_root 尾部统一补画, 盖在一切之上); 命中 =
  hn_rt_button 弹层块(行几何与绘制同式)。弹层暂无滚动(超视口截断)——如实声明。
- **脱流元素命中修复**(副产品, 深层): position:absolute 元素在无流内容页
  (html/body 盒高 0)曾永远命不中 —— hit_walk 按祖先盒包含剪枝, html(高 0)
  直接剪掉 body/弹层。修复: 根链(html/body)按浏览器语义覆盖整块画布 +
  脱流子级在祖先盒检查**之前**优先试命中。
- **visibility 子级复显**: visibility 入继承集 + paint 逐节点自绘门
  (hidden 节点跳自身盒/文本但照常递归子级 —— 子级 visible 按标准复显)。
  **顺带修深层 bug**: 子级跳转用 `goto children_only` 越过了 csx/child_alpha
  的初始化 —— 不确定值让子级在 alpha 守卫处全灭(实测复现: 子级背景/文本
  全部消失)。声明挪到跳转之前。
- **文本 visibility 门**: 文本节点不吃级联 —— 用父级计算样式判
  hidden 父下文本不画(ghost), visible 链上照画(复显)。
- **range 键盘调节**: 聚焦后 Left/Down = -step, Right/Up = +step(HN_IN_RANGE
  可成为键盘目标; BUTTON 仍排除)。
- **渐变去色带**(高清绘制): hnsoft 渐变采样加 8×8 bayer 有序抖动(±半档
  量化噪声) —— 平滑渐变的相邻像素同桶色带被打散成不可见细颗粒(实测
  256px 低反差渐变行边界翻转 6 → 122, 色带消失; 色域分布不变)。
- 探针: events_probe 66→72(+6: select 开/行选/外点关闭); css_probe 106
  (+复显断言); html_probe 53(visibility 语义断言更新)。

### 2026-10-15 渲染能力升级轮(高斯/着色/滤镜, 已修+探针锚定)

- **box-shadow 真高斯模糊**: 旧实现 6 层扩边矩形叠透明(观感"一圈圈光环")。
  新实现与浏览器同路数: 阴影形状 → 独立 alpha 蒙版(SDF 覆盖率) →
  **3-pass box blur(Kovesi 三盒逼近高斯, 误差 <3%, 滑窗 O(n) 与半径无关)**
  → 蒙版 × 阴影色 blend 回主帧。σ = blur/2(CSS 口径), 蒙版域 = bbox+3σ。
  小 blur(≤1.5px) 单层 SDF 直绘。实测: 阴影边缘单调平滑过渡、无光环
  台阶(相邻像素最大跳变 4, 单调递减)。
- **three.js Blinn-Phong 双灯着色**: 旧单灯 Lambert(0.35+0.65·n·L)。
  新: key 灯漫反射 + Blinn 高光(^64, 强度 0.55) + fill 灯(反侧 0.25) +
  半球环境光(法线 y 分量 0.18..0.32 插值)。球体呈现光泽高光、盒面
  明暗层次立体(实测球心高光 123 像素 >460 亮度)。
- **Canvas ctx.filter='blur(Npx)'**: glue 属性 + 借 shadow 管线下送
  (shb=2σ, 偏移 0, 本体 col=0 只画模糊蒙版); ctx.filter 读回 'blur(Npx)'/'none'。
  ~~蒙版内部饱和度异常~~ **复核结案(2026-10-17)**: 非缺陷 —— 引擎
  getImageData 是**非预乘 RGBA**(RGB 直通色, 模糊浓度衰减在 **alpha 通道**);
  此前观察读的是 RGB 通道故误判 0.39。逐层验证: replay 参数链正确
  (shcol/blur/ks 直查) → paint_shadow 蒙版行完美高斯(0/0.03/0.28/0.72/0.97/1.0)
  → A 通道实测 0..253 单调衰减。dom_probe 两断言钉住(A=255 实心 + 边缘衰减)。
- **渐变去色带**(上轮): bayer 抖动, 与本轮高斯阴影叠加 = 绘制画质整体升级。
- 回归: 全部 16+ 探针家族全绿(render 83/0 · events 72 · css 108 · three ·
  quad3d · perf/anim 门禁通过)。

### 2026-10-15 handwired 研究与时间栅格轮(已落地)

- **chenglou/handwired 研究**: "冻结 UI" —— UI 编译为冻结 FP32 运算
  列表(affine/reglu/boolean/indexRead 四指令), 无训练、精确、可微;
  HandwiredOS(4,646 运算驱动 3 窗口桌面)。完整笔记
  `docs/handwired-notes.md`(执行模型/行为选择/host-timing/patterns
  + 对 hn 的采纳映射: ✅ 已落地 / 📋 排期 / ❌ 不采纳)。
- **动画时钟 1/64ms 栅格量化**(host-timing 手法落地): 帧时间戳先量化
  到 1/64ms 栅格再做差 —— 任意拆分的时间跨度严格相等, 过渡/关键帧/
  平滑滚动采样点跨帧率(10/30/60/120fps)落在同一条缓动曲线上。
  daemon 与 hn_app 双端同改。复核实测: runtime 174 测试全过,
  HandwiredOS 求值 42.86µs/帧。
- **handwired #1 事件在时间戳处应用**(📋 排期第一项落地, 2026-10-21):
  输入事件(button/pointer/key/scroll)带单调钟时间戳(1/64ms 栅格),
  到达即把动画钟推进到事件时刻(新目的地立即开始拉动), 消耗量从帧
  调度扣除(同一真实跨度恰推进一次)。顺带修: 无 pd 的 click 监听器
  跑过后缺全路径重算 —— 过渡晚一拍才启动(实测 500ms 只走 ~0)。
  events_probe 72→81(+9: 事件时间戳确定性段 —— 1s 线性变色 500ms
  步进精确中点/999ms 近终态/1000ms 到位/超步进不回退); timer/anim/
  perf 门禁复核通过(虚拟步进口径不受真实钟影响)。

### 2026-10-15 冻结指令片段轮(handwired 思想落地, 已实现+锚定)

- **L1 冻结绘制片段**: 静态子树 hn_cmd 序列按全量指纹缓存(FNV-1a:
  标签/属性/文本/计算后样式/布局盒/滚动/select_open), 命中帧 memcpy
  整块回放跳过 paint walk。失效: 指纹/继承偏移变化自动重录; 动画中/
  聚焦/动态元素(canvas/lottie/three/video/gif/progress/stream)不缓存;
  上限 256 条目 × 8192 cmd; HN_FRAGMENT=0 关闭。
- 正确性锚: 缓存开/关渲染**逐位一致**(showcase/three 几何/visibility
  复显页/控件页 cmp 通过); 动画子树自动走 miss(无动画回归)。
- 顺带修: `rt->range_drag/range_id` 曾落进 QuickJS #ifdef, 无 JS 构建
  编译失败(孤立复现工具撞上); hoist 到 struct 公共区。
- **canvas filter blur "0.39 饱和度"复核结案**: 非缺陷 —— 引擎
  getImageData 是非预乘 RGBA, RGB 直通, 浓度衰减在 alpha 通道
  (A 实测 0..253 单调)。dom_probe 两断言钉住(A 口径)。
- 设计文档 `docs/freeze-design.md`: L1 已实现 / L2 冻结布局 /
  L3 槽位 / L4 组件模板(模板类行为=冻结一次填逻辑)路线。

### 2026-10-15 Smart-UX 出现/消失动效轮(呈现方式优化)

"内容忽然出现/忽然消失"的呈现问题本轮收口(路线图 smart-ux 项):

- **动态插入默认入场**: 运行时 appendChild/insertBefore 的新节点自动
  **淡入 + 上浮 8px**(200ms, 继承作者 transition 缓动) —— 区别于首帧
  文档(即终态出现, 不拖慢首屏)与显式 animation 声明(优先级更高)。
  实现: 节点插入标记 `anim.fresh = 2`(区别解析产物 fresh=1), anim_walk
  消费 `c->enter_dynamic_default`(rt 首帧后经
  `hn_context_set_dynamic_enter` 置位)。
- **退出 ghost 淡出**: JS remove 的带盒子树推入 exit ghost(容量 8,
  环形丢弃), **原位**(最后布局盒位)以 alpha 1→0 淡出 160ms 后出列;
  paint_root 尾部统一补画(不参与命中/布局); anim tick 推进并在途时
  报 interactive(帧节拍正确)。换文档/compact 统一出列(节点指针随
  arena 悬空)。
- 探针: anim_probe 新增 D 组 6 断言(动态插入首帧 opacity<0.9 → 收敛 1;
  ghost t=0 在列表 → 半程 alpha<1 → 生命周期后消失)。
- 边界如实声明: ghost 只覆盖 JS remove 路径(display:none/visibility
  切换不淡出); select 弹层暂无渐显。

### 2026-10-17 窗口呈现动效轮(桌面即画板, 已实现)

"窗口突然弹出"的呈现问题收口 —— 窗口被**画**上桌面而不是弹出来:

- **入场渐显**: 开窗即隐(alpha 0, 无白闪), **首帧内容就绪后**淡入 +
  微缩放浮起(220ms, ease-out, 底边锚定 0.97→1.0)。实现于
  hnp_macos.c: `reveal_t` 状态机(开窗 -1 → reveal_begin 置 0 → 帧循环
  window_reveal_step 单调钟步进)。透明窗/桌宠跳过(自身 alpha 语义冲突)。
- **关闭淡出**: hnp_window_close_faded —— alpha 1→0(150ms)后真正
  释放; hn_app 退出循环 nanosleep 驱动至完成。
- **帧节拍联动**: 渐显/淡出在途时宿主帧节拍提到 16ms
  (`hnp_window_reveal_active` 查询) —— 静态页空闲 250ms 节拍下渐显
  不变幻灯片。
- **Windows hnwin 同步接入**: 全程 WS_EX_LAYERED + 开窗即隐(alpha 0) →
  首帧后 16ms WM_TIMER 步进 SetLayeredWindowAttributes 淡入(220ms);
  WM_CLOSE 拦截 → 淡出 150ms → DestroyWindow。Linux/headless 空桩
  (接口齐全, 上轮); 三平台窗口呈现口径一致。
- 顺带修: 早前中断的构建留下 0 字节 hnapp-macos-arm64(检窗先看大小);
  hnp_linux/hnp_headless 的 reveal 桩曾被插进函数体中部(正则锚定失误)。
- 视觉验收: HN_REVEAL_TRACE 实测 alpha 斜坡
  0→0.79→0.92→0.96→0.99→1.00(ease-out)。

### 2026-10-17 JS 面精简裁决(用户裁决: 精简保留)

- **保留**(轻能力, H5 页面常用): 属性读写四件套、classList、dataset、
  style 代理、getBoundingClientRect/offset 四件套、checked/disabled/hidden、
  textContent/value、focus/dispatchEvent/click、width/height 属性写、
  getElementById(null 语义)、addEventListener(once)。
- **下线**(偏浏览器重能力): querySelector/querySelectorAll、
  createElement、insertBefore/replaceChild、parentElement/children/
  first·lastElementChild、scrollTop/scrollLeft/scrollIntoView。
  dom_probe 同步裁剪(53→36 断言); video-player 示例改 getElementById。
- 裁剪过程中的教训: 对 800 行 C 字符串胶水做**行级手术**极易切碎多行
  语句 —— 最终以 node --check 验证整块重写(提取→语法校验→回填)收口。
- canvas.width/height 动态改尺寸(hn_canvas_sync_element_size)、
  ctx.filter/letterSpacing/reset 等与本裁决无关, 均保留。

- GPU multiply/screen 在半透明目标上与 hnsoft 结构性不等价(契约已声明; 直呈 v1 限不透明窗, 该域逐位等价由探针锚定)。
- neb3 原生纹理直入合成器(免 CPU 回读)为下一阶段: 需要 wgpu↔Metal 设备互操作与跨队列同步设计, 计划独立推进。
- ~~hn_find_hn 回退 `/tmp/hncore` 可能选中陈旧二进制~~ **已修**(2026-10-15): 缺省优先 `dist/hncore-<os>-<arch>`, 落 `/tmp` 时打带 mtime 的醒目告警(详见 ISSUES-1.md C1)。
- text-transform 仅 ASCII 范围(多字节 UTF-8 原样保留); aspect-ratio 按盒尺寸近似(未做 padding/border 参与的替换尺寸模型); overflow-x/-y 与 overflow 同轴(引擎单轴裁剪模型)—— 均为如实声明的能力边界, 非缺陷。

### 2026-10-15 H5 标准扩展轮(已修, 探针锚定)

- **arcTo 弧向判定**(glue): 叉积→ccw 判据在 y-down 屏幕坐标不成立, 直角画出 270° 长弧(roundRect 全坏)。改为"短弧 + 中点朝顶点"几何判据。此前无探针覆盖 arcTo。
- **globalAlpha 负 int32 下送**(glue): 位运算产生负数经 JSON 到引擎丢失 → alpha 混合完全不落像素。`>>>0` 归正。
- **glue 正则被 C 转义吃掉**(glue): `\s`/`\w` 在 C 字符串中退化为字面量(classList 全套静默失效)。glue 装载失败现打 `[canvas-glue]` stderr。
- **HTML5**: `<template>` 不渲染; `<button>`/`<input type=button|submit|reset>` 按钮控件(收缩+居中+缺省皮肤); 语义标签 UA 皮肤补全。
- **CSS**: text-transform / aspect-ratio / overflow-x / :root / :only-child。
- **JS DOM**: textContent get、属性四件套、classList、dataset、style 代理、checked/disabled/hidden、querySelector(All)(引擎 `hn_doc_query_selector`)、createElement/appendChild/remove、树关系、document.body/documentElement、getElementById null 语义。
- **Canvas**: rect/roundRect/getTransform/createImageData/isPointInPath/toDataURL/globalAlpha/lineJoin/miterLimit。
- **根背景画布传播**(浏览器标准): `hn_doc_canvas_background`; rt_raster/daemon shot/hncore render 三口同源; 透明窗不传播。
- 探针: 新增 `dom_probe` 38 断言; layout 40→43 / css 96→102 / html 37→43。

---

## 附: 存量简化(散落多处, 排期防漏)

- Cubism 混合观感精修(对照官方 Web runtime, 见 hn_cubism.c 头注)。
  ~~遮罩反转缺失~~ **已修**(2026-10-15): 反转 mask(csmIsInvertedMask, 常量
  标志)= 非零环绕的纯数据取反 —— 正绕矩形(盖 mask 包围盒)+ mask 三角
  全部归一反绕, 矩形内·mask 外才画; hnsoft(pip_poly)与 GPU
  (path_mask_raster)同规则两端一致, 退化包围盒回落普通 mask。
  `tools/cubism_mask_probe.{c,sh}`: 合成序列像素断言(反转/普通两组,
  normal+ASan 可跑)+ miku 真模型装载/结构自洽审计(miku 样例无 mask
  drawable, 夹具备注; 新引擎版本 `hn_cubism_inverted/mask_count` 读口)。
- ~~hn_three.c neb3 永久回退后其余缓存槽泄漏~~ **已修+回归锚定**(2026-10-15): 回退时整链释放全部槽(`t3_neb3_release_all_slots`, 触发槽先单放不双 free)。新增 `tools/neb3_fallback_probe.{c,sh}` + `tools/fake_neb3.c`(全符号替身 dylib, 对象级存活计数): 三场景(render_fail 4 槽全装配/create_fail/scene_fail)断言回退后 live==0 + 软件路径出图 + 永久性(state=2 不重入), normal+ASan 双跑。
- hn_three.c --advance 确定性对 hn-three 不成立(文档已修正口径)
- ~~CG 后端 hn_pe surface 尺寸失败残留旧尺寸(debug 构建触发断言)~~ **已修+回归锚定**: 修复在 fef4c63(失败路径先清零槽 w/h 并 memset, PX_SurfaceFree 由 w!=0 守卫)。新增 `tools/pe_surface_probe.{c,sh}`: PE 池 32MB 上界使超大 surface(4000x3500≈56MB)创建必然失败 —— 正常→失败→正常三段序列断言无残留复用(normal+ASan 双跑); SDK 缺席时 skip。
- GPU: 直呈/圆角 GPU 化/合成层缓存
- ~~描边环几何错位(边框幽灵化)+ 圆角裁剪硬边 + QUAD 三档边~~ **已修+回归锚定**(2026-10-17, 第二轮):
  复现页逐边实测锁定三处(用户"继续修锯齿"):
  ① paint_rect 描边环把带画在盒边**中心**(旧式 |d|∈[bw/2±0.5] + 内调制
  因子), 宽边框被压成 ~半透明幽灵环(实测 2px 边只剩 8% 覆盖, 3px 圆环
  只剩一个 44% 像素)且半透明环自身是硬边 —— 改为 CSS 语义环带
  [sd ∈ (-bw, 0)]: 覆盖 = clamp01(0.5-sd) - clamp01(0.5-sd-bw), 过渡带
  4-tap AA4(sd_ring/sd_ring_aa4);
  ② fb_clip_ok 圆角裁剪二值判定 —— overflow:hidden 圆角容器内容(头像)
  裁边 1px 硬台阶 —— 改 fb_clip_cov 覆盖率混合(fb_blend 乘 clip 覆盖,
  边缘像素 AA4; 直边/内部像素同费用), poly 裁剪仍二值(如实声明);
  ③ QUAD(3D 面片)边缘 0/0.5/1 三档 —— 改扫描线跨与像素域的解析重叠长。
  `render_probe` 新增 test_stroke_ring(3px 圆环全带 ≥70% + 内侧回落)+
  test_rounded_clip_aa(头像 45° 裁边过渡 ≥15%), 86→93 断言全绿;
  css/canvas/three/quad3d/perf/hidpi/media_window 全量回归无回归。
- ~~圆/圆角边缘 1px 台阶锯齿~~ **已修+回归锚定**(2026-10-17): 根因是
  paint_rect 对 SDF 覆盖率单像素单采样 —— 覆盖量化成 1px 台阶, 大圆/大
  圆角呈楼梯感(用户圆桌应用截图实证)。修复 `sd_rounded_aa4`(4-tap 旋转
  网格 RGSS 亚像素超采样, 5 级覆盖过渡), 仅边缘像素(cov∈(0.02,0.98))
  调用, 内部像素仍走 solid 快路, 开销与周长成正比。`render_probe` 圆边
  断言: border-radius:50% 圆周 3600 采样, 中间 G 通道占比断言 ∈[0.20,
  0.60](实测 0.265; 修复前单采样 ≈0)。GPU 端 MSAA 路径不受此影响。
- ~~macos_deep_probe ② 两呈现路径截图分歧(存量, 待专项)~~ **已消解**
  (2026-10-17): 根源是有标题栏窗口的 overlay 标题栏语义 —— 窗口默认
  无边框后两路径逐位一致, 探针转绿(见上文"窗口默认无标题栏"裁决)。
  原始实测记录:
  有标题栏窗口(static.html 360x360)在当前 macOS(darwin 27)上, 新直呈
  路径(drawRect)与旧 NSImage 回退路径(HN_BLIT_FALLBACK=1)窗口截图
  逐位对照失败 —— 直呈臂 1:1 忠实引擎位图(与 --shot 地面真值逐列吻合,
  顶部 20..28px 被系统标题栏覆盖, 疑似系统 overlay 标题栏语义);
  回退臂被 NSImageView 按视野比例缩放 0.92(200 宽红框实测 184, 垂直
  332/360 压缩+水平居中)。钉副屏/主屏都复现(主屏 diff_px=111359 更重),
  与当日改动(副屏钉位/AA4)无关 —— 探针 ② 本机历史绿是在旧 macOS 上。
  修复方向: 回退臂 NSImageView 设 NSImageScaleNone 1:1 起画, 并核实
  contentRect/frame 语义; 与"macOS Metal 真直呈"阶段同场收口。
- hn-drag / hn-dismiss / --ttl 到期自毁 / hn new / hn dev

---

## 待裁决

1. 网络层已落地为运行时系统 transport；后续可继续扩展代理、证书策略和更多响应头映射。
2. **窗口默认无标题栏(hn 语义裁决, 2026-10-17)**: hn 产出的窗口默认
   frameless(窗口即画布, 标题栏是例外) —— `hn_doc_manifest` frameless
   缺省改 1, `hn-frameless="0"` 显式要系统标题栏。三平台对齐: macOS
   style=0 / Linux _MOTIF_WM_HINTS(原有) + **hnwin 补齐**(此前从不读
   frameless, 恒 WS_OVERLAPPEDWINDOW —— 现按声明 WS_POPUP, 并补
   WM_NCHITTEST 背景拖拽: 非交互内容=HTCAPTION, hx 载体/输入控件/
   手型光标/可滚动区=HTCLIENT, 与 macOS movableByWindowBackground 同
   观感)。**副产品**: macos_deep_probe ② "两呈现路径截图分歧(存量待
   专项)"随之消解 —— 分歧根源是有标题栏窗口的 overlay 标题栏语义,
   默认无边框后 drawRect 直呈与 NSImage 回退逐位一致, 该探针转绿。
3. 收尾清理轮(2026-10-17): ① **回归修**: fb_clip_cov 的 ±0.5px 外扩早出框
   误用于直角裁剪 —— clip 外缘渗出半像素且与 blend_rgba 快路径谓词不一致;
   现外扩只属于圆角羽化带, 直角走严格 bbox(与快路径逐位同式)。
   ② clearRect 裁剪边缘从"任一覆盖即全填"改**分数覆盖回填**(SOURCE 语义
   乘 cov), 与填充同口径。 ③ 死代码: fb_clip_ok 包装(无调用者)、
   hn_rt node_id_of(无调用者)、hn_paint lottie_player_bg 未用变量删除。
   ④ 警告清零: hnsoft.h/hnsoft.c/hn_platform.h 嵌套 `/*` 注释、hn_rt
   13 处匿名参数补名(QuickJS `_` 惯例) —— hnsoft/hn_context/hn_paint/
   hn_rt 四核心 -Wall -Wunused -Wcomment 全 0。 ⑤ 残缺补全: render_probe/
   mesh_probe/sysbridge_probe 三个有断言无运行器的探针补齐 tools/*.sh
   薄封装(此前只能手工编译), 全量 23 探针族绿。
2. 桌面画板呈现(画刷铺色)已落地(2026-10-17): "内容像颜料直接画上桌面"。
   **形态语义(用户两轮裁决)**: 画上去 = **逐像素透明窗**专属 —— 未铺区域
   alpha=0 直接透出桌面; 不透明窗(标题栏/无边框不透明)会先出现一块
   "画板"再画内容(第一轮误做, 用户指出), 缺省只走平台淡入; hn-reveal=
   none 一律硬出现。实现: 遮罩数学单源 `hnsoft_paint_mask_cov`(对角轴
   + 20% 羽化笔锋 + 波纹摆边, 纯函数); rt 层生命周期 `hn_rt_paint_reveal_*`
   (hn_app 接线, blit 前盖遮罩) + hnwin 同款(WM_TIMER 步进)。
   **关键修复**: 静态页壳只强制 blit 不重光栅 → 遮罩原地叠加, 几十拍把
   内容乘没(不透明页收敛成纯画板/透明页全透明) —— `raster_gen` 帧代数
   守卫, apply 遇同代数先内部重出干净帧。观测口径: `HN_REVEAL=paint|
   fade|none`(风格覆盖), `HN_REVEAL_MS`(时长, 缺省 480ms), `HN_REVEAL_
   TRACE=1`(铺色推进)。anim_probe E 节 15 断言(端点/单调/羽化带/透明
   积分/两类不 eligible 负断言/raster_gen 守卫幂等/收口无残留)。
   **审查轮加固**(同日): ① 收口缺帧 —— active 翻 false 后不再 blit,
   窗口停在 u≈0.99 遮罩帧(透明窗有半透明残影), hn_app/hnwin 补"收口拍
   干净帧"(实测收口帧与 --shot 干净帧逐像素一致 97332/97332); ② 守卫
   重出改 ambient 轻路径(repaint_light + raster, 免 relayout 4 倍浪费,
   且 rt_raster 不查 paint_defer, 挂起窗口无 defer 洞; 重出失败不盖旧
   帧); ③ 遮罩波纹免三角快判 —— ripple 有界 ±1.5%L, 投影距离出带即
   判满/空(与精确式严格一致), 只有笔锋带内算 sinf(2x 大窗 ~10ms →
   ~1-2ms/拍)。
2. 测试显示纪律(用户裁决): 真窗测试一律钉**副屏** —— `HN_WINDOW_POS`
   环境钮(app/hn_app.c, 优先级盖过 manifest 与几何记忆, 且不写几何记忆)
   + `tools/_secondary_screen.sh`(NSScreen 枚举, 单屏输出空回落居中)。
   已接线: perf/hidpi/media_window/macos_deep 四探针; 后续新增真窗探针
   照此办理, 不再占开发者主屏。
