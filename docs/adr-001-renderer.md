# ADR 001: 统一绘制引擎的选择

状态: 已接受(渐进实施) · 日期: 2026-09-17 · **校准: 2026-10-13**

> 校准注: 本文档保留 2026-09-17 的原始决策记录与候选评估(**曾评估**, 见下),
> 但"背景"一节的当时现状(WebKit 兜底 / Linux-Windows 无绘制)已被后续实现推翻 ——
> 终态见文末"校准: 2026-10-13 的终态"。历史过程不删, 结论按现状重述。

## 背景（2026-09-17 当时 —— 已过时, 见文末校准）

引擎产出的是**14 种指令的纯数据显示列表**(RECT/TEXT/IMAGE/CLIP_PUSH/CLIP_POP/
CLIP_PATH_PUSH/QUAD/POLYGON/MESH/BITMAP/CLEAR_RECT/PDATA/PATTERN/TRIS)。
当时各路径:

| 路径 | 绘制 | 现状(当时) |
|---|---|---|
| macOS | CoreGraphics + CoreText | 完整, 原生 |
| 兜底 | 系统 WKWebView | 可用但重(有渲染子进程) |
| Linux/Windows | **无** | 只有 `hncore`(显示列表文本输出) |

Linux 桌面图形栈碎片化(X11/Wayland/GTK/Qt 各成体系), Windows 的
Direct2D 又重且绑定系统版本 —— 所以跨平台绘制需要一个"不依赖桌面栈"的方案。

## 候选评估（曾评估; 保留历史记录, 结论见文末）

### Skia(工业标准: Chrome/Flutter/Android 在用) — 曾评估, 未采用
- 优点: 极完整(文字/合成/GPU/滤镜), 品质上限最高
- 成本: **C++ + GN/ninja 构建体系**, 与本项目的 zig 交叉编译链不兼容;
  二进制 10-50MB; 引入后"零外部依赖"的承诺即告终
- 结论: 能力过剩 —— 当时的显示列表已有 14 种指令, 仍用不到 Skia 的 95%

### cairo — 曾评估, 已采用(可选后端)
- C, 成熟, 各发行版都有包
- 成本: 仍是外部依赖; 渐变/阴影支持有限(阴影要手写);
  API 面向"画布"而非"指令重放", 与显示列表模型不贴合
  (后两条在 2026-09-19 实测修正, 见文末"补充")

### 自写软件光栅器(hnsoft)✅
- 成本: 显示列表只有 14 种指令 —— 圆角矩形(SDF 抗锯齿)、线性渐变、
  简单阴影、矩形裁剪, 全部是几百行 C 可覆盖的范畴; 文本用 **FreeType**
  (Linux/Android/Chrome 字体光栅化的事实标准, 静态库随处可得)
- 收益: **零外部 UI 依赖**(文本的 FreeType 除外且可按平台取舍),
  `hncore render file.html out.png` 在任何平台可用 —— 服务器端渲染
  截图、CI 视觉回归、Linux/Windows 桌面绘制全走同一条代码
- 风险: 品质上限低于 GPU 管线; 大面积滚动会比合成器慢
  → 缓解: 软件路径面向"静态渲染/低频更新"; 高频交互仍走平台原生路径

## 决定（2026-09-17 原始决定）

1. **显示列表就是绘制后端的接缝** —— 已有 `hn_cmd` 纯数据结构, 谁消费都行。
   新增 `hnsoft`(纯 C 软件光栅器) 作为 Linux/Windows/无 GUI 场景的后端。
2. **macOS 继续用 CoreGraphics**(原生质量与性能优先), 不强行统一到软件渲染。
3. **Skia 暂不引入**: 当前指令集用不到; 架构上"显示列表 → 后端"的接缝已存在,
   若未来需要 GPU/复杂合成, 在同一接缝插入 Skia 后端即可, 不必现在付成本。
4. 文本: FreeType(CJK 支持, 静态库可链接)。无 FreeType 的目标平台
   以 `HN_NO_TEXT` 编译, 输出无文本版本(布局不受影响)。

## 验证标准

- `hncore render examples/dashboard.html out.png 520 692` 在 macOS/Linux/Windows
  产出视觉一致的 PNG(同一渲染代码, 同一显示列表)
- 不新增 Skia/cairo 依赖; macOS 构建仍不链接 WebKit
  (后续: cairo 成为**可选**后端, 见文末; macOS 面确实从未链接 WebKit)
- 引擎回归断言保持全绿(绘制后端变化不影响解析/布局)

## 后续

- 高频交互若需要 GPU 合成, 评估在接缝处加 Skia/Metal 后端(接口已预留)
  → **已落地(2026-10)**: 接缝以 `hnp_gpu_host` **原语表** + Metal v1 后端
  实现, 未引入 Skia(见文末校准)。
- `hnsoft` 文本质量跟进: 亚像素定位、粗体模拟、字距

## 实施状态

`hnsoft.c` 已实现并通过验证：
- 圆角矩形 SDF 抗锯齿、线性渐变、多层扩边阴影近似、轴对齐裁剪栈
- FreeType 文本光栅化 + 字形缓存（中文/英文/日文均可，系统字体路径探测）
- PNG 编码（存储型 deflate + CRC32，合法格式）
- `hncore render examples/dashboard.html out.png 520 692` 实测通过
  （中文清晰可读、进度条/圆角/边框正确、mmx 视觉验收确认）

