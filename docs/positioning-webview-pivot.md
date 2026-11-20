# 定位决策:Pure-WebView 转型(2026-11-20)

> 状态: ✅ 已裁决 · P0 画板壳 + P1 核心接线已落地(2026-11-20, 验收 30/30)
> P1 的降级项、与 ADR 原文的修正见 §5.2/§5.3。
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
| `hnweb_macos.c` / `hnweb_linux.c` / `hnwin.c` | 保留为冻结管线的窗口壳; macOS 的 daemon 首选 WebView 壳, 引擎壳作兜底; Linux/Windows 仍走引擎壳 |

## 4. 分期

| 期 | 内容 | 状态 |
|---|---|---|
| **P0** | WebView 画板壳: 透明/无边框窗、JS→原生桥、相对资产按页面目录、`--probe` 自检 | ✅ 2026-11-20(`tools/hnweb_wk_macos.c`) |
| **P1** | daemon 接线(`open/update/close/list/shot` 走 WebView 后端)、`hn-window` manifest(尺寸/透明/无边框/标题)、WKSnapshot 截图、300ms 热重载、隐藏模式 | ✅ 2026-11-20 核心面通过(验收 30/30)。**降级**: 交互面(输入法/右键/缩放/快捷键)未专项实现也未验收; 就地快照/预热复用/状态保持型热更新推迟 P2 |
| **P2** | 常驻画布宿主 + A2UI/AG-UI 现成协议接入(设计见 genui-runtime-design.md)。**P2a ✅**(2026-11-20 `tools/hncanvas_host_macos.c` + `tools/wk_host_probe.sh` 21 断言): 多画布/JSON-lines 控制/同步快照/中性桥, 取代 P1 的每窗 spawn 与截屏临时进程; P2b daemon 接宿主 / P2c A2UI / P2d AG-UI / P2e 画布形态收口 | ◐ |
| **P3** | 文档/skill 叙事切换(README 主叙述从 "NO-WEB" 改为双面定位)、三平台壳(WebKitGTK/WebView2)、探针口径重定(WebView 面新增端到端断言) | ⏳ |

### 4.1 P1 实现事实(2026-11-20)

#### 4.1.1 daemon 接线

`hn open <id> <page> --window` 的完整链路:

1. **CLI**(`tools/hn_cli.c`): 源文件路径与 `shot` 输出路径都转绝对(以 CLI 的 cwd 为基准; 相对源文件用 `realpath` 解析, 绝对路径原样, 输出文件尚不存在时拼 cwd), 读整页 HTML 后经 Unix socket 上送 `{op,id,window:1,html,path,w,h}`。daemon 的 cwd 与 CLI 无关, 不绝对化会把文件落到错误目录。
2. **选后端**(`tools/hn_daemon.c`): 先找 daemon 可执行文件同目录, 再找 cwd 的 `dist/`。macOS arm64 的候选顺序是 `hnweb-wk-macos-arm64` → `hnweb-wk-macos-x86_64`(Rosetta) → `hnapp-macos-arm64` → `hnapp-macos-x86_64`(引擎兜底); x86_64 镜像同理。Linux/Windows 只有引擎壳(Windows 还接受 `hnweb-windows-*`, 同为冻结引擎壳)。命中的壳类型记入窗口表: `backend: webview|engine`。
3. **解析 manifest**: daemon 用冻结引擎的 `hn_parse_html` + `hn_doc_manifest` 解析上送 HTML(与 headless open 同一条解析路径), 得到 `{w,h,transparent,frameless,title}`。`hn-window` 的 w/h > 0 时压过 CLI `--w/--h`(与 `rt/hn_rt.c` 的 manifest 优先同口径); 未给尺寸时 daemon 缺省 480x700。
4. **spawn**: `hnweb-wk-macos-* <path> <w> <h> [--transparent] [--frameless] [--title T]`, 每窗一进程。回包 `{id,pid,window:true,w,h,backend,transparent,title}`。
5. **list**: 窗口项带 `pid/w/h/backend/transparent/title`; headless 项仍是 `{id,kind:"headless",w,h}`。
6. **update**: 窗口分支不动子进程, 把新 HTML(可选 css 以 `<style>` 前缀并入)**原子覆写**回 spawn 时的源文件(同目录 tmp + rename); 壳的 300ms 文件指纹(mtime 纳秒 + 尺寸)发现变化后 `reloadFromOrigin`。回包 `{window:true,reloaded:true}`。
7. **close**: SIGTERM + waitpid, 500ms 顽抗则 SIGKILL, 不留僵尸/孤儿窗。
8. **shot**: 见 §4.1.3。
9. **resident 重开**: 账本只存 `{id,file,w,h}`; daemon 启动重开时从文件重新解析 manifest, 透明/无边框/标题不丢。
10. **隐藏模式**: 环境变量 `HN_WEBVIEW_HIDDEN=1` 时壳不 `makeKeyAndOrderFront`/不激活应用, 窗口不显示但仍运行; daemon 子进程继承该变量, 验收除可见冒烟段外全程使用。

