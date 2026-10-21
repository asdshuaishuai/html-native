# FAQ

**Q: 和 Electron/Tauri 有什么区别?**
A: 没有浏览器内核。Electron 带整个 Chromium(~200MB), Tauri 带系统
WebView; Neb 是自研 C99 引擎直接把 hn 编码(HTML 语法子集)光栅成
像素 —— 单二进制 ~1.5MB、零运行时依赖、跨架构布局逐字节一致。
代价是不兼容任意网页(子集即语言规范, `hn pack` 可机检差异)。

**Q: 为什么我的样式没生效?**
A: 三步定位:
1. 确认跑的是新构建(dist 可能是陈旧二进制);
2. `HN_STYLE_MEMO=0 HN_LAYOUT_SKIP=0 HN_FRAGMENT=0` 三关对照 ——
   关掉后正常 = 缓存层 bug(请提 issue), 仍不正常 = 该特性未支持
   (对照 README 子集表);
3. `hncore paint app.html` 看指令是否产出, `boxes` 看几何。

**Q: 支持哪些 CSS?**
A: README「支持的子集(v1)」是完整矩阵(断言即规范)。核心: flex 全
家桶、absolute/fixed、渐变/阴影/圆角/逐侧描边、ellipsis/line-clamp、
伪元素、transition/@keyframes、transform 3D。不支持: Grid(排期)、
float、Web 字体、复杂选择器。

**Q: JS 是必须的吗?**
A: 不是。交互用 `hx-*` 属性(htmx 风格)声明; JS(QuickJS)是可选的
本地作者能力 —— agent 推的界面用声明式编码, 无代码执行面。

**Q: 怎么部署到没有显示器的服务器?**
A: `hnapp-macos-headless` / daemon 无头模式 / `hncore render` 全链路
不触 GUI; `--shot`/`--advance` 出确定性 PNG(动画可用 `HN_CLOCK` 推到
指定时刻)。

**Q: 性能怎么样?**
A: 基线(2026-10-13): 2000 卡文档 parse 5.8ms / layout 13.8ms /
paint 17.6ms; 空转 CPU 0%(空闲零唤醒); 内存 daemon+1 应用 11.7MB。
冻结三层落地后重复重排负载再提速 4-5x —— 见
[性能与冻结架构](性能与冻结架构.md)。

**Q: 怎么把已有网页变成桌面应用?**
A: `hn pack <file|dir|url>`: 兼容性体检(逐页报告不支持特性) → 注入
hn-* meta → 收集资产 → 项目目录 + .hnapp 胶囊。见[打包与分发](打包与分发.md)。

**Q: Windows 什么时候支持?**
A: hncore/hnapp 交叉产物已可构建; 真窗壳 `hnp_win32.c` 在路线图
(hnwin 旧壳可用)。见 [平台支持与边界](平台支持与边界.md)。

**Q: 如何参与/扩展引擎?**
A: 读 [architecture.md](../architecture.md) + `include/hn.h` 单头
API; 新能力必须带探针(断言即规范); 性能优化必须带开/关双跑对照。
