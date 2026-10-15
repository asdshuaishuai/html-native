# three.js 类 UI 支持度矩阵

> 检验对象: html-native 引擎(C99, HTML/CSS/JS → 原生窗口;3D = QUAD 透视投影)。
> 检验页: `examples/threejs-ui.html`(920x880 深色数据大屏,四个典型 three.js UI 场景)。
> 截图: `screenshots/threejs-ui.png`(HN_CLOCK=1500)、`screenshots/threejs-ui-t0.png`(HN_CLOCK=0,立方体转 90°,证明动画在走)。
> 口径: 本引擎**不是 WebGL**。3D = CSS 3D 变换 + 元素盒四角过 4x4 矩阵 + 透视除法(k = d/(d−z))成投影四边形(QUAD),渐变/边框/文字逐像素随面片采样。
> 校准: 2026-10-13 按当日代码复核(深度排序/关键帧字段两处已更新, 见下); 2026-10-14 增补
> 软件渲染路径垂直切片(矩阵新行 + 文末"已落地/下一步"两节); 2026-10-15 增补 neb3 backend
> 一节(C ABI/dlopen 回退/MSAA/深度/透明), --advance 确定性表述按 `app/hn_app.c`
> `advance_media` 当日实现复核修正(分支判据是"synth 宿主**或**文档无 `<video>`", 此前
> 只写了后半); 2026-10-15(第二批) hn-three 推进到全量 spec(几何 box|plane|sphere /
> 轨道相机 / 纹理贴图), 软件路径可见性改为**全量发射 + 画家序**(原"背面剔除"口径作废,
> 见文末第二批"已落地"节)。
>
> 注(与 README 的索引关系): 本页是 3D/QUAD 专项; 深度排序的引擎实现见
> `Sources/CHtmlNative/hn_paint.c` 的 `paint_children_depth_sorted`(画家算法),
> 口径由 `tools/quad3d_probe.sh`(33 断言)钉死。

## 支持度矩阵

