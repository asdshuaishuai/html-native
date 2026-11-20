# GenUI 运行时设计(P2)：常驻画布宿主 + A2UI/AG-UI 现成协议

> 状态: P2a ✅ 落地(2026-11-20) · 上游裁决: 纯 WebView 转型(docs/positioning-webview-pivot.md)
> 口径: WebView 常驻；我们只提供**运行时维护 + 画布形态**；内容 = 原生 HTML/CSS/JS；
> UI 协议**用现成的** A2UI 与 AG-UI，不自造 JSON 层。

## 1. 分层与数据流

```
agent(SDK/框架)
   │  A2UI Transport(JSONL/SSE, agent→renderer) + action/function 回传
   ▼
daemon ──(Unix socket JSON-lines: create/update/snapshot/destroy… 画布控制)──► 画布宿主(常驻)
                                                                               │ 多画布(NSWindow+WKWebView)
                                                                               ▼
                                                        WebView 内 web 运行时(官方渲染器/客户端 bundle)
                                                                               │  AG-UI(HTTP+SSE, agent事件流)
                                                                               ▼
                                                                           agent 端点
```

- **协议归属**: A2UI 管"画什么"(声明式 UI 数据)；AG-UI 管"聊什么"(run/事件流/状态/工具)。
  两者正交，渲染器与客户端都在 WebView 内(官方 TS 包)。
- **宿主不解释 UI 协议**: 宿主只做画布生命周期 + 形态 + 快照 + 控制通道；
  A2UI/AG-UI 消息由 WebView 内 runtime 消费。

## 2. 画布宿主(已实现, `tools/hncanvas_host_macos.c`)

常驻进程；Unix socket；JSON-lines 请求/响应(一行一条)；画布上限 16。

| op | 语义 |
|---|---|
| `ping` / `list` | 活体检查 / 画布清单(含 form) |
| `create` | `{id, url\|html, base?, w,h, hidden?, form:{transparent,frameless,level,click_through}}` |
| `update` | `{id, url\|html, base?}`(装载稳定后应答) |
| `reload` / `show` / `hide` / `destroy` | 生命周期 |
| `snapshot` | `{id, path}` → WKSnapshot 出 PNG(同步语义) |
| `quit` | 收尾退出(关画布/删 socket) |

- **内容零侵入**: 画布形态由 create 参数表达，HTML 里没有任何 `hn-*` 子集属性。
- **桥**: 宿主向页面注入极简 `window.neb.post(msg)`(中性名) → 原生按
  `message.webView` 反查画布 id 并打印/转交(后续接 daemon)。
- **相对资产**: `loadFileURL:allowingReadAccessToURL:`(根=页面目录)。
- **纪律**: 纯 C99 + dlopen 系统 WebKit/AppKit；零 ObjC 编译单元；冻结引擎一行不改
  (仅链接 `hn_json.c` 做协议解析)。

验收: `bash tools/wk_host_probe.sh`(21 断言: 隔离 socket/红绿逐像素/update 变更/
桥回环/destroy/quit 清理)。

## 3. A2UI v1.0 映射(用现成)

**协议要点**(a2ui.org/specification/v1.0-a2ui/): agent→renderer 四类消息
`createSurface` / `updateComponents` / `updateDataModel` / `deleteSurface`；
renderer→agent 为 `action` 与函数 RPC(`callRendererFunction` / `callAgentFunction` /
`rendererFunctionResponse` / `agentFunctionResponse`)；传输层契约 = 有序投递 +
消息分帧 + 元数据 + 可选回传通道；v1.0 用 `@` 前缀做动态绑定与信封键。

**落地方式**:
- 渲染器 = 官方 `@a2ui/web_core` + 框架适配器 + 官方基础 catalog。WebView 是浏览器环境,
  选 **`@a2ui/lit`**(或 react) 均可；以 **预构建 ESM bundle** 形式 vendor 到
  `vendor/a2ui/<version>/runtime.js`(锁定 npm 版本、记录来源与许可证，附
  `tools/build-a2ui-bundle.sh` 可复现构建)。运行时不联网装包。
- 传输 = 宿主的 JSON-lines 控制通道天然满足 A2UI 传输契约：daemon 收 A2UI 消息后
  以 `{op:"a2ui", id, msg}` 投递给画布；渲染器 `action` 经 `window.neb.post` 回传。
- 画布页面 = 一个官方 runtime 承载页(空白 + 渲染器 bootstrap)，不是业务页面。

## 4. AG-UI 1.0 映射(用现成)

**协议要点**(docs.ag-ui.com/spec/1.0): 一次请求(`RunAgentInput`)进, 一条**有序 typed
事件流**出; 事件族含 run 生命周期(RunStarted/RunFinished/RunError)、文本消息
(TextMessageStart/Content/End)、工具调用(ToolCall*)、状态快照/增量(StateSnapshot/
StateDelta)、步骤(StepStarted/Finished)、推理/透传等; 传输 HTTP+SSE(规范另有 Protobuf
绑定); schema 权威、行为规范权威分离。

**落地方式**:
- 客户端 = 官方 `@ag-ui/client`(TS, WebView 内可直接 fetch/SSE)，同样 vendor 预构建 bundle。
- **AG-UI × A2UI 胶水(薄)**: `TextMessageContent` → 文本/消息组件流式追加;
  `ToolCall*` → 状态卡; `StateDelta` → A2UI dataModel 补丁; `RunFinished` → 空闲态。
  胶水只做事件→UI 数据映射，不解释业务语义。
- 传输直连: WebView 内 SSE 直连 agent 端点(CORS/证书受 WebKit 约束，文档写明；
  需要代理时由 daemon 侧做反向代理，P2d 评估)。

## 5. 生命周期

- daemon 首次需要画布时拉起宿主, **保活**(热窗口零冷启动)，宿主崩溃 → daemon 重启并重建画布;
- 无画布空闲 N 分钟 → 宿主退出(P2b 落地); daemon 退出 → 宿主收尾;
- 快照由常驻宿主直接响应(取代 P1 的临时 spawn hack)。

## 6. 分期

| 期 | 内容 | 状态 |
|---|---|---|
| P2a | 常驻画布宿主 + 控制协议 + 同步快照 + 桥 + 门禁 21 断言 | ✅ |
| P2b | daemon 接宿主(open/update/shot 改走宿主; 保活/空闲退出/崩溃重启; 删 P1 临时 spawn) | ⏳ |
| P2c | A2UI: vendor 官方 runtime bundle + 宿主 `a2ui` op + 承载页 + action 回传 | ⏳ |
| P2d | AG-UI: vendor 官方 client + SSE 直连 + 事件→UI 胶水 + 示例 agent 页 | ⏳ |
| P2e | GenUI 画布形态收口(多画布/层级/点击穿透/跨桌面) + 文档/skill 重写(去 hn-* 子集) | ⏳ |

## 7. 边界(如实)

- WebView 内存/冷启动与三平台内核差异(ADR §5 已声明)不变;
- 像素确定性只在冻结引擎(hncore)侧成立，WebView 面以行为断言(装载/像素中心/桥/生命周期)为准;
- vendor 的官方 bundle 需要 Node 工具链**构建期**参与(build script)，运行期零依赖;
  许可证与版本锁定记录在 vendor 目录 README。
