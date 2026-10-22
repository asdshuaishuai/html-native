//! neb3-wgpu — Optional Rust/wgpu 3D backend for the Neb engine.
//!
//! Phase B: real wgpu pipeline behind the same C ABI as the default C99
//! `hn_three_soft.c` implementation, so the host can dlopen either one.
//!
//! Rendering contract (approved plan):
//! - wgpu device/queue init, Metal preferred, automatic backend fallback;
//! - MSAA 4x color target + resolve, downgraded 2x/1x by device capability;
//! - Depth24Plus attachment (per-scene toggle);
//! - indexed triangle box mesh, unlit per-face solid material + alpha;
//! - transparent clear (alpha = 0) when `neb3_desc.transparent != 0`;
//! - straight (non-premultiplied) RGBA8 readback into `neb3_render` output.

pub mod native_surface;
pub mod png;

use std::borrow::Cow;
use std::ffi::{c_char, c_float, c_int, c_uchar, c_void, CString};
#[cfg(target_os = "macos")]
use objc2::rc::Retained;
use std::future::Future;
use std::pin::pin;
use std::sync::{Arc, Condvar, Mutex};
use std::task::{Context, Poll, Wake, Waker};

use glam::{Mat4, Vec3};
use wgpu::util::DeviceExt;

// ---------------------------------------------------------------------------
// C ABI POD types (mirror Sources/CHtmlNative/include/neb3.h)
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct Neb3Vec3 { pub x: f32, pub y: f32, pub z: f32 }
#[repr(C)]
pub struct Neb3Color { pub r: f32, pub g: f32, pub b: f32, pub a: f32 }

#[repr(C)]
pub struct Neb3Desc {
    pub width: c_int,
    pub height: c_int,
    pub msaa_samples: c_int,
    pub transparent: c_int,
    pub pixel_ratio: c_float,
}

#[repr(C)]
pub struct Neb3RenderInfo {
    pub width: c_int,
    pub height: c_int,
    pub msaa_samples: c_int,
    pub depth_enabled: c_int,
    pub gpu: c_int,
}

/// 原生纹理直入合成器的出参(macOS; 镜像 include/neb3.h 的 neb3_native)。
#[repr(C)]
pub struct Neb3NativeInfo {
    pub iosurface: *mut c_void,
    pub width: c_int,
    pub height: c_int,
    pub generation: u64,
}

// ---------------------------------------------------------------------------
// std-only block_on — wgpu 30 init futures (enumerate_adapters/request_device)
// complete without external I/O, so a condvar park suffices. Keeps the crate
// at exactly two dependencies (wgpu, glam).
// ---------------------------------------------------------------------------

struct Signal(Mutex<bool>, Condvar);
impl Wake for Signal {
    fn wake(self: Arc<Self>) {
        let mut done = self.0.lock().unwrap();
        *done = true;
        self.1.notify_all();
    }
    fn wake_by_ref(self: &Arc<Self>) { self.clone().wake(); }
}

fn block_on<F: Future>(fut: F) -> F::Output {
    let signal = Arc::new(Signal(Mutex::new(false), Condvar::new()));
    let waker = Waker::from(signal.clone());
    let mut cx = Context::from_waker(&waker);
    let mut fut = pin!(fut);
    loop {
        if let Poll::Ready(out) = fut.as_mut().poll(&mut cx) { return out; }
        let mut done = signal.0.lock().unwrap();
        while !*done { done = signal.1.wait(done).unwrap(); }
        *done = false;
    }
}

// ---------------------------------------------------------------------------
// WGSL shader — unlit, per-face solid shade (axis-dominant normal tint)
// ---------------------------------------------------------------------------

const SHADER: &str = r#"
struct Uniforms {
    view_proj : mat4x4<f32>,
    model     : mat4x4<f32>,
    color     : vec4<f32>,
};
@group(0) @binding(0) var<uniform> u : Uniforms;

struct VSOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) n : vec3<f32>,
};

@vertex
fn vs_main(@location(0) pos : vec3<f32>, @location(1) nrm : vec3<f32>) -> VSOut {
    var out : VSOut;
    out.pos = u.view_proj * (u.model * vec4<f32>(pos, 1.0));
    out.n = (u.model * vec4<f32>(nrm, 0.0)).xyz;
    return out;
}

@fragment
fn fs_main(in : VSOut) -> @location(0) vec4<f32> {
    // Unlit: each face is one flat color; faces are told apart by a baked
    // per-axis shade of the dominant normal axis. Alpha passes through
    // unchanged (straight, non-premultiplied blending).
    let n = normalize(in.n);
    let a = abs(n);
    var shade : f32;
    if (a.y >= a.x && a.y >= a.z) {
        shade = select(0.55, 1.0, n.y > 0.0);   // top / bottom
    } else if (a.x >= a.z) {
        shade = select(0.70, 0.88, n.x > 0.0);  // +X / -X
    } else {
        shade = select(0.62, 0.80, n.z > 0.0);  // +Z / -Z
    }
    return vec4<f32>(u.color.rgb * shade, u.color.a);
}
"#;

/// 原生(预乘输出)变体: 片元先预乘, 混合 One/OneMinusSrcAlpha ——
/// IOSurface 内容 = 预乘域, 与 Metal 合成器的混合口径一致。
const SHADER_PREMULT: &str = r#"
struct Uniforms {
    view_proj : mat4x4<f32>,
    model     : mat4x4<f32>,
    color     : vec4<f32>,
};
@group(0) @binding(0) var<uniform> u : Uniforms;

