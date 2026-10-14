---
workflow: general-video
flow: automation
storyboard: no
message: "agent 写下的 HTML，就是桌面级原生窗"
destination: local-mp4
aspect: 1920x1080
language: zh-CN
length: 95s
audience: 2026 上海开源软件应用创新大赛评委与开发者
angle: product-showcase
narration: no
voice: none
music: none
---

## Intent

为开源项目 html-native（引擎品牌 Neb = NO-WEB）制作大赛参赛视频：产品 showcase，1920x1080 16:9，目标 90–100s（硬上限 3 分钟）。定位句：agent 写下的 HTML，就是桌面级原生窗——桌宠、数字人、数据大屏、流式会话。无配音（无 TTS），叙事由大字排版与字幕条承载；无背景音乐（未确认有免登录免版权音乐 provider，留白）。

## Assets

- /tmp/neb-demo-p1.mov — 真实桌面录屏 39s，3440x1440：数据大屏关键帧动画 → 透明桌宠悬浮 → Live2D 官方 motion 驱动数字人
- /tmp/neb-demo-p2.mov — 真实桌面录屏 43s：AVPlayerLayer 媒体窗 → 流式会话逐句推送 → 多窗同屏
- 若素材缺失，重录命令：`bash /Volumes/data/dev/coding/deepcodeUI/deepStudio/html-native/docs/oscc-2026/record-demo.sh p1|p2`（本次已确认两文件存在，直接复用）

## Customizations

- 分镜按任务给定的 7 段执行（封面 / 三堵墙 / Neb 的回答 / 实机演示 / 数字指标 / 标准格式与边界 / 结尾），每段时长见 STORYBOARD.md。
- 演示段每段用大字标签压角（数据大屏 / 桌宠 / 数字人 / 媒体窗 GPU 合成 / 流式会话 / 多窗同屏）。
- 数字指标段四张大数字卡敲入：20.6k 行 C99 / 1145+ 探针断言 / 10 类绘制指令 / 0 依赖 0 SDK；补一行"跨架构确定性: 同一文档, 逐位一致"。
- 诚实段必须保留一行注：moc3 专有格式不解析, 以官方 motion 数据驱动自有 rig 替代。

## Notes

- 技术事实（全部真实，可引用）：引擎核心 20.6k 行纯 C99；显示列表 10 类指令；三后端同源（hnsoft 纯 C 光栅 / cairo / Metal v1）；24 探针 1145+ 项断言锚定渲染口径，跨架构确定性逐位对账；GPU 原语契约 hnp_gpu_host（平台出设备原语，引擎出画法），覆盖不了的指令整帧静默回退；真窗 <video> AVPlayerLayer GPU 合成；静态页真窗 CPU 0.0%；MIT 单仓库 github.com/asdshuaishuai/html-native。
- 结尾金句：agent 时代的渲染引擎，价值不在渲染本身，而在"渲染结果可被机器验证"——探针锚定的像素口径，就是 agent 与像素之间的翻译层。
- 视觉规范：底色 #0e1220 深色系；强调色 #4f7cff（蓝）/ #a7b6ff（浅紫）/ #3ecf6f（绿）；大字号，PingFang SC；动效滑入/敲入/淡入，全程确定性（seek-safe）。
- 纪律：只在 oscc-neb-video 工程目录与 /tmp 工作；不碰 html-native 仓库任何文件；不 git commit。
- 渲染目的地：本地 MP4，复制到 /Users/kelthas/Desktop/oscc-2026/neb-作品视频.mp4。
- 环境备注：`npx hyperframes usage --json` 返回 status unknown（unsupported_harness），不猜测额度，按 brief 既定范围执行。