窗口位置仍由壳居中计算; manifest 的 `hn-window ...@x,y` 会被引擎解析, 但 WebView 后端不使用 x/y。

#### 4.1.2 manifest 支持面(WebView 后端)

| 声明 | 行为 |
|---|---|
| `hn-window` | 尺寸; `WxH`/`WxH@x,y` 均可解析, WebView 后端只用 w/h, 位置仍居中; w/h > 0 压过 CLI `--w/--h` |
| `hn-transparent` | 透明窗(非 `0`/`false`); 同时强制无边框 |
| `hn-frameless` | 缺省 1(窗口即画布); `0`/`false` 才要系统标题栏 |
| `hn-title` | 窗口标题; open 回包与 `hn list` 可见; 仅 `hn-frameless=0` 且非透明时出现在标题栏 |

未物化: `hn-surface`(popup/layer 仍按普通窗)、`hn-level`、`hn-spaces`、`hn-click-through`、`hn-shadow`、`hn-reveal`、`hn-applet`、`hn-presence`、`hn-lifecycle`。这些字段引擎 manifest 仍解析(冻结管线/引擎壳照旧), 但 P1 的 WebView 后端只消费上表四项。

`hn` CLI 没有 `--title` 选项; 标题来自页面 `hn-title`。daemon 协议留了 `title` 字段兜底, 但 CLI 不发送它。

#### 4.1.3 `hn shot` 口径: 一次性 WKSnapshot, 不是就地截屏

窗口 shot 的真实行为:

1. daemon 用**同一后端壳**临时 spawn 一个一次性进程: `<shell> <path> <w> <h> [--transparent] --shot <out.png>`; 先删旧图, 旧图不参与成功判据。
2. 子进程重新 `loadFileURL` 装载**当前磁盘上的源文件**, `isLoading` 连续 10 拍为假(≈160ms 稳定窗)后 `WKSnapshot`, 经 `NSBitmapImageRep` 写 PNG。
3. PNG 尺寸 = daemon 记录的窗口尺寸(spawn 时的 manifest/CLI 尺寸; 验收夹具 200x120 → PNG 200x120); 用户后续缩放窗口不回写该值。透明窗保留 alpha(`min_alpha=0` 可断言)。
4. daemon 等待子进程 ≤10s, 成功判据 = **exit 0 且输出文件非空**; 成功回 `{path,backend:"webview"}`, 失败回 `webview snapshot failed`。
5. `engine` 后端(`hnapp-*`)没有快照能力, 对 engine 窗口 `shot` 明确回 `shot unsupported for engine window backend`, 不伪造图片。

因此 shot 反映的是"源文件 + 一次全新装载"的画面, **不保留**活动窗口里的 JS/DOM 状态、滚动位置、动画时刻、页面内计时器; 与活动窗口的当前帧可能不同。就地快照需要 daemon 与窗口子进程之间的像素通道(例如 socketpair), 是 P2 的工作。

#### 4.1.4 `hn update` 口径: 原子写回 + 整文档重载

- 写回: 新 HTML 原子覆写源文件(可选 css 以 `<style>` 前缀并入), 源文件会被**持久改动**。
- 触发: 交互模式下壳每 300ms 比较 `mtime_ns + size` 指纹; 变化即 `reloadFromOrigin`, 日志 `[wk] reload size=N`。
- 保持: 窗口/进程/尺寸不变; 不重新解析 manifest, **不重新应用**尺寸/标题/透明/无边框。
- 不保持: 整文档重载会重置页面内 JS/DOM/滚动/动画状态。README 里"热更新(同 id 推送新编码, 窗口与状态保持)"的承诺, 在 WebView 面目前只兑现了"窗口与进程保持"; 状态保持推迟 P2。
- 附带: `hn open --window --css` 的 css 当前**不参与**窗口装载(窗口壳只装载源文件); `hn update` 才把 css 以 `<style>` 前缀写回源文件。

