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

## 窗口模型与定位（核心概念）

**两种开法，分工不同**：

| | headless（缺省） | `--window`（真桌面窗） |
|---|---|---|
| 进程 | daemon 内无头槽位 | daemon spawn 平台壳，**每窗一进程** |
| 用途 | agent 的渲染/截图/取证管线 | 给人看的桌面窗 |
| `shot/text/dump/event` | ✅ 全支持 | ❌ `unsupported for window apps`（文档在子进程里，daemon 无句柄） |
| 热更新 | `hn update`（整文档替换） | `hn update`（新内容**原子写回源文件**，子进程 mtime watch ~300ms 自行重载；进程与窗口不动） |
| 关闭 | `hn close`（销毁槽位） | `hn close`（SIGTERM+waitpid 回收，关窗即退） |

**agent 典型双轨**：同一份 HTML，先 headless 打开（shot 看效果、text 验内容、event 驱动），调好后 `--window` 开给人看。

**窗口定位 = 声明 + 自动记忆**（没有运行时 move 子命令；定位是声明式的）：

```html
<meta name="hn-window" content="480x700@120,100">   <!-- 宽x高@屏幕x,y（左上原点）; @x,y 可省 -->
```

- **几何记忆（自动）**：窗口被用户拖动/resize 后，位置(和 window 表面的尺寸)落盘到
  `~/.html-native/geometry/<文件名去扩展名>.json`；下次同一文件打开**回到原位**。
  记忆键是文件名不是 app id —— 换 id 开同一页面仍回原处。
- 优先级：槽位记忆（hn-applet）> 页面声明 `@x,y` > 屏幕居中。
- 多窗口 = 多次 `hn open` 不同 id（每窗一进程互不干扰）；同 id 重复 open 报 dup。

**直开模式（不经 daemon）**：`dist/hnapp-macos-arm64 app.html` 直接开真窗（开发/桌宠常用），
`--shot out.png` 离屏渲染落盘、`HN_GPU=metal HN_GPU_PRESENT=1` 启用 GPU 直呈。

**持久化与胶囊（daemon socket op）**：
- `{"op":"persist","id":"a","path":"x.hnapp"}` 打包当前应用为胶囊（内嵌 HTML + 指令说明），`{"op":"restore","path":"x.hnapp"}` 拆包还原。
- `{"op":"resident","file":"pet.html","id":"pet","w":360,"h":360}` 登记常驻 —— daemon 重启后自动重开该窗（桌宠"关不掉"的回归方式）。

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
`:hover :active :focus :disabled :enabled :checked` / `:nth-child(odd|even|N)` /
`:first-child :last-child :only-child :first-of-type :last-of-type :root` / `:not(简单选择器, 可链式)` /
属性选择器 `[attr] [attr=v] [attr^=v] [attr$=v] [attr*=v] [attr~=v]` / `@media (min|max-width)`。
`text-transform: uppercase|lowercase|capitalize`（ASCII 范围, 度量绘制同源）；
`aspect-ratio: 16/9`（一维定另一维 auto 时补齐）；`border-top|right|bottom|left` 分侧边框；
`min() / max() / clamp()`（绝对单位; 含 % 实参不支持, 如实声明）；
`visibility: hidden`（保布局不画; **子级 `visibility:visible` 可复显**, 标准继承语义）/
`pointer-events: none`（子树不参与命中）。
**伪元素** `::before` / `::after` + `content: "字面"`（装饰文本; 其余声明转节点内联样式;
`content:none`/`attr()`/计数器未做——如实声明）。
**没有**：grid / float / 伪元素 / @font-face / calc 里两个以上 % 项 / min·max 含 %（边界如实声明, 路线图上）。

## 动画（声明式, 引擎插值）

```css
.card { transition: 200ms; }                          /* 属性变化自动过渡 */
.card:hover { translate: 0 -2; background: #222; }
@keyframes spin { from { rotate: 0; } to { rotate: 360; } }
.r { animation: spin 3s linear infinite; }
```
`@keyframes` 可用字段：rotate / rotateX / rotateY / translate-x/y / scale / opacity / background / color。
3D：`transform: rotate(45deg) scale(1.2)` + `perspective: 400`（父或自身）。
box-shadow 模糊为**真高斯**（3-pass box 逼近, 无光环）; three.js 子集为
Blinn-Phong 双灯着色（高光 + 半球环境光）。

