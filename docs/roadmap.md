# html-native 路线图：从玩具到 OS-PWA 级框架

> 版本: 1.0 · 日期: 2026-09-17 · 状态: 规划中
>
> 本文档是 html-native 从当前原型(v0.1)演进到生产可用框架的
> 深度规划。包含差距分析、竞品对标、分阶段路线图与架构演进。

---

## 1. 当前状态（诚实审计）

### 1.1 有什么（已验证）

| 层 | 代码量 | 能力 |
|---|---|---|
| C99 引擎 | 6800+ 行 | HTML 解析 · CSS 级联 · flex/行内布局 · 绘制指令(8 种) · 动画插值 · Lottie 求值 · 网格变形 · PNG 解码 · 结构自检 |
| macOS 运行时 | ~4100 行 Swift | CoreGraphics 落屏 · 图层合成(网格变形) · 窗口/弹窗/图层物化 · FreeType · JavaScriptCore · sys:// 桥 · hx-* · 流式视图 |
| 软件光栅器 | ~1400 行 C | SDF 圆角矩形 · 线性渐变 · 多边形扫描线 · 网格纹理映射 · **PNG 解码(自带 inflate)** · FreeType 文本 · PNG 编码 |
| JS 运行时 | ~400 行 Swift | JavaScriptCore · DOM 桥 · 事件派发 |
| 测试 | 1700+ 行 | 202 项断言 |
| 示例 | 14 个 | 仪表盘 / 网页写法 / 轨迹面板 / 消息卡 / 输入控件 / Lottie / 透明背板 |

### 1.2 没有什么（差距，按严重度排列）

#### 🔴 阻断级（缺了就无法构建真实应用）

| 缺口 | 影响 | 涉及层 |
|---|---|---|
| `position: absolute/fixed` | 无法做弹窗/下拉/浮层/遮罩/工具提示/Header 固定 | 引擎(C) + 绘制 |
| `z-index` | 无法控制层叠顺序 | 引擎(C) |
| `top/right/bottom/left` | 配合 position 使用 | 引擎(C) |
| **增量渲染** | 每次变化触发全量重排——大文档不可用 | 引擎(C) |
| ~~**真实事件系统**~~ ✅ | 已完成(v0.2): 统一派发管道 + 冒泡 + preventDefault + 键盘/焦点/输入 | — |
| **组件模型** | 无可复用/可组合的组件、无生命周期钩子 | 运行时(Swift) + JS |
| ~~**网络层**~~ ✅ | 已完成(v0.2): hn.fetch Promise + 主机白名单 + 错误传播 | — |
| **网络(HTTP)** | 无法调 API / 加载数据 / WebSocket | 运行时(Swift) |
| **表单** | 无 form 元素 / submit / validation | 引擎(C) + 运行时 |
| `text-overflow: ellipsis` | 每个 UI 都需要 | 引擎(C) |
| **逐侧边框** (border-top/left/...) | 几乎所有设计系统都需要 | 引擎(C) + 绘制 |

#### 🟡 重要级（没有则严重受限）

| 缺口 | 影响 |
|---|---|
| `::before` / `::after` 伪元素 | 装饰性 UI 的标配 |
| `white-space` / `word-break` / `overflow-wrap` | 文本布局精确控制 |
| CSS Grid | 现代布局（可延后到 flex 覆盖大部分场景后） |
| ~~`transform` (rotate/matrix)~~ ✅ | 已完成(v0.2): rotate/rotateX/rotateY/perspective + 四边形光栅化 |
| 路由 / 多视图导航 | 多页面应用 |
| 状态管理 | 响应式数据绑定 |
| 背景图片 | 视觉设计 |
| `:not()` / 属性选择器 `[attr=v]` | 精确样式匹配 |
| 无障碍 (ARIA) | 生产必备 |
| 开发者工具 | 调试效率 |

#### 🟢 加分级（有更好）