#### 4.1.5 验收命令与结果

```bash
bash tools/wk_acceptance.sh
```

- 前置: macOS(系统 WebKit) + python3(脚本用它解码 PNG/计时)。
- 脚本自己跑 `tools/build-wk-macos.sh` + `tools/build-daemon-cli.sh`, 缺产物直接失败, 不 skip。
- 隔离: `mktemp -d` 独立 HOME + 独立 socket; daemon 的 `--socket` 解析已改为顺序无关, `--no-resident --socket PATH` 不再静默落回默认(用户) socket。除最后一段可见窗冒烟外都 `HN_WEBVIEW_HIDDEN=1`; 只杀脚本记录的 pid, 不全局 pkill。
- 可见窗段: 该段会短暂显示窗口(设计上 ≤2s); `hn` CLI 没有 `--socket` 选项, 该段复用同一隔离 HOME 的默认 socket; 此时第一只 daemon 已退出, 不会碰用户实例。
- 壳直拍显式传 `200 120`: 壳不链引擎、不解析 manifest; daemon 路径另有断言覆盖"manifest 压过 CLI 尺寸"。
- 30 条行为断言: 壳 probe/透明 probe、壳直拍 PNG 像素、daemon open(manifest 压 CLI 尺寸/backend/transparent/title)、list、shot、update 后重载与像素变化、close、headless 旧管线回归、daemon SIGTERM 收口、可见窗 open→close ≤2s。
- 2026-11-20 本机(arm64 macOS)实测: **30/30 PASS**; 可见窗 open→close **173ms**(仅记录一次冒烟, 不是性能承诺)。
- 本轮未跑 44 探针全量回归; 只跑了该验收脚本(内含两个构建脚本)与两条 P0 `--probe` 自检, 因此本文件不把"全量回归绿"列为 P1 证据。

## 5. 边界(如实声明)

### 5.1 定位级边界(P1 修正后)

- **像素确定性消失**: 44 个逐位断言探针只覆盖冻结管线。WebView 面**不做逐位/逐帧比对**; P1 的 `wk_acceptance.sh` 对受控夹具做粗粒度像素断言(尺寸/中心色 ±8/透明 alpha), 用来证明"确实画出来了", 不是跨版本像素等价。
- **内存与冷启动**: WKWebView 每窗一进程(shot 再起一次性进程), 内存与冷启动**本轮未测**; ADR 原计划"P1 用预热/复用缓解"**未实现**, 推迟 P2。可见窗 173ms 只是一次 open→close 的冒烟数字, 不能当冷启动指标。
- **三平台内核不一**: P1 的 WebView 后端只有 macOS(WKWebView)。Linux/Windows 的窗口仍走 `hnapp-*` 引擎壳, `shot` 对 engine 窗口明确报错; WebKitGTK/WebView2 壳在 P3。

### 5.2 P1 新增边界

- **shot 不是就地截屏**: 见 §4.1.3。活动窗口的 JS/DOM/滚动/动画时刻不在其中; 与活动窗口当前帧可能不同。
- **update 是整文档重载**: 见 §4.1.4。窗口/进程保持, 页面内状态不保持; manifest 形态不重应用。
- **无边框窗的交互面缺失**: daemon 缺省 `frameless=1`, 窗无标题栏/关闭按钮/缩放边框; 壳未实现窗口拖动、自定义右键菜单、缩放/快捷键, 输入法只有 WebView 系统默认行为且未验收。P1 的关闭路径是 `hn close <id>`(或杀进程)。
- **manifest 只物化四项**: 见 §4.1.2; 其余 `hn-*` 在 WebView 后端无效(引擎壳仍照旧)。
- **窗口应用的求值/检视接口仍缺**: `eval`/`dump`/`text` 对窗口应用明确回 headless-only; `event` 对窗口应用落到 `app not found`。这些在 P1 未扩到 WebView 后端。
- **每窗一进程 + shot 一次性进程**: 没有预热/窗口复用/进程池; 桌宠/画布场景的常驻成本与冷启动仍待测。
- **窗口槽位不回收(预存在)**: `HN_DAEMON_MAX_WINS=64` 只增不减; 同一 daemon 生命周期内累计 spawn 达上限后, `open --window` 会落回 headless(回包带 `fallback:"headless"`), `resident` 重开报 spawn failed; 需重启 daemon。P1 未改此行为。
- **后端全缺席时静默降级**: 壳二进制缺失/spawn 失败时, `open --window` 落回 headless 并在回包标 `fallback:"headless"`; 调用方应检查 `backend`/`fallback`, 不能假定 `--window` 一定得到真窗。
- **update 会改源文件**: 与引擎窗口同口径, 热推送把内容写回 spawn 时的源文件; 用临时夹具或可写副本, 不要把不可变源文件交给 update。
- **验收不覆盖真实输入**: 30 条断言覆盖装载/桥/像素/生命周期, 不含键盘/鼠标/输入法/多屏/休眠唤醒; 44 探针全量回归本轮未执行。