struct VSOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) n : vec3<f32>,
};

@vertex
fn vs_main(@location(0) pos : vec3<f32>, @location(1) nrm : vec3<f32>) -> VSOut {
    var out : VSOut;
    out.pos = u.view_proj * (u.model * vec4<f32>(pos, 1.0));
    out.n = (u.model * vec4<f32>(nrm, 0.0)).xyz;
    return out;
}

@fragment
fn fs_main(in : VSOut) -> @location(0) vec4<f32> {
    let n = normalize(in.n);
    let a = abs(n);
    var shade : f32;
    if (a.y >= a.x && a.y >= a.z) {
        shade = select(0.55, 1.0, n.y > 0.0);
    } else if (a.x >= a.z) {
        shade = select(0.70, 0.88, n.x > 0.0);
    } else {
        shade = select(0.62, 0.80, n.z > 0.0);
    }
    let rgba = vec4<f32>(u.color.rgb * shade, u.color.a);
    return vec4<f32>(rgba.rgb * rgba.a, rgba.a);
}
"#;

// ---------------------------------------------------------------------------
// GPU context
// ---------------------------------------------------------------------------

/// Device-scoped resources shared by runtime, meshes and scenes.
struct GpuCtx {
    device: wgpu::Device,
    queue: wgpu::Queue,
    bgl: wgpu::BindGroupLayout,
    pipeline_depth: wgpu::RenderPipeline,
    pipeline_nodepth: wgpu::RenderPipeline,
    /// 原生纹理直入路径专用: 输出**预乘** alpha(IOSurface 的消费方
    /// —— Metal 合成器在预乘域做 src-over); 读回路径契约是非预乘,
    /// 两条管线并存, 语义各自成立(neb3_surface_probe 双路径对照
    /// 相应地做预乘换算后 ±2 容差对照)。
    pipeline_native_depth: wgpu::RenderPipeline,
    pipeline_native_nodepth: wgpu::RenderPipeline,
    sample_count: u32,
}

impl GpuCtx {
    fn init(requested_msaa: u32) -> Result<Self, String> {
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor {
            backends: wgpu::Backends::METAL
                | wgpu::Backends::VULKAN
                | wgpu::Backends::DX12
                | wgpu::Backends::GL,
            ..wgpu::InstanceDescriptor::new_without_display_handle()
        });
        // Metal first (macOS), then whatever else the platform offers.
        let mut adapters = block_on(instance.enumerate_adapters(wgpu::Backends::METAL));
        if adapters.is_empty() {
            adapters = block_on(instance.enumerate_adapters(wgpu::Backends::all()));
        }
        let Some(adapter) = adapters.into_iter().next() else {
            return Err("no compatible GPU adapter".to_string());
        };
        let sample_count = pick_sample_count(&adapter, requested_msaa);
        let (device, queue) = block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: Some("neb3-wgpu-device"),
            required_features: wgpu::Features::empty(),
            required_limits: wgpu::Limits::default(),
            experimental_features: wgpu::ExperimentalFeatures::default(),
            memory_hints: wgpu::MemoryHints::default(),
            trace: wgpu::Trace::Off,
        }))
        .map_err(|e| format!("wgpu request_device failed: {e}"))?;

        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("neb3-bgl"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: true,
                    min_binding_size: None,
                },
                count: None,
            }],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("neb3-unlit"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(SHADER)),
        });
        let module_premult = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("neb3-unlit-premult"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(SHADER_PREMULT)),
        });
        let pipeline_depth = make_pipeline(
            &device, &bgl, &module, sample_count, Some(wgpu::TextureFormat::Depth24Plus), false,
        );
        let pipeline_nodepth =
            make_pipeline(&device, &bgl, &module, sample_count, None, false);
        let pipeline_native_depth = make_pipeline(
            &device, &bgl, &module_premult, sample_count,
            Some(wgpu::TextureFormat::Depth24Plus), true,
        );
        let pipeline_native_nodepth =
            make_pipeline(&device, &bgl, &module_premult, sample_count, None, true);
        Ok(GpuCtx {
            device,
            queue,
            bgl,
            pipeline_depth,
            pipeline_nodepth,
            pipeline_native_depth,
            pipeline_native_nodepth,
            sample_count,
        })
    }
}

/// MSAA 4x preferred; degrade to 2x/1x (honor 8 when the host asks for it and
/// the device allows). A sample count is usable only if BOTH the color format
/// and the depth format accept it on this adapter.
fn pick_sample_count(adapter: &wgpu::Adapter, requested: u32) -> u32 {
    let color_ok = adapter
        .get_texture_format_features(wgpu::TextureFormat::Rgba8Unorm)
        .flags;
    let depth_ok = adapter
        .get_texture_format_features(wgpu::TextureFormat::Depth24Plus)
        .flags;
    let candidates: &[u32] = match requested {
        0 | 1 => &[1],
        2 => &[2, 1],
        3 | 4 => &[4, 2, 1],
        _ => &[8, 4, 2, 1],
    };
    candidates
        .iter()
        .copied()
        .find(|&c| color_ok.sample_count_supported(c) && depth_ok.sample_count_supported(c))
        .unwrap_or(1)
}