| 缺口 | 场景 |
|---|---|
| 虚拟滚动 / IntersectionObserver | 长列表性能 |
| Web Workers 等价 | 后台计算 |
| ~~启动持续动画~~ ✅ | 已完成(v0.2): @keyframes 多段动画 + 时间轴采样 |
| ~~Lottie 矢量动画~~ ✅ | 已完成(v0.3): JSON 解析 + 关键帧求值 → POLYGON 指令(三后端同源) |
| ~~网格变形贴图(Live2D 类)~~ ✅ | 已完成(v0.3): MESH 指令 + 三角形仿射纹理映射; rig 由脚本驱动 |
| Live2D 官方 `.moc3` | ⛔ **不实现**: 专有格式, 需 Cubism SDK 商业授权(见 1.4) |
| CSS `contain` / `will-change` | 性能提示 |
| 自定义字体 (`@font-face`) | 品牌字体 |
| 触摸事件 | 移动端 |

### 1.3 性能基线（当前实测）

| 指标 | 数值 | 评价 |
|---|---|---|
| 布局(2000 卡片) | 2ms | ✅ 优秀 |
| 滚动+重绘 | 0.01ms/帧 | ✅ 优秀(预算 16.6ms) |
| 全量重排 | 1.5ms | ⚠️ 2000 卡片场景下每帧 1.5ms 也可接受，但 10 倍规模会超预算 |
| 内存(流式 60s) | 稳定 80MB | ✅ 有 arena 压缩 |
| 二进制 | 124-228 KB | ✅ 极小(vs Electron 200MB) |

### 1.4 明确的边界：为什么不做 Live2D 的 `.moc3`

v0.3 加入了 **Lottie 矢量动画**与**网格变形贴图**，两者都作为**引擎原语**
（产出平台无关的 POLYGON / MESH 绘制指令），因此三个后端自动同等支持。
这里要把边界讲清楚，避免误解：

| 项 | 状态 | 说明 |
|---|---|---|
| Lottie (`.json`) | ✅ 已实现 | `bodymovin` 是公开格式；引擎自解析 + 自求值，无第三方依赖 |
| 网格变形绘制 | ✅ 已实现 | 三角形仿射纹理映射（CoreGraphics / 软件光栅双后端） |
| 网格动画声明 | ✅ 已实现 | `hn-mesh` 正弦形变 + `hn_node_set_mesh_verts()` 脚本逐帧驱动 |
| 骨骼 / 物理 / 口型同步 | ⚪ 应用层 | 引擎只做"网格 + 贴图"合成；rig 装配逻辑不属于引擎 |
| Live2D `.moc3` 解码 | ⛔ 不做 | **专有格式**，解码需 Cubism SDK（商业授权 + 授权协议约束） |
| Live2D `motion3.json` 等 | ⚪ 可解析 | 若已有 `.moc3` 之外的公开数据，可用同一 JSON 解析器消费 |

**结论**：我们交付的是 Live2D 视觉效果背后的**技术原理**（网格变形 + UV 贴图
+ 逐帧顶点驱动），而不是它的专有资产格式。想要"立绘呼吸/摆动/口型"这类效果，
用 `hn-mesh` 或脚本驱动顶点即可；想要直接加载市面上的 `.moc3` 模型，
需要走 Live2D 官方的商业授权路径，这不是本项目要做的事。

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
| JS 支持 | 完整(V8) | 无(Dart) | 完整(JSC/Hermes) | 完整(WebView) | **JavaScriptCore 子集** |
| 跨平台 | ✅ | ✅ | ✅(非 Web) | ✅ | **规划中(hnsoft)** |

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

#### 1a. 定位系统（引擎 + 绘制 + 布局）
```
position: relative | absolute | fixed
top / right / bottom / left
z-index
```
- 引擎: layout 阶段增加 absolute 定位计算（脱离常规流，参照最近 positioned 祖先）
- 绘制: paint_walk 需按 z-index 排序子节点
- 工作量: ~2 周（布局 + 绘制 + 断言）

#### 1b. 文本增强
```
text-overflow: ellipsis
white-space: nowrap | pre | pre-wrap
word-break: break-all | break-word
```
- 引擎: 行内排版增加省略号绘制（超出容器宽度时截断并画 "…"）
- 工作量: ~1 周