| 典型 three.js UI 能力 | 引擎等价物 | 支持度 | demo 对应节 |
|---|---|---|---|
| Scene 场景 + 对象层级 | DOM 树 + `transform-style: preserve-3d` 父子矩阵复合;flat 元素复位矩阵 | ✅(层级复合) | 03 立方体、04 卡片环 |
| PerspectiveCamera 透视相机 | `perspective` 视距 + 消失点(声明者盒中心),可声明在舞台父级或元素自身 | ✅(单一固定视点) | 01 数据条、02 舞台 |
| 网格地面 GridHelper / Plane | 普通块流元素 `rotateX(56°)`(origin 50% 100%)+ 渐变/边框;格线为 preserve-3d 子级随面片投影 | ✅(静态伪 3D 地面) | 02 舞台全景 |
| BoxGeometry 盒子几何 | 6 个 absolute 面 `rotateY/X + translateZ(半边长)`,容器 preserve-3d 复合 | ✅(手工拼六面,无自动几何) | 03 中央立方体 |
| 环形/圆周排布(对象摆上圆环) | 环容器 preserve-3d + 各卡 `rotateY(60°·i) + translateZ(半径)` | ✅(静态排布) | 04 环形仪表 |
| 渐变材质(map / vertexColors) | `linear-gradient` 逐像素画在元素平面、随 QUAD 投影(等效"先画后变换") | ✅ | 01–04 全部面片 |
| 文字贴片(PlaneGeometry + 纹理字) | 文字直接绘制在 3D 面片上,锚点与字号缩放正确,面片内近大远小 | 近似(字形本身不透视畸变) | 02 面板、03 面上字母、04 卡标 |
| 动画循环 requestAnimationFrame + 物体旋转 | `@keyframes` 采样字段(2026-10-13 实测确认): opacity / translate-x / translate-y / scale / rotate(rotate-z) / **rotate-x / rotate-y** / background(-color) / color(单数值与颜色); `infinite`;离屏用 `HN_CLOCK=<ms>` 确定性取帧 | ✅(面片旋转与底色/文字色都能逐帧插值; `transform` 简写/两值 translate 不进时间轴) | 03 立方体 rotate 0→360°/6s(两截图即 t=0 与 t=1500 的对比) |
| translateZ 近大远小(景深) | 自身 `perspective` + `translateZ(±)`,k = d/(d−z) | ✅ | 01 顶部数据条(Z −80/+10/+90) |
| 深度缓冲 / 正确遮挡 | preserve-3d 内自动深度排序(2D 仍文档序)—— 面片按中心视空间 z 从远到近发射(画家算法,稳定排序:同深保持文档序);非 3D 内容与 z-index/2D 语义不受影响;跨 preserve-3d 边界不排序 | ✅ 已落地(`hn_paint.c` `paint_children_depth_sorted`;quad3d_probe 33 断言含"交叠区像素 = 近面片遮挡远面片"与"非 3D 交错对仍按文档序"两条对照) | 03 六面遮挡、04 卡片环(已转出交错深度,不再手工排序) |
| OrbitControls 相机控制器 | `hn-three-camera-orbit`(2026-10-15): 指针拖拽驱动方位角/仰角(0.35°/px, 仰角钳 ±85°), 松手惯性 exp(−t/0.6s) 衰减; `hn-three-azimuth/elevation/dist` 显式初值(缺省反解自固定视点 eye(1.7,2.3,3.1) —— 声明本身不改变缺省观感); "抓住" = `hn_context_hit_node`(与 hover/click 同一命中语义)。无指针事件的离屏 shot 恒停初值, HN_CLOCK/--advance 确定性口径不变 | 🔶(拖拽轨道; 无滚轮缩放/阻尼参数面) | examples/threejs-sphere.html |
| 光照 / 阴影 / PBR 材质(MeshStandardMaterial、阴影贴图) | ❌ 无光照模型、无法线、无阴影计算;仅 2D 装饰性 box-shadow/text-shadow(不随 3D 变换参与光照) | ❌ | —(demo 用渐变+亮边框冒充发光材质) |
| 软件渲染路径(Scene/Camera/Mesh 语义的 CPU 几何管线) | `hn-three` 属性元素 → `hn_three.c`(2026-10-14 垂直切片 → 2026-10-15 全量): CPU 场景图 + PerspectiveCamera + **`hn-three-geo="box|plane|sphere"`**(盒 12 三角逐面六色 / 平面 1.3×1.3 双面 2 三角 / UV 球 32×16 段 960 三角, `hn-three-color="#rrggbb"` 单色覆盖) → 顶点汤;**可见性 = 全量发射 + 画家排序**(视空间质心 z 远→近, 无背面剔除 —— 凸体下与 z-buffer 逐像素同像, 离散采样无剪影缺口; 原叉积剔除口径作废, 见文末第二批"已落地");逐面 Lambert 在模块 CPU 算好进 fill;`hn-three-tex="path"` 纹理贴图走 `HN_CMD_TRIS`(资产后端解码门 → 逐角点 UV → `paint_tris` 逐像素采样, unlit);自转取节点 ext_clock(`hn-three-spin` 缺省 6s/圈, HN_CLOCK/--advance 确定性取样);可选 neb3 backend(GPU, `NEB3_LIB` dlopen, 见文末专节; 声明了 geo/orbit/tex/color 的元素恒走软件路径) | ✅ 全量 spec(单 Mesh 场景; glTF/多 Mesh/节点池仍无) | examples/threejs-demo.html、threejs-sphere.html、threejs-plane.html、threejs-pet.html |
| 3D 模型加载(glTF/OBJ + 纹理) | ❌ 无模型管线;几何 = 内置 box/plane/sphere 参数面(2026-10-15 起), 纹理可贴任意几何(`hn-three-tex`, PNG 口径);glTF/OBJ 仍无 | ❌(参数几何 + 贴图已覆盖其一小角) | — |
| 粒子系统 / Points | ❌ 无;只能用大量小元素模拟(无实例化,性能上限低) | ❌ | — |

