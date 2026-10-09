// swift-tools-version:5.9
import PackageDescription

// 纯 C99 包清单: c49cfbd 已把 Swift 全层(HtmlNative/HnDaemon/Hn/HnMcp/
// HnShot/RenderTest/DemoApp + 旧 Package.swift)移除, 引擎是唯一渲染路径。
// 这份清单只为 `swift build` 这类 SPM 入口存在 —— 目标只有 C99 引擎本体,
// 不含任何 Swift 源码。
let package = Package(
    name: "html-native",
    platforms: [.macOS(.v12)],
    targets: [
        // C99 核心: HTML/CSS 解析 → 级联 → 布局 → 绘制指令。
        // hn_cairo.c 刻意 exclude: 它是**可选** cairo 绘制后端(由
        // -DHN_USE_CAIRO 开启, 依赖 cairo/FreeType 外部库), 默认路径是
        // hnsoft 软件光栅 —— 与 build-multiplatform.sh 的核心产物同源,
        // 不把 cairo 依赖拉进默认构建。
        .target(name: "CHtmlNative", exclude: ["hn_cairo.c", "hn_cairo.h"],
                cSettings: [.unsafeFlags(["-I/opt/homebrew/include/freetype2"])],
                linkerSettings: [.unsafeFlags(["-L/opt/homebrew/lib", "-lfreetype"])]),
    ]
)
