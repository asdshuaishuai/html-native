//! Phase B self-test: drive the real wgpu pipeline through the public C ABI
//! (exactly as the C host would), assert the rendering contract, and write
//! `out/cube.png` under the crate root.
//!
//! Run: cargo run --release --example render_cube
//!
//! Assertions (per the approved Phase B plan):
//! 1. render succeeds, frame is 640x480x4 bytes;
//! 2. non-fully-transparent coverage > 0 (the box is actually drawn);
//! 3. all four corner pixels have alpha == 0 (transparent clear);
//! 4. corner RGB != 0 while alpha == 0 — proves the readback is
//!    non-premultiplied (straight alpha);
//! 5. neb3_last_error is safe on an empty error (readable, NUL).

use neb3_wgpu::{
    neb3_box_mesh, neb3_create, neb3_destroy, neb3_last_error, neb3_material_destroy,
    neb3_material_unlit, neb3_mesh_destroy, neb3_render, neb3_release_frame, neb3_scene_add_box,
    neb3_scene_create, neb3_scene_destroy, neb3_scene_set_camera_perspective,
    neb3_scene_set_depth, neb3_scene_set_rotation, Neb3Color, Neb3Desc, Neb3RenderInfo,
};

const W: i32 = 640;
const H: i32 = 480;

fn main() {
    let out_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("out")
        .join("cube.png");

    unsafe {
        // -- runtime: transparent clear, MSAA 4x requested --
        let desc = Neb3Desc {
            width: W,
            height: H,
            msaa_samples: 4,
            transparent: 1,
            pixel_ratio: 1.0,
        };
        let rt = neb3_create(&desc);
        assert!(!rt.is_null(), "neb3_create failed (no GPU adapter?)");

        // item 3 regression: empty error must return a readable, NUL pointer.
        let err = neb3_last_error(rt);
        assert!(!err.is_null());
        assert_eq!(*err, 0, "empty error must point at a NUL byte");

        // -- scene: one unlit box, depth on, camera + turntable rotation --
        let scene = neb3_scene_create(rt);
        assert!(!scene.is_null(), "neb3_scene_create failed");

        let mesh = neb3_box_mesh(rt, 1.0, 1.0, 1.0);
        assert!(!mesh.is_null(), "neb3_box_mesh failed");
        let color = Neb3Color { r: 0.85, g: 0.35, b: 0.15, a: 1.0 };
        let material = neb3_material_unlit(rt, color); // struct-by-value ABI (item 4)
        assert!(!material.is_null(), "neb3_material_unlit failed");

        assert_eq!(neb3_scene_add_box(scene, mesh, material), 1);
        assert_eq!(neb3_scene_set_camera_perspective(scene, 45.0, W as f32 / H as f32, 0.1, 100.0), 1);
        assert_eq!(neb3_scene_set_rotation(scene, 0.6), 1);
        assert_eq!(neb3_scene_set_depth(scene, 1), 1);

        // -- render --
        // Background RGB is deliberately non-zero: with a transparent clear
        // (alpha forced to 0) a straight-alpha readback keeps those RGB bits,
        // while a premultiplied pipeline would have zeroed them.
        let clear = Neb3Color { r: 0.05, g: 0.07, b: 0.10, a: 1.0 };
        let mut rgba: *const u8 = std::ptr::null();
        let mut bytes: usize = 0;
        let mut info = Neb3RenderInfo {
            width: 0,
            height: 0,
            msaa_samples: 0,
            depth_enabled: 0,
            gpu: 0,
        };
        let ok = neb3_render(scene, 0.0, &clear, &mut rgba, &mut bytes, &mut info);
        assert_eq!(ok, 1, "neb3_render failed");
        assert_eq!(bytes, (W * H * 4) as usize, "frame must be 640x480 RGBA8");
        assert_eq!((info.width, info.height), (W, H));
        assert_eq!(info.gpu, 1, "info.gpu must flag GPU rendering");
        assert_eq!(info.depth_enabled, 1);
        assert!(
            [1, 2, 4, 8].contains(&info.msaa_samples),
            "msaa_samples must be a real device sample count, got {}",
            info.msaa_samples
        );

        let frame = std::slice::from_raw_parts(rgba, bytes);

        // -- coverage: some pixels must actually be drawn --
        let stride = (W * 4) as usize;
        let opaque = (0..(H as usize))
            .map(|y| (0..W as usize).filter(|&x| frame[y * stride + x * 4 + 3] > 0).count())
            .sum::<usize>();
        let total = (W * H) as usize;
        let ratio = opaque as f64 / total as f64;
        assert!(ratio > 0.0, "expected non-fully-transparent coverage > 0");
        // A centered unit box at fov 45° covers roughly 20-45% of the frame;
        // a degenerate full-screen quad or empty frame would break this band.
        assert!(
            (0.05..0.6).contains(&ratio),
            "coverage {ratio:.4} outside the plausible box band"
        );

        // -- corners: transparent clear + non-premultiplied RGB --
        for (x, y) in [(0usize, 0usize), (W as usize - 1, 0), (0, H as usize - 1), (W as usize - 1, H as usize - 1)] {
            let p = y * stride + x * 4;
            let (r, g, b, a) = (frame[p], frame[p + 1], frame[p + 2], frame[p + 3]);
            assert_eq!(a, 0, "corner ({x},{y}) alpha must be 0, got {a}");
            assert!(
                r > 0 && g > 0 && b > 0,
                "corner ({x},{y}) RGB must keep the clear color when alpha=0 \
                 (non-premultiplied), got ({r},{g},{b},{a})"
            );
        }

        // -- dump PNG --
        neb3_wgpu::png::write_png(&out_path, W as u32, H as u32, frame)
            .expect("writing out/cube.png");

        println!("neb3-wgpu render_cube: OK");
        println!("  frame        : {W}x{H} RGBA8 ({} bytes)", bytes);
        println!("  msaa_samples : {}", info.msaa_samples);
        println!("  coverage     : {ratio:.4} ({} opaque px)", opaque);
        println!("  corners      : all alpha==0, RGB kept (straight alpha)");
        println!("  png          : {}", out_path.display());

        neb3_release_frame(scene);
        neb3_material_destroy(material);
        neb3_mesh_destroy(mesh);
        neb3_scene_destroy(scene);
        neb3_destroy(rt);
    }
}