#### 1c. 逐侧边框
```
border-top: 2 solid #4f7cff;
border-left-color: #333;
```
- 引擎: style 拆分 border 为四边独立值；paint 分四条路径绘制
- 工作量: ~1 周

#### 1d. 事件系统（运行时层）✅ 已完成

**实施结果**: 引擎提供 `hn_event` 数据与冒泡路径(`hn_event_path_at`),
运行时 `emit()` 是唯一派发出口, JS 与 hx-trigger 共享同一来源;
`preventDefault()` 中止冒泡。已修复三个静默失效缺陷(raw text 折叠 /
包装对象未缓存 / DOM 变更未重排)。154 断言覆盖。
```
-- 事件类型: mousedown/mouseup/click/dblclick
--            mousemove/mouseenter/mouseleave
--            keydown/keyup
--            focus/blur
--            scroll
--            input/change
--            事件冒泡 + preventDefault
```
- 运行时: HtmlNativeView 已有 mouseDown/Up/scrollWheel/keyDown，
  需要统一为事件派发管道 → JS addEventListener + hx-trigger 均可消费
- JS: `el.addEventListener('mousemove', fn)` 已有壳，需接通引擎事件
- 工作量: ~2 周

#### 1e. 网络层（运行时层）✅ 已完成

**实施结果**: `hn.fetch(url, opts) → Promise<{ok,status,headers,text(),json()}>`,
走 URLSession; 4xx/5xx reject 且带 status; 主机白名单(子域支持);
20s 超时。已修复三个静默失效缺陷(JavaScriptCore 无 setTimeout /
shim 链式赋值语法 / jsString 转义不全)。165 断言覆盖。
```
hn.fetch(url, options) → Promise<response>
  options: { method, headers, body }
  response: { status, headers, text(), json() }
```
- 运行时: URLSession 异步请求，回调 JS Promise
- 依赖: JS 运行时需支持 Promise(JSC 原生支持)
- 工作量: ~1 周

#### 1f. 表单处理
```
<form hn-submit="sys://api/submit">
  <input name="title" required>
  <textarea name="body"></textarea>
  <button type="submit">提交</button>
</form>
```
- 引擎: form 元素收集子 input/textarea/select 的 name→value 映射
- 运行时: submit 事件触发时自动编码发送
- 工作量: ~1 周

#### 1g. 增量布局
- 当前: 任何变化触发 `hn_context_layout` 全量重排
- 优化: 标记 dirty 子树，只重排受影响的分支
- 触发场景: hx-swap 只改变一个容器 → 只有该容器需要重新布局
- 工作量: ~2 周（需要仔细的不变量维护）

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

#### 3a. 合成层 / GPU 加速
- 当前: 每帧全量重绘整个视图
- 目标: 不变的 UI 缓存为层(bitmap/纹理), 动画元素单独合成
- 方案: `will-change: transform` 声明 → 该子树渲染到离屏层
- 工作量: ~4 周

#### 3b. 虚拟滚动
```html
<list-view items="10000" item-height="44">
  <template><div class="row">{$item.name}</div></template>
</list-view>
```
- 只渲染可见区域 ±buffer; 滚动时回收/复用 DOM 节点
- 工作量: ~2 周

#### 3c. 开发者工具
- **Inspector**: 元素树 + 盒模型可视化(类似浏览器 DevTools)
- **Console**: JS 交互式终端
- **Network**: hx-* 请求日志
- **Performance**: 帧时间线(layout/paint/animation 各阶段耗时)
- 形态: 独立窗口 + Unix socket 通信(与守护进程同架构)
- 工作量: ~4 周

#### 3d. 无障碍 (ARIA)
- `role` / `aria-label` / `aria-expanded` 属性解析
- macOS: NSAccessibility 协议桥接
- 工作量: ~2 周

#### 3e. 安全模型
- per-app 沙箱(文件/网络/系统调用白名单)
- CSP 等价(限制可加载的资源)
- JS 权限分级(读-only / 读写 / 系统调用)
- 工作量: ~3 周

**Phase 3 总工作量: ~15 周**

---

### Phase 4: OS-PWA（v1.0）—— "像 PWA 一样开发，像原生应用一样运行"