fn make_pipeline(
    device: &wgpu::Device,
    bgl: &wgpu::BindGroupLayout,
    module: &wgpu::ShaderModule,
    sample_count: u32,
    depth_format: Option<wgpu::TextureFormat>,
    premult_output: bool,
) -> wgpu::RenderPipeline {
    let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("neb3-layout"),
        bind_group_layouts: &[Some(bgl)],
        immediate_size: 0,
    });
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(if depth_format.is_some() {
            "neb3-pipeline-depth"
        } else {
            "neb3-pipeline"
        }),
        layout: Some(&layout),
        vertex: wgpu::VertexState {
            module,
            entry_point: Some("vs_main"),
            compilation_options: Default::default(),
            buffers: &[Some(wgpu::VertexBufferLayout {
                array_stride: 24, // pos: 3xf32 + normal: 3xf32
                step_mode: wgpu::VertexStepMode::Vertex,
                attributes: &[
                    wgpu::VertexAttribute {
                        format: wgpu::VertexFormat::Float32x3,
                        offset: 0,
                        shader_location: 0,
                    },
                    wgpu::VertexAttribute {
                        format: wgpu::VertexFormat::Float32x3,
                        offset: 12,
                        shader_location: 1,
                    },
                ],
            })],
        },
        // TriangleList, no cull — the depth test alone is correct for a
        // closed convex box and keeps winding mistakes harmless.
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: depth_format.map(|format| wgpu::DepthStencilState {
            format,
            depth_write_enabled: Some(true),
            depth_compare: Some(wgpu::CompareFunction::LessEqual),
            stencil: wgpu::StencilState::default(),
            bias: wgpu::DepthBiasState::default(),
        }),
        multisample: wgpu::MultisampleState {
            count: sample_count,
            mask: !0,
            alpha_to_coverage_enabled: false,
        },
        fragment: Some(wgpu::FragmentState {
            module,
            entry_point: Some("fs_main"),
            compilation_options: Default::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rgba8Unorm,
                // 读回路径: straight(非预乘) src-over; 原生路径: 预乘输出
                // (One/OneMinusSrcAlpha —— IOSurface 的消费方在预乘域合成)。
                blend: Some(if premult_output {
                    wgpu::BlendState {
                        color: wgpu::BlendComponent {
                            src_factor: wgpu::BlendFactor::One,
                            dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                            operation: wgpu::BlendOperation::Add,
                        },
                        alpha: wgpu::BlendComponent {
                            src_factor: wgpu::BlendFactor::One,
                            dst_factor: wgpu::BlendFactor::OneMinusSrcAlpha,
                            operation: wgpu::BlendOperation::Add,
                        },
                    }
                } else {
                    wgpu::BlendState::ALPHA_BLENDING
                }),
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    })
}

/// Box geometry: 24 vertices (4 per face), 36 indices, per-face axis normals.
/// Returns (vertex bytes, index bytes, index count).
fn box_geometry(sx: f32, sy: f32, sz: f32) -> (Vec<u8>, Vec<u8>, u32) {
    let (hx, hy, hz) = (sx * 0.5, sy * 0.5, sz * 0.5);
    // Quads per face in order: +Z, -Z, +X, -X, +Y, -Y.
    let quads: [[[f32; 3]; 4]; 6] = [
        [[-hx, -hy, hz], [hx, -hy, hz], [hx, hy, hz], [-hx, hy, hz]],
        [[hx, -hy, -hz], [-hx, -hy, -hz], [-hx, hy, -hz], [hx, hy, -hz]],
        [[hx, -hy, hz], [hx, -hy, -hz], [hx, hy, -hz], [hx, hy, hz]],
        [[-hx, -hy, -hz], [-hx, -hy, hz], [-hx, hy, hz], [-hx, hy, -hz]],
        [[-hx, hy, hz], [hx, hy, hz], [hx, hy, -hz], [-hx, hy, -hz]],
        [[-hx, -hy, -hz], [hx, -hy, -hz], [hx, -hy, hz], [-hx, -hy, hz]],
    ];
    let normals: [[f32; 3]; 6] = [
        [0.0, 0.0, 1.0],
        [0.0, 0.0, -1.0],
        [1.0, 0.0, 0.0],
        [-1.0, 0.0, 0.0],
        [0.0, 1.0, 0.0],
        [0.0, -1.0, 0.0],
    ];
    let mut verts = Vec::with_capacity(24 * 24);
    let mut idx = Vec::with_capacity(36 * 4);
    for (f, quad) in quads.iter().enumerate() {
        for v in quad {
            verts.extend_from_slice(&v[0].to_le_bytes());
            verts.extend_from_slice(&v[1].to_le_bytes());
            verts.extend_from_slice(&v[2].to_le_bytes());
            let n = &normals[f];
            verts.extend_from_slice(&n[0].to_le_bytes());
            verts.extend_from_slice(&n[1].to_le_bytes());
            verts.extend_from_slice(&n[2].to_le_bytes());
        }
        let base = (f as u32) * 4;
        for i in [0u32, 1, 2, 0, 2, 3] {
            idx.extend_from_slice(&(base + i).to_le_bytes());
        }
    }
    (verts, idx, 36)
}

struct MeshGpu {
    vertex_buf: wgpu::Buffer,
    index_buf: wgpu::Buffer,
    index_count: u32,
    size: [f32; 3],
}

pub struct Neb3Mesh {
    gpu: Arc<MeshGpu>,
}

pub struct Neb3Runtime {
    width: u32,
    height: u32,
    transparent: bool,
    gpu: Option<Arc<GpuCtx>>,
    // CString(не String): neb3_last_error 直接交出其指针 —— CString 恒 NUL
    // 结尾; 空 error 走静态 b"\0" 指针(String::as_ptr() 在空串上悬垂且
    // 不保证 NUL 结尾, 2026-10-15 复审修复)。生命周期 = rt 本体。
    error: CString,
}

