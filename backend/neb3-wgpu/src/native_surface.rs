//! 原生纹理直入合成器(macOS): wgpu↔Metal IOSurface 互操作。
//!
//! 目标: 帧不落 CPU —— wgpu 把 resolve 结果**直接渲进 IOSurface 背书的
//! MTLTexture**, 合成器侧在宿主 Metal 设备上把同一 IOSurface 包成自己的
//! MTLTexture 消费(IOSurface 是 Apple 跨 MTLDevice 共享图像的标准机制)。
//!
//! 同步纪律: 渲染提交后 `device.poll(Wait)` 保证 GPU 完成对 IOSurface 的
//! 写入, 宿主同帧在其后消费; 严格跨设备同步(MTLSharedEvent)为升级项。
//!
//! 采样: 内部 MSAA 目标照常(与 readback 路径同一管线), resolve 目标 =
//! IOSurface 纹理 —— 内容与 readback 路径逐位可比(neb3_surface_probe)。

use std::sync::Arc;

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2::AnyThread;
use objc2_foundation::{NSDictionary, NSNumber};
use objc2_io_surface::{
    IOSurface, IOSurfacePropertyKeyAllocSize, IOSurfacePropertyKeyBytesPerRow,
    IOSurfacePropertyKeyHeight, IOSurfacePropertyKeyPixelFormat, IOSurfacePropertyKeyWidth,
    IOSurfaceRef, IOSurfaceLockOptions,
};
use objc2_metal::{MTLPixelFormat, MTLStorageMode, MTLTextureDescriptor, MTLTextureType, MTLTextureUsage};
use wgpu_hal::metal as hal_metal;

use super::GpuCtx;

/// 'RGBA' fourcc(与 wgpu Rgba8Unorm 字节序一致的字面排布)。
const PIXEL_FORMAT_RGBA: u64 = u32::from_be_bytes(*b"RGBA") as u64;

pub struct SurfaceNative {
    /// IOSurface(ObjC 类; Retained 归 SurfaceNative, Drop 即 objc release)。
    pub iosurface: Retained<IOSurface>,
    pub texture: wgpu::Texture,
    pub view: wgpu::TextureView,
    pub generation: u64,
    pub width: u32,
    pub height: u32,
    /// 内部 MSAA 目标(与 readback 路径同管线; resolve 到 IOSurface 纹理)。
    pub msaa_view: Option<wgpu::TextureView>,
    pub depth_view: wgpu::TextureView,
}

