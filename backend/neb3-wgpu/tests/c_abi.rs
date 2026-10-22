//! C 侧 neb3 ABI 回归(backend/neb3-wgpu/c/abi_regression.c):
//! 1. 编译 abi_regression.c(链 include/neb3.h, 经 cdylib 导出符号);
//! 2. 运行并要求退出码 0 + PASS 行。
//!
//! 覆盖 ISSUES.md P1-1(结构体传值 ABI)与 P1-2(空 error 返回可读 NUL 指针)。
//!
//! 运行前先构建 release 库: `cargo build --release`，再执行
//! `cargo test --release --test c_abi`。该回归只调用 C ABI/错误字符串导出，
//! 不创建 GPU 设备、不提交渲染工作；无 GPU 或图形会话也应可运行。

use std::path::Path;
use std::process::Command;

#[test]
fn c_abi_regression() {
    let manifest_dir = env!("CARGO_MANIFEST_DIR");
    let c_src = Path::new(manifest_dir).join("c/abi_regression.c");
    let inc = Path::new(manifest_dir).join("../../Sources/CHtmlNative/include");
    let bin = Path::new(env!("CARGO_TARGET_TMPDIR")).join("abi_regression");

    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let libdir = if cfg!(debug_assertions) {
        Path::new(manifest_dir).join("target/debug")
    } else {
        Path::new(manifest_dir).join("target/release")
    };
    let lib = libdir.join(if cfg!(target_os = "macos") {
        "libneb3_wgpu.dylib"
    } else if cfg!(windows) {
        "neb3_wgpu.dll"
    } else {
        "libneb3_wgpu.so"
    });
    assert!(lib.exists(), "expected {lib:?}; run cargo build first");

    let out = Command::new(&cc)
        .arg("-std=c99")
        .arg("-Wall")
        .arg("-I")
        .arg(&inc)
        .arg(&c_src)
        .arg(&lib)
        .arg("-Wl,-rpath,")
        .arg(&libdir)
        .arg("-o")
        .arg(&bin)
        .output()
        .expect("spawn cc");
    assert!(
        out.status.success(),
        "cc failed:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );

    let run = Command::new(&bin).output().expect("spawn abi_regression");
    assert!(
        run.status.success(),
        "abi_regression exited {:?}:\nstdout: {}\nstderr: {}",
        run.status.code(),
        String::from_utf8_lossy(&run.stdout),
        String::from_utf8_lossy(&run.stderr)
    );
    assert!(
        String::from_utf8_lossy(&run.stdout).contains("abi_regression: PASS"),
        "missing PASS marker, stdout: {}",
        String::from_utf8_lossy(&run.stdout)
    );
}