---

# 补充(2026-09-19): cairo 作为跨平台绘制后端落地

原评估对 cairo 的两条判断, 实测后需要修正:

| 原判断 | 实测 |
|---|---|
| "渐变/阴影支持有限(阴影要手写)" | 渐变完全没问题;**阴影确实没有内置模糊**, 用"渲到 1/N 小面再放大 + BILINEAR"近似(约 20 行) |
| "API 面向画布而非指令重放, 与显示列表模型不贴合" | 不成立。显示列表到 cairo 是直接的一对一映射, `hn_cairo.c`(898 行)覆盖全部 |

**为什么最终还是接了 cairo**: 原决策选了 hnsoft, 前提是"每个平台各写一份太麻烦"这个
代价还没付。等 Windows 运行时真的写出来, 代价就显形了 —— hnsoft 自己承认的妥协
(阴影 6 层扩边近似、圆角裁剪退化为轴对齐矩形、PNG 无压缩)就是"手写"的账单。
cairo 把这三项换成生产级实现, 并且给 macOS/Linux/Windows/iOS/Android 同一份代码。

**接入方式**: `hncairo_render` 与 `hnsoft_render` **同签名**, 所以 Windows 运行时
(`tools/hnwin.c`)、无头 `hncore`、PNG 输出全都只调一个宏, 换后端不改调用方。
`-DHN_USE_CAIRO` 编译期切换。

**文本仍走 FreeType 而不是 cairo 的字体后端**: 布局期用 FreeType 的字形 advance
排布, 绘制若换另一套字体后端, 字形会落出排好的行盒。测量与绘制必须同源。

**实测差距**: 同一份文档 cairo 输出 PNG 31 KB, hnsoft 1007 KB(存储型 deflate 不压缩)。
(2026-10-13 复测 `hncore-cairo-macos-arm64 renderc examples/dashboard.html 460 560`
实测 31.1 KB, 一致。)

**踩到的三个不显然的坑**(都在 `hn_cairo.c` 注释里):
1. 渐变轴必须跨**矩形自身**外接盒。建在画布上时矩形左端落在渐变 61% 处 ——
   看着"渐变能用", 颜色全错。
2. `cairo_pattern_set_matrix` 期望 **user→pattern** 矩阵。传反了不报错, 只会静默
   画出错的贴图(采样点落到三角形外 → 整块透明)。
3. `cairo_new_sub_path` 是"追加子路径"。不清空路径的话, 后一条指令的 fill 会把
   前面所有形状一起涂掉 —— 症状是"第二个矩形一出现, 第一个就变色"。

**保留 hnsoft 的理由**: 零外部依赖的承诺对"任何有 C 编译器的平台都能出 PNG"仍然
成立(服务器端渲染、CI 视觉回归、无 cairo 的环境)。两者并存, 按场景取舍。

---

# 校准(2026-10-13): 今天的事实终态

上面 2026-09-17 的"背景"与"决定"已完全过时。按今日代码逐项重述:

## 决策重述(终态)

1. **渲染路径只有自研 C99 一条**。系统 WebView(WKWebView/WebView2/WebKitGTK)
   与 Swift 层已于 c49cfbd 全部删除; "WKWebView 兜底"**从未进入终态**, 不在任何
   产物里(可 `otool -L dist/hn-daemon` 复核: 只链 FreeType + libSystem)。
2. **绘制后端三档, 同一显示列表接缝**:
   - `hnsoft`(软件光栅, 默认; `Sources/CHtmlNative/hnsoft.c`)—— 全平台,
     零外部依赖(文本的 FreeType 可按 `HN_NO_TEXT` 取舍);
   - `hn_cairo.c`(**可选**, `-DHN_USE_CAIRO`; 与 hnsoft 同签名, 一份实现喂
     所有平台, 真模糊阴影/任意路径裁剪/真 zlib);
   - `hnp_gpu_host` **GPU 原语契约** + `hnp_gpu_metal.c`(Metal v1, macOS;
     引擎侧 `hn_gpu.c` 做显示列表 → 四边形批翻译, 平台只出原语; 覆盖不了的
     指令整帧回退 hnsoft; `HN_GPU=metal` 显式开启)。
3. **Skia 未采用** —— 原"接缝预留"的预判正确: 接缝真的被用上了, 但走的是
   "平台原语表 + 引擎画法"的自研 GPU 契约, 不是引入 Skia。候选评估段的其余
   历史判断保持不变。
4. **文本仍走 FreeType**(测量与绘制同源; GPU 后端也 1:1 抄 hnsoft 的字形口径)。

## 今日验证标准(替代原文)

- `hncore render` / `renderc` 同一份显示列表两条绘制路径可对照(hnsoft vs cairo);
- GPU 后端与 hnsoft **同页逐像素色差 ≤16/255**(探针 `tools/gpu_probe.sh` 实测
  ≤1/255; 不支持指令整帧回退后逐位一致);
- 跨架构确定性门禁(同 HN_NO_TEXT 口径下布局输出逐字节一致)在
  `tools/build-multiplatform.sh` 里守着;
- macOS 构建仍不链接 WebKit/Swift(链依赖实测只有 FreeType + libSystem)。
