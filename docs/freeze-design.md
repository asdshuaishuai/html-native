# 冻结指令片段(freeze)设计 —— handwired 思想的 hn 落地

> 提议: 把 HTML/CSS 组件**冻结成现成的引擎指令**, 之后直接驱动指令;
> 模板类行为 = 冻结一次, 填逻辑即可。
> 结论: **能搞, 且已在 hn 的架构上找到天然落点** —— hn 每帧产出的
> 显示列表(hn_cmd 数组)就是"运算列表", 缺的只是跨帧保留 + 参数化。
> 本文按 L1..L4 分级给出路线; L1 已实现(hn_paint.c "冻结指令片段")。

## 一、与 handwired 的对应

| handwired | hn 冻结 |
|---|---|
| JS 编译成 FP32 运算列表(冻结) | HTML/CSS 子树编译成 hn_cmd 片段(冻结) |
| cell 输入(事件位/状态) | 槽位: 文本/样式值/结构(v2) |
| 每帧全量求值, 零分支零循环 | 命中帧整块 memcpy 回放, 跳过 paint walk |
| 程序逐位确定 | 缓存开/关渲染逐位一致(cmp 已验) |

## 二、L1 冻结绘制片段(已实现)

### 命中条件(全部满足才回放)
- 子树指纹一致: FNV-1a 全量哈希 —— 每节点的 kind/标签/属性/文本字节/
  **计算后样式**(含 hover/继承带来的变化)/布局盒/滚动偏移/select_open,
  顺序敏感(子级顺序变化 = 不同指纹)。
- 继承偏移一致: 冻结时传入 paint_walk 的 sx 与当前帧相同
  (祖先滚动/位移变化 → 自动失效, 内容滚动正确)。
- 祖先无动画进行中(g->anc_anim == 0): 祖先每帧数值变, 片段必过期。
- 排除: 动画中(slot_active/entering)、聚焦元素(焦点环/caret)、
  动态元素(canvas/video/audio/progress/select 自身、hn-lottie/three/
  cubism/mesh/live/stream/gif)。

### 生命周期
- 登记: 首次绘制时窗口 [start, n_cmds) 深拷贝入片段(指针字段
  text/poly/segs/verts/uv 深拷贝归片段所有)。
- 失效: 指纹不一致 → 释放旧 cmds → 按当帧重录。文档替换/compact/
  context 销毁 → hn_paint_frag_clear 全清(hn_canvas_dispose 同点挂钩)。
- 上限: 256 条目 / 每条 8192 cmd / 子树 4096 节点 / 深 64 —— 超限回退
  正常路径。HN_FRAGMENT=0 全局关闭; HN_FRAGMENT_TRACE=1 打命中统计。

### 正确性锚
- 缓存开/关渲染**逐位一致**: showcase/three 几何页/visibility 复显页/
  控件页(details/checkbox/radio/range/select/progress)全部 cmp 通过。
- 动画页(three 球)自动走 miss 路径(动画中不缓存) → 无动画回归。

### 已知边界(v1)
- 溢出滚动容器内部的子树: 滚动改变继承偏移 → 每帧失效(正确但无加速);
  滚动条内容冻结是 L2 课题。
- 焦点/悬停高频切换的子树: 指纹含计算样式 → 正确失效(无加速, 无错误)。

## 三、L2 冻结布局(前置 + 本体第一刀已落地)

现状: 每帧全文档级联(仍全量)+ 布局(已收敛到脏分支)。L2 = 脏子树 relayout:
- 样式差分只在属性/文本变化处标脏; 布局只重排脏子树(盒不变则跳)。
- 与 L1 组合: 静态部分零成本, 动态部分局部成本。
- ✅ 前置(2026-10-21): **文本测量缓存**(hn_layout.c
  `hn_layout_mcache_*`)。measure(字体五元组 size_px/weight/italic/
  letter_spacing/family + 文本字节 → 宽度)是纯函数, 在 `measure_run`
  单一咽喉点做记忆化 —— 每上下文一份, 每遍 `hn_layout_root` 入口 sync
  文本后端(换人即全清); 键逐字段逐位比较, 命中浮点与重测严格相等。
  纪律: 条目上限 16384, 满即整表清空(不做 LRU);
  `HN_MEASURE_CACHE=0` 关闭, `HN_MEASURE_TRACE=1` 每遍打命中统计。