## 富媒体（agent 母语）

| 能力 | 声明 |
|---|---|
| Markdown/JSON/CSV 直染 | `<div hn-md>…原文…</div>` / `hn-json` / `hn-csv`（源文本装载即变 DOM；CSV 配 `hn-csv-sep=";"`） |
| Lottie | `<img src="a.json" hn-lottie hn-lottie-fit="contain">`；倍速 `hn-lottie-speed="1.5"`，多动画 `hn-lottie-index` |
| GIF 动图 | `<img src="a.gif">` 开箱即动；倍速 `hn-gif-speed="2"` |
| 网格变形(Live2D 感) | `<img src="c.png" hn-mesh="12x10" hn-mesh-sway="6" hn-mesh-anchor="bottom">`；`hn-mesh-look` 视线跟随指针，`hn-mesh-breath` 呼吸，`hn-mesh-idle` 待机摆动 |
| Canvas 2D | `<canvas>` + `<script>` JS glue |
| 3D(three.js 子集) | `<div hn-three hn-three-geo="box\|plane\|sphere" hn-three-tex="t.png" hn-three-spin="6" hn-three-color="#4f7cff" hn-three-camera-orbit>`（orbit = **按住拖拽**转，松手惯性） |
| Live2D/Cubism | `<img hn-cubism src="model3.json">`；动作组 `hn-cubism-motion="Idle"`、`hn-cubism-index`（需 Cubism SDK 构建的 hnapp） |
| PainterEngine 呼吸立绘 | `<img hn-live="char.png">`（呼吸/摇摆 rig, hnapp-pe 构建） |
| 透明桌宠 | `hn-transparent=1` + `hn-click-through` + `hn-level` |
| **视频/音频播放** | `<video id="v" src="x.mp4">` 真窗 AVFoundation 解码出声；JS 控制桥见下 |

**Lottie 播放器标签**（同名 JS 库的 C99 原生子集, 零脚本, 2026-10-16 起）：

```html
<lottie-player src="anim.json" speed="2" background="#0b0e13"
               style="display:block;width:476px;height:392px"></lottie-player>
<dotlottie-wc animation="anim.lottie"></dotlottie-wc>
<dotlottie-player src="anim.lottie"></dotlottie-player>
```

- `src` 支持 bodymovin JSON 与 dotLottie `.lottie` ZIP 两种格式; `animation` 是 dotlottie-wc 的别名
- autoplay 恒开 + 循环; `speed` 倍速; `background="#hex"` 底色
- **必须** `display:block` + CSS `width/height` 定盒（元素无固有尺寸, 与 canvas 同约定）
- 支持: 形状层/预合成/纯色/图片层、椭圆/矩形/路径(含变形关键帧)、填充/描边/渐变、trim/repeater/mask、**parent 骨架层链**(AE 卡通绑定手法)
- 已知边界: 表达式/文字层/轨道 matte 不支持; 渐变填充在 hnsoft 下呈色带近似

**视频/音频 JS 控制桥**（自定义播放器 UI 的完整 API, 均按元素 id）：

```js
hnMediaPlay('v');   hnMediaPause('v');   hnMediaSeek('v', 10);   // 跳到 10s
hnMediaTime('v');       // 当前播放位置(秒, -1 = 不可用)
hnMediaDuration('v');   // 总时长(秒)
hnMediaVolume('v', .5); // 音量(v<=0 同时静音)
```

## 特有标签与窗口行为

**`<include>`（零构建组件）**：文件级片段引入，可嵌套 6 层（自动检测循环）。
```html
<include src="head.html">        <!-- 引入样式/结构片段 -->
<include src="parts/row.html">   <!-- 复用 UI 块; 这是组件模型之前的复用层 -->
```

**窗口形态 meta**（写在 `<head>`）：