### 5.3 对 ADR 原文的修正(P1 实测)

| ADR 原文(§4/§5) | P1 实际 | 处置 |
|---|---|---|
| P1 交互面: 输入法/右键/缩放/快捷键 | 壳未专项实现, 也未验收; daemon 窗缺省无边框无关闭按钮 | 推迟 P2, 见 §5.2 |
| P1 用预热/复用缓解内存与冷启动 | 未实现; 每窗一进程, shot 再起一次性进程; 内存未测 | 推迟 P2 |
| WebView 面"不做像素比对" | 验收对受控夹具做尺寸/中心色/alpha 粗断言, 不做逐位/逐帧比对 | 口径改为"不做逐位比对" |
| P2 热更新 | P1 已有内容热重载, 但整文档 reload, 页面状态不保持 | P2 继续做状态保持 |

## 6. 验收证据(2026-11-20)

### P0: 画板壳自检(当前构建重跑)

```
$ ./dist/hnweb-wk-macos-arm64 examples/wk-bridge-test.html --probe
[wk] window 960x752 transparent=0 hidden=0 frameless=0
[bridge] hn hello-neb
[wk] probe loaded in 258ms, bridge=1, transparent=0        → exit 0

$ ./dist/hnweb-wk-macos-arm64 examples/demo/index.html 1280 900 --transparent --probe
[wk] window 1280x900 transparent=1 hidden=0 frameless=0
[wk] probe loaded in 249ms, bridge=0, transparent=1
[wk] window opaque=0 (want 0)                              → exit 0
```

P0 当日另做过交互模式 2s 冒烟: 事件泵无崩溃(KitDefined 过滤生效), 桥消息照常。

### P1: daemon 接线 + 截图 + 热重载(`tools/wk_acceptance.sh`)

关键行摘录(共 30 条断言):

```
$ bash tools/wk_acceptance.sh
== build ==
✓ dist/hnweb-wk-macos-arm64
✓ dist/hn-daemon +QuickJS
✓ dist/hn
PASS probe loaded + bridge
PASS probe transparent window is opaque=0
PASS direct snapshot PNG 200x120 red + alpha (center=(255,0,0) min_alpha=0)
PASS open returns webview backend
PASS open manifest overrides CLI size
PASS open transparent from manifest
PASS open title from manifest
PASS daemon spawned WebView shell (hidden=1, manifest size)
PASS list shows backend webview
PASS daemon shot PNG 200x120 red (center=(255,0,0) min_alpha=0)
PASS WebView shell reloaded after atomic update
PASS post-update shot PNG 200x120 green (center=(0,255,0) min_alpha=255)
PASS update changed snapshot pixels
PASS window child exited
PASS headless shot PNG 200x120 green (center=(0,255,0) min_alpha=255)
PASS daemon exited 0, socket removed, no residual children
PASS visible window open→close 173ms (≤2000ms)
PASS visible daemon exited 0, socket removed
== ALL PASS (30 assertions) ==
```

构建: `bash tools/build-wk-macos.sh` → `dist/hnweb-wk-macos-<arch>`(架构感知产物名); `-std=c99 -fblocks` —— `-fblocks` 是 WKSnapshot completionHandler 需要的 clang C 扩展, 仍是零 ObjC 编译单元、零链接框架(WebKit/AppKit 经 dlopen)。daemon/CLI: `bash tools/build-daemon-cli.sh` → `dist/hn-daemon` / `dist/hn`。
