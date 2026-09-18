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
    /// 轻应用槽位名(声明了 hn-applet 才有): 几何按这个名字记忆/还原。
    /// var 是因为 `hn applet remove` 会把活着的实例摘下来 —— 否则运行中
    /// 的实例一关就把刚删掉的槽位又写回去了, "删除"随即失效。
    public internal(set) var applet: String?
    var ttlTimer: Timer?

    init(id: String, host: any HNWebHost, renderer: HNRenderer, window: NSWindow,
         surface: HNSurface, html: String, css: String?, label: String, applet: String? = nil) {
        self.id = id
        self.host = host
        self.renderer = renderer
        self.window = window
        self.surface = surface
        self.html = html
        self.css = css
        self.label = label
        self.applet = applet
        super.init()
        window.delegate = self
    }

    deinit { ttlTimer?.invalidate() }

    // MARK: - 槽位几何记忆(半固化)

    /// 尺寸只在"用户本来就能改"时才记: 固定尺寸表面的宽高是页面的声明, 不该
    /// 被一次运行覆盖掉 —— 否则页面作者改了 hn-window 却看不到变化, 而症状
    /// 是"改了没生效", 极难往槽位上排查。
    private var remembersSize: Bool { window.styleMask.contains(.resizable) }

    func saveSlot() {
        guard let name = applet else { return }
        // 存绝对屏幕坐标: 与窗口停在哪块屏、哪块屏是不是主屏都无关。
        // 早先这里把 y 换算成"相对所在屏顶边向下", 而 open() 还原时用的是
        // 主屏顶边 —— 两屏顶边不等高时(副屏下沿对齐很常见), 每次回来都往下
        // 挪一截, 而且只在该小组件被拖到副屏之后才出现。
        let f = window.frame
        HNAppletStore(name: name).save(
            x: Double(f.minX),
            y: Double(f.minY),
            w: remembersSize ? Double(f.width) : Double(host.asView.bounds.width),
            h: remembersSize ? Double(f.height) : Double(host.asView.bounds.height),
            appId: id)
    }

    public func windowWillClose(_ n: Notification) {
        /* 唯一的收口点: 用户点红按钮和程序 close() 都会走到 window.close(),
           所以槽位落盘放在这里只需一处。 */
        saveSlot()
        HNEngine.shared.unregister(id: id)
    }

    public func windowDidMove(_ notification: Notification) { saveSlot() }

    /// 拖完边缘才存 —— liveResize 期间会高频触发, 每次都写盘既浪费又会让
    /// 槽位文件处于半拖拽状态(中途宿主退出就记了个奇怪的尺寸)。
    public func windowDidEndLiveResize(_ notification: Notification) { saveSlot() }
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
            record("update", id: id, ["htmlLen": html.utf8.count])
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
        /* ---- 槽位还原(半固化) ----
           声明了 hn-applet 的表面是桌面轻应用: 上次停在哪由槽位记住, 下次打开
           回到原处。优先级刻意排成
               调用方显式指定 > 槽位记忆 > 页面声明 > 默认
           调用方排第一是为了让 agent 能临时把它摆到别处而不破坏记忆; 槽位排在
           页面声明之前, 是因为"记住用户动过的位置"正是这个特性的全部意义 ——
           反过来页面声明就再也改不动位置了。
           尺寸则只对**可调整大小**的表面取槽位: 固定尺寸的卡片, 宽高是页面的
           声明而不是运行期的状态, 一并记住会让作者改 hn-window 看不到变化,
           而症状是"改了没生效", 极难往槽位上排查。 */
        let slotName = m.applet.map { String(cString: $0) }
        let slotGeo = slotName.flatMap { HNAppletStore(name: $0).geometry() }
        let resizable = (kind == .window)   // 与 window.styleMask 一致, 见 HNApp
        let w = size?.width
            ?? (resizable ? slotGeo.map { CGFloat($0.w) } : nil)
            ?? (m.w > 0 ? CGFloat(m.w) : 460)
        let h = size?.height
            ?? (resizable ? slotGeo.map { CGFloat($0.h) } : nil)
            ?? (m.h > 0 ? CGFloat(m.h) : 560)
        var frame = NSRect(x: 0, y: 0, width: w, height: h)
        var at: NSPoint? = origin
        var restored = false
        if at == nil, let g = slotGeo {
            /* 槽位存的是绝对屏幕坐标, 直接可用 —— 与窗口现在在哪块屏无关。 */
            frame = Self.clampAppletFrame(NSRect(x: g.x, y: g.y, width: w, height: h))
            restored = true
        } else if at == nil, m.x > 0 || m.y > 0 {
            /* 清单的 hn-x/hn-y 是"相对主屏顶边、y 向下"的老约定: 单屏下与
               绝对坐标等价, 这里保持它以免改动已有页面的观感。 */
            at = NSPoint(x: CGFloat(m.x), y: CGFloat(m.y))
        }
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
        /* ---- 插件感: 让它"不像一个 App" ----
           macOS 上这几项正是"桌面小组件/插件"与"常规应用"的分界:
           - collectionBehavior .ignoresCycle: 不进 Cmd+Tab 的应用切换列表
           - .canJoinAllSpaces / .fullScreenAuxiliary: 跟随所有空间、可浮在
             全屏应用之上(悬浮工具的预期行为)
           - .moveToActiveSpace: 打开时出现在当前空间而不是自己的空间
           注意 collectionBehavior 会整体覆盖, 所以这里显式列全需要的项。 */
        if kind != .window {
            /* 注意: canJoinAllSpaces 与 moveToActiveSpace 在 macOS 上互斥,
               同时设置会抛 NSInternalInconsistencyException 直接崩掉宿主。
               小组件语义要的是"跟随所有空间", 所以取 canJoinAllSpaces。 */
            window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary,
                                         .ignoresCycle]
            /* 无边框表面默认不夺取焦点(插件不该把用户的键盘抢走)。
               需要交互的页面用 hn-window 声明 window 表面。 */
            window.hidesOnDeactivate = false
        }

        window.contentView = host.asView
        if at != nil || restored {
            window.setFrameOrigin(frame.origin)
        } else {
            window.center()
        }
        /* 存在感: 只有明确声明 app(或 window 表面)才把宿主提升为带 Dock
           图标的常规应用。此前**每次 open 都提升**, 于是一张透明卡片也会
           让宿主在 Dock 里出现并带上图标 —— 与"引擎弱化存在感"正好相反。 */
        Self.applyPresence(m.presence, surface: kind)

        let label = title
            ?? m.title.map { String(cString: $0) }
            ?? id
        host.storeId = id   // 本地 KV 隔离: 每个应用一个存储文件
        let app = HNApp(id: id, host: host, renderer: renderer, window: window,
                        surface: kind, html: html, css: css, label: label,
                        applet: slotName)
        /* 打开即落盘 —— 槽位不只记"被移动过", 而是"这个轻应用有个位置"。
           不在打开时写的话, 一个从没被拖动过的小组件在 `hn applet list` 里
           完全不存在, agent 会以为它没生效(声明看着是对的, 也不报错)。
           但**调用方显式给了位置/尺寸时不能写**: 那是 agent 在临时指定摆位,
           落盘会直接把槽位记忆覆盖成这一次的临时值 —— 于是"摆过一次就再也
           回不到原处", 而且没有任何迹象。还原自槽位时也不必写(值已在盘上)。 */
        if origin == nil, size == nil, slotGeo == nil { app.saveSlot() }
        apps[id] = app
        record("open", id: id, ["surface": kind.rawValue,
                                "w": Int(w), "h": Int(h),
                                "applet": slotName ?? ""])
        // 页面声明的自动行为: hx-trigger="load" 即时拉取, every Ns 启动轮询
        // (webkit 路径由注入脚本自理 → loadActions 为空)
        for action in host.loadActions() { host.performHx(action) }
        host.startPolling()
        /* 小程序式生命周期: 首帧就绪后派发 launch —— 页面据此做初始化
           (读启动参数、拉首屏数据)。未声明 hn-lifecycle 的页面零开销。 */
        if let nv = host as? HtmlNativeView {
            nv.dispatchLifecycle(HN_EV_LAUNCH)
            nv.dispatchLifecycle(HN_EV_SHOW)
        }
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
        /* 记在 render 之前: 万一 render 抛了, 轨迹里也该留下"曾经想改成什么" ——
           排查"界面怎么变成这样"时, 那次失败的尝试往往正是线索。 */
        record("update", id: id, ["htmlLen": html.utf8.count])
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
        /* 关闭前给页面一次收尾机会(未声明则不打扰) */
        if let app = apps[id], let nv = app.view {
            nv.dispatchLifecycle(HN_EV_HIDE)
            nv.dispatchLifecycle(HN_EV_DESTROY)
        }
        record("close", id: id)
        guard let app = apps.removeValue(forKey: id) else { return }
        app.ttlTimer?.invalidate()
        if let mon = dismissMonitors.removeValue(forKey: id) {
            NSEvent.removeMonitor(mon)
        }
        app.window.orderOut(nil)
        app.window.close()
        if apps.isEmpty { Self.deactivateAppIdentity() }
    }

    /// 按声明施加系统身份。ghost = 全程不碰 Dock(默认形态 accessory);
    /// app = 驻留 Dock 并带图标。图标由 CoreGraphics 现场绘制
    /// (渐变圆角方块 + hn 字标), 无需资源文件。
    static func applyPresence(_ presence: hn_presence, surface: HNSurface) {
        let wantApp: Bool
        switch presence {
        case HN_PRESENCE_GHOST: wantApp = false
        case HN_PRESENCE_APP:  wantApp = true
        default:                wantApp = (surface == .window)
        }
        if wantApp {
            NSApp.setActivationPolicy(.regular)
            NSApp.applicationIconImage = Self.drawAppIcon()
        } else {
            NSApp.setActivationPolicy(.accessory)
            NSApp.applicationIconImage = nil
        }
    }

    /// 全部关闭后归还后台形态(不留 Dock 残留)
    static func deactivateAppIdentity() {
        NSApp.setActivationPolicy(.accessory)
        NSApp.applicationIconImage = nil
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

    // MARK: - 操作历史(打进胶囊, 让"怎么驱动它"可回放)

    /// 每个应用最近经历过的可回放操作。上限刻意压小: 这是给人/agent 读的轨迹,
    /// 不是审计日志; 真需要全量应当外挂到自己的存储里。
    private static let opLogLimit = 200
    private var opLogs: [String: [[String: Any]]] = [:]
    private let opLock = NSLock()

    /// 记录一次操作。只记**会改变状态**的: open/update/event/lifecycle/close,
    /// 纯读(dom/dump/text/eval/list)不记 —— 否则轨迹里全是噪声, 反而看不出
    /// 这个应用是怎么被驱动起来的。
    public func record(_ op: String, id: String, _ fields: [String: Any] = [:]) {
        opLock.lock(); defer { opLock.unlock() }
        var entry: [String: Any] = ["op": op, "id": id,
                                    "at": ISO8601DateFormatter().string(from: Date())]
        for (k, v) in fields { entry[k] = v }
        var log = opLogs[id] ?? []
        log.append(entry)
        if log.count > Self.opLogLimit { log.removeFirst(log.count - Self.opLogLimit) }
        opLogs[id] = log
    }

    public func opLog(for id: String) -> [[String: Any]] {
        opLock.lock(); defer { opLock.unlock() }
        return opLogs[id] ?? []
    }

    public struct AppInfo {
        public let id: String
        public let surface: HNSurface
        public let title: String
        /// 占了哪个轻应用槽位(未声明 hn-applet 则为 nil)
        public let applet: String?
    }

    public func list() -> [AppInfo] {
        apps.values.map { AppInfo(id: $0.id, surface: $0.surface,
                                  title: labels[$0.id] ?? $0.label,
                                  applet: $0.applet) }
            .sorted { $0.id < $1.id }
    }

    /// 把活着的实例从某个槽位上摘下来(`hn applet remove` 用)。
    ///
    /// 不能只删文件: 运行中的实例关闭时还会 saveSlot() 把它写回去, 于是
    /// "删除"看起来生效了、一关窗口又复活。摘掉实例的 applet 引用才能真的忘掉。
    public func detachApplet(named name: String) {
        for app in apps.values where app.applet == name { app.applet = nil }
    }

    // MARK: - 胶囊持久化(.hnapp = 单文件: 文档 + 数据 + 几何 + agent 指令)

    /// 打成胶囊。指令缺失时按能力图自动导出, 来源标为 derived ——
    /// **不允许**产出没有指令的胶囊: 一个没有"怎么驱动它"的持久化应用等于
    /// 一张截图, 下一个 agent 拿到也用不动。
    public func capsule(id: String, instructions: String = "") throws -> HNCapsule {
        guard let app = apps[id] else {
            throw NSError(domain: "HNEngine", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "应用不存在: \(id)"])
        }
        return HNCapsule.make(app: app, instructions: instructions)
    }

    /// 写入胶囊文件(默认 ~/.html-native/apps/<id>.hnapp)。
    /// 返回 (URL, 胶囊): 调用方需要 instructionsSource 来提示 agent 补指令。
    @discardableResult
    public func persist(id: String, instructions: String = "",
                        to url: URL? = nil) throws -> (url: URL, capsule: HNCapsule) {
        let cap = try capsule(id: id, instructions: instructions)
        let target = url ?? HNPaths.apps.appendingPathComponent("\(cap.id).hnapp")
        let fm = FileManager.default
        try fm.createDirectory(at: target.deletingLastPathComponent(),
                               withIntermediateDirectories: true)
        try cap.serialize().write(to: target, options: .atomic)
        return (target, cap)
    }

    /// 拆胶囊: 文档 + 数据 + 几何全部装回, 不只是窗口。
    /// v1 老格式(无 version 段)同样支持 —— 那时没有数据和几何可装。
    @discardableResult
    public func restore(from url: URL) -> Bool {
        guard let cap = HNCapsule.load(from: url) else { return false }
        // 页面自己的数据: 装进该应用的 KV 文件
        if !cap.store.isEmpty { HNStore(id: cap.id).restore(cap.store) }
        // 槽位几何: 装回槽位文件, 让 open 时能还原到原处
        if let s = cap.slot {
            HNAppletStore(name: s.name).save(x: s.x, y: s.y, w: s.w, h: s.h, appId: cap.id)
        }
        var size: NSSize?
        if cap.w > 0, cap.h > 0 { size = NSSize(width: cap.w, height: cap.h) }
        _ = open(id: cap.id, html: cap.html, css: cap.css.isEmpty ? nil : cap.css,
                 surface: HNSurface(rawValue: cap.surface),
                 title: cap.title.isEmpty ? nil : cap.title, size: size)
        record("restore", id: cap.id, ["from": url.lastPathComponent])
        return true
    }

    /// 兼容旧调用形态: 从默认位置按 id 恢复
    @discardableResult
    public func restore(id: String) -> Bool {
        restore(from: HNPaths.apps.appendingPathComponent("\(id).hnapp"))
    }
}