pub struct Neb3Scene {
    gpu: Arc<GpuCtx>,
    width: u32,
    height: u32,
    transparent: bool,
    instances: Vec<Instance>,
    rotation: f32,
    depth: bool,
    fov_deg: f32,
    aspect: f32,
    near_z: f32,
    far_z: f32,
    frame: Vec<u8>,
    targets: Option<FrameTargets>,
    uniforms: Option<SceneUniforms>,
    /// 原生纹理直入合成器(macOS IOSurface 互操作; native_surface 模块)
    native: Option<native_surface::SurfaceNative>,
}

struct Instance {
    mesh: Arc<MeshGpu>,
    color: [f32; 4],
}

struct FrameTargets {
    width: u32,
    height: u32,
    // Kept alive alongside their views so ownership stays obvious.
    _msaa: wgpu::Texture,
    _resolve: wgpu::Texture,
    _depth: wgpu::Texture,
    msaa_view: wgpu::TextureView,
    resolve_view: wgpu::TextureView,
    depth_view: wgpu::TextureView,
    readback: wgpu::Buffer,
    bytes_per_row: u32,
}

struct SceneUniforms {
    buffer: wgpu::Buffer,
    bind_groups: Vec<wgpu::BindGroup>,
}

/// One 256-byte-aligned slot: [view_proj 64][model 64][color 16][pad 112].
/// 256 is the min_uniform_buffer_offset_alignment floor on every backend.
const UNIFORM_SLOT: u64 = 256;

fn push_mat(dst: &mut Vec<u8>, m: &Mat4) {
    for col in m.to_cols_array_2d() {
        for s in col {
            dst.extend_from_slice(&s.to_le_bytes());
        }
    }
}

fn set_error(rt: &mut Neb3Runtime, msg: &str) {
    rt.error = CString::new(msg).unwrap_or_default();
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

/// # Safety
/// `desc` must point to a valid Neb3Desc, or be null for defaults.
#[no_mangle]
pub unsafe extern "C" fn neb3_create(desc: *const Neb3Desc) -> *mut Neb3Runtime {
    let (w, h, msaa_req, transparent) = if desc.is_null() {
        (640, 480, 4, true)
    } else {
        let d = &*desc;
        (
            if d.width > 0 { d.width as u32 } else { 640 },
            if d.height > 0 { d.height as u32 } else { 480 },
            if d.msaa_samples > 0 { d.msaa_samples as u32 } else { 4 },
            d.transparent != 0,
        )
    };
    let mut error = CString::default();
    let gpu = match GpuCtx::init(msaa_req) {
        Ok(ctx) => Some(Arc::new(ctx)),
        Err(e) => {
            error = CString::new(e).unwrap_or_default();
            None
        }
    };
    Box::into_raw(Box::new(Neb3Runtime {
        width: w,
        height: h,
        transparent,
        gpu,
        error,
    }))
}

/// # Safety
/// `rt` must come from neb3_create and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_destroy(rt: *mut Neb3Runtime) {
    if !rt.is_null() {
        drop(Box::from_raw(rt));
    }
}

/// # Safety
/// `rt` must be a live handle from neb3_create.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_create(rt: *mut Neb3Runtime) -> *mut Neb3Scene {
    if rt.is_null() {
        return std::ptr::null_mut();
    }
    let r = &mut *rt;
    let Some(gpu) = r.gpu.clone() else {
        set_error(r, "neb3_scene_create: wgpu device unavailable");
        return std::ptr::null_mut();
    };
    Box::into_raw(Box::new(Neb3Scene {
        gpu,
        width: r.width,
        height: r.height,
        transparent: r.transparent,
        instances: Vec::new(),
        rotation: 0.0,
        depth: true,
        fov_deg: 45.0,
        aspect: 0.0, // 0 = derive from width/height
        near_z: 0.1,
        far_z: 100.0,
        frame: Vec::new(),
        targets: None,
        uniforms: None,
        native: None,
    }))
}

/// # Safety
/// `scene` must come from neb3_scene_create and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_destroy(scene: *mut Neb3Scene) {
    if !scene.is_null() {
        drop(Box::from_raw(scene));
    }
}

/// # Safety
/// `rt` must be a live handle; the returned handle is freed by
/// neb3_mesh_destroy exactly once.
#[no_mangle]
pub unsafe extern "C" fn neb3_box_mesh(
    rt: *mut Neb3Runtime, sx: c_float, sy: c_float, sz: c_float,
) -> *mut c_void {
    if rt.is_null() {
        return std::ptr::null_mut();
    }
    let Some(gpu) = (*rt).gpu.clone() else {
        return std::ptr::null_mut();
    };
    let size = [
        if sx > 0.0 { sx } else { 1.0 },
        if sy > 0.0 { sy } else { 1.0 },
        if sz > 0.0 { sz } else { 1.0 },
    ];
    let (verts, idx, index_count) = box_geometry(size[0], size[1], size[2]);
    let vertex_buf = gpu
        .device
        .create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("neb3-box-vb"),
            contents: &verts,
            usage: wgpu::BufferUsages::VERTEX,
        });
    let index_buf = gpu
        .device
        .create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("neb3-box-ib"),
            contents: &idx,
            usage: wgpu::BufferUsages::INDEX,
        });
    Box::into_raw(Box::new(Neb3Mesh {
        gpu: Arc::new(MeshGpu {
            vertex_buf,
            index_buf,
            index_count,
            size,
        }),
    })) as *mut c_void
}

