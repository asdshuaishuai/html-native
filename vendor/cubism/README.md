# Live2D Cubism Core(可选组件, 二进制不入库)

`hn-cubism` 属性(实时 moc3 引擎渲染)依赖 Live2D 官方 Core 运行库。
**本仓库不携带其二进制/头文件**(Live2D Proprietary Software License
不允许以源码仓库形式再分发)。需要启用时:

    bash tools/fetch-cubism-core.sh        # 从官方发布包提取到本目录

提取后本目录出现 include/Live2DCubismCore.h 与 lib/macos-arm64/
libLive2DCubismCore.a(均被 .gitignore)。授权与 NOTICE 见 README
"Live2D (Cubism moc3) 授权说明"。个人/小规模事業者免费授权线内使用。
未提取时 hn-cubism 节点显示下载指引占位卡(页面可见而非静默)。