| meta | 作用 |
|---|---|
| `hn-surface` | `window`（普通窗）/ `popup`（不抢焦点, 消息卡）/ `layer`（浮层）——窗口开/关自带 Smart-UX 动效（首帧就绪后淡入+微缩放浮起 220ms; 关闭淡出 150ms; macOS/Windows 三平台一致） |
| `hn-presence` | `ghost` 不占系统身份（不进 Dock）; `app` 驻留 Dock —— 桌宠用 ghost |
| `hn-lifecycle` | `"launch show hide destroy"` 声明关心的生命周期事件 |
| `hn-transparent` | 逐像素透明窗口（HUD/圆形控件; html/body 不要设不透明底色） |
| `hn-frameless` | 无边框 |
| `hn-level` | 悬浮层级（桌宠置顶） |
| `hn-click-through` | 鼠标穿透 |
| `hn-shadow` | 窗口投影开关 |
| `hn-spaces` | 跨桌面空间 |
| `hn-applet` | `"clock"` 具名轻应用槽位（daemon 可枚举） |

**窗口内交互属性**：
- `hn-draggable` — 无边框/透明窗的空白区拖动（**默认开**，`=0` 关闭）
- `hn-stream` + `hn-stream-loop="14"` — 聊天式流式容器：自动贴底跟随 + 环形缓冲只留最近 N 条（agent 轨迹/日志页标配）

**表单控件**：`<input type="text|password|checkbox|radio|button|submit|reset|range">`、`<textarea>`（多行+光标）、`<button>`、`<progress value max>`（轨道+填充, 无 value = 不定态往复动画）、`<select>`（含弹层: 点击开 → 选项行列表 → 点行选中/点外关闭; 弹层内
Up/Down 移高亮、Enter 提交、Esc 关闭、滚轮滚动;
弹层内 Up/Down 移高亮、Enter 提交、Esc 关闭、滚轮滚动）开箱可用。
**Smart-UX 出现/消失**（默认开启, 无需声明）：运行时插入的内容（appendChild /
insertBefore）自动**淡入 + 上浮 8px**（200ms ease-out）浮现; JS remove 的元素
在**原位淡出 160ms** 后消亡 —— 内容不再硬闪出现/消失。首屏文档与显式
`animation:` 声明不受影响; 引擎可经 `hn_context_set_dynamic_enter(c, 0)` 关闭。
**原生默认动作**（浏览器同序: JS click 派发后、无 preventDefault 才执行）：
checkbox 点击切换、radio 同名组互斥、range 点击/拖拽设值 + **方向键调值**（聚焦后
Left/Down = -step, Right/Up = +step; 写回 value 属性）、
`<summary>` 点击切换 `<details>` 折叠（`details[open]` 属性选择器可用）。
`:focus :disabled :checked :placeholder` 生效；按钮按标签内容收缩；表单控件
UA 盒按 border-box（range 160x20 / select 160x26 / progress 200x14）；回车自动提交最近载体。

**命名实体**：常见 70 个（nbsp/amp/copy/mdash/laquo/中国标点/希腊字母等）+ 数字实体 `&#NN; &#xHH;` 全量。

## JS DOM 与 Canvas（标准 API 面, 页面 `<script>` 直接用）

```js
// 标准 DOM 读改写（写透引擎 DOM, 下一帧生效）
var el = document.getElementById('k')          // 无此节点 → null（非空包装）
el.textContent                                   // 读 = 子树拼接文本; 写 = 整体替换
el.getAttribute / setAttribute / removeAttribute / hasAttribute
el.classList.add / remove / toggle / contains   // 写透 class 属性
el.dataset.userId = '7'                          // ↔ data-user-id（camel 自动拆线）
el.style.color = 'red'; el.style.cssText         // ↔ style 属性（setProperty/cssText 同款）
el.checked / el.disabled / el.hidden             // 布尔属性语义
el.getBoundingClientRect()                       // {left,top,right,bottom,width,height,x,y}
el.offsetWidth / offsetHeight / offsetLeft / offsetTop
el.focus()
document.addEventListener / window.addEventListener  // 冒泡链尾
el.dispatchEvent({type:'custom', detail: 7})     // 合成事件(冒泡链全程)
el.click()                                       // 合成 click(含原生默认动作)
addEventListener(type, fn, {once: true})         // once/capture 支持
window.innerWidth / innerHeight / devicePixelRatio
canvas.width = 300                               // 属性写 → 位图变化 + 画布清空
```
(2026-10-17 精简裁决: querySelector/createElement/树遍历/scroll 已下线;
元素定位走 getElementById。)
事件对象含 `type/target/currentTarget/clientX/Y/key/code/detail/shift...` +
`preventDefault/stopPropagation`（与浏览器同签名）。

