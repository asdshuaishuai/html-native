# three.js 类 UI 支持度矩阵

> 检验对象: html-native 引擎(C99, HTML/CSS/JS → 原生窗口;3D = QUAD 透视投影)。
> 检验页: `examples/threejs-ui.html`(920x880 深色数据大屏,四个典型 three.js UI 场景)。
> 截图: `screenshots/threejs-ui.png`(HN_CLOCK=1500)、`screenshots/threejs-ui-t0.png`(HN_CLOCK=0,立方体转 90°,证明动画在走)。
> 口径: 本引擎**不是 WebGL**。3D = CSS 3D 变换 + 元素盒四角过 4x4 矩阵 + 透视除法(k = d/(d−z))成投影四边形(QUAD),渐变/边框/文字逐像素随面片采样。

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
| 动画循环 requestAnimationFrame + 物体旋转 | `@keyframes`(仅采样 opacity / translate-x / translate-y / scale / rotate 单数值)+ `infinite`;离屏用 `HN_CLOCK=<ms>` 确定性取帧 | ✅(字段受限;transform 简写/两值 translate 不进时间轴) | 03 立方体 rotate 0→360°/6s(两截图即 t=0 与 t=1500 的对比) |
| translateZ 近大远小(景深) | 自身 `perspective` + `translateZ(±)`,k = d/(d−z) | ✅ | 01 顶部数据条(Z −80/+10/+90) |
| 深度缓冲 / 正确遮挡 | 无逐深度排序 —— 遮挡 = 文档序,凸体按"远→近"手工排序;绕视平面法线的旋转不改变深度序,排序全程有效 | 近似(受限,作者负责排序) | 03 六面排序、04 方位角排序 |
| OrbitControls 相机控制器 | ❌ 无运行时相机;视点固定,换角度 = 改 CSS 角度重新布局/绘制 | ❌ | — |
| 光照 / 阴影 / PBR 材质(MeshStandardMaterial、阴影贴图) | ❌ 无光照模型、无法线、无阴影计算;仅 2D 装饰性 box-shadow/text-shadow(不随 3D 变换参与光照) | ❌ | —(demo 用渐变+亮边框冒充发光材质) |
| 3D 模型加载(glTF/OBJ + 纹理) | ❌ 无模型管线;几何只能用 CSS 盒/面片拼 | ❌ | — |
| 粒子系统 / Points | ❌ 无;只能用大量小元素模拟(无实例化,性能上限低) | ❌ | — |

## 引擎特有口径(本页实测踩到并绕开的坑)

1. **同一元素勿混用 2D scale 与 3D 变换** —— 自身面片不含自身 scale;要缩放用外层壳包一层。
2. **preserve-3d 旋转面片上的 absolute 子级必须自带 transform**(哪怕 `translateZ(0.02px)`)才走"自身 3D 矩阵"路径;无自身 transform 的子级按父内坐标套父矩阵,会整体脱出面片(demo 02 地板格线因此各带 translateZ)。
3. **带 translateZ 投影的面片里,子级反过来不能再带自身 transform**(会脱锚);demo 04 表盘 tick 因此不带 rotate,只用 margin-top 排弧。
4. `@keyframes` 只采样五个单数值字段;`transform` 简写与两值 translate 不进时间轴。
5. 面内偏移不要用 `transform: translate(x,y)` 叠 3D —— QUAD 平移一次、文字锚点平移两次会脱出面片(见 `examples/threejs-lab.html` 页首注)。

## 诚实结论

本引擎是 **CSS 3D 变换 + QUAD 透视投影**,不是 WebGL,没有 GPU 渲染管线。它能覆盖的是 three.js 观感里"**数据大屏 / 卡片 UI 类**"的那一半:透视舞台、网格地板、盒子标识、圆环卡片排布、渐变材质、面片文字、无限旋转动画 —— 这些用纯 CSS 口径即可逼近,且离屏渲染对 `HN_CLOCK` 完全确定(两张截图逐字节可比对)。它覆盖不了另一半:没有相机控制器、没有光照/阴影/材质系统、没有模型加载与粒子系统,遮挡靠文档序手工排序,字形不做透视畸变。因此评估结论是:**把本引擎当"CSS 3D 大屏渲染器"看,three.js 类 UI 的静态观感与简单动画可以达到以假乱真;当"three.js 替代品"看,3D 能力上限是装饰级的,不是场景级的。**
