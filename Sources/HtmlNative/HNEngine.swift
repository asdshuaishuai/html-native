import AppKit
import Foundation
import CHtmlNative

/// 表面种类: 引擎把 hn 编码物化成的系统级 UI 形态。
public enum HNSurface: String {
    case window   // 有标题栏的应用窗口
    case popup    // 无边框浮动弹窗(HUD/通知/卡片), 不抢焦点
    case layer    // 覆盖图层
}

public enum HNPaths {
    public static var home: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".html-native", isDirectory: true) }
    public static var apps: URL { home.appendingPathComponent("apps", isDirectory: true) }
    public static var socket: String { home.appendingPathComponent("hn.sock").path }
}

/// 一个运行中的应用实例: hn 文档 + 已物化的原生表面。
/// 窗口关闭即注销; html 更新走热渲染(窗口保持)。
public final class HNApp: NSObject, NSWindowDelegate {
    public let id: String
    /// 渲染宿主: native(C99 引擎) 或 webkit(WKWebView 兜底)
    public let host: any HNWebHost
    /// 自研引擎视图(仅 native 路径存在; 供内省/截图使用)
    public var view: HtmlNativeView? { host as? HtmlNativeView }
    /// 本应用使用的渲染器
    public let renderer: HNRenderer
    public let window: NSWindow
    public let surface: HNSurface
    public private(set) var html: String
    public let css: String?
    public let label: String /* 声明标题(无边框表面无窗口标题, 供管理/列举) */
    var ttlTimer: Timer?

    init(id: String, host: any HNWebHost, renderer: HNRenderer, window: NSWindow,
         surface: HNSurface, html: String, css: String?, label: String) {
        self.id = id
        self.host = host
        self.renderer = renderer
        self.window = window
        self.surface = surface
        self.html = html
        self.css = css
        self.label = label
        super.init()
        window.delegate = self
    }

    deinit { ttlTimer?.invalidate() }

    public func windowWillClose(_ n: Notification) {
        HNEngine.shared.unregister(id: id)
    }
}

/// 引擎会话: 应用的创建/热更新/销毁/持久化。
/// 所有方法须在主线程调用(宿主负责调度)。
public final class HNEngine {

    /// 取页面 body 背景色作窗口底色(标题栏融合); 无背景时用近黑兜底
    static func windowBackdrop(of host: any HNWebHost) -> NSColor {
        guard let col = host.bodyBackgroundHex() else {
            return NSColor(calibratedWhite: 0.06, alpha: 1)
        }
        let r = CGFloat((col >> 24) & 0xFF) / 255
        let g = CGFloat((col >> 16) & 0xFF) / 255
        let b = CGFloat((col >> 8) & 0xFF) / 255
        return NSColor(calibratedRed: r, green: g, blue: b, alpha: 1)
    }
    public static let shared = HNEngine()
    private var apps: [String: HNApp] = [:]
    private var dismissMonitors: [String: Any] = [:]