/// # Safety
/// `mesh` must come from neb3_box_mesh and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_mesh_destroy(mesh: *mut c_void) {
    if !mesh.is_null() {
        drop(Box::from_raw(mesh as *mut Neb3Mesh));
    }
}

/// # Safety
/// `rt` must be a live handle.
/// `color` is passed by value as a neb3_color struct — matches the stable C
/// ABI in include/neb3.h (2026-10-15 re-review fix: a 4-float signature is
/// NOT ABI-compatible with the struct form on SysV x86-64).
#[no_mangle]
pub unsafe extern "C" fn neb3_material_unlit(
    rt: *mut Neb3Runtime,
    color: Neb3Color,
) -> *mut c_void {
    if rt.is_null() {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new(color)) as *mut c_void
}

/// # Safety
/// `material` must come from neb3_material_unlit and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_material_destroy(material: *mut c_void) {
    if !material.is_null() {
        drop(Box::from_raw(material as *mut Neb3Color));
    }
}

/// # Safety
/// `scene`/`mesh`/`material` must be live handles. The scene snapshots the
/// mesh GPU handle (Arc clone) and the material color; both original handles
/// stay owned by the caller and may be destroyed independently afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_add_box(
    scene: *mut Neb3Scene,
    mesh: *mut c_void,
    material: *mut c_void,
) -> c_int {
    if scene.is_null() || mesh.is_null() || material.is_null() {
        return 0;
    }
    let s = &mut *scene;
    // Read-only access: never take ownership of caller-owned handles here.
    let m = &*(mesh as *const Neb3Mesh);
    let color = &*(material as *const Neb3Color);
    s.instances.push(Instance {
        mesh: m.gpu.clone(),
        color: [color.r, color.g, color.b, color.a],
    });
    1
}

/// # Safety
/// `scene` must be a live handle. `fov` is degrees (per neb3.h).
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_set_camera_perspective(
    scene: *mut Neb3Scene,
    fov: c_float,
    aspect: c_float,
    near: c_float,
    far: c_float,
) -> c_int {
    if scene.is_null() {
        return 0;
    }
    let s = &mut *scene;
    s.fov_deg = fov.clamp(1.0, 170.0);
    s.aspect = if aspect > 0.0 { aspect } else { 0.0 };
    s.near_z = if near > 0.0 { near } else { 0.1 };
    s.far_z = if far > s.near_z { far } else { s.near_z * 100.0 };
    1
}

/// # Safety
/// `scene` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_set_rotation(
    scene: *mut Neb3Scene,
    radians: c_float,
) -> c_int {
    if scene.is_null() {
        return 0;
    }
    (*scene).rotation = radians;
    1
}

/// # Safety
/// `scene` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_set_depth(scene: *mut Neb3Scene, enabled: c_int) -> c_int {
    if scene.is_null() {
        return 0;
    }
    (*scene).depth = enabled != 0;
    1
}

/// Phase B: real wgpu pass — MSAA color target (+resolve), Depth24Plus,
/// indexed box draws, straight-alpha blending, RGBA8 readback.
///
/// # Safety
/// `scene` must be a live handle; `rgba`/`bytes`/`info` must be writable.
/// The returned `rgba` pointer stays valid until the next render/release on
/// this scene.
#[no_mangle]
pub unsafe extern "C" fn neb3_render(
    scene: *mut Neb3Scene,
    _time_ms: c_float,
    clear: *const Neb3Color,
    rgba: *mut *const c_uchar,
    bytes: *mut usize,
    info: *mut Neb3RenderInfo,
) -> c_int {
    if scene.is_null() || rgba.is_null() || bytes.is_null() {
        return 0;
    }
    let s = &mut *scene;
    let (w, h) = (s.width, s.height);
    if w == 0 || h == 0 {
        return 0;
    }

    // Clear color: caller's RGB; alpha forced to 0 when the runtime was
    // created with transparent != 0 (neb3.h contract).
    let (cr, cg, cb) = if clear.is_null() {
        (0.0, 0.0, 0.0)
    } else {
        let c = &*clear;
        (c.r as f64, c.g as f64, c.b as f64)
    };
    let clear_color = wgpu::Color {
        r: cr,
        g: cg,
        b: cb,
        a: if s.transparent { 0.0 } else { 1.0 },
    };

    if let Err(e) = render_pass(s, clear_color) {
        // Never hand out stale frame data on failure.
        s.frame.clear();
        *rgba = s.frame.as_ptr();
        *bytes = 0;
        eprintln!("neb3-wgpu: render failed: {e}");
        return 0;
    }

    if !info.is_null() {
        let i = &mut *info;
        i.width = w as c_int;
        i.height = h as c_int;
        i.msaa_samples = s.gpu.sample_count as c_int;
        i.depth_enabled = s.depth as c_int;
        i.gpu = 1;
    }
    *rgba = s.frame.as_ptr();
    *bytes = s.frame.len();
    1
}

