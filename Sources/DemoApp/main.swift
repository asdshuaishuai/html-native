import AppKit
import Foundation
import HtmlNative

// 演示: 用 HTML 描述一个完整的应用窗口(顶栏/侧栏/主区/状态栏)。
// htmx 属性(hx-get/hx-target/hx-trigger)是 HTML 的网络能力声明;
// 这里注入进程内 transport 直接生成片段(模拟 agent 即后端),
// 真实场景去掉 transport, 设 baseURL 即可从任意 HTTP 端点拉片段。
// 背后没有浏览器, 也没有本地服务器, 只有原生渲染。
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow?
    var clockTimer: Timer?

    func applicationDidFinishLaunching(_ note: Notification) {
        makeWindow()
        startClock()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    // MARK: - 窗口

    private func makeWindow() {
        let html = loadResource("app", "html") ?? "<h1>资源缺失</h1>"
        let css = loadResource("app", "css")

        let view = HtmlNativeView(html: html, css: css)

        // htmx 网络: 注入进程内 transport(异步返回片段, 模拟数据往返)。
        // 不设 hxTransport 而设 baseURL 时, 相同的 hx-get 会走 URLSession。
        view.hxTransport = { action, done in
            let frag = PanelFragments.generate(route: action.urlString)
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.15) {
                DispatchQueue.main.async { done(frag) }
            }
        }

        // 页面级 hx-trigger="load": 启动即拉首个面板
        for action in view.loadActions() {
            view.performHx(action)
        }

        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 860, height: 580),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false
        )
        window!.title = "html-native · 系统监视器"
        window!.contentView = view
        window!.center()
        window!.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func loadResource(_ name: String, _ ext: String) -> String? {
        if let url = Bundle.module.url(forResource: name, withExtension: ext, subdirectory: "Resources")
                    ?? Bundle.module.url(forResource: name, withExtension: ext) {
            return try? String(contentsOf: url, encoding: .utf8)
        }
        return nil
    }

    // MARK: - 时钟(不走网络的本地热更新通道: set_text)

    private func startClock() {
        let fmt = DateFormatter()
        fmt.dateFormat = "HH:mm:ss"
        clockTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let view = self?.window?.contentView as? HtmlNativeView else { return }
            view.setText(fmt.string(from: Date()), onElement: "clock")
        }
    }
}

// MARK: - 面板片段生成(模拟 agent/后端角色: 路由 → HTML 片段)

enum PanelFragments {
    private static var cpu = 34.0, mem = 58.0, disk = 71.0, net = 12.4
    private static let procs = [
        ("native-ui", 8.2), ("coretextd", 3.1), ("hx-agent", 12.7),
        ("display-list", 1.4), ("spotlight", 0.8),
    ]

    static func generate(route: String) -> String {
        cpu = min(96, max(4, cpu + Double.random(in: -9...9)))
        mem = min(94, max(20, mem + Double.random(in: -4...4)))
        disk = min(99, max(20, disk + Double.random(in: -1...1)))
        net = max(0.2, net + Double.random(in: -5...6))
        let pct = { String(format: "%.0f", $0) }

        switch route {
        case "/panel/overview":
            return """
            <div class="panel-title">概览</div>
            <div class="cards">
              <div class="card"><div class="k">CPU</div><div class="v">\(pct(cpu))%</div>
                <div class="bar"><div class="fill" style="width: \(pct(cpu))%"></div></div></div>
              <div class="card"><div class="k">内存</div><div class="v">\(pct(mem))%</div>
                <div class="bar"><div class="fill" style="width: \(pct(mem))%"></div></div></div>
              <div class="card"><div class="k">磁盘</div><div class="v">\(pct(disk))%</div>
                <div class="bar"><div class="fill" style="width: \(pct(disk))%"></div></div></div>
            </div>
            <div class="sec-title">活跃进程</div>
            \(procs.map { "<div class=\"row\"><div class=\"pname\">\($0.0)</div><div class=\"pmeta\">\(String(format: "%.1f", $0.1))%</div></div>" }.joined())
            <div class="note">片段由进程内数据源即时生成, hx-get 声明 → transport → 热换入, 全程无 JS 无浏览器。</div>
            """
        case "/panel/cpu":
            return """
            <div class="panel-title">CPU</div>
            <div class="card"><div class="k">总体使用率</div><div class="v">\(pct(cpu))%</div>
              <div class="bar"><div class="fill" style="width: \(pct(cpu))%"></div></div></div>
            <div class="sec-title">核心</div>
            <div class="cards">
              <div class="card"><div class="k">性能核</div><div class="v-sm">\(pct(min(99, cpu * 1.2)))%</div></div>
              <div class="card"><div class="k">能效核</div><div class="v-sm">\(pct(max(1, cpu * 0.4)))%</div></div>
            </div>
            """
        case "/panel/memory":
            return """
            <div class="panel-title">内存</div>
            <div class="cards">
              <div class="card"><div class="k">已用</div><div class="v">\(pct(mem))%</div>
                <div class="bar"><div class="fill" style="width: \(pct(mem))%"></div></div></div>
              <div class="card"><div class="k">已缓存</div><div class="v">\(pct(100 - mem))%</div></div>
            </div>
            """
        case "/panel/disk":
            return """
            <div class="panel-title">磁盘</div>
            <div class="card"><div class="k">Macintosh HD</div><div class="v">\(pct(disk))%</div>
              <div class="bar"><div class="fill" style="width: \(pct(disk))%"></div></div>
              <div class="note">剩余 \(pct(100 - disk))% · 2 TB</div></div>
            """
        case "/panel/net":
            return """
            <div class="panel-title">网络</div>
            <div class="card"><div class="k">Wi-Fi · 下行</div>
              <div class="v">\(String(format: "%.1f", net)) MB/s</div>
              <div class="k hi">连接正常</div></div>
            """
        default:
            return "<div class=\"loading\">404 · \(route)</div>"
        }
    }
}
