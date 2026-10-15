---
name: hn
description: 用 hn CLI 在 macOS 桌面上开原生渲染应用 —— 写 HTML/CSS 得到真窗口(非 WebView), sys:// 系统数据零网络, hn.fetch 网络层, hn-md/lottie/canvas/three 富媒体。agent 感知(open/text/event/shot)与驱动闭环。
---

# hn — 原生渲染桌面应用 CLI

写 HTML/CSS → `hn open` → **真正的原生窗口**（自研 C99 引擎光栅，非浏览器非 WebView）。
应用就是磁盘上的一个 `.html` 文件，秒开秒销毁，保存即热更新。

## 核心 AI 工作流（感知 → 修改 → 验证）

```bash
hn-daemon &                      # 1. 常驻宿主（没起会提示"daemon 未响应"）
hn open myapp app.html           # 2. 打开应用（缺省 headless；加 --window 开真窗口）
hn shot myapp out.png            # 3. 截图 → 看渲染结果（视觉反馈闭环）
hn text myapp --element title    # 4. 读某元素文本（比读整棵 DOM 省 token）
hn event myapp click --target go # 5. 注入点击（等价用户操作，触发 hx-*/JS）
hn update myapp app.html         # 6. 改完文件热更新（窗口与状态保留）
hn list && hn close myapp        # 7. 列出/销毁
hn dump myapp                    # 绘制指令数（管线健康自检）
```

`hn event` 三态回包：`"JS 处理器"`=有监听器被调；`"已派发但无人监听"`=target 选错；`"目标未找到"`=id 不存在。
支持事件：`click dblclick mousedown mouseup mousemove mouseenter mouseleave keydown keyup focus blur input change submit scroll`。

## 应用骨架

```html
<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="hn-window"  content="480x700@120,100">  <!-- 尺寸@位置, 可省 -->
<meta name="hn-title"   content="我的应用">
<meta name="hn-theme"   content="dark">              <!-- 设计令牌基座, 零 CSS 也精致 -->
<meta name="hn-surface" content="window|popup|layer">
<meta name="hn-transparent" content="1">             <!-- 透明背板(桌宠/HUD) -->
</head><body>
  <div class="card">
    <div hx-get="sys://cpu" hx-trigger="load every 3s">…</div>
  </div>
</body></html>
```

## 数据获取

**本地系统数据（sys:// 桥, 零网络）**：`sys://cpu memory disk uptime host battery info`
`sys://store/get?key=k` / `hx-post="sys://store/set?key=k"`（本地 KV 持久化）
`sys://clipboard/get` `/ hx-post="sys://clipboard/set"` · `hx-post="sys://notify"` · `sys://open?url=…`

**网络（http(s)://）**：
```html
<div hx-get="https://api.example.com/data" hx-trigger="load" hx-target="out">…</div>
```
GET 自动拼表单 query；POST 自动带 `application/x-www-form-urlencoded`。

**JS（QuickJS, 可选）**：
```js
hn.fetch(url, {method:'POST', headers:{...}, body:'...'})   // → Promise
  .then(r => r.json())                                       // r.ok/status/headers/text()/json()
  .catch(e => e.status);                                     // 4xx/5xx reject 带 status
```
另有 `hnSetText/hnSetValue/hnCanvas2D/hnMeshParam/hnWasm*` 桥；`addEventListener`/定时器/RAF 可用。

## CSS 子集（写错了不报错, 只是无效）

flex（方向/对齐/gap/grow）/ block / inline-block / absolute+z-index / overflow 滚动；
盒模型 / 圆角(含 50%) / linear-gradient / box-shadow / opacity / transition / cursor；
`#hex rgb() hsl() transparent` / px pt % em rem vw vh / 免单位数值（`padding:16` ≡ 16px）；
`:hover :active :focus :nth-child` / `@media (min|max-width)`。
**没有**：grid / float / `:not()` / 伪元素 / @font-face（边界如实声明, 路线图上）。

## 动画（声明式, 引擎插值）

```css
.card { transition: 200ms; }                          /* 属性变化自动过渡 */
.card:hover { translate: 0 -2; background: #222; }
@keyframes spin { from { rotate: 0; } to { rotate: 360; } }
.r { animation: spin 3s linear infinite; }
```
`@keyframes` 可用字段：rotate / rotateX / rotateY / translate-x/y / scale / opacity / background / color。
3D：`transform: rotate(45deg) scale(1.2)` + `perspective: 400`（父或自身）。

## 富媒体（agent 母语）

| 能力 | 声明 |
|---|---|
| Markdown/JSON/CSV 直染 | `<div hn-md>…原文…</div>` / `hn-json` / `hn-csv`（源文本装载即变 DOM） |
| Lottie | `<img src="a.json" hn-lottie hn-lottie-fit="contain">` |
| 网格变形(Live2D 感) | `<img src="c.png" hn-mesh="12x10" hn-mesh-sway="6">` |
| Canvas 2D | `<canvas>` + `<script>` JS glue |
| 3D(three.js 子集) | `<div hn-three hn-three-geo="sphere" hn-three-tex="t.png" hn-three-camera-orbit>` |
| 透明桌宠 | `hn-transparent=1` + `hn-click-through` + `hn-level` |

## 调试纪律（界面不对时不靠肉眼猜）

```bash
hn shot myapp x.png     # 先看渲染结果
hn text myapp --element sel   # 读元素文本验证内容
hn dump myapp           # 绘制指令数(0 或暴增 = 管线问题)
hn event myapp click --target btn   # 注入事件验证交互
```

判断要点：单行文本 runs 应为 1-3 段（逐字符成段=碎裂 bug）；同一文字不应在指令里出现多次（重影）。
