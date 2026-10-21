# Neb 架构文档(html-native 项目文档)

> 本文是引擎的**架构与运行时行为**参考: 一帧怎么走、状态放在哪、缓存
> 何时失效。上手教程见 [getting-started.md](getting-started.md), 完整
> 使用参考见 [manual.md](manual.md), 路线图见 [roadmap.md](roadmap.md)。

---

## 1. 分层总览

```
┌────────────────────────────────────────────────────────────┐
│ 宿主层: hn-daemon(常驻, socket JSON-lines) / hn CLI /      │
│         任何语言的 socket 客户端                             │
├────────────────────────────────────────────────────────────┤
│ 应用层: app/hn_app.c 唯一主循环(帧循环/呈现动效/几何记忆)   │
│         + hn_media_platform.c(媒体宿主装配)                 │
│         + hn_media_synth.c(合成宿主, 确定性测试替身)        │
├────────────────────────────────────────────────────────────┤
│ 公共运行时 rt/hn_rt.c: 文档生命周期 · hx-* 执行 · sys:// 桥 │
│         · QuickJS(可选) · 定时器/RAF · 事件监听面 · GPU 槽  │
├────────────────────────────────────────────────────────────┤
│ 引擎核心 Sources/CHtmlNative(C99, 零依赖, 零平台):          │
│   解析 → 级联 → 布局 → 绘制指令(display list) → 动画时钟    │
├────────────────────────────────────────────────────────────┤
│ 平台层 platform/hnp_*.c: 每平台一个薄胶水(运行期 dlopen,    │
│         编译期零 SDK): 窗口/blit/事件/时钟/光标/媒体/GPU     │
├────────────────────────────────────────────────────────────┤
│ 光栅后端: hnsoft(默认, 自研) · cairo(可选) · Metal(GPU)    │
└────────────────────────────────────────────────────────────┘
```

三条铁律:

1. **引擎不含平台/UI/网络**。`hx-*` 语义在 rt, transport 可注入;
   网络只在运行时层(`hn.fetch`/hx-get), 引擎核心零 I/O。
2. **平台差异收敛为契约**。`platform/hn_platform.h` ~15 函数 + GPU
   原语表, 每平台一个 `hnp_*.c` 实现; 换平台 = 换链接的 hnp 文件。
3. **显示列表是唯一交付物**。引擎产出 14 种绘制指令
   (`HN_CMD_*`, 见 include/hn.h), 三后端(hnsoft/cairo/Metal)消费
   同一份列表; GPU 覆盖不了的指令整帧回退 hnsoft。

## 2. 一帧的流水线(daemon / hn_app 同构)

```
事件/帧钟 → hn_rt_frame(rt, dt)
  ├─ hn_context_layout(c, w, h, &text_backend)      ── 全路径重排
  │    ① hn_style_compute_all      级联(含级联记忆化, §4.3)
  │    ② details_apply_state       <details> 折叠态(display 改写)
  │    ③ hn_layout_root            布局(含子树布局记忆, §4.2)
  │       └ 前置: ls_refresh       整树 Merkle 键刷新(O(N) 纯哈希)
  │    ④ hn_paint_root             显示列表重建(含冻结片段回放, §4.1)
  │    ⑤ hn-stream 贴底/让出锚定; 必要时第二遍 layout_pass
  ├─ hn_context_anim_tick(dt)       过渡/@keyframes/Lottie/网格/ghost
  │    └ 写样式字段的动画 → 下一帧走全路径; ambient 动画走轻路径
  │      (hn_context_repaint_light: 只重建显示列表, 不重排)
  └─ blit/呈现(平台层)
```

帧时间戳纪律(handwired host-timing): 帧钟与事件钟先量化到 **1/64ms
栅格**再做差(dt), 任意拆分的时间跨度相加严格相等 —— 动画时钟跨帧率
(10/30/60/120fps)重放出同一画面; 事件在**到达时刻**应用于动画钟
(`rt_events_advance`), 过渡目标在事件后立即启动。详见
[handwired-notes.md](handwired-notes.md)。

## 3. 数据与内存纪律

- **文档 arena**: DOM 节点/文本字节全部从文档 arena 分配, 无 free;
  文档替换(新 doc/compact)= 整个 arena 释放, 节点指针随之失效 ——
  所有跨帧缓存(GIF/Lottie/片段/媒体会话)在 set_doc/compact 统一清空。
- **tmp arena**: 每遍 `layout_pass` 销毁重建; 指令引用的顶点/文本副本/
  CSS 变量数组从这里分配, 生命周期 = 本帧显示列表。
- **runs**(行内排版结果)挂在节点上、跨帧存活; 由"谁重建谁清"的
  **wipe-once 帧代数**(`g_lgen`/`runs_wipe`)管理 —— 同帧多次清理是
  无操作(多行内组容器的省略号 runs 组间不互毁), 跳过子树的 runs
  原样保留(子树布局记忆的前提)。
- **缓存四件套**(上下文级, 各带失效纪律):
  | 缓存 | 失效 |
  |---|---|
  | Lottie/GIF 解码缓存 | 路径键; 文档替换/compact/换资产后端清 |
  | 冻结指令片段(L1) | 子树指纹; 文档替换/compact 清 |
  | 文本测量缓存(L2 前置) | 键=字体五元组+文本字节; 文本后端换人清 |
  | 级联记忆化 | 输入哈希; 表集 serial/视口/属性/父样式变化自动失配 |

## 4. 冻结三层(性能架构, handwired 思想落地)