- ✅ 本体第一刀(2026-10-21): **子树布局记忆**(hn_layout.c lskip 块)。
  每节点存子树 Merkle 键: 自身输入哈希(FNV-1a: kind/tag/attrs/文本
  字节/计算样式字节)⊕ 子级当前键 —— **每遍布局前自底向上整树刷新**
  (O(N) 纯哈希, 级联本就全量的廉价伴跑)。刷新保证键恒反映当前内容:
  外部突变(set_text/attr/append/remove 与 anim_tick 的样式写)当帧即
  被父层感知 —— 教训: 首版按"父折子级已存键"增量维护, 子级未被重排
  前键是旧的, 父层误命中吞掉突变(anim_probe ghost 断言与最小复现
  实测后改整树刷新, 代价是每帧 ~0.1ms 级哈希)。键**不含盒/原点**
  (盒是输出)。`layout_box` 入口 O(1): lf_valid && 记录键==当前键 &&
  布局输入(可用宽高/强制宽高/度量模式)逐项相等 → 整子树跳过重排
  (runs/盒/content_h 原样保留); 原点不同只 `translate_subtree`
  (排版对绝对原点平移不变)。未中 → 正常布局后记录几何与当时的键。
  效果(96 段中英混排文档): 改一处文本 = 脏链 4 次重排 + 后方兄弟
  90 次平移回放; 纯布局 300 遍 bench(含刷新与级联的地板成本):
  全同输入 2.57x / 单点流式变更 2.48x / 逐遍改宽 1.55x(最坏情形:
  键全失配只付哈希 —— 如实报告)。
  正确性边界(bail, 不缓存不跳过, 向祖先传播): 替换元素与输入控件
  (img/video/audio/canvas/progress/select/button/input/textarea ——
  layout 期把解析尺寸写回计算样式, 尺寸链依赖图片后端/媒体会话等
  外部状态); 脱离流(position:absolute/fixed —— 摆放依赖子树外的
  祖先盒); 动态表现型(hn-lottie/three/cubism/mesh/live/stream/gif,
  与 L1 同清单)。display:none 子树照常刷新键(隐藏期变更可追踪),
  显形帧整树重排。
  runs 清理从"根清"(clear_runs(root))改"谁重建谁清"(IFC 落盘点
  `ifc_wipe` + 文本/控件终排自带清) —— 命中子树 runs 必须原样保留。
  正确性锚(`tools/freeze_l2_probe.sh`): 命中路径(同输入双遍)按位
  cmp 逐字节一致; 突变序列(含平移回放)与全新布局存在 ~1ulp 浮点
  结合序差(`旧值+dy` ≠ 新原点重排, 数学等价、像素不可见), 按
  ≤0.01px 容差逐 token 对照 + 命中/平移断言。
  `HN_LAYOUT_SKIP=0` 关闭; `HN_LAYOUT_SKIP_TRACE=1` 每遍布局出口打
  hit/moved/miss/bail 统计。
- 余下排期: ① 级联差分(样式重算收敛到脏链; Merkle 键已把脏检测
  内容寻址化, 剩下的是级联本身的全量成本); ② 替换元素外部输入
  版本号(图片固有尺寸/媒体 meta 变更可感知后 bail 面收敛, 含图
  卡片也能跳); ③ 平移回放逐位化(runs 相对原点存储) —— 当前
  ~1ulp 漂移如实声明; ④ 前置余下: runs 结构缓存(从缓存盒恢复)。

## 四、L3/L4 组件模板(排期)

- L3: 槽位化 —— 组件模板声明命名槽(文本槽/样式槽), 实例只写槽值,
  片段回放时打补丁(不用整段失效)。
- L4: `<template>`/注册组件 API —— register(name, html/css),
  createElement(name) 克隆进文档; 每实例 = 槽位表 + 冻结程序引用。
  这就是"模板类行为直接填逻辑"的最终形态; hn 的 include(静态)与
  DOM API(动态)已备好两侧。

## 五、度量

- 正确性: 上表逐位一致锚 + events/dom probe 全绿。
- 性能: 组件重模板页(百卡级)paint walk 显著下降; 量化基准排期
  (perf_probe 扩展一页"冻结命中率+帧时间")。
- L2 已有锚: `tools/freeze_l2_probe.sh`(测量缓存 + 子树布局记忆两层:
  命中路径逐位 cmp + 突变序列 ≤0.01px 容差对照 + 命中/平移断言 +
  三负载计时, 2026-10-21)。
