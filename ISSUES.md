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

### 已知问题(仍开放)

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

- Cubism 遮罩/混合观感精修(对照官方 Web runtime, 见 hn_cubism.c 头注)
- hn_three.c neb3 永久回退后其余缓存槽泄漏
- hn_three.c --advance 确定性对 hn-three 不成立(文档已修正口径)
- CG 后端 hn_pe surface 尺寸失败残留旧尺寸(debug 构建触发断言)
- GPU: 直呈/圆角 GPU 化/合成层缓存
- hn-drag / hn-dismiss / --ttl 到期自毁 / hn new / hn dev

---

## 待裁决

1. 网络层已落地为运行时系统 transport；后续可继续扩展代理、证书策略和更多响应头映射。
