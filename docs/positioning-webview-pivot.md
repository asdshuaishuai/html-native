# 定位决策:Pure-WebView 转型(2026-11-20)

> 状态: ✅ 已裁决(P0 落地) · 裁决人: 项目所有者
> 触发: "大概可能需要改一下定位了…自己做还是太麻烦了"

## 1. 新定位(两产品面)

| 产品面 | 形态 |
|---|---|
| **桌面小程序守护进程运行时** | 以 web 技术为核心的小程序宿主: daemon socket 协议、`.hnapp` 胶囊、应用生命周期、`hn` CLI、窗口形态语义(透明/无边框/桌宠/层级/跨桌面) |
| **GenUI 桌面级画布运行时** | **WebView 为唯一画板**, 实时渲染; GenUI 语义(`hn-*` 属性/窗口形态/动效词汇)通过运行时注入落到 WebView 之上 |

一句话: **WebView 负责画, 我们负责"把画变成桌面上的东西"** —— 窗口形态、进程生命周期、打包分发、agent 接口。

## 2. 为什么转

自研渲染管线(CSS 解析/级联/布局/软件光栅/GPU 合成 + Metal parity)维护成本过高:
每补一个观感能力(模糊文字阴影/多层阴影/backdrop-filter/字体面/纹理底图)都要在
两三个后端逐像素对齐, 而 WebKit 一行搞定且跨页一致。引擎做得再好, 用户感知的是
"这页好不好看" —— 与其自证, 不如借力。

## 3. 资产处置

| 资产 | 处置 |
|---|---|
| daemon socket 协议 / `hn` CLI / `.hnapp` 胶囊 / `hncore pack/build` / 窗口形态语义 / agent skill | **全保留**(与画板无关, 新定位同源) |
| C99 渲染引擎(Neb) | **冻结**: 不再追加渲染特性; 仅作为 **无头确定性后端** 保留 —— `hncore` 的 render/parse/layout/paint/verify/pack 与 44 探针继续服务: agent 自验截图、H5 兼容性体检、CI、无 WebView 环境 |
| htmx 语义面(`hx-*`/swap/push-url) | C99 面保留自研实现; **WebView 面直接使用真 htmx**(用户既有裁决回填) |
| `hnweb_macos.c` / `hnweb_linux.c` / `hnwin.c` | 保留为冻结管线的窗口壳; 新入口是 WebView 壳 |

## 4. 分期

| 期 | 内容 | 状态 |
|---|---|---|
| **P0** | WebView 画板壳: 透明/无边框窗、JS→原生桥、相对资产按页面目录、`--probe` 自检 | ✅ 2026-11-20(`tools/hnweb_wk_macos.c`) |
| P1 | daemon 接线(`open/update/shot` 走 WebView 后端; shot 用 WKSnapshot)、`hn-window` manifest(尺寸/透明/标题)、交互面(输入法/右键/缩放/快捷键) | ⏳ |
| P2 | 小程序运行时时序(胶囊生命周期/热更新/权限) + GenUI 运行时注入(`hn-*` 属性 → 运行时语义) + 真 htmx 内置 | ⏳ |
| P3 | 文档/skill 叙事切换(README 主叙述从 "NO-WEB" 改为双面定位)、三平台壳(WebKitGTK/WebView2)、探针口径重定(WebView 面新增端到端断言) | ⏳ |

## 5. 边界(如实声明)

- **像素确定性消失**: 44 个逐位断言探针只覆盖冻结管线; WebView 面以"装载完成 + 桥回环 + 交互无崩"为断言口径, 不做像素比对。
- **内存与冷启动**: WKWebView 进程模型(每窗 ~百 MB 级)、冷启动秒级 —— 对桌宠/画布场景需在 P1 用预热/复用缓解。
- **三平台内核不一**: WKWebView / WebKitGTK / WebView2 行为有差异, 跨端一致性从"同引擎"降为"尽力对齐"。

## 6. P0 验收证据(2026-11-20)

```
$ ./dist/hnweb-wk-macos-arm64 examples/wk-bridge-test.html --probe
[wk] window 960x752 transparent=0
[bridge] hn hello-neb
[wk] probe loaded in 410ms, bridge=1, transparent=0        → exit 0

$ ./dist/hnweb-wk-macos-arm64 examples/demo/index.html 1280 900 --transparent --probe
[wk] window 1280x900 transparent=1
[wk] probe loaded in 264ms, bridge=0, transparent=1
[wk] window opaque=0 (want 0)                              → exit 0

交互模式 2s 冒烟: 事件泵无崩溃(KitDefined 过滤生效), 桥消息照常。
```

构建: `bash tools/build-wk-macos.sh`(纯 C99, dlopen WebKit/AppKit, 零链接框架, 零 ObjC 编译单元)。
