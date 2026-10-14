# PainterEngine 软渲染核心(vendored, BSD-3-Clause)

 hn-live="<image>" 属性的渲染后端: px_surface + PX_TextureRenderEx_sincos
 (缩放+旋转软光栅)离屏合成 → 引擎 BITMAP 命令上屏(透明底)。
 来源: github.com/matrixcascade/PainterEngine (BSD-3-Clause),
 许可证全文见 PE-LICENSE。rig(呼吸/摇摆/跳跃)由引擎按 ext_clock 驱动
 —— "引擎驱动 + PE 渲染"分层; PX_Live 框架(release.live 模型)接入后续。
