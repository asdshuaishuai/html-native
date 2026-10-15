---
name: html-native-dev
description: html-native 引擎开发技能 —— 构建/探针门禁/架构地图/环境钮/贡献纪律。改引擎、跑回归、加功能前必读。
---

# html-native 开发技能

C99 自研渲染引擎（Neb）：HTML/CSS 子集 → 布局 → 显示列表 → 软件光栅/Metal 直呈。
无浏览器、无 WebView、零编译期依赖（系统库全部运行期 dlopen）。

## 构建命令

```bash
bash tools/build-daemon-cli.sh        # 本机 daemon + hn CLI(QuickJS 可选探测)
bash tools/build-pe-dev.sh            # PainterEngine + canvas_ity/CG + neb3 开发目标
bash tools/build-cubism-dev.sh        # Cubism(Live2D) 目标 —— SDK 缺席时跳过
ZIG=zig bash tools/build-multiplatform.sh   # 三平台交叉矩阵 + 跨架构确定性门禁
```

- **Live2d/Cubism 是可选项**：`vendor/cubism` 整目录删除后 daemon/CLI 构建与全部核心探针照常通过（`hn_cubism.c` 无 `HN_HAVE_CUBISM` 宏时安全降级）。只有 `build-cubism-dev.sh` 需要它。
- 常驻开发二进制在 `dist/`；`hn`/`hn-daemon` 已安装到 `~/.local/bin`。

## 探针门禁（改引擎后按面跑）

| 面 | 命令 | 覆盖 |
|---|---|---|
| 网络 | `bash tools/http_probe.sh` | hx GET/POST、白名单、重定向、hn.fetch resolve/reject |
| 系统桥 | `bash tools/sysroute_probe.sh` | sys:// 路由 + stderr 诊断 |
| Canvas | `bash tools/canvas_probe.sh` + `bash tools/canvas_backend_probe.sh` | 账本 178 断言 + ity/CG 后端全变体 |
| 3D | `bash tools/three_probe.sh all` + `bash tools/quad3d_probe.sh` | 纹理缓存生命周期(ASan) + CSS 3D |
| GPU | `HN_GPU=metal bash tools/gpu_probe.sh` | 指令翻译像素对照 + readback 基准 |
| 布局/CSS/HTML | `bash tools/{layout,css,html}_probe.sh dist/hncore-macos-arm64` | 盒几何/规则/解析 |
| 性能 | `bash tools/perf_probe.sh` | 真窗 CPU/RSS/alloc-churn(修正后口径) |
| Linux 对账 | `ZIG=zig bash tools/linux_parity_check.sh` | 交叉编译 + 符号 + 字段消费 |

已知红：`timer_probe` 8c 两条（GIF ambient 像素）为 HEAD 既有问题，非回归。

## 架构地图（改哪里看哪里）

```
Sources/CHtmlNative/    引擎核心(C99)
  hn_html/css/style/layout/paint   解析→级联→布局→显示列表
  hnsoft.c               软件光栅(SDF/渐变/多边形/网格/FreeType 文本)
  hn_gpu.c               GPU 指令翻译 + 帧事务(g_failed 整帧回退) + 直呈入口
  hn_canvas*.c/cpp       Canvas 2D 账本 + ity/CG 后端(槽位绑定 context)
  hn_three.c             three.js 子集(软件路径 world 变换已进投影; orbit 按下拖拽)
  neb3_soft.c            neb3 C ABI 兜底(诚实失败 → hn_three 软件路径)
rt/hn_rt.c              运行时: 帧循环/JS(QuickJS)/定时器/事件/hx/sys/异步 hn.fetch
rt/hn_http.c            HTTP transport(libcurl dlopen; 手动重定向逐跳白名单)
rt/hn_sys.c             sys:// 单源路由(daemon 与 rt 共用)
platform/hnp_gpu_metal.c  Metal 后端: 离屏 readback + CAMetalLayer 直呈
app/hn_app.c            唯一主循环; HN_GPU_PRESENT=1 直呈装配
tools/hn_daemon.c       常驻宿主(select 事件循环 + 真实时钟帧推进)
```

## 环境钮（新）

| 钮 | 作用 |
|---|---|
| `HN_GPU=metal HN_GPU_PRESENT=1` | 不透明窗 CAMetalLayer 直呈（presented 帧零 readback；退出打统计行） |
| `HN_FETCH_ALLOW=host1,host2` | HTTP 主机白名单（子域匹配；重定向逐跳重校验） |
| `HN_FETCH_TIMEOUT_MS` | HTTP 超时（默认 20000） |
| `HN_CANVAS_BACKEND=ity\|cg\|system` | Canvas 后端选择（缺省账本重放） |
| `HN_ANIM_FPS=16\|30\|60` | ambient 节拍 |
| `HN_RT_RENDER_TRACE=1` / `HN_GPU_TRACE=1` | 渲染/直呈诊断 |

## 贡献纪律

1. 探针先行：先让断言红，再修到绿。
2. 失败必须传播：GPU 帧事务 / Canvas 能力预检 / transport 错误 —— 静默缺笔比回退更糟。
3. 外科手术式改动：每一行改动都能追溯到需求；不顺手重构。
4. OBJC autorelease 对象跨帧持有必须显式 retain（drawable/cmdbuf 教训）。
5. 全量回归后再宣称完成（上表命令 + `git diff --check` + `bash -n tools/*.sh`）。