**目标：** 开发体验 = PWA(HTML/CSS/JS + 离线缓存 + 安装),
运行体验 = 原生(轻量、快速、系统集成)。

- 后台任务(定时器、推送)
- 文件系统访问(沙箱化的读写)
- 系统传感器(位置、摄像头 —— 平台 API 桥接)
- 多窗口协调(窗口间通信)
- 离线数据同步
- 应用间通信(URL scheme / socket)
- 打包分发(.hnapp 格式 + 安装器)
- 国际化框架
- 触摸/手势支持

**工作量: 持续迭代**

---

## 4. 架构演进路径

### 当前架构
```
HTML/CSS → C99 引擎(解析→级联→布局→指令) → 平台运行时 → 屏幕
                                                    ↑ JS(JavaScriptCore)
```

### v0.2 架构
```
HTML/CSS/JS → C99 引擎 → 增量布局 → 显示列表 → 后端选择:
                                                  ├ CoreGraphics (macOS)
                                                  ├ hnsoft 软件光栅 (Linux/Win)
                                                  └ WebKit 兜底
```

### v0.3+ 架构
```
应用定义(.hnapp 包)
  ├ 页面(HTML/CSS/JS)
  ├ 组件(自定义元素)
  ├ 状态(hn.store)
  └ 路由(视图注册)
       ↓
引擎管线(增量布局 + 脏区域追踪)
       ↓
合成层管理(静态层缓存 + 动画层独立)
       ↓
后端(CoreGraphics / hnsoft / Skia 可插拔 / WebKit)
```

### 关键架构决策

| 决策 | 选项 | 倾向 |
|---|---|---|
| 组件模型 | Web Components 式 vs 自有系统 | 自有系统(更轻, 不需要 Shadow DOM 的完整性) |
| 状态管理 | 响应式Proxy vs 显式绑定 | 显式绑定(更可控, JSC 的 Proxy 实现不保证性能) |
| JS 引擎 | JavaScriptCore vs QuickJS vs 无 | JavaScriptCore(系统自带, 零依赖) + QuickJS 备选(嵌入引擎的场景) |
| 增量布局 | 脏子树标记 vs 布局树缓存 | 脏子树标记(与 CSS containment 对齐) |
| Skia | 现在引入 vs 接缝预留 | 接缝预留(显示列表已是边界; 引入是增量的, 不阻塞其他工作) |

---

## 5. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 增量布局引入回归 | 高 | 高 | 每步全量回归断言 + 结构自检 |
| absolute 定位与 flex 交互复杂 | 中 | 高 | 先支持脱离流的简单场景, 逐步增加嵌套定位 |
| JS 桥性能(每操作跨语言调用) | 中 | 中 | 批量操作 API + 命令缓冲 |
| 组件模型设计过度 | 中 | 中 | 先支持最小生命周期, 按需增加 |
| FreeType 交叉编译 | 中 | 低 | HN_NO_TEXT 降级路径已实现 |
| 单人维护 bandwidth | 高 | 高 | 分阶段发布, 每阶段独立可用 |

---

## 6. 里程碑总结

| 版本 | 主题 | 周期 | 交付 |
|---|---|---|---|
| v0.1 ✅ | 引擎原型 | 已完成 | 5 指令渲染 · hx-* · sys:// · 流式 · 动画 · JS · 三平台 |
| v0.2 ✅ | 可用工具 | 已完成(定位/省略号/逐侧边框/事件/网络/@keyframes/3D) | 8 指令渲染 · 事件管道 · 网络层 · 3D 变换 |
| v0.3 ✅ | 富媒体 | 已完成(Lottie/网格变形/透明背板) | Lottie 矢量动画 · 网格变形贴图 · 逐像素透明窗口 |
| **v0.4** | **应用框架** | ~10 周 | 组件 · 状态 · 路由 · 伪元素 · Grid |
| **v0.5** | **生产可用** | ~15 周 | 合成层 · 虚拟滚动 · DevTools · 无障碍 · 安全 |
| **v1.0** | **OS-PWA** | 持续 | 后台任务 · 文件 · 传感器 · 多窗口 · 打包分发 |