> 设计与分级路线: [freeze-design.md](freeze-design.md)。三层独立开关,
> 命中语义都是"逐位一致"或"≤1ulp 平移等价"(探针锚定)。

### 4.1 L1 冻结指令片段(paint 层, `HN_FRAGMENT`)

静态子树首次绘制时把 hn_cmd 序列深拷贝入片段缓存; 之后帧按子树全量
指纹(FNV-1a: 标签/属性/文本/计算后样式/布局盒/滚动)命中即整块
memcpy 回放, 跳过 paint walk。动画中/聚焦/动态元素(canvas/video/
select/...)不缓存; 文档替换全清。

### 4.2 L2 子树布局记忆(layout 层, `HN_LAYOUT_SKIP`)

每节点存子树 **Merkle 键**(自身输入哈希 ⊕ 子级当前键; 不含盒/原点),
每遍布局前整树刷新(O(N) 纯哈希)。`layout_box` 入口: 记录键==当前键
且布局输入(可用宽高/强制宽高/度量模式)逐项相等 → 整子树跳过重排;
原点变了只 `translate_subtree` 平移回放。替换元素/输入控件/脱离流
bail(向祖先传播)。**教训入档**: 键必须每帧从当前内容刷新 —— 首版
按"子级已存键"增量维护, 外部突变在子级重排前不可见, 父层误命中吞掉
突变(anim_probe ghost + 最小复现实测)。

### 4.3 L2 前置 文本测量缓存 + 级联记忆化(`HN_MEASURE_CACHE` / `HN_STYLE_MEMO`)

- **文本测量缓存**: measure(字体五元组+文本字节→宽度)在 `measure_run`
  单点记忆化; 文本后端换人(memcmp)整表清空; 上限 16384 条满即清。
- **级联记忆化**: 输入哈希覆盖选择器可见的全部状态(自身 tag/属性/
  伪类位 + 祖先选择器上下文传播 + 兄弟前缀与全序哈希 + 父样式哈希 +
  表集 serial/视口/透明开关), 命中即逐位复用上帧计算样式, 规则匹配
  整段跳过; 动画进行中/替换元素强制重算。`style_hash`(样式字节内容
  哈希)是"父样式哈希"输入与后续增量级联的接驳点。

### 4.4 效果口径(96 段中英混排文档, 300 遍纯布局)

| 负载 | 全关 | 三层全开 | 提速 |
|---|---|---|---|
| 全同输入(ambient/重复重排) | 210ms | 46ms | **4.5x** |
| 单点文本流式变更 | 210ms | 48ms | **4.3x** |
| 逐遍改宽(最坏情形) | 210ms | 147ms | 1.4x |

改一处文本 = 脏链重排 + 后方兄弟平移回放 + 其余子树命中。

## 5. 事件与交互

- **管道**: 平台层原始事件 → 命中测试(`hn_context` 命中链) → 带 id
  元素冒泡 → 消费(hx-trigger / JS addEventListener / select 弹层)。
  种类: mouse 系/keyboard/focus/scroll/input-change/submit/hover +
  launch/show/hide/destroy 生命周期。
- **hx-\***(htmx 风格, rt 层解释): `hx-get/post/trigger/every` —
  click/load/定时/回车提交/表单收集; transport 注入(进程内/`sys://`)。
- **JS 面**(QuickJS, 可选): `addEventListener`(命中链冒泡+标准事件
  对象)、DOM 标准(querySelector/classList/dataset/createElement...)、
  `hn.fetch`(Promise, 白名单 `HN_FETCH_ALLOW`)、setTimeout/RAF、
  Canvas 2D(`hnCanvas2D`)、`hnSetText/hnGet2D` 等小桥。完整清单见
  [manual.md](manual.md) §JS。
- **Smart-UX**: 进场/退场默认动效(淡入上浮/原位淡出 ghost)、呈现动画
  (画刷铺色/淡入)、`prefers` 无关的确定性动效时钟 —— 语义在引擎,
  壳只 blit。

## 6. GPU 路径

`hn_gpu.c` 把显示列表翻译为 `hnp_gpu_host` 原语批(四边形/纹理/裁剪/
readback); `hnp_gpu_metal.c` 运行期 dlopen Metal + MSL 运行期编译。
`supported_cmds` 位集判定, 覆盖不了的指令**整帧回退 hnsoft**(静默,
零漂移策略)。媒体(`video`)不经显示列表: AVPlayerLayer(macOS)/Xv
(Linux) 原生图层直通。`HN_GPU=metal` 开启。已知边界见 ISSUES.md
(rect 页 parity 回归, 待 Metal 专项)。

## 7. 测试与确定性

- **探针 42 族**(`tools/*_probe.sh`, ~1300+ 断言): 每族一个主题,
  断言失败即退出非零; 构建门禁 = 全量探针绿。
- **确定性锚**: 同一份 hn 编码在 macOS/Linux/Windows 交叉产物上布局
  输出逐字节一致; 探针普遍用"机制开/关双跑对照"(逐位 cmp 或 ≤1ulp
  容差)钉住优化不改变语义。
- **freeze_l2_probe**: 冻结三层的专门锚(命中路径按位 cmp + 突变序列
  容差 + 命中/平移断言 + 三负载计时)。
- **门禁纪律**: 改引擎必跑全量探针; dist 二进制可能是陈旧构建 ——
  探针优先用 `dist/hncore-<os>-<arch>`(C1 裁决: 不盲信 /tmp 副本)。