## 引擎特有口径(本页实测踩到并绕开的坑)

1. **同一元素勿混用 2D scale 与 3D 变换** —— 自身面片不含自身 scale;要缩放用外层壳包一层。
2. **preserve-3d 旋转面片上的 absolute 子级必须自带 transform**(哪怕 `translateZ(0.02px)`)才走"自身 3D 矩阵"路径;无自身 transform 的子级按父内坐标套父矩阵,会整体脱出面片(demo 02 地板格线因此各带 translateZ)。
3. **带 translateZ 投影的面片里,子级反过来不能再带自身 transform**(会脱锚);demo 04 表盘 tick 因此不带 rotate,只用 margin-top 排弧。
4. `@keyframes` 采样 9 个字段:opacity / translate-x / translate-y / scale / rotate(rotate-z) / rotate-x / rotate-y / background(-color) / color;`transform` 简写与两值 translate 不进时间轴(2026-10-13 复核 `hn_context.c` `kf_collect`)。
5. 面内偏移不要用 `transform: translate(x,y)` 叠 3D —— QUAD 平移一次、文字锚点平移两次会脱出面片(见 `examples/threejs-lab.html` 页首注)。
6. **深度排序是面片级的**(画家算法, 按面片中心视空间 z): 互相穿插的两个面片(非前后关系)不保证逐像素正确 —— 需要逐像素深度缓冲(WebGL 语义)的场景做不到;这是"QUAD 透视投影"与真 GPU 管线的本质差别。

## 已落地(2026-10-15 第二批 · hn-three 全量 spec: 几何 / 轨道相机 / 纹理贴图)

检验对象: `Sources/CHtmlNative/hn_three.c`(本批全部改动在该文件 + demo 页 + 检验页,
接线零改动 —— `hn_paint.c` 钩子/`hn_context.c` 环境钟/构建清单均为既有);检验页:
`examples/threejs-sphere.html`(UV 球 + 轨道相机)与 `examples/threejs-plane.html`
(双面平面 + 棋盘贴图);贴图资产 `examples/assets/three-checker.png`(128×128, 8×8 棋盘,
四角方位标记: 左上红/右上绿/左下蓝/右下琥珀 = UV 朝向可直接从 shot 断言)。

- **几何(`hn-three-geo`)**: `box`(缺省; 未识别值回退 box, 与 CSS 未知值同口径)= 既有
  8 顶点 12 三角逐面六色表(审计常量未动, 惰性展开成按三角汤表); `plane` = 1.3×1.3
  双面 2 三角(PlaneGeometry 口径, 每角点 UV); `sphere` = UV 球 32×16 段
  (SphereGeometry(0.72,32,16) 口径): 561 顶点 → 960 三角(极带退化四边形只出单三角),
  惰性构建一次(纯算术, 与时钟无关, 不影响确定性)。`hn-three-color="#rrggbb"` 覆盖
  全部三角基色(严格 6 位十六进制, 非法 → 不覆盖); box 沿用六色面板。几何表常驻
  .bss 约 60KB(sphere 汤表为大头), 有界。
- **可见性口径变更(全量发射 + 画家序)**: 原"背面剔除(世界法线·视线 ≤ 0 丢)"**作废**。
  实测两类缺陷: ① 弦平面叉积在剪影细长条上翻面(顶点在球面、弦在球内), 把可见轮廓
  三角误剔 —— 960 三角球在 spin 90° 的右缘缺一块(显示列表 union bbox 实测右缘
  248px, 真剪影 313px); ② 改质心/顶点径向判后, 可见帽边界(acos(R/d) ≈ 80.2°)处的
  顶点采样稀疏方向整段跳空(可见帽最后顶点只投影到 343px)。最终口径: **凸几何不剔除,
  960 三角全量发射, 按视空间质心 z 远→近画家排序** —— 全体三角投影之并 = 球面投影
  之并 = 完整圆盘, 缺口在构造上不可能; 凸体下与 z-buffer 逐像素同像(每像素最后画到
  的必是最近三角)。多画的一半背面三角被近面覆盖(径向像无差), 代价仅过染。
  0.75px 边法线外扩只做**近侧三角**(世界法线朝眼; 背面三角外扩会在轮廓外戳出 2-4px
  暗缘, 实测盒右缘暗带即此, 已改)。本批顺带修复: 演示页立方体在 spin≈90° 右缘
  同源缺口(旧 build union bbox 248 vs 313)随之消失 —— 两处症状同根。
