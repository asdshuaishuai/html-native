---
format: 1920x1080
duration: 99s
message: "agent 写下的 HTML，就是桌面级原生窗"
arc: 封面 → 三堵墙 → Neb 的回答 → 实机演示 → 数字指标 → 标准格式与边界 → 结尾金句
audience: 2026 上海开源软件应用创新大赛评委与开发者
mode: autonomous
---

## Frame 1 — 封面

- scene: 深色底大标题封面，白/浅紫混排双行大字 + 副题
- duration: 6s
- transition_in: cut
- status: animated
- poster: 3s
- src: compositions/frames/01-cover.html
- rules: waterfall-entry (两行标题依次落版) + ambient-glow-bloom (标题后光晕)

底色 #0e1220。大标题两行："agent 写下的"（白）/"HTML，就是原生窗"（HTML 用 #a7b6ff）。副题 "html-native · Neb = NO-WEB · 为 AI agent 定制的 HTML 原生渲染引擎"。背景 ghost 字 "NO-WEB" + 蓝紫径向光晕。

## Frame 2 — 三堵墙

- scene: 三张痛点卡依次滑入，编号 01/02/03
- duration: 12s
- transition_in: cut
- status: animated
- poster: 8s
- src: compositions/frames/02-walls.html
- rules: waterfall-entry (卡片自下滑入级联) + sine-wave-loop (微幅环境漂移)

kicker "现状 · 三堵墙"。卡1 "WebView 内核百兆起步 / 内存黑洞"；卡2 "AI 生成 UI 不可验证 / 没有机器口径"；卡3 "桌面形态缺位 / agent 会画网页，住不进桌面"。

## Frame 3 — Neb 的回答

- scene: 三层架构条自上而下展开 + 关键词标签横向滚动
- duration: 12s
- transition_in: cut
- status: animated
- poster: 8s
- src: compositions/frames/03-architecture.html
- rules: stat-bars-and-fills (架构条 scaleX 展开) + sine-wave-loop (标签带匀速漂移)

标题 "Neb 的回答"。三层条：Neb 引擎核心 (20.6k 行纯 C99) → rt 运行时 → platform 平台层 (Metal / cairo / hnsoft 同源)。标签滚动带：纯 C99 · 零依赖 · 零 SDK · 10 类指令 · 三后端同源 · 探针锚定。

## Frame 4 — 实机演示

- scene: 真实桌面录屏 6 段剪辑，每段大字标签压角 + 底部字幕条与进度
- duration: 30s
- transition_in: cut
- status: animated
- poster: 10s
- src: compositions/frames/04-demo.html
- rules: waterfall-entry (每段标签落版)

6 段（素材 3440x1440 contain 于 1920x804，上下留信息带）：
1. 数据大屏 p1@4.5s 4.5s
2. 透明桌宠 p1@11.5s 4.5s
3. Live2D 数字人 p1@18.0s 5.5s
4. 媒体窗 GPU 合成 p1@27.0s 4.5s
5. 流式会话 p2@3.0s 5.5s
6. 多窗同屏 p2@16.5s 5.5s

## Frame 5 — 数字指标

- scene: 四张大数字卡依次敲入 + 确定性一行
- duration: 14s
- transition_in: cut
- status: animated
- poster: 9s
- src: compositions/frames/05-metrics.html
- rules: counting-dynamic-scale (数字滚动+scale 生长) + spring-pop-entrance (卡片敲入) + ambient-glow-bloom (落位光晕)

卡：20.6k 行 C99 / 1145+ 探针断言 / 10 类绘制指令 / 0 依赖 0 SDK。底线："跨架构确定性：同一文档，逐位一致"。

## Frame 6 — 标准格式与边界

- scene: 四条格式能力行依次亮起 + 一行诚实注
- duration: 14s
- transition_in: cut
- status: animated
- poster: 8s
- src: compositions/frames/06-standards.html
- rules: waterfall-entry (行级联落版)

行：dotLottie ZIP 直读 · Live2D motion3 驱动 · GIF / object-fit · markdown / JSON / CSV 直染。诚实注："moc3 专有格式不解析，以官方 motion 数据驱动自有 rig 替代"。

## Frame 7 — 结尾

- scene: 金句 + 仓库信息落版
- duration: 11s
- transition_in: cut
- status: animated
- poster: 6s
- src: compositions/frames/07-ending.html
- rules: spring-pop-entrance (金句落定) + ambient-glow-bloom (光晕收束)

金句："agent 时代的渲染引擎，价值不在渲染本身，而在「渲染结果可被机器验证」——探针锚定的像素口径，就是 agent 与像素之间的翻译层"。落款 github.com/asdshuaishuai/html-native · MIT · Neb = NO-WEB。
