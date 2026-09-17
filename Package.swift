// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "html-native",
    platforms: [.macOS(.v12)],
    targets: [
        // C99 核心: HTML/CSS 解析 → 级联 → 布局 → 绘制指令
        .target(name: "CHtmlNative", cSettings: [.unsafeFlags(["-I/opt/homebrew/include/freetype2"])],
                linkerSettings: [.unsafeFlags(["-L/opt/homebrew/lib", "-lfreetype"])]),
        // macOS 原生运行时: AppKit 窗口 + CoreText 测量 + CoreGraphics 绘制 + htmx
        .target(name: "HtmlNative", dependencies: ["CHtmlNative"]),
        // 演示: 复杂应用窗口 + 本地 HTTP 服务返回 htmx 片段
        .executableTarget(
            name: "DemoApp",
            dependencies: ["HtmlNative"],
            resources: [.copy("Resources")]
        ),
        // 离屏渲染测试: 输出 PNG + 断言(无窗口)
        .executableTarget(
            name: "RenderTest",
            dependencies: ["HtmlNative"]
        ),
        // 常驻宿主: Unix socket JSON-lines, 供 agent 创建/热更/销毁/持久化应用
        .executableTarget(
            name: "HnDaemon",
            dependencies: ["HtmlNative"]
        ),
        // hn 命令行: agent 的系统入口, 自动拉起宿主
        .executableTarget(
            name: "Hn",
            dependencies: ["HtmlNative"]
        ),
        // 真实视图截图(cacheDisplay 自绘, 无需屏幕录制权限)
        .executableTarget(
            name: "HnShot",
            dependencies: ["HtmlNative"]
        ),
    ]
)