```js
// Canvas 2D（标准方法面 + 少量如实声明的近似）
var c = document.getElementById('cv').getContext('2d');
c.fillRect / strokeRect / clearRect / beginPath / moveTo / lineTo / arc / ellipse /
c.quadraticCurveTo / bezierCurveTo / arcTo / rect / roundRect / fill / stroke / clip /
c.fillText / strokeText / measureText / drawImage(3/5/9 参) / createLinear·RadialGradient /
c.createPattern / setLineDash / save / restore / translate / scale / rotate / set·transform /
c.getTransform() / createImageData / getImageData / putImageData / isPointInPath / toDataURL()
c.fillStyle / strokeStyle / lineWidth / lineCap / lineJoin / miterLimit / font / textAlign /
c.textBaseline / globalAlpha / globalCompositeOperation(lighter/multiply/screen) / shadow*
c.letterSpacing / wordSpacing（字距进度量与绘制） / c.reset()（清画布+状态归缺省）
c.filter = 'blur(6px)'（模糊滤镜; 引擎 3-pass 高斯; 浓度衰减在 alpha 通道——getImageData 非预乘口径）
```
边界如实声明：`isPointInPath` 按已渲染像素 alpha 判定（抗锯齿边缘 1px 级差异）；
`toDataURL` 恒出 PNG；stroke 遇 pattern 回落纯色。

## 调试纪律（界面不对时不靠肉眼猜）

```bash
hn shot myapp x.png     # 先看渲染结果（headless 应用; window 应用会如实报 unsupported）
hn text myapp --element sel   # 读元素文本验证内容
hn dump myapp           # 绘制指令数(0 或暴增 = 管线问题)
hn event myapp click --target btn   # 注入事件验证交互
```

判断要点：单行文本 runs 应为 1-3 段（逐字符成段=碎裂 bug）；同一文字不应在指令里出现多次（重影）。

## 速查：环境钮（启动 daemon/hnapp 时设）

| 钮 | 作用 |
|---|---|
| `HN_FETCH_ALLOW=host1,host2` | HTTP 主机白名单（子域匹配, 重定向逐跳重校验） |
| `HN_FETCH_TIMEOUT_MS=N` | HTTP 超时（缺省 20000） |
| `HN_ANIM_FPS=16\|30\|60` | 环境动画节拍（缺省 30） |
| `HN_RENDER_SCALE=1\|2` | 渲染缩放覆盖（真窗 Retina 缺省 2） |
| `HN_WINDOW_POS="X,Y"` | 窗口钉位（左上原点; 探针真窗钉副屏用, 不写几何记忆） |
| `HN_REVEAL=paint\|fade\|none` | 呈现动画风格（画刷铺色/平台淡入/硬出现） |
| `HN_REVEAL_MS=N` | 画刷铺色时长（缺省 480ms; 验收连拍/慢放用） |
| `HN_GPU=metal HN_GPU_PRESENT=1` | hnapp 真窗 GPU 直呈（不透明窗; 零回读） |

真窗探针钉副屏（不占开发者主屏）：

```bash
export HN_WINDOW_POS="$(bash tools/_secondary_screen.sh 360 360)"   # 单屏输出空→回落居中
```

呈现动画口径：**画刷铺色（"内容被画上桌面"）只属于逐像素透明窗**
（hn-transparent=1）—— 未铺区域 alpha=0 透出桌面；不透明窗缺省只走
平台淡入（否则会先出现一块画板再画内容, 是错误形态）。页面要用画刷
呈现时, body 不要铺全窗底色（否则画的是底色矩形）。

窗口形态口径：**hn 产出的窗口默认无标题栏**（窗口即画布）——想要系统
标题栏必须显式 `hn-frameless="0"`。无边框窗的可拖面 = 非交互内容
（hx 载体/输入控件/手型光标/可滚动区照常收点击）。
