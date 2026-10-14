# neb3 — Neb 3D runtime(可选 backend)

批准的 three.js 支持路线:场景树/相机/网格/材质/深度/MSAA/灯光/动画/glTF
由可选 backend 承担,C99 核心与 hnsoft fallback 不强制依赖 Rust。

- `neb3.h`(Sources/CHtmlNative/include/): 稳定 C ABI(纯 POD, 不暴露
  Rust/wgpu 类型)
- `neb3_soft.c`: C99 fallback(Phase A 为透明 fixture 帧; 绘制仍由
  hn_three 显示列表承担)
- `neb3-wgpu/`: Rust/wgpu cdylib(同符号导出; Phase A 零依赖骨架,
  Phase B 接 wgpu MSAA/depth/material/glTF)

授权: MIT。Cargo 不在默认 C99 构建链; 引入 backend 时 dlopen
libneb3_wgpu.dylib 或按 profile 静态链。