- **轨道相机(`hn-three-camera-orbit`)**: 指针拖拽驱动方位角/仰角(0.35°/px; 仰角钳
  ±85°), 松手惯性(指数衰减 τ=0.6s, 亚阈值 0.02°/s 停表); 积分 dt = 节点 ext_clock 差
  (与自转同一时钟源); "抓住" = `hn_context_hit_node(pointer)==本元素`(引擎现成命中,
  与 hover/click 同一语义)。状态存模块进程级 8 槽表(键 = 文档+节点+节点 arena 三重
  校验, LRU; hn_node 归 hn_internal.h 所有不加字段)。`hn-three-azimuth/elevation/dist`
  显式初值; **缺省初值精确反解固定视点 eye(1.7,2.3,3.1)**(az 28.7°/el 33.0°/dist 4.22)
  —— 声明 orbit 本身不改变缺省观感。无指针事件(has_pointer=0, 离屏 shot)恒停初值,
  HN_CLOCK/--advance 双 shot 逐字节确定口径不变(实测双 shot 一致)。
- **纹理贴图(`hn-three-tex="path"`)**: 资产后端(`hn_context_assets`)取字节 →
  `hn_png_decode` 验证"可达且可解码"(PNG 口径; 正/负判定进 4 槽缓存, 负缓存免缺图页
  每帧重跑) → 每三角角点 UV → **一条 `HN_CMD_TRIS`**(顶点汤按画家序展开, `mesh_rows=1`
  屏幕绕序标记, `text`=同一路径) → `hnsoft.c paint_tris` 逐像素重心 UV 采样 + 解析 AA
  (Cubism 同一颗内核)。两条如实声明: ① 贴图路径 = **unlit**(TRIS 内核无逐顶点色输入,
  Lambert 不参与); ② 解码双重发生是通道形状所限(TRIS 指令只有贴图路径字段没有像素
  指针, hn.h 公共契约本文件不得扩) —— 资产后端解码是"可贴图"的诚实门, 像素取样由
  hnsoft 按同路径经自己的缓存解码一次(标准运行时两端同一 cwd 相对路径空间,
  hn_app 先 chdir 文档目录)。门失败(缺后端/缺文件/非 PNG)→ 回退 POLYGON Lambert
  纯色, 不静默空白(桩探针四例全过: 可达→TRIS, 缺后端/缺文件/非 PNG→POLYGON)。
  双面几何背面三角反转绕序发射(TRIS rows=1 的屏幕绕序剔除口径)。
- **backend 分流**: neb3 backend(Phase A)只会装配缺省白盒 —— 声明了 geo(≠box)/
  orbit/tex/color 任一的元素恒走软件路径(`t3_backend_capable`), 不让 backend 画出
  语义不符的缺省盒; 纯 `hn-three`(±spin/msaa)维持既有 backend 路径不变。