impl Neb3Scene {
    fn ensure_targets(&mut self) {
        let (w, h) = (self.width, self.height);
        let samples = self.gpu.sample_count;
        let fresh = match &self.targets {
            Some(t) => t.width != w || t.height != h,
            None => true,
        };
        if !fresh {
            return;
        }
        let device = &self.gpu.device;
        let msaa = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("neb3-msaa"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: samples,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        let resolve = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("neb3-resolve"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        });
        let depth = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("neb3-depth"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: samples,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Depth24Plus,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        });
        // RGBA8 rows padded to the 256-byte COPY_BYTES_PER_ROW_ALIGNMENT.
        let bytes_per_row = w.div_ceil(64) * 256;
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("neb3-readback"),
            size: (bytes_per_row as u64) * (h as u64),
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        self.targets = Some(FrameTargets {
            width: w,
            height: h,
            msaa_view: msaa.create_view(&wgpu::TextureViewDescriptor::default()),
            resolve_view: resolve.create_view(&wgpu::TextureViewDescriptor::default()),
            depth_view: depth.create_view(&wgpu::TextureViewDescriptor::default()),
            _msaa: msaa,
            _resolve: resolve,
            _depth: depth,
            readback,
            bytes_per_row,
        });
    }

    fn ensure_uniforms(&mut self) {
        let count = self.instances.len().max(1);
        let rebuild = match &self.uniforms {
            Some(u) => u.bind_groups.len() < count,
            None => true,
        };
        if !rebuild {
            return;
        }
        let device = &self.gpu.device;
        let buffer = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("neb3-uniforms"),
            size: UNIFORM_SLOT * count as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind_groups = (0..count)
            .map(|i| {
                device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("neb3-bg"),
                    layout: &self.gpu.bgl,
                    entries: &[wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                            buffer: &buffer,
                            offset: UNIFORM_SLOT * i as u64,
                            size: None,
                        }),
                    }],
                })
            })
            .collect();
        self.uniforms = Some(SceneUniforms { buffer, bind_groups });
    }

}

/// 均匀上传(读回/原生两路共用)。
fn upload_uniforms(
    gpu: &GpuCtx, uniforms: &SceneUniforms, instances: &[Instance], rotation: f32,
    fov_deg: f32, aspect_set: f32, near_z: f32, far_z: f32, width: u32, height: u32,
) {
    let view_proj = view_proj_for(
        instances, fov_deg, aspect_set, near_z, far_z, width, height,
    );
    let slot_count = instances.len().max(1);
    let mut data = Vec::with_capacity(UNIFORM_SLOT as usize * slot_count);
    for (i, inst) in instances.iter().enumerate() {
        let sz = inst.mesh.size;
        let model = Mat4::from_rotation_y(rotation)
            * Mat4::from_rotation_x(0.45)
            * Mat4::from_scale(Vec3::new(sz[0], sz[1], sz[2]));
        push_mat(&mut data, &view_proj);
        push_mat(&mut data, &model);
        for c in inst.color {
            data.extend_from_slice(&c.to_le_bytes());
        }
        let slot_end = (i + 1) * UNIFORM_SLOT as usize;
        data.resize(slot_end, 0);
    }
    if data.len() < UNIFORM_SLOT as usize {
        data.resize(UNIFORM_SLOT as usize, 0);
    }
    gpu.queue.write_buffer(&uniforms.buffer, 0, &data);
}

/// 绘制编码(读回/原生两路共用): MSAA/1x 与深度开合由 (color, resolve, depth) 表达。
fn encode_pass(
    gpu: &GpuCtx, uniforms: &SceneUniforms, instances: &[Instance],
    encoder: &mut wgpu::CommandEncoder,
    color: &wgpu::TextureView, resolve: Option<&wgpu::TextureView>,
    depth: Option<&wgpu::TextureView>, clear_color: wgpu::Color,
    premult_output: bool,
) {
    let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some(if premult_output { "neb3-pass-native" } else { "neb3-pass" }),
        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
            view: color,
            depth_slice: None,
            resolve_target: resolve,
            ops: wgpu::Operations {
                load: wgpu::LoadOp::Clear(clear_color),
                store: wgpu::StoreOp::Store,
            },
        })],
        depth_stencil_attachment: depth.map(|view| wgpu::RenderPassDepthStencilAttachment {
            view,
            depth_ops: Some(wgpu::Operations {
                load: wgpu::LoadOp::Clear(1.0),
                store: wgpu::StoreOp::Discard,
            }),
            stencil_ops: None,
        }),
        timestamp_writes: None,
        occlusion_query_set: None,
        multiview_mask: None,
    });
    let pipeline = match (depth.is_some(), premult_output) {
        (true, false) => &gpu.pipeline_depth,
        (false, false) => &gpu.pipeline_nodepth,
        (true, true) => &gpu.pipeline_native_depth,
        (false, true) => &gpu.pipeline_native_nodepth,
    };
    pass.set_pipeline(pipeline);
    for (i, inst) in instances.iter().enumerate() {
        pass.set_bind_group(0, &uniforms.bind_groups[i], &[(i as u32 * 256)]);
        pass.set_vertex_buffer(0, inst.mesh.vertex_buf.slice(..));
        pass.set_index_buffer(inst.mesh.index_buf.slice(..), wgpu::IndexFormat::Uint32);
        pass.draw_indexed(0..inst.mesh.index_count, 0, 0..1);
    }
}

/// 相机矩阵(原 Neb3Scene::view_proj, 自由函数化供两路共用)。
fn view_proj_for(
    instances: &[Instance],
    fov_deg: f32, aspect_set: f32, near_z: f32, far_z: f32,
    width: u32, height: u32,
) -> Mat4 {
    let aspect = if aspect_set > 0.0 {
        aspect_set
    } else {
        width as f32 / height as f32
    };
    let fov_y = fov_deg.to_radians();
    let ext = instances
        .iter()
        .map(|i| i.mesh.size[0].max(i.mesh.size[1]).max(i.mesh.size[2]))
        .fold(1.0f32, f32::max);
    let eye = Vec3::new(0.0, 0.6 * ext, 2.9 * ext);
    let view = glam::camera::rh::view::look_at_mat4(eye, Vec3::ZERO, Vec3::Y);
    // WebGPU NDC: Z in [0, 1], Y-up (glam "directx" convention).
    let proj = glam::camera::rh::proj::directx::perspective(fov_y, aspect, near_z, far_z);
    proj * view
}

