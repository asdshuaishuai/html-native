# anime.js v4 研究笔记 —— 对 Neb 动画面的提炼与映射

> 研究对象: https://animejs.com (v4, 2026-10 快照; v3.2.2 为最后的 UMD 版)
> 记录: 2026-10-23 · 口径同 handwired-notes: ✅ 已落地 · 📋 可采纳(排期) · ❌ 不采纳(理由)
> 结论先行: **运行时不引**(v4 是 ESM + DOM 写入面, 与引擎自有动画时钟双驱动冲突);
> **语义/原语采纳**——stagger、物理弹簧积分器、timeline 编排、JS API 形状,
> 归入 hn-anim 路线(freeze-design §9.3 的动效语言同族)。

## 一、anime.js v4 是什么

模块化 JS 动画库(全量 24.5KB, 按模块 tree-shake): Timer 引擎(5.6KB)
+ Animation(+5.2KB) + Timeline(+0.55KB) + Spring(+0.52KB) + Stagger
(+0.48KB) + Animatable(+0.4KB) + Draggable(+6.4KB)/Scroll(+4.3KB)/
Scope/SVG/WAAPI(浏览器面)。全局 Engine 管时钟(timeUnit/speed/fps/
precision/pauseOnDocumentHidden), Timer/Animation 挂其上按帧推进。