- **验证(当日实测)**: `bash tools/build-cubism-dev.sh` 与 `bash tools/build-pe-dev.sh`
  均零 error; `zig cc -target x86_64-windows-gnu -std=c99 -c hn_three.c` 通过
  (仅 T3_NEB3_SYMS 在 _WIN32 分支未用的 const 警告 —— dlopen 门控使符号表仅
  POSIX 分支引用, Windows 目标正常出 .o); 三检验页 `--shot --advance 1500` 在
  双二进制(hnapp-cubism/hnapp-pe)输出**逐字节一致**且双 shot 逐字节一致;
  `--advance 1500 vs 3500` 输出不同(动画在走); `HN_CLOCK=1500 hncore paint` 双跑
  一致(sphere 显示列表 960 POLYGON); 行为桩探针(源码内嵌, 现场 cc, 不入仓库)
  七项全过: 无指针确定性 / 初值反解 / 拖拽 0.35°/px / 松手惯性衰减 / 仰角钳 ±85° /
  无 orbit 属性固定视点指针无关 / 贴图门四态+缓存。渲染质量: sphere/box 全轮廓
  (放射审计 min/max 半径同前版剪影缺口对比见上)。
- **已知边界(诚实清单)**: 全量发射的过染成本(球体 960 vs 可见 ~530 条, 插入排序
  O(n²) 在面片数上可测量); 凹体/互穿面片仍不保证逐像素正确; 轨道无滚轮缩放/
  阻尼参数面; 贴图 unlit + PNG 口径; 多 Mesh/节点池/glTF 仍无(见下一步)。

## 已落地(2026-10-14 · three.js 子集垂直切片)

> 历史节(垂直切片当日的口径存档)。其中"出像素通道"的**背面剔除**与"逐面 Lambert
> 着色在本模块 CPU"的表述已被 2026-10-15 第二批更新(全量发射 + 画家序; Lambert 仍
> 在模块 CPU, 但可见性不再依赖法线) —— 以上一节为准。

上表"软件渲染路径"一行的落地内容与边界。检验对象: `Sources/CHtmlNative/hn_three.c`(新引擎源,
声明与绘制钩子在 `hn_internal.h` / `hn_paint.c`, 环境钟分类在 `hn_context.c` anim_walk);
检验页: `examples/threejs-demo.html`(560x680, `<div hn-three hn-three-spin="6" hn-three-msaa="4">`
一个场景元素; 2026-10-15 起舞台无底色 + MSAA/深度口径上页面)与 `examples/threejs-pet.html`
(透明无边框桌宠窗, 渲染来源口径见下节)。

- **管线语义**(与 three.js 同构的最小面): Scene(恒等) → pivot Group(rotY=自转动画, rotX=−20° 固定倾角)
  → Mesh(BoxGeometry: 8 顶点 12 三角, 逐面法线/基色, 尺寸 1.15) ; PerspectiveCamera fov 50°,
  eye(1.7, 2.3, 3.1) lookAt 原点, near 0.1 / far 100, aspect = 元素盒比。矩阵数学全在模块 CPU
  (行主 mat4, GL 口径透视/lookAt), 引擎渲染管线零改动。
- **出像素通道**: 顶点汤(世界→视→裁剪→NDC→元素盒屏幕坐标) → 背面剔除(世界法线·视线 ≤ 0 丢)
  → 画家排序(视空间质心 z 远→近, 稳定插入排序) → **每三角一条 `HN_CMD_POLYGON`(3 顶点)**
  → `paint_polygon` 既有扫描内核(与 Lottie 矢量填充同一颗, 不新写扫描线)。顶点数组进引擎临时区
  (生命周期 = 本次显示列表); 常驻开销零(无全局状态、无堆分配, 未声明 hn-three 的页面不进新路径)。
- **为何不走 `HN_CMD_TRIS`**: TRIS 的契约是"逐像素 UV 贴图三角汤"(`hnsoft.c paint_tris`:
  `text` 缺图 → 静默跳过), 表达不了无贴图的逐面平直着色; POLYGON 纯色填充正是本切片要的通道。
  代价: 显示列表含 POLYGON → `hn_gpu_available` 如实拒绝 → 整帧回退 hnsoft(与 Lottie/Cubism
  页面同一现状, 不静默画错)。