    /// 打开(或热更新)一个应用: 同 id 已存在则只热渲染, 窗口与状态保持。
    @discardableResult
    public func open(id: String, html: String, css: String? = nil,
                     surface: HNSurface? = nil, title: String? = nil,
                     size: NSSize? = nil, origin: NSPoint? = nil,
                     ttl: TimeInterval? = nil) -> HNApp {
        if let app = apps[id] {
            app.host.render(html, css: css)
            app.host.startPolling()
            return app
        }

        // 表面声明优先级: 调用参数 > hn 清单(meta) > 默认 window
        guard let doc = hn_parse_html(html, html.utf8.count) else { fatalError("hn_parse_html") }
        var m = hn_manifest()
        hn_doc_manifest(doc, &m)
        let kind: HNSurface = surface
            ?? (m.surface == HN_SURFACE_POPUP ? .popup
               : m.surface == HN_SURFACE_LAYER ? .layer : .window)

        let renderer = HNRenderer.declared(in: html)
        let host: any HNWebHost
        switch renderer {
        case .native:
            host = HtmlNativeView(doc: doc, css: css)
        case .webkit:
            let wk = HNWebKitHost()
            host = wk
            wk.render(html, css: css)
        }
        let w = size?.width ?? (m.w > 0 ? CGFloat(m.w) : 460)
        let h = size?.height ?? (m.h > 0 ? CGFloat(m.h) : 560)
        var frame = NSRect(x: 0, y: 0, width: w, height: h)
        var at: NSPoint? = origin
        if at == nil, m.x > 0 || m.y > 0 { at = NSPoint(x: CGFloat(m.x), y: CGFloat(m.y)) }
        if let at {
            let sf = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
            frame = NSRect(x: at.x, y: sf.maxY - at.y - h, width: w, height: h)
        }

        let window: NSWindow
        switch kind {
        case .window:
            window = NSWindow(contentRect: frame,
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
            if let title {
                window.title = title
            } else if let t = m.title {
                window.title = String(cString: t)
            }
            // 无缝标题栏: 透明标题条 + 窗口底色/外观跟随页面 body 背景
            window.titlebarAppearsTransparent = true
            window.isMovableByWindowBackground = m.draggable == 1
            if m.transparent == 1 {
                /* 透明背板: 窗口不画底色, 内容里的透明区域真透出下层 */
                window.isOpaque = false
                window.backgroundColor = .clear
                window.hasShadow = m.shadow == 1
                /* 剥离根底色只是引擎路径需要做的事(它会重新绘制 html/body 背景)。
                   webkit 路径没有引擎上下文: 那里靠 WKWebView 的
                   underPageBackgroundColor = .clear + 页面自身不画根底色实现,
                   因此这里只需判空跳过, 不能拿假指针去调引擎。 */
                if let ectx = host.engineContextOrNil {
                    hn_context_strip_root_background(ectx)
                }
            } else {
                window.backgroundColor = Self.windowBackdrop(of: host)
            }
        case .popup, .layer:
            let panel = NSPanel(contentRect: frame,
                                styleMask: [.borderless, .nonactivatingPanel],
                                backing: .buffered, defer: false)
            panel.level = kind == .popup ? .floating : .statusBar
            panel.isFloatingPanel = true
            panel.isOpaque = false
            panel.backgroundColor = .clear
            panel.hasShadow = m.shadow == 1
            panel.hidesOnDeactivate = false
            panel.isReleasedWhenClosed = false
            /* 弹窗/图层本身就是无边框表面: 声明 hn-transparent 时同样要剥离
               根底色, 否则 html/body 的实色背景会铺满整个矩形, 圆角与局部
               透明全部失效(表现为"声明了透明但还是个方块")。 */
            if m.transparent == 1, let ectx = host.engineContextOrNil {
                hn_context_strip_root_background(ectx)
            }
            window = panel
        }
        window.contentView = host.asView
        if at != nil {
            window.setFrameOrigin(frame.origin)
        } else {
            window.center()
        }
        Self.activateAppIdentity()

        let label = title
            ?? m.title.map { String(cString: $0) }
            ?? id
        host.storeId = id   // 本地 KV 隔离: 每个应用一个存储文件
        let app = HNApp(id: id, host: host, renderer: renderer, window: window,
                        surface: kind, html: html, css: css, label: label)
        apps[id] = app
        // 页面声明的自动行为: hx-trigger="load" 即时拉取, every Ns 启动轮询
        // (webkit 路径由注入脚本自理 → loadActions 为空)
        for action in host.loadActions() { host.performHx(action) }
        host.startPolling()
        // 消息卡片语义: ttl 到期热销毁(关闭窗口并注销)
        if let ttl {
            app.ttlTimer = Timer.scheduledTimer(withTimeInterval: ttl, repeats: false) { [weak self] _ in
                self?.close(id: id)
            }
        }
        // hn-dismiss: 弹窗/图层在点击其它应用时自动关闭(系统级 HUD 语义)
        if kind != .window, let nv = host as? HtmlNativeView,
           hn_doc_has_attr(hn_context_doc(nv.engineContext), "hn-dismiss") == 1 {
            let mid = id
            let mon = NSEvent.addLocalMonitorForEvents(matching: .leftMouseDown) { [weak self] ev in
                guard let self, let w = self.apps[mid]?.window else { return ev }
                if ev.window !== w, !w.frame.contains(NSEvent.mouseLocation) {
                    self.close(id: mid)
                    return nil
                }
                return ev
            }
            dismissMonitors[id] = mon
        }
        // 不激活宿主: 展示不打扰当前工作(agent 语义)
        if kind == .window { window.makeKeyAndOrderFront(nil) } else { window.orderFrontRegardless() }
        return app
    }

    /// 热更新: 替换文档内容, 窗口保持; 清单(标题)变化会同步到窗口
    @discardableResult
    public func update(id: String, html: String) -> Bool {
        guard let app = apps[id] else { return false }
        app.host.render(html, css: app.css)
        // webkit 路径: render 即整页重载, 注入脚本会自行跑 load/轮询
        if app.renderer == .native { refreshLabel(app) }
        // 新文档的 load 触发即时重拉, 骨架/占位不等到下个轮询周期
        for action in app.host.loadActions() { app.host.performHx(action) }
        return true
    }

    /// 从文档清单刷新应用标签与窗口标题(热更新后标题可能变化)
    func refreshLabel(_ app: HNApp) {
        guard let nv = app.view, let doc = hn_context_doc(nv.engineContext) else { return }
        var m = hn_manifest()
        hn_doc_manifest(doc, &m)
        if let t = m.title {
            let s = String(cString: t)
            if !s.isEmpty {
                app.window.title = s
                setLabel(app, s)
            }
        }
    }

    func setLabel(_ app: HNApp, _ label: String) {
        // HNApp.label 为 let — 通过 KVC 式重建? 直接改存储: 用字典标签镜像
        labels[app.id] = label
    }

    private var labels: [String: String] = [:]

    public func app(id: String) -> HNApp? { apps[id] }

    public func close(id: String) {
        guard let app = apps.removeValue(forKey: id) else { return }
        app.ttlTimer?.invalidate()
        if let mon = dismissMonitors.removeValue(forKey: id) {
            NSEvent.removeMonitor(mon)
        }
        app.window.orderOut(nil)
        app.window.close()
        if apps.isEmpty { Self.deactivateAppIdentity() }
    }

    /// 应用身份: 首个窗口打开时驻留 Dock(含图标), 全部关闭后归还后台形态。
    /// 图标由 CoreGraphics 现场绘制(渐变圆角方块 + hn 字标), 无需资源文件。
    static func activateAppIdentity() {
        NSApp.setActivationPolicy(.regular)
        NSApp.applicationIconImage = Self.drawAppIcon()
    }

    static func deactivateAppIdentity() {
        NSApp.setActivationPolicy(.accessory)
    }

    /// 程序化生成 1024px 应用图标: 深蓝→紫渐变圆角方块, 中心 "hn" 字标
    static func drawAppIcon() -> NSImage {
        let size = CGFloat(1024)
        let img = NSImage(size: NSSize(width: size, height: size))
        img.lockFocus()
        if let cg = NSGraphicsContext.current?.cgContext {
            let rect = CGRect(x: 0, y: 0, width: size, height: size)
            cg.saveGState()
            let path = CGPath(roundedRect: rect.insetBy(dx: size * 0.06, dy: size * 0.06),
                              cornerWidth: size * 0.22, cornerHeight: size * 0.22, transform: nil)
            cg.addPath(path)
            cg.clip()
            let colors = [NSColor(calibratedRed: 0.31, green: 0.49, blue: 1.0, alpha: 1).cgColor,
                          NSColor(calibratedRed: 0.66, green: 0.42, blue: 1.0, alpha: 1).cgColor]
            if let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                                     colors: colors as CFArray, locations: [0, 1]) {
                cg.drawLinearGradient(grad, start: CGPoint(x: 0, y: size),
                                      end: CGPoint(x: size, y: 0), options: [])
            }
            let para = NSMutableParagraphStyle()
            para.alignment = .center
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: size * 0.42, weight: .bold),
                .foregroundColor: NSColor.white,
                .paragraphStyle: para,
            ]
            let str = NSAttributedString(string: "hn", attributes: attrs)
            let bs = str.size()
            str.draw(in: CGRect(x: 0, y: (size - bs.height) / 2 - size * 0.02,
                                width: size, height: bs.height))
            cg.restoreGState()
        }
        img.unlockFocus()
        return img
    }

    func unregister(id: String) {
        apps.removeValue(forKey: id)
    }

    public struct AppInfo {
        public let id: String
        public let surface: HNSurface
        public let title: String
    }

    public func list() -> [AppInfo] {
        apps.values.map { AppInfo(id: $0.id, surface: $0.surface,
                                  title: labels[$0.id] ?? $0.label) }
            .sorted { $0.id < $1.id }
    }

    // MARK: - 持久化(.hnapp = JSON 文档包, 可离线再次唤起)

    public func persist(id: String) throws -> URL {
        guard let app = apps[id] else {
            throw NSError(domain: "HNEngine", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "应用不存在: \(id)"])
        }
        let fm = FileManager.default
        try fm.createDirectory(at: HNPaths.apps, withIntermediateDirectories: true)
        let obj: [String: Any] = [
            "id": app.id,
            "html": app.html,
            "css": app.css ?? "",
            "surface": app.surface.rawValue,
            "title": app.window.title,
            "w": Int(app.host.asView.bounds.width),
            "h": Int(app.host.asView.bounds.height),
        ]
        let data = try JSONSerialization.data(withJSONObject: obj)
        let url = HNPaths.apps.appendingPathComponent("\(app.id).hnapp")
        try data.write(to: url)
        return url
    }

    @discardableResult
    public func restore(id: String) -> Bool {
        let url = HNPaths.apps.appendingPathComponent("\(id).hnapp")
        guard let data = try? Data(contentsOf: url),
              let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let html = obj["html"] as? String else { return false }
        let surface = (obj["surface"] as? String).flatMap { HNSurface(rawValue: $0) }
        let title = obj["title"] as? String
        var size: NSSize?
        if let w = obj["w"] as? Double, let h = obj["h"] as? Double, w > 0, h > 0 {
            size = NSSize(width: w, height: h)
        }
        _ = open(id: id, html: html, css: obj["css"] as? String,
                 surface: surface, title: title, size: size)
        return true
    }
}