fn render_pass(s: &mut Neb3Scene, clear_color: wgpu::Color) -> Result<(), String> {
    s.ensure_targets();
    s.ensure_uniforms();
    let Some(targets) = &s.targets else {
        return Err("targets unavailable".to_string());
    };
    let Some(uniforms) = &s.uniforms else {
        return Err("uniforms unavailable".to_string());
    };
    let ctx = &s.gpu;
    let device = &ctx.device;

    // ---- uniform upload: one 256-byte slot per instance ----
    upload_uniforms(
        ctx, uniforms, &s.instances, s.rotation,
        s.fov_deg, s.aspect, s.near_z, s.far_z, s.width, s.height,
    );

    // ---- encode: MSAA pass (+resolve, +depth24) ----
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
        label: Some("neb3-encoder"),
    });
    let (color, resolve) = if ctx.sample_count > 1 {
        (&targets.msaa_view, Some(&targets.resolve_view))
    } else {
        (&targets.resolve_view, None)
    };
    let depth = if s.depth { Some(&targets.depth_view) } else { None };
    encode_pass(ctx, uniforms, &s.instances, &mut encoder, color, resolve, depth, clear_color, false);

    // ---- copy resolve -> readback buffer (256B row padding handled above) ----
    encoder.copy_texture_to_buffer(
        targets._resolve.as_image_copy(),
        wgpu::TexelCopyBufferInfo {
            buffer: &targets.readback,
            layout: wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(targets.bytes_per_row),
                rows_per_image: None,
            },
        },
        wgpu::Extent3d {
            width: targets.width,
            height: targets.height,
            depth_or_array_layers: 1,
        },
    );
    ctx.queue.submit([encoder.finish()]);

    // ---- wait for GPU, map, de-pad rows into frame ----
    device
        .poll(wgpu::PollType::wait_indefinitely())
        .map_err(|e| format!("poll after submit failed: {e:?}"))?;
    let (tx, rx) = std::sync::mpsc::channel();
    targets
        .readback
        .slice(..)
        .map_async(wgpu::MapMode::Read, move |res| {
            let _ = tx.send(res);
        });
    device
        .poll(wgpu::PollType::wait_indefinitely())
        .map_err(|e| format!("poll before map failed: {e:?}"))?;
    rx.recv()
        .map_err(|_| "map callback lost".to_string())?
        .map_err(|e| format!("buffer map failed: {e:?}"))?;

    // De-pad 256-byte rows into a local buffer first: `targets`/`uniforms`
    // borrow `s`, so `s.frame` may only be touched after those borrows end.
    let stride = (targets.width * 4) as usize;
    let bpr = targets.bytes_per_row as usize;
    let mut rows = Vec::with_capacity(stride * targets.height as usize);
    {
        let mapped = targets
            .readback
            .slice(..)
            .get_mapped_range()
            .map_err(|e| format!("get_mapped_range failed: {e:?}"))?;
        for y in 0..targets.height as usize {
            rows.extend_from_slice(&mapped[y * bpr..y * bpr + stride]);
        }
    }
    targets.readback.unmap();
    s.frame = rows;
    Ok(())
}

/// 原生纹理直入合成器: 渲进 IOSurface(零 CPU 回读; sync = poll(Wait))。
fn render_native_pass(s: &mut Neb3Scene, clear_color: wgpu::Color) -> Result<(), String> {
    // 预乘域清屏: LoadOp::Clear 写入字面色(不走混合), 而 IOSurface 的
    // 消费方在预乘域合成 —— 透明页的清屏色必须预乘(半透明底色同理),
    // 否则 a=0 的背景像素携带非零 RGB 污染预乘表面(实测对照超差)。
    let clear_color = wgpu::Color {
        r: clear_color.r * clear_color.a,
        g: clear_color.g * clear_color.a,
        b: clear_color.b * clear_color.a,
        a: clear_color.a,
    };
    if s.native.is_none() {
        let n = native_surface::SurfaceNative::create(&s.gpu, s.width, s.height)?;
        s.native = Some(n);
    }
    s.ensure_uniforms();

    #[cfg(target_os = "macos")]
    {
        let Neb3Scene {
            gpu,
            uniforms,
            native,
            instances,
            depth: use_depth,
            rotation,
            fov_deg,
            aspect,
            near_z,
            far_z,
            width,
            height,
            transparent: _,
            frame: _,
            targets: _,
        } = s;
        let n = native.as_mut().ok_or("native surface unavailable")?;
        let uniforms = uniforms.as_ref().ok_or("uniforms unavailable")?;
        upload_uniforms(
            gpu, uniforms, instances, *rotation,
            *fov_deg, *aspect, *near_z, *far_z, *width, *height,
        );
        let (color, resolve) = n.color_and_resolve();
        let depth = if *use_depth { Some(&n.depth_view) } else { None };
        let device = &gpu.device;
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("neb3-native-encoder"),
        });
        encode_pass(gpu, uniforms, instances, &mut encoder, color, resolve, depth, clear_color, true);
        gpu.queue.submit([encoder.finish()]);
        // 同步纪律: 等 GPU 完成写 IOSurface 后才交还调用方(宿主其后消费)。
        device
            .poll(wgpu::PollType::wait_indefinitely())
            .map_err(|e| format!("native poll failed: {e:?}"))?;
        n.generation += 1;
        Ok(())
    }
    #[cfg(not(target_os = "macos"))]
    {
        Err("native surface 仅 macOS(wgpu↔Metal IOSurface 互操作)".to_string())
    }
}
/// # Safety
/// `scene` must be a live handle matching the last neb3_render.
#[no_mangle]
pub unsafe extern "C" fn neb3_release_frame(scene: *mut Neb3Scene) {
    if !scene.is_null() {
        (*scene).frame.clear();
    }
}