- **逐面 Lambert 着色在本模块 CPU**(base × (0.35 + 0.65·max(0, n̂·L)), 固定方向光) —— 明暗是算好
  进 fill 颜色的, 引擎光栅仍无光照模型, 上文"无光照"口径不变。同色相邻三角的共享边以 0.75px
  **边法线**外扩消接缝: 每条边沿自身外法线平移 0.75px、顶点取相邻两边法线的 miter 和(`paint_polygon`
  的半像素边缘覆盖两侧叠加只有 0.75; 边法线外扩保证共享边两侧各 ≥0.75px 垂直重叠、过剩覆盖 ≥1,
  与 `paint_tris` 解析 AA "过剩覆盖"同一目的)。外扩机制当日实拍逐行核验: 共享对角线上欠覆盖
  像素归零(R 恒 145, 与面内一致; 原"顶点自质心径向推"版在边中段法向分量不足, 实拍有 ~46% 掉深
  暗缝, 已弃)。
- **动画/确定性**: 自转角 = 节点 ext_clock(hn_context.c 环境动画类已纳入 hn-three), 周期
  `hn-three-spin` 秒(缺省 6, 下限 0.5)。`HN_CLOCK=<ms> hncore paint|render` 与
  `hn_app --shot … --advance <ms>` 两路都可确定性取帧。后者的分支判据是
  **"合成宿主(synth)或文档无 `<video>`"**(`app/hn_app.c` `advance_media`:
  `!strcmp(hn_media_platform_kind(), "synth") || !doc_has_video(rt)`, 2026-10-15 按当日
  代码复核)—— 满足其一就走虚拟钟 16ms 步进瞬时快进, 总推进取 16ms 网格(不足一帧补齐),
  帧数只由 --advance 决定, 与墙钟无关; 含视频页面**在合成宿主下同样落虚拟钟分支**(合成宿主
  本就是假帧假时钟, 无"真播"可言)。只有平台真宿主(AVFoundation)**且**文档含 `<video>`
  才走墙钟真播循环(每 16ms 一帧直到视频时刻 ≥ 目标, 6s 上限防呆)—— 该分支下帧数随真播
  速度漂, ext_clock 类动画不保证逐字节确定。hn-three/hn-mesh/Lottie/GIF 页面无 `<video>`,
  在两类宿主都落虚拟钟分支, 确定性口径与 `HN_CLOCK` 一致(2026-10-14 修复前平台真宿主对
  无视频页一律进墙钟循环, 空跑 6s 逐次漂移 —— 即"文档无 `<video>`"这半个判据是当时补的;
  "合成宿主"那半个自始就成立, 此前文档漏记)。
- **验证**(当日实测): `bash tools/build-cubism-dev.sh` 与 `bash tools/build-pe-dev.sh` 均零 error
  (冷编译仅有 rt/hn_rt.c 等既有 warning, 无 hn_three 新增); `hn_app --shot` 立方体可见(绿顶/琥珀前/红右,
  透视收敛正确); `hncore paint` 在 HN_CLOCK=0/1500/3000/6000 下可见面着色与包围盒随角旋转
  (0°: 红/绿/琥珀 → 90°: 蓝/绿/暗琥珀 → 180°: 青/蓝/绿 → 6000ms ≡ 0, 周期闭合), 同钟重渲染
  逐字节一致; 另以 include hn_three.c 的独立探针逐角打印 6 面世界法线, 与显示列表 fill 逐面吻合;
  dashboard/threejs-ui/canvas-lab/live2d-demo 四既有页 --probe 回归通过(两二进制)。
- **接线清单**(名下三件之外的最小改动, 逐处): `hn_internal.h` +6 行(声明+注释, 紧邻 hn_pe_paint);
  `hn_paint.c` +7 行(hook, 紧邻 hn-cubism 分支, opacity 折入口径同 lottie); `hn_context.c` 动一行
  (anim_walk 环境钟条件追加 hn-three); `tools/build-cubism-dev.sh`、`tools/build-pe-dev.sh` 各在
  编译清单追加 hn_three.c(pe 脚本两条 zig cc 各一处)。

