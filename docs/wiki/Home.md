# Neb Wiki · 导航

> html-native / Neb(NO-WEB) 引擎的 wiki 入口。**Neb = 为 AI agent
> 定制的 HTML 原生渲染引擎**: C99 全自研、无 WebView、无构建、
> 单二进制 ~1.5MB, agent 生成 HTML/CSS 即得桌面级原生窗口。

## 页面索引

| 页面 | 内容 |
|---|---|
| [快速上手](快速上手.md) | 5 分钟出第一个原生窗 |
| [hn 语法参考](hn-语法参考.md) | 表面清单/meta、布局样式子集、富媒体标签 |
| [交互-脚本与系统能力](交互-脚本与系统能力.md) | hx-*、JS 桥面、sys:// 系统桥 |
| [CLI 参考](CLI-参考.md) | hncore / hnapp / hn / daemon 全命令 |
| [打包与分发](打包与分发.md) | 项目目录 · .hnapp 胶囊 · hn pack · hn build |
| [性能与冻结架构](性能与冻结架构.md) | 冻结三层(测量缓存/子树布局记忆/级联记忆化) |
| [引擎内部与测试](引擎内部与测试.md) | 一帧流水线 · 内存纪律 · 探针门禁 |
| [平台支持与边界](平台支持与边界.md) | 三平台能力矩阵 · 诚实清单 |
| [FAQ](FAQ.md) | 常见问题 |

## 深度文档(仓库内)

- [README](../../README.md) — 定位/宣言/支持的子集(v1 语言规范)
- [docs/getting-started.md](../getting-started.md) — 上手教程
- [docs/manual.md](../manual.md) — **完整使用手册**(本文大部分主题的展开)
- [docs/architecture.md](../architecture.md) — 架构文档
- [docs/roadmap.md](../roadmap.md) — 路线图 · [docs/platforms.md](../platforms.md) — 平台矩阵
- [docs/freeze-design.md](../freeze-design.md) — 冻结设计 · [docs/handwired-notes.md](../handwired-notes.md) — handwired 研究
- [docs/media-design.md](../media-design.md) — 媒体契约 · [docs/threejs-support.md](../threejs-support.md) — 3D 支持度
- [docs/adr-001-renderer.md](../adr-001-renderer.md) — 绘制引擎决策 · [ISSUES.md](../../ISSUES.md) — 存量债务

## 一分钟速览

```bash
# 人类: 写一个 app.html, 一条命令开窗(无构建)
dist/hnapp-macos-arm64 app.html

# agent: 常驻宿主 + 秒开秒毁热推送
dist/hn-daemon &
dist/hn open card examples/agent-card.html
dist/hn update card < <(生成新文档)

# 无头渲染(agent 感知/CI 视觉回归)
dist/hncore-macos-arm64 render app.html out.png 480 620
```