impl SurfaceNative {
    /// 创建 IOSurface(行距 64 对齐, RGBA8)+ wgpu 设备上的 IOSurface 纹理
    /// (MTLStorageMode::Shared, RenderAttachment|ShaderRead)。
    pub(crate) fn create(gpu: &Arc<GpuCtx>, w: u32, h: u32) -> Result<Self, String> {
        assert!(w > 0 && h > 0);
        unsafe {
            let bytes_per_row = (w * 4).div_ceil(64) * 64;
            let n_w = NSNumber::new_usize(w as usize);
            let n_h = NSNumber::new_usize(h as usize);
            let n_bpr = NSNumber::new_usize(bytes_per_row as usize);
            let n_alloc = NSNumber::new_usize(bytes_per_row as usize * h as usize);
            let n_fmt = NSNumber::new_u64(PIXEL_FORMAT_RGBA);
            let dict = NSDictionary::from_slices(
                &[
                    IOSurfacePropertyKeyWidth,
                    IOSurfacePropertyKeyHeight,
                    IOSurfacePropertyKeyBytesPerRow,
                    IOSurfacePropertyKeyAllocSize,
                    IOSurfacePropertyKeyPixelFormat,
                ],
                &[&*n_w, &*n_h, &*n_bpr, &*n_alloc, &*n_fmt],
            );
            // 现代 macOS: IOSurface 的 C 函数(IOSurfaceCreate 等)不再从
            // 框架导出 —— 走 ObjC 类 alloc/initWithProperties(运行时解析,
            // 无链接期符号依赖)。返回 Retained(+1), Drop 即 objc release。
            let iosurface: Option<Retained<IOSurface>> =
                objc2::msg_send![IOSurface::alloc(), initWithProperties: &*dict];
            let iosurface = iosurface.ok_or("IOSurface creation failed")?;

            // wgpu 设备的 raw MTLDevice → 在其上把 IOSurface 包成 MTLTexture
            let hal_guard = gpu.device.as_hal::<hal_metal::Api>();
            let Some(dev) = &hal_guard else {
                return Err("hal device unavailable (non-metal backend)?".to_string());
            };
            let desc = MTLTextureDescriptor::new();
            desc.setPixelFormat(MTLPixelFormat::RGBA8Unorm);
            desc.setWidth(w as usize);
            desc.setHeight(h as usize);
            desc.setDepth(1);
            desc.setArrayLength(1);
            desc.setStorageMode(MTLStorageMode::Shared);
            desc.setUsage(MTLTextureUsage::RenderTarget | MTLTextureUsage::ShaderRead);
            let iosurface_ref =
                &*(Retained::as_ptr(&iosurface) as *const IOSurfaceRef);
            let mtl: *mut std::ffi::c_void = objc2::msg_send![
                dev.raw_device(),
                newTextureWithDescriptor: &*desc,
                iosurface: iosurface_ref,
                plane: 0u64,
            ];
            if mtl.is_null() {
                return Err("IOSurface texture creation failed on wgpu device".to_string());
            }
            let proto: Retained<ProtocolObject<dyn objc2_metal::MTLTexture>> =
                Retained::from_raw(mtl as *mut _).ok_or("null MTLTexture")?;
            let created = hal_metal::Device::texture_from_raw(
                proto,
                wgpu::TextureFormat::Rgba8Unorm,
                MTLTextureType::Type2D,
                1, // array layers
                1, // mip levels
                wgpu_hal::CopyExtent { width: w, height: h, depth: 1 },
                None,
            );

            let desc = wgpu::TextureDescriptor {
                label: Some("neb3-native-surface"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                view_formats: &[],
            };
            let texture = gpu.device.create_texture_from_hal::<hal_metal::Api>(
                created,
                &desc,
                wgpu::TextureUses::UNINITIALIZED,
            );
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());

            // 内部 MSAA/depth(非共享; resolve 目标 = IOSurface 纹理)
            let samples = gpu.sample_count;
            let (msaa_view, depth_view) = if samples > 1 {
                let msaa = gpu.device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("neb3-native-msaa"),
                    size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: samples,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Rgba8Unorm,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                    view_formats: &[],
                });
                let mv = msaa.create_view(&wgpu::TextureViewDescriptor::default());
                let depth = gpu.device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("neb3-native-depth"),
                    size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: samples,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Depth24Plus,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                    view_formats: &[],
                });
                let dv = depth.create_view(&wgpu::TextureViewDescriptor::default());
                (Some(mv), dv)
            } else {
                let depth = gpu.device.create_texture(&wgpu::TextureDescriptor {
                    label: Some("neb3-native-depth"),
                    size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                    mip_level_count: 1,
                    sample_count: 1,
                    dimension: wgpu::TextureDimension::D2,
                    format: wgpu::TextureFormat::Depth24Plus,
                    usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
                    view_formats: &[],
                });
                (None, depth.create_view(&wgpu::TextureViewDescriptor::default()))
            };

            Ok(SurfaceNative {
                iosurface,
                texture,
                view,
                generation: 0,
                width: w,
                height: h,
                msaa_view,
                depth_view,
            })
        }
    }

    /// 锁 IOSurface 读出当前内容(去行距), 返回 w×h×4 字节。
    /// GPU 写入的可见性由调用方的 poll(Wait) 纪律保证(§同步纪律)。
    pub fn read_rgba(&self) -> Result<Vec<u8>, String> {
        unsafe {
            let mut seed: u32 = 0;
            let r = self.iosurface.lockWithOptions_seed(IOSurfaceLockOptions::empty(), &mut seed);
            if r != 0 {
                return Err(format!("IOSurfaceLock failed: {r}"));
            }
            let stride = (self.width * 4) as usize;
            let bpr = self.iosurface.bytesPerRow() as usize;
            let base = self.iosurface.baseAddress().as_ptr();
            let mut out = Vec::with_capacity(stride * self.height as usize);
            for y in 0..self.height as usize {
                let row = base.add(y * bpr) as *const u8;
                out.extend_from_slice(std::slice::from_raw_parts(row, stride));
            }
            let _ = self.iosurface.unlockWithOptions_seed(IOSurfaceLockOptions::empty(), &mut seed);
            Ok(out)
        }
    }

    /// 颜色目标与 resolve 目标(MSAA>1 时颜色为内部 MSAA, resolve=IOSurface)。
    pub fn color_and_resolve(&self) -> (&wgpu::TextureView, Option<&wgpu::TextureView>) {
        if let Some(msaa) = &self.msaa_view {
            (msaa, Some(&self.view))
        } else {
            (&self.view, None)
        }
    }
}
