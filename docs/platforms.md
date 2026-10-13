# platforms.md — 平台能力矩阵(同一契约, 各平台实现)

Neb(NO-WEB)的跨平台策略不是"每个平台移植一遍", 而是**契约先行的抽象收口**:
能力先定义成 C 头文件里的一张回调表/一组函数, 平台只提供实现, 引擎与上层
(app 壳 / daemon / 探针)只认契约。本文是这个格局的**总账**: 一张表把
"同一契约、各平台实现"逐格钉住 —— 每格都按代码现状逐项核实, 不是意愿清单。

> 校准: 2026-10-13(GPU 契约与视频图层两段按当日代码补入; 断言数为实测)。

图例: ✅ 实现(有回归锚) · ⚠️ 近似/条件(注明差距) · ❌ 路线图(契约已留位)

## 能力矩阵

| 能力契约 | macOS | Linux/X11 | headless | Windows |
|---|---|---|---|---|
| **窗口 flags**(契约: `platform/hn_platform.h` `hnp_window_desc`; 声明: 文档 meta, 解析: `hn_context.c` `hn_doc_manifest`; 装配: `app/hn_app.c`) | | | | |
| · floating(悬浮, `hn-level="floating"`) | ✅ `setLevel:` NSFloatingWindowLevel(`hnp_macos.c:205`) | ✅ `_NET_WM_STATE_ABOVE`(`hnp_linux.c:369`) | ❌ 无物理窗(字段按契约优雅忽略) | ❌ 新架构无 `hnp_win32.c`; 旧壳 hnwin 无层级 |
| · desktop(桌面级, `hn-level="desktop"`) | ✅ kCGDesktopWindowLevel(`hnp_macos.c:206`) | ✅ `_NET_WM_STATE_BELOW`(同上:370) | ❌ 同上 | ❌ 同上 |
| · sticky(跨空间, `hn-spaces="all"`) | ✅ `setCollectionBehavior:` CanJoinAllSpaces(`hnp_macos.c:208`) | ✅ `_NET_WM_STATE_STICKY`(`hnp_linux.c:371`) | ❌ 同上 | ❌ 同上 |
| · borderless(无框, `hn-frameless`) | ✅ styleMask=0(`hnp_macos.c:188`) | ✅ `_MOTIF_WM_HINTS`(`hnp_linux.c:376`) | ❌ 同上 | ⚠️ 旧壳仅 WS_EX_LAYERED(透明), 无独立无框位 |
| · no_shadow(去投影, `hn-shadow="false"`) | ✅ `setHasShadow:NO`(`hnp_macos.c:209`) | ⚠️ EWMH 无标准对应, 显式优雅忽略(`hnp_linux.c:389`) | ❌ 同上 | ❌ 同上 |
| · click_through(穿透, `hn-click-through`) | ✅ `setIgnoresMouseEvents:` + 运行期切换(`hnp_macos.c:211,290`) | ✅ XShape 输入形 + 运行期切换, Xext 缺席优雅降级(`hnp_linux.c:412,463`) | ❌ 同上 | ❌ 同上 |
| · transparent(逐像素透明, `hn-transparent`) | ✅ `setOpaque:NO`+clearColor(`hnp_macos.c:201`) | ⚠️ 24 位窗不可 ARGB, 声明被忽略(`hnp_linux.c:338`); 32 位 visual 是路线图 | ⚠️ 字段记账, 无表面可透 | ⚠️ 旧壳 WS_EX_LAYERED 近似 |
| · x,y 开窗位(`hn-window WxH@x,y`) | ✅ 非 AUTO 即 `setFrameOrigin`(`hnp_macos.c:232`) | ✅ 非 AUTO 即 XMoveWindow(`hnp_linux.c:400`) | ❌ 无屏幕坐标 | ❌ 同上 |
| · origin 读回(几何记忆的采样口, `hnp_window_origin`) | ✅(`hnp_macos.c:301`) | ✅ XTranslateCoordinates(`hnp_linux.c:472`) | ❌ 恒返 0, 调用方整段跳过(`hnp_headless.c:73`) | ❌ 同上 |
| **媒体**(契约: `hn.h` `hn_media_host`; 装配: `app/hn_media_platform.c`; 合成替身单一来源: `app/hn_media_synth.c`) | | | | |
| · 播放(<video>/<audio> 状态机出声/出帧) | ✅ AVFoundation 运行期 dlopen(`hnp_media_macos.c`, `supported()=1`) | ⚠️ synth 合成替身(梯度帧+假时钟, 确定性可断言; 真解码是路线图) | ⚠️ 同 Linux(synth) | ❌ 旧壳未装配宿主(c->media==NULL → 占位 RECT) |
| · 抽帧(`host.frame` → HN_CMD_BITMAP) | ✅ AVPlayer NV12→RGBA(`hnp_media_frame`) | ⚠️ synth 64x64 梯度帧 | ⚠️ 同 Linux | ❌ 无帧(占位) |
| · 音频出声 | ✅ AVPlayer 直出 | ⚠️ synth 不出真声 | ⚠️ 同 Linux | ❌ |
| **视频图层**(契约: `hn_platform.h` 尾部 `hnp_video_layer_attach/frame/detach`; 装配: 壳/hn_rt 媒体路径) | ✅ **AVPlayerLayer 原生图层直通**(`hnp_macos.c:450` attach/frame/detach): 解码帧由系统合成器直接合成, 像素不过 CPU; `media_window_probe` 实测图层模式 10s CPU 0.30 cores(硬线 <60%), `HN_MEDIA_LAYER=0` 退回 CPU 逐帧 | ✅ **Xv 原语就位**(`hnp_linux.c:889` libXv 运行期 dlopen: grab 端口 + XvImage 图层子窗 + XvPutImage 摆位; libXv 缺席/无端口 attach 返 0 → CPU 逐帧兜底); ⚠️ **行为级待验**(需 Linux+X11+libXv 主机; `linux_parity_check.sh` 到符号级) | ❌ 无表面(离屏路径恒 CPU 拉帧, 确定性口径) | ❌ 路线图 |
| **GPU**(契约: `hn_platform.h` 尾部 `hnp_gpu_host` 原语表; 画法: `Sources/CHtmlNative/hn_gpu.c`; 宿主注入: `hn_rt_set_gpu_host`, 壳开关 `HN_GPU=metal`) | ✅ **Metal v1**(`hnp_gpu_metal.c`, 682 行): 运行期 dlopen + MSL 运行期编译, 链接期零 framework; `gpu_probe` 7 项断言(与 hnsoft 逐像素色差 ≤1/255, 契约容差 ±40 收紧到 ≤16); 当前为**离屏 readback** 口径(直呈是路线图) | ❌ 路线图(GL/mesa, 同一原语表; `hnp_linux.c` `hnp_gpu_backend()` 返回 NULL → 恒 hnsoft) | ❌ 无(GPU 后端返 NULL, 呼出方回退 hnsoft) | ❌ 路线图(D3D11) |
| **文本渲染**(FreeType; `hnsoft.c`) | ✅ homebrew FreeType(macOS 产物带 `+文本`; daemon/hncore-macos-arm64 实测) | ⚠️ 引擎同一份, 交叉产物 `-DHN_NO_TEXT`; 部署平台接系统 FreeType 即开启 | ⚠️ 同 Linux(hnapp-headless 本机构建带 FreeType) | ❌ vcpkg 路线(构建脚本注释已留位) |
| **3D/QUAD**(`hn_mesh.c`, `HN_CMD_QUAD`, `hn-mesh-*`) | ✅ 引擎级 | ✅ 引擎级 | ✅ 引擎级 | ✅ 引擎级 |
| **canvas / GIF / Lottie**(`hn_canvas/hn_gif/hn_lottie/hn_json.c`) | ✅ 引擎级(全平台同源同份代码, 跨架构确定性由构建自检钉住) | ✅ 引擎级 | ✅ 引擎级 | ✅ 引擎级 |
| **桌面常驻**(resident: daemon `resident` op + `~/.html-native/persist/resident.json` 账本 + 启动自动重开) | ✅(`resident_probe.sh` 16 断言) | ⚠️ daemon/壳候选表全 POSIX 同源(`hnapp-linux-*` 已在 win_shell_path 探测表), 未经真 Linux 桌面回归 | ❌ 无窗可常驻(回退 headless) | ❌ daemon 是 POSIX(unix socket/posix_spawn), 未移植 |
| **几何记忆**(`app/hn_app.c` geo_* + origin 读回落盘 + 开窗读回) | ✅(`resident_probe.sh` 钉住 geometry 文件) | ⚠️ origin 读回已实现, 记忆链路待真 Linux 桌面回归 | ❌ origin 恒 0, 整链按契约跳过 | ❌ |

