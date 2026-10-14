//! neb3-wgpu — Optional Rust/wgpu 3D backend for the Neb engine.
//!
//! Phase A: ABI skeleton. Exports the same C ABI as neb3_soft.c so the host
//! can dlopen either implementation. Zero dependencies in this phase; wgpu,
//! glam and image arrive in Phase B behind the same symbols.
//!
//! Rendering contract (approved plan): transparent clear = alpha 0, MSAA 4x
//! downgraded by device capability, depth24+ attachment, RGBA8 readback.

use std::ffi::{c_char, c_int, c_uchar, c_float, c_void};

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

pub struct Neb3Runtime {
    width: i32,
    height: i32,
    msaa: i32,
    transparent: bool,
    error: String,
}

pub struct Neb3Scene {
    rotation: f32,
    depth: bool,
    frame: Vec<u8>,
}

/// # Safety
/// `desc` must point to a valid Neb3Desc, or be null for defaults.
#[no_mangle]
pub unsafe extern "C" fn neb3_create(desc: *const Neb3Desc) -> *mut Neb3Runtime {
    let (w, h, msaa, transparent) = if desc.is_null() {
        (640, 480, 1, true)
    } else {
        let d = &*desc;
        (
            if d.width > 0 { d.width } else { 640 },
            if d.height > 0 { d.height } else { 480 },
            if d.msaa_samples > 0 { d.msaa_samples } else { 1 },
            d.transparent != 0,
        )
    };
    Box::into_raw(Box::new(Neb3Runtime {
        width: w, height: h, msaa, transparent,
        error: String::new(),
    }))
}

/// # Safety
/// `rt` must come from neb3_create and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_destroy(rt: *mut Neb3Runtime) {
    if !rt.is_null() { drop(Box::from_raw(rt)); }
}

#[no_mangle]
pub unsafe extern "C" fn neb3_scene_create(rt: *mut Neb3Runtime) -> *mut Neb3Scene {
    if rt.is_null() { return std::ptr::null_mut(); }
    Box::into_raw(Box::new(Neb3Scene { rotation: 0.0, depth: true, frame: Vec::new() }))
}

/// # Safety
/// `scene` must come from neb3_scene_create and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_destroy(scene: *mut Neb3Scene) {
    if !scene.is_null() { drop(Box::from_raw(scene)); }
}

/// # Safety
/// Placeholder mesh handle; geometry upload lands in Phase B.
#[no_mangle]
pub unsafe extern "C" fn neb3_box_mesh(
    _rt: *mut Neb3Runtime, sx: c_float, sy: c_float, sz: c_float,
) -> *mut c_void {
    Box::into_raw(Box::new((sx, sy, sz))) as *mut c_void
}

/// # Safety
/// `mesh` must come from neb3_box_mesh and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_mesh_destroy(mesh: *mut c_void) {
    if !mesh.is_null() { drop(Box::from_raw(mesh as *mut (f32, f32, f32))); }
}

/// # Safety
/// Placeholder material handle; GPU material lands in Phase B.
#[no_mangle]
pub unsafe extern "C" fn neb3_material_unlit(
    rt: *mut Neb3Runtime, r: c_float, g: c_float, b: c_float, a: c_float,
) -> *mut c_void {
    if rt.is_null() { return std::ptr::null_mut(); }
    Box::into_raw(Box::new((r, g, b, a))) as *mut c_void
}

/// # Safety
/// `material` must come from neb3_material_unlit and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn neb3_material_destroy(material: *mut c_void) {
    if !material.is_null() { drop(Box::from_raw(material as *mut (f32, f32, f32, f32))); }
}

/// # Safety
/// `scene`/`mesh`/`material` must be live handles.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_add_box(
    scene: *mut Neb3Scene, _mesh: *mut c_void, _material: *mut c_void,
) -> c_int {
    if scene.is_null() { return 0; }
    1
}

/// # Safety
/// `scene` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_set_camera_perspective(
    scene: *mut Neb3Scene, _fov: c_float, _aspect: c_float,
    _near: c_float, _far: c_float,
) -> c_int {
    if scene.is_null() { return 0; }
    1
}

/// # Safety
/// `scene` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_set_rotation(scene: *mut Neb3Scene, radians: c_float) -> c_int {
    if scene.is_null() { return 0; }
    (*scene).rotation = radians;
    1
}

/// # Safety
/// `scene` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn neb3_scene_set_depth(scene: *mut Neb3Scene, enabled: c_int) -> c_int {
    if scene.is_null() { return 0; }
    (*scene).depth = enabled != 0;
    1
}

/// Phase A: returns a transparent frame. Phase B replaces the body with a
/// wgpu pass: MSAA color target + depth24 + resolve + RGBA8 readback.
///
/// # Safety
/// `scene` must be a live handle; `rgba`/`bytes`/`info` must be writable.
#[no_mangle]
pub unsafe extern "C" fn neb3_render(
    scene: *mut Neb3Scene, _time_ms: c_float, _clear: *const Neb3Color,
    rgba: *mut *const c_uchar, bytes: *mut usize, info: *mut Neb3RenderInfo,
) -> c_int {
    if scene.is_null() || rgba.is_null() || bytes.is_null() { return 0; }
    let s = &mut *scene;
    s.frame.clear();
    // Phase A fixture frame: sized, transparent. wgpu pass replaces this.
    s.frame.reserve(64);
    if !info.is_null() {
        let info = &mut *info;
        info.width = 0; info.height = 0;
        info.msaa_samples = 4; info.depth_enabled = s.depth as c_int; info.gpu = 1;
    }
    *rgba = s.frame.as_ptr();
    *bytes = s.frame.len();
    1
}

/// # Safety
/// `scene` must be a live handle matching the last neb3_render.
#[no_mangle]
pub unsafe extern "C" fn neb3_release_frame(scene: *mut Neb3Scene) {
    if !scene.is_null() { (*scene).frame.clear(); }
}

/// # Safety
/// `rt` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn neb3_last_error(rt: *mut Neb3Runtime) -> *const c_char {
    if rt.is_null() { return b"no runtime\0".as_ptr() as *const c_char; }
    (*rt).error.as_ptr() as *const c_char
}