## neb3 backend(可选 3D runtime · C ABI / dlopen 回退 / MSAA / 深度 / 透明)

> 路线定稿(2026-10-15, 见 `backend/README.md`): 3D runtime 的完整面(场景树/相机/网格/
> 材质/深度/MSAA/灯光/动画/glTF)由**可选 backend** 承担; C99 核心与 hnsoft fallback
> 不强制依赖 Rust。本节是 backend 契约与两路的口径对照; 检验页:
> `examples/threejs-demo.html`(透明舞台 + `hn-three-msaa="4"`)、
> `examples/threejs-pet.html`(透明无边框桌宠窗 + 渲染来源注明)。

- **C ABI**(`Sources/CHtmlNative/include/neb3.h`): 稳定 POD 边界, 不暴露 Rust/wgpu
  类型。请求侧 `neb3_desc{ width, height, msaa_samples(1/2/4/8, backend 可降档),
  transparent(非零 = clear alpha 0), pixel_ratio }`; 输出侧 `neb3_render` 返回
  **RGBA8 readback**, 并经 `neb3_render_info{ msaa_samples(实际档), depth_enabled,
  gpu }` 如实回报所用配置。符号表: create/destroy、scene_create/destroy、box_mesh、
  material_unlit、scene_add_box、set_camera_perspective、set_rotation、set_depth、
  render、release_frame、last_error。
- **两份实现, 同一符号表**:
  - `Sources/CHtmlNative/neb3_soft.c` — C99 兜底(Phase A: 返回全透明 fixture 帧,
    `gpu=0 / msaa=1`, 深度如实上报; **绘制仍由 hn_three 显示列表承担**, 即软件路径的
    像素与上节垂直切片完全同源)。已进 `tools/_engine_sources.sh` 与 daemon/multiplatform
    构建清单, C99 构建链零 Rust 依赖。
  - `backend/neb3-wgpu/` — Rust/wgpu cdylib 骨架(Phase A 零依赖同符号; Phase B 接
    wgpu MSAA/depth24+/材质/glTF; `cargo build --release` 已过)。
- **加载与回退(dlopen)**: 默认不链任何 3D 动态库; 宿主按 `NEB3_LIB` 环境变量指向的
  路径 dlopen(如 `libneb3_wgpu.dylib`), 亦可按 profile 静态链(`backend/README.md`)。
  **页面渲染来源(CPU/GPU)由此决定**: `NEB3_LIB` 在场且加载成功 → GPU 路径
  (`neb3_render_info.gpu=1`); 未设置/加载失败 → C99 兜底(neb3_soft + hn_three 软件路径)。
  回退是**显式降级不是静默切换语义** —— 两路的 MSAA/深度能力不同精度, 各自如实回报
  (ADR 001"不做静默切换"同口径)。`examples/threejs-pet.html` 页面按此口径注明渲染来源。
- **MSAA口径**: `hn-three-msaa="4"`(元素属性)→ `neb3_desc.msaa_samples`。GPU 路径 =
  4x 多重采样, 设备能力不足时 backend 可降档(实际档位看 `neb3_render_info.msaa_samples`,
  页面不得假设请求即所得)。**C99 软件路径没有 MSAA**: 等效抗锯齿是 hn_three 的三角沿
  边法线外扩 0.75px(消共享边接缝)+ hnsoft `paint_polygon` 扫描线内核的边界像素 4x4
  子采样(斜边无阶梯, 内部单采样; hnsoft.c `poly_edge_coverage`, 2026-10-15 落地,
  hn_three 三角走 `HN_CMD_POLYGON` 即经此路径)。勘误(2026-10-15 复审): hnsoft.c 另有
  `paint_polygon_triangle`(直接三角重心覆盖)定义但**从未被调用**, 属未接线备件, 不在
  现行机制内 —— 此前本文误把它与 `poly_edge_coverage` 并列为落地机制, 已更正;
  属性在软件路径为前瞻声明, 引擎如实忽略。