注: "引擎级" = 能力实现在 `Sources/CHtmlNative/`(引擎公共 API 一份代码),
随引擎进每一个产物 —— 三平台交叉产物由 `tools/build-multiplatform.sh` 的
跨架构确定性自检(布局输出逐字节一致)钉住, 不存在平台分叉。

## 回归锚

| 锚 | 覆盖 |
|---|---|
| `tools/media_element_probe.sh` | synth 宿主行为口径(梯度帧逐字节/假时钟/状态机)—— 40 断言 |
| `tools/media_probe.sh` | macOS 真宿主(AVFoundation)18 断言(实测 18 ok 0 fail) |
| `tools/media_window_probe.sh` | 真窗 + AVPlayerLayer 图层模式(CPU 硬线/截图帧差)+ synth 降级 —— 30 断言 |
| `tools/gpu_probe.sh` | GPU 契约: Metal vs hnsoft 逐像素色差 + 不支持指令整帧回退 + readback 口径 —— 7 项断言(无 Metal 设备时 SKIP) |
| `tools/window_probe.sh` / `tools/resident_probe.sh` | daemon 开窗(壳候选表)/常驻/几何记忆 25+16 断言 |
| `tools/linux_parity_check.sh` | Linux 编译级+符号级对账(含 Xv 视频图层原语三函数/窗口字段消费) |
| `tools/build-multiplatform.sh` 自检 | 跨架构确定性 + 布局不变量 |