/// 原生纹理直入合成器(macOS): 渲进 IOSurface, 零 CPU 回读。
///
/// # Safety
/// `scene` must be a live handle; `out` must be writable.
/// 返回的 IOSurfaceRef 由 scene 持有(调用方不得释放), 到下一次
/// render_native/scene 销毁为止。
#[no_mangle]
#[cfg(target_os = "macos")]
pub unsafe extern "C" fn neb3_render_native(
    scene: *mut Neb3Scene,
    _time_ms: c_float,
    clear: *const Neb3Color,
    out: *mut Neb3NativeInfo,
) -> c_int {
    if scene.is_null() || out.is_null() {
        return 0;
    }
    let s = &mut *scene;
    let (w, h) = (s.width, s.height);
    if w == 0 || h == 0 {
        return 0;
    }
    let (cr, cg, cb) = if clear.is_null() {
        (0.0, 0.0, 0.0)
    } else {
        let c = &*clear;
        (c.r as f64, c.g as f64, c.b as f64)
    };
    let clear_color = wgpu::Color {
        r: cr,
        g: cg,
        b: cb,
        a: if s.transparent { 0.0 } else { 1.0 },
    };
    if let Err(e) = render_native_pass(s, clear_color) {
        eprintln!("neb3-wgpu: render_native failed: {e}");
        return 0;
    }
    if let Some(n) = &s.native {
        let o = &mut *out;
        o.iosurface = Retained::as_ptr(&n.iosurface) as *mut c_void;
        o.width = n.width as c_int;
        o.height = n.height as c_int;
        o.generation = n.generation;
    }
    1
}

/// # Safety
/// `scene` must be a live handle; `out` must be writable.
#[no_mangle]
#[cfg(target_os = "macos")]
pub unsafe extern "C" fn neb3_native_info(
    scene: *mut Neb3Scene, out: *mut Neb3NativeInfo,
) -> c_int {
    if scene.is_null() || out.is_null() {
        return 0;
    }
    match &(*scene).native {
        Some(n) => {
            let o = &mut *out;
            o.iosurface = Retained::as_ptr(&n.iosurface) as *mut c_void;
            o.width = n.width as c_int;
            o.height = n.height as c_int;
            o.generation = n.generation;
            1
        }
        None => 0,
    }
}

/// 非 macOS: 原生表面路径不可用(wgpu↔Metal 互操作仅 Apple)。
#[no_mangle]
#[cfg(not(target_os = "macos"))]
pub unsafe extern "C" fn neb3_render_native(
    _scene: *mut Neb3Scene, _time_ms: c_float, _clear: *const Neb3Color,
    _out: *mut Neb3NativeInfo,
) -> c_int {
    0
}

/// 读出原生表面的当前内容(锁 IOSurface, 去行距, w×h×4 字节)。
/// GPU 写入可见性由 render_native 的 poll(Wait) 纪律保证。
///
/// # Safety
/// `scene` must be a live handle; `buf`/`cap` describe a writable buffer.
#[no_mangle]
#[cfg(target_os = "macos")]
pub unsafe extern "C" fn neb3_native_read_rgba(
    scene: *mut Neb3Scene, buf: *mut c_uchar, cap: usize,
) -> c_int {
    if scene.is_null() || buf.is_null() {
        return 0;   /* 无场景/无缓冲 = 无数据(非错误) */
    }
    match &(*scene).native {
        Some(n) => match n.read_rgba() {
            Ok(frame) => {
                let n_bytes = frame.len().min(cap);
                if n_bytes == 0 {
                    return 0;
                }
                std::ptr::copy_nonoverlapping(frame.as_ptr(), buf, n_bytes);
                n_bytes as c_int
            }
            Err(_) => -2,
        },
        None => 0,
    }
}

/// 非 macOS: 不可用。
#[no_mangle]
#[cfg(not(target_os = "macos"))]
pub unsafe extern "C" fn neb3_native_read_rgba(
    _scene: *mut Neb3Scene, _buf: *mut c_uchar, _cap: usize,
) -> c_int {
    -1
}

/// # Safety
/// `scene` must be a live handle; `out` must be writable.
#[no_mangle]
#[cfg(not(target_os = "macos"))]
pub unsafe extern "C" fn neb3_native_info(
    _scene: *mut Neb3Scene, _out: *mut Neb3NativeInfo,
) -> c_int {
    0
}

/// # Safety
/// `rt` must be a live handle. 返回值生命周期 = rt(销毁前有效);
/// 空 error 返回指向静态 "\0" 的合法指针(C 侧 `(e && *e)` 判空安全;
/// String/CString::as_ptr() 在空串上会悬垂, 2026-10-15 复审修复)。
#[no_mangle]
pub unsafe extern "C" fn neb3_last_error(rt: *mut Neb3Runtime) -> *const c_char {
    static EMPTY_ERROR: &[u8] = b"\0";
    if rt.is_null() {
        return b"no runtime\0".as_ptr() as *const c_char;
    }
    let e = &(*rt).error;
    if e.is_empty() {
        EMPTY_ERROR.as_ptr() as *const c_char
    } else {
        e.as_ptr()
    }
}