- **深度口径**: GPU 路径 = depth24+ 逐像素深度附件(`neb3_scene_set_depth`), 真 WebGL
  语义的逐像素遮挡; C99 软件路径 = 画家算法面片级排序(上文垂直切片), 无逐像素深度缓冲
  —— 凸体全量发射 + 画家序(2026-10-15 起, 原背面剔除口径已废)与 z-buffer 逐像素同像, 互穿面片不保证。两路深度语义不同精度, 由
  `neb3_render_info.depth_enabled` 如实区分, 不互相冒充。
- **透明口径**: `neb3_desc.transparent` 非零 = clear alpha 0, GPU 与 soft fixture 一致;
  readback RGBA8 进既有合成。与窗口级 `hn-transparent`(逐像素 alpha + 全透明像素点击
  穿透, 命中口径见 `platform/hnp_macos.c` / `hnp_linux.c` 注释)是两层正交开关:
  元素透明(3D 背板 clear)≠ 窗口透明(逐像素命中)。`threejs-pet.html` 是两层全开的
  组合样例: 透明窗 + 透明 3D 背板 + 立方体本体不透明像素可拖可点。

## 下一步(按切片顺序; 2026-10-15 第二批后更新)

已完成(见上"第二批已落地"): 属性驱动几何(box|plane|sphere)、交互相机(拖拽轨道 +
惯性)、贴图三角(HN_CMD_TRIS 既有内核)、全量发射 + 画家序的可见性口径。

1. **泛化场景图**: 场景仍硬编码(Scene→Group→单 Mesh), 几何是属性选参数面。下一片:
   节点池 + 任意层级(多 Mesh/多光源挂点), 再往后是 QuickJS 桥的最小 three.js 面
   (`hnThree.scene()/mesh()/camera()` —— 引擎已有 QuickJS 求值面 rt/hn_rt.c, 账本式
   协议可循 hnCanvas2D 先例)。
2. **近平面剖分**: 现为"任一顶点 clip.w ≤ 0 整三角剔除"的诚实边界(demo 相机距离下
   永不触发); 穿近平面的几何需要 1→2 三角剖分。
3. **材质升级余项**: 贴图已走 TRIS(2026-10-15); 余下顶点色(TRIS 内核无逐顶点色输入,
   需扩公共契约或模块 CPU 预乘)、多光源与 Phong 高光仍属"模块 CPU 算色"边界, 引擎
   光栅不引入光照。
4. **GPU 路径(可选)**: hn_gpu.c 现对 POLYGON/TRIS 如实整帧回退 hnsoft; 若要 GPU 化需
   原语表扩三角批绘制, 明确扩, 不做静默切换(ADR 001 口径)。

## 诚实结论

本引擎是 **CSS 3D 变换 + QUAD 透视投影**,不是 WebGL,没有 GPU 3D 渲染管线(有 GPU **光栅**后端: `hnp_gpu_host` 原语表 + Metal v1 把 QUAD 当四边形贴图绘制, 但几何/排序/透视仍是引擎算的 CPU 路径, 见 ADR 001 与 docs/platforms.md)。它能覆盖的是 three.js 观感里"**数据大屏 / 卡片 UI 类**"的那一半:透视舞台、网格地板、盒子标识、圆环卡片排布、渐变材质、面片文字、无限旋转动画 —— 这些用纯 CSS 口径即可逼近,且离屏渲染对 `HN_CLOCK` 完全确定(两张截图逐字节可比对)。它覆盖不了另一半:没有相机控制器、没有光照/阴影/材质系统、没有模型加载与粒子系统,遮挡是 preserve-3d 子树内的画家算法(面片级排序,非逐像素深度缓冲,跨子树边界不排序),字形不做透视畸变。因此评估结论是:**把本引擎当"CSS 3D 大屏渲染器"看,three.js 类 UI 的静态观感与简单动画可以达到以假乱真;当"three.js 替代品"看,3D 能力上限是装饰级的,不是场景级的。**