## 架构原则(表尾三行)

1. **契约在 `platform/hn_platform.h` / `hn.h` 的 `hn_media_host` / 引擎公共 API
   —— 平台只有实现差异。** 上层(hn_app / hn_daemon / 探针)只 include 契约头,
   换平台 = 换链接的 `hnp_*.c`, 调用点零分支。
2. **实现缺席时的降级也写进契约**: 媒体缺真宿主 → synth 合成替身(不是空);
   窗口能力缺失 → 字段优雅忽略(声明是意图, 物化看平台); daemon 壳候选全缺席
   → 回退 headless 并在响应标注 —— 每一档降级都有探针钉住。
3. **本矩阵是抽象收口的跟踪账**: 每收口一处"写死在某平台"的实现(如
   `win_shell_path` 平台探测表、`hn_media_synth` 单一来源), 就在这里升格一格;
   新增平台能力先补契约与本表, 再写实现。

## GPU 契约的定位(平台出原语, 引擎出画法)

GPU 是这张矩阵里**最新收口的一格**(2026-10 落地), 也是"契约先行"的又一实例:

- **平台只出原语**: `hnp_gpu_host` 定义设备生命周期(init/shutdown/begin/
  end_readback)、纹理(create/update/free)、四边形批绘制(draw_quads:
  顶点 [x,y,u,v] 周界序 + 可选纹理)、轴对齐裁剪栈(clip_push/pop)—— **没有
  一条语义是"画一个圆角矩形"**。平台实现(`hnp_gpu_metal.c`)只做这些。
- **引擎出画法**: 显示列表 → 四边形批的全部翻译在 `Sources/CHtmlNative/hn_gpu.c`
  —— 渐变烘焙 256×1 LUT、文本走 FreeType 1:1 字形纹理、圆角/阴影这类
  四边形原语覆盖不了的指令由 `hn_gpu_available` 如实报告, 呼出方(hn_rt)
  **逐帧静默回退 hnsoft** —— 所以任何平台实现不到位都不会出伪影, 只会慢。
- **像素口径与 hnsoft 等价**: 探针(`gpu_probe.sh`)要求同页逐像素色差
  ≤16/255(实测 ≤1/255); 预乘/非预乘换算链逐条写在 `hn_gpu.c` 头注释。
- **平台实现矩阵**(契约头注释同源): macOS = Metal v1 ✅ ·
  Linux = GL/mesa 路线图 · Windows = D3D11 路线图 · headless = 无(supported=0)。
- **未来**(路线图): 直呈(end_readback 之外的上屏路径)、圆角/任意裁剪的
  GPU 化、合成层缓存 —— 都在同一原语表上增量, 不动引擎语义。