核心 API:
- `animate(targets, params)` — targets = CSS 选择器/元素/**JS 对象**;
  可动画属性 = CSS 属性/transform/CSS 变量/JS 对象属性/SVG 属性;
  值类型 = 数值/单位换算/相对(+=)/颜色/函数值; 播放设置 = duration/
  delay/loop/loopDelay/alternate/reversed/frameRate/playbackRate;
  回调 = onBegin/onUpdate/onComplete/onLoop...(+then)。
- `createTimeline()` — 多动画顺序编排(.add 逐段)。
- `createSpring()` — stiffness/damping 物理弹簧缓动。
- `stagger()` — 时间/数值/位置错档(列表动画的核心原语)。
- `createDraggable()/onScroll()/createScope()` — 拖拽/滚动同步/
  响应式作用域(浏览器交互面)。
- 缓动: 内置预设族 + cubic-bezier + linear + steps + irregular + spring。

## 二、执行模型(对 Neb 的启示)

| anime.js | Neb 现状 | 判定 |
|---|---|---|
| 全局 Engine 时钟 + 每帧 update | `hn_context_anim_tick(dt)`(帧驱动, handwired 1/64ms 栅格) | 同构 ✓ |
| Timer(delay/duration/loop/alternate/reversed/frameRate) | @keyframes 状态机(kf_clock/kf_done) + 过渡(ms/delay) | 部分同构: alternate/reversed/frameRate 缺 |
| 缓动 = 函数(进度→值) | HN_EASE_*: linear/css/in/out/in-out/**spring/bounce/elastic** + cubic-bezier(cb[4]) | 缺省族已对齐; spring 是闭式 ease 非物理积分 |
| per-property 槽位推进 | hn_anim per_prop + slot_active/t[HN_TPROP_N] | 同构 ✓(样式热换立即生效口径一致) |
| stagger(时间/数值错档) | 无 | 📋 缺口 |
| timeline(跨元素顺序编排) | @keyframes 单元素; 无跨元素原语 | 📋 缺口 |
| JS 对象属性动画 | 无(引擎动画只作用样式/几何) | 📋 可经 QuickJS 薄桥补 |

## 三、直接跑 anime.js(QuickJS)的可行性

- **v4 仅 ESM**: QuickJS 支持模块装载, 但要为每应用铺模块解析面;
  v3.2.2(UMD/全局 `anime`)技术面更近, 但已非主线。
- **运行时面**: 核心需要 rAF(引擎有)、performance.now(rt_mono_q 同款)、
  querySelectorAll(有)、style 写入(代理有)、getComputedStyle(部分)、
  **每帧字符串构建 transform + 写回**(与引擎 C 管线的交互帧路径重复
  —— 引擎自己的过渡已在 C 内逐帧插值, anime.js 叠加 = 双驱动时钟冲突)。
- **性能形态**: 每帧 JS 字符串拼 transform → 解析进样式表 → 全量重算,
  对 C99 引擎的显示列表管线是倒退(handwired 笔记同判: 运行时面在
  QuickJS 跑不动的部分恰是它的核心价值面)。
- **裁决: ❌ 不引运行时**。与 htmx 同一模式 —— **语义/词表/参数形状
  采纳, 执行在引擎原生层**。

## 四、📋 可采纳(排期, 按 hn-anim 路线归并)

1. ✅ **stagger 原语**(2026-10-23 首项落地): 容器属性
   `hn-stagger="每项ms"`, 子项入场按"前序同批入场兄弟数"负进度起步
   (与 animation-delay 同机制, t<0 停在起始态); 同批 = 前序兄弟
   anim.entering 仍在途 —— 聊天流单条追加恒 idx=0 不受影响, 批量插入
   依次浮现; 显式入场路径与 Smart-UX 默认入场双路生效。anim_probe
   断言: 错档序 o0>o1>o2 / 收敛全 1 / 无 stagger 零回归 / stagger=0。
2. **物理弹簧积分器**(stiffness/damping): handwired 笔记已挂路线图;
   现行 `spring` 是闭式 ease —— 参数化物理弹簧是它的真补齐, 参数形状
   对齐 anime spring(stiffness/damping/velocity)。
3. **timeline 编排原语**: ✅ 第一刀落地(2026-10-23): 容器属性
   `hn-timeline`(loop/alternate/reversed/interval), 后代入场声明为步骤,
   全落定重播/反向呼吸, 退役派 `hn-timeline:complete`(核心事件出口
   `hn_context_set_event_sink` → rt 接 JS 派发, 兼作 A4 onComplete 通路)。
   剩 JS 桥 createTimeline 形状对齐(归 A4)。
4. **`hn.animate(targets, params)` 薄桥**(QuickJS): ✅ 第一刀落地
   (2026-10-23): EVENT_GLUE 纯 JS —— targets(id/元素/数组), params
   {属性: 目标|[from,to], duration/delay/stagger/ease/onComplete};
   执行走引擎自有过渡(写 transition + 目标值, JS 零每帧驱动);
   onComplete 走引擎定时器。会 anime.js 的 agent 零学习成本。
   第二刀(同日): loop=N|'infinite' + alternate/reversed —— 重播链走
   引擎定时器(轮间异向单写/正向两写重置), loop 语义需 [from,to] 数组
   形态; 剩选择器面(桥 DOM 面 id 键控, 如实声明)。
5. **alternate/reversed/frameRate 播放设置**: @keyframes 状态机补齐
   (现有 loop/kf_done 之上)。

## 五、❌ 不采纳

- **运行时引入**(v4 ESM / v3 UMD): 见 §三 —— 双驱动时钟冲突 + DOM
  写入面 + 每帧字符串管线的性能倒退。
- **WAAPI/SVG 模块**: WAAPI 是浏览器 API 面; SVG Neb 不支持(画像面
  之外), 两者无映射。
- **Draggable/Scroll/Scope**: 拖拽已有 hn-drag 路线; 滚动同步与响应式
  作用域是引擎更基础的缺口(滚动容器语义), 不借道动画库补。
- **CSS 变量动画**: 引擎无 CSS 变量面(路线图另册)。

## 六、与既有工作的关系

- freeze-design §9.3 Smart-UX 动效语言: stagger/timeline 是其"进场/
  退场动效语言"的原语级补齐 —— agent 生成列表页的错档入场是高频面。
- handwired-notes: 物理弹簧积分器 = 该文"settle 三件套"路线的参数化
  形态; 1/64ms 栅格时钟是两者共用的时间基座。
- htmx parity: 同一裁决模式 —— htmx 采纳协议词表, anime 采纳动画
  原语词表; 执行都在引擎原生层, JS 面只是形状桥。
