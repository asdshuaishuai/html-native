import AppKit
import Foundation
import CHtmlNative
import HtmlNative

// HnDaemon — html-native 常驻宿主。
// 引擎的进程外形态: agent/工具通过 Unix socket 上的 JSON-lines 协议
// 创建/热更新/销毁/持久化应用表面。随时生成, 随时销毁, 可持久化。
//
// 协议(每行一个 JSON 请求, 回一行 JSON 响应):
//   {"op":"open","id":"card","html":"...","css":"","surface":"popup","title":"..","w":360,"h":520}
//   {"op":"update","id":"card","html":"..."}
//   {"op":"close","id":"card"}
//   {"op":"list"}            → {"apps":[{"id","surface","title"}]}
//   {"op":"persist","id":"x"} → {"path":"~/.html-native/apps/x.hnapp"}
//   {"op":"restore","id":"x"}
//   {"op":"ping"}

let app = NSApplication.shared
let delegate = HostDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory) // 菜单栏级常驻, 不占 Dock
app.run()

final class HostDelegate: NSObject, NSApplicationDelegate {
    var server: UnixLineServer?

    func applicationDidFinishLaunching(_ note: Notification) {
        do {
            server = try UnixLineServer(path: HNPaths.socket) { line in
                HNProtocol.handle(line: line)
            }
            FileHandle.standardError.write("hn-daemon: listening \(HNPaths.socket)\n".data(using: .utf8)!)
        } catch {
            FileHandle.standardError.write("hn-daemon: \(error)\n".data(using: .utf8)!)
            exit(1)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false // 常驻: 窗口全关也存活
    }
}

// MARK: - 开发模式监听(文件变化 → 热更新)

enum DevWatcher {
    private static var timers: [String: Timer] = [:]
    private static var stamps: [String: (html: (Date, UInt64), css: (Date, UInt64)?)] = [:]
    private static let lock = NSLock()

    static func stamp(_ u: URL?) -> (Date, UInt64)? {
        guard let u, let v = try? u.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey]) else { return nil }
        return (v.contentModificationDate ?? .distantPast, UInt64(v.fileSize ?? 0))
    }

    static func start(id: String, html: URL, css: URL?) {
        stop(id: id)
        lock.lock()
        stamps[id] = (stamp(html) ?? (.distantPast, 0), stamp(css))
        lock.unlock()
        let t = Timer(timeInterval: 0.5, repeats: true) { _ in
            guard current(id: id) else { return }
            let h = stamp(html), c = stamp(css)
            lock.lock()
            let old = stamps[id]
            func eq(_ a: (Date, UInt64)?, _ b: (Date, UInt64)?) -> Bool {
                switch (a, b) {
                case (nil, nil): return true
                case let (x?, y?): return x.0 == y.0 && x.1 == y.1
                default: return false
                }
            }
            let changed = !eq(h, old?.html) || !eq(c, old?.css)
            if changed { stamps[id] = (h ?? (.distantPast, 0), c) }
            lock.unlock()
            guard changed else { return }
            // 读文件并热更新(主线程); <link rel=stylesheet> 在此内联
            let prepared = IncludeExpander.prepare(
                html: (try? String(contentsOf: html, encoding: .utf8)) ?? "",
                css: css.flatMap { try? String(contentsOf: $0, encoding: .utf8) },
                baseDir: html.deletingLastPathComponent())
            let newHtml = prepared.html
            let newCss = prepared.css
            DispatchQueue.main.async {
                if HNEngine.shared.app(id: id) != nil {
                    _ = HNEngine.shared.update(id: id, html: newHtml)
                    HNEngine.shared.app(id: id)?.host.startPolling()
                } else {
                    _ = HNEngine.shared.open(id: id, html: newHtml, css: newCss)
                }
            }
        }
        lock.lock()
        timers[id] = t
        lock.unlock()
        DispatchQueue.main.async { RunLoop.main.add(t, forMode: .common) }
    }

    static func stop(id: String) {
        lock.lock()
        let t = timers.removeValue(forKey: id)
        stamps.removeValue(forKey: id)
        lock.unlock()
        if let t { DispatchQueue.main.async { t.invalidate() } }
    }

    private static func current(id: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return timers[id] != nil
    }
}

// MARK: - 协议实现

enum HNProtocol {
    static func handle(line: String) -> String {
        guard let data = line.data(using: .utf8),
              let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let op = obj["op"] as? String else {
            return resp(false, ["error": "bad request"])
        }
        switch op {
        case "ping":
            return resp(true, ["engine": "html-native", "surfaces": ["window", "popup", "layer"]])

        case "open":
            guard let html = obj["html"] as? String, !html.isEmpty else {
                return resp(false, ["error": "html required"])
            }
            let id = (obj["id"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "app-" + UUID().uuidString.prefix(6)
            let surface = (obj["surface"] as? String).flatMap(HNSurface.init(rawValue:))
            let title = obj["title"] as? String
            var size: NSSize?
            var origin: NSPoint?
            if let w = obj["w"] as? Double, let h = obj["h"] as? Double, w > 0, h > 0 {
                size = NSSize(width: w, height: h)
            }
            if let x = obj["x"] as? Double, let y = obj["y"] as? Double {
                origin = NSPoint(x: x, y: y)
            }
            let ttl = obj["ttl"] as? Double
            var created = false
            DispatchQueue.main.sync {
                created = HNEngine.shared.list().contains { $0.id == id }
                HNEngine.shared.open(id: id, html: html, css: obj["css"] as? String,
                                     surface: surface, title: title, size: size, origin: origin,
                                     ttl: ttl.map { TimeInterval($0) })
            }
            return resp(true, ["id": id, "created": !created])

        case "update":
            guard let id = obj["id"] as? String, let html = obj["html"] as? String else {
                return resp(false, ["error": "id/html required"])
            }
            var ok = false
            DispatchQueue.main.sync { ok = HNEngine.shared.update(id: id, html: html) }
            return ok ? resp(true, ["id": id]) : resp(false, ["error": "not open: \(id)"])

        case "dev":
            guard let path = obj["path"] as? String else {
                return resp(false, ["error": "path required"])
            }
            let id = (obj["id"] as? String).flatMap { $0.isEmpty ? nil : $0 }
                ?? ("dev-" + URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent)
            let htmlURL = URL(fileURLWithPath: path)
            let cssURL = (obj["css"] as? String).map { URL(fileURLWithPath: $0) }
            let surface = (obj["surface"] as? String).flatMap(HNSurface.init(rawValue:))
            var opened = false
            DispatchQueue.main.sync {
                let prepared = IncludeExpander.prepare(
                    html: (try? String(contentsOf: htmlURL, encoding: .utf8)) ?? "<h1>读取失败</h1>",
                    css: cssURL.flatMap { try? String(contentsOf: $0, encoding: .utf8) },
                    baseDir: htmlURL.deletingLastPathComponent())
                _ = HNEngine.shared.open(id: id, html: prepared.html, css: prepared.css, surface: surface)
                opened = true
            }
            _ = opened
            DevWatcher.start(id: id, html: htmlURL, css: cssURL)
            return resp(true, ["id": id, "watching": path])

        case "lifecycle":
            /* 小程序式生命周期: 运行时在表面状态变化时自动派发(launch/show/
               hide/destroy), 这里提供 agent 手动触发的能力 —— 例如模拟
               应用被切到后台再回来, 验证页面的 onHide/onShow 行为。 */
            guard let id = obj["id"] as? String, let k = obj["kind"] as? String else {
                return resp(false, ["error": "id/kind required"])
            }
            var out = false
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id), let nv = app.view {
                    let kind: hn_event_kind
                    switch k {
                    case "launch":  kind = HN_EV_LAUNCH
                    case "show":    kind = HN_EV_SHOW
                    case "hide":    kind = HN_EV_HIDE
                    case "destroy": kind = HN_EV_DESTROY
                    default:        return
                    }
                    out = nv.dispatchLifecycle(kind)
                }
            }
            return resp(true, ["dispatched": out, "id": id, "kind": k])

        case "close":
            guard let id = obj["id"] as? String else { return resp(false, ["error": "id required"]) }
            DispatchQueue.main.sync { HNEngine.shared.close(id: id) }
            DevWatcher.stop(id: id)
            return resp(true, ["id": id])

        case "event":
            guard let id = obj["id"] as? String,
                  let kindStr = obj["kind"] as? String else {
                return resp(false, ["error": "id/kind required"])
            }
            var out = false
            var oc: [String: Any] = [:]
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id), let nv = app.view {
                    let kind: hn_event_kind
                    switch kindStr {
                    case "click": kind = HN_EV_CLICK
                    case "mousedown": kind = HN_EV_MOUSEDOWN
                    case "mouseup": kind = HN_EV_MOUSEUP
                    case "mousemove": kind = HN_EV_MOUSEMOVE
                    case "mouseenter": kind = HN_EV_MOUSEENTER
                    case "mouseleave": kind = HN_EV_MOUSELEAVE
                    case "keydown": kind = HN_EV_KEYDOWN
                    case "keyup": kind = HN_EV_KEYUP
                    case "focus": kind = HN_EV_FOCUS
                    case "blur": kind = HN_EV_BLUR
                    case "input": kind = HN_EV_INPUT
                    case "change": kind = HN_EV_CHANGE
                    case "submit": kind = HN_EV_SUBMIT
                    case "scroll": kind = HN_EV_SCROLL
                    default: kind = HN_EV_NONE
                    }
                    // 目标: 指定 id, 或按坐标命中
                    var target: OpaquePointer?
                    if let tid = obj["target"] as? String,
                       let ctx = Optional(nv.engineContext), let doc = hn_context_doc(ctx) {
                        target = hn_doc_find_by_id(doc, tid)
                    }
                    var pt: NSPoint?
                    if let x = obj["x"] as? Double, let y = obj["y"] as? Double {
                        pt = NSPoint(x: x, y: y)
                    }
                    if target == nil && pt == nil {
                        out = false
                        oc = ["noTarget": true]
                    } else {
                        out = nv.emit(kind, at: pt, node: target,
                                      keyCode: Int32(obj["keyCode"] as? Int ?? 0),
                                      key: obj["key"] as? String,
                                      modifiers: UInt32(obj["modifiers"] as? Int ?? 0),
                                      text: obj["text"] as? String)
                        /* 回传三态, 让 agent 能区分"没人监听"与"处理了但没阻止冒泡"。
                           之前只有一个 consumed, 于是"点击确实改了界面"也会报
                           false, agent 会误判成没生效。 */
                        let oc2 = nv.lastEventOutcome
                        oc = ["handled": oc2.handled,
                              "prevented": oc2.prevented,
                              "hx": oc2.hx]
                    }
                }
            }
            return resp(true, ["consumed": out, "id": id, "detail": oc])

        case "eval":
            guard let id = obj["id"] as? String, let js = obj["js"] as? String else {
                return resp(false, ["error": "id/js required"])
            }
            var value: Any = "(未找到应用: \(id))"
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id) {
                    // 两个渲染器都实现 evalSync: native 走 JavaScriptCore,
                    // webkit 走 WKWebView。agent 因此不必关心用了哪个渲染器。
                    value = app.host.evalSync(js) ?? "(无 JS 环境)"
                }
            }
            return resp(true, ["value": value])

        case "dump":
            guard let id = obj["id"] as? String else { return resp(false, ["error": "id required"]) }
            var items: [[String: Any]] = []
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id) {
                    items = app.host.dumpDisplayList()
                }
            }
            return resp(true, ["id": id, "cmds": items])

        case "anim":
            guard let id = obj["id"] as? String else {
                return resp(false, ["error": "id required"])
            }
            var lines: [String] = []
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id), let v = app.view,
                   let ctx = Optional(v.engineContext), let doc = hn_context_doc(ctx) {
                    // 报告所有声明了动画的元素
                    var idx: Int32 = 0
                    while let n = hn_doc_find_attr(doc, "class", idx) {
                        idx += 1
                        var name: UnsafePointer<CChar>?
                        var ms: Float = 0
                        var iter: Int32 = 0
                        hn_node_debug_anim(n, &name, &ms, &iter)
                        guard let np = name else { continue }
                        var rot: Float = 0
                        hn_node_debug_rotate(n, &rot)
                        let id2 = hn_node_attr(n, "id").map { String(cString: $0) } ?? "-"
                        lines.append("\(String(cString: np)) \(Int(ms))ms iter=\(iter) rotate=\(Int(rot))° (id=\(id2))")
                    }
                    if lines.isEmpty { lines.append("(无动画元素)") }
                }
            }
            return resp(true, ["id": id, "anim": lines])

        case "text":
            guard let id = obj["id"] as? String, let eid = obj["element"] as? String else {
                return resp(false, ["error": "id/element required"])
            }
            var txt = ""
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id), let v = app.view {
                    txt = v.textOf(id: eid) ?? "(未找到 \(eid))"
                }
            }
            return resp(true, ["text": txt])

        case "dom":
            guard let id = obj["id"] as? String else { return resp(false, ["error": "id required"]) }
            var lines: [String] = []
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id) { lines = app.host.dumpDOM() }
            }
            return resp(true, ["id": id, "dom": lines])

        case "list":
            var items: [[String: Any]] = []
            DispatchQueue.main.sync {
                items = HNEngine.shared.list().map {
                    var d: [String: Any] = ["id": $0.id, "surface": $0.surface.rawValue,
                                            "title": $0.title]
                    if let a = $0.applet { d["applet"] = a }
                    return d
                }
            }
            return resp(true, ["apps": items])

        case "applets":
            /* 轻应用槽位清单(agent 用): 槽位是持久的, 应用是瞬时的 —— 所以这里
               把"盘上记着的"和"此刻开着的"合成一份, 让 agent 一眼看出哪些槽位
               还活着、哪些只是留了个位置。 */
            var items: [[String: Any]] = []
            DispatchQueue.main.sync {
                var live: [String: HNApp] = [:]
                for info in HNEngine.shared.list() {
                    if let a = info.applet, let app = HNEngine.shared.app(id: info.id) {
                        live[a] = app
                    }
                }
                var slots = HNAppletStore.enumerate()
                /* 调用方显式指定了位置的实例不落盘(见 HNEngine.open), 于是它
                   在盘上没有槽位文件 —— 但它是开着的, 枚举就得如实报出来,
                   否则 agent 看到的是"这个轻应用不存在"。这里按运行态补上,
                   位置取窗口当前位置。补进来的条目不带 appId/lastSeen。 */
                for (name, app) in live where !slots.contains(where: { $0.name == name }) {
                    let f = app.window.frame
                    slots.append(HNAppletStore.SlotInfo(
                        name: name, x: Double(f.minX), y: Double(f.minY),
                        w: Double(f.width), h: Double(f.height),
                        appId: nil, lastSeen: nil, open: true))
                }
                items = slots.sorted { $0.name < $1.name }.map { s in
                    /* 对外一律给展示坐标(左上原点、y 向下、相对主屏顶边),
                       与 hn-x/hn-y 同一套读法; 槽位文件里的绝对坐标是实现细节。 */
                    let d0 = s.displayTopLeft
                    var d: [String: Any] = ["name": s.name,
                                            "x": d0.x, "y": d0.y, "w": d0.w, "h": d0.h,
                                            "open": live[s.name] != nil]
                    if let a = s.appId { d["appId"] = a }
                    if let t = s.lastSeen { d["lastSeen"] = t }
                    if let app = live[s.name] { d["id"] = app.id }
                    return d
                }
            }
            return resp(true, ["applets": items])

        case "applet-remove":
            /* 忘记槽位: 下次打开回到页面声明的位置。
               注意要连活着的实例一起摘掉 —— 否则运行中的实例关闭时会把几何
               又写回去, 删除随即失效(见 HNEngine.detachApplet)。 */
            guard let name = obj["name"] as? String else { return resp(false, ["error": "name required"]) }
            var existed = false
            DispatchQueue.main.sync {
                existed = HNAppletStore(name: name).geometry() != nil
                HNAppletStore.remove(named: name)
                HNEngine.shared.detachApplet(named: name)
            }
            return resp(true, ["name": name, "removed": existed])

        case "persist":
            guard let id = obj["id"] as? String else { return resp(false, ["error": "id required"]) }
            do {
                var url: URL = HNPaths.apps
                try DispatchQueue.main.sync { url = try HNEngine.shared.persist(id: id) }
                return resp(true, ["id": id, "path": url.path])
            } catch {
                return resp(false, ["error": "\(error.localizedDescription)"])
            }

        case "restore":
            guard let id = obj["id"] as? String else { return resp(false, ["error": "id required"]) }
            var ok = false
            DispatchQueue.main.sync { ok = HNEngine.shared.restore(id: id) }
            return ok ? resp(true, ["id": id]) : resp(false, ["error": "no saved app: \(id)"])

        default:
            return resp(false, ["error": "unknown op: \(op)"])
        }
    }

    static func resp(_ ok: Bool, _ fields: [String: Any]) -> String {
        var obj = fields
        obj["ok"] = ok
        let data = (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8)
        return String(data: data, encoding: .utf8) ?? "{}"
    }
}

// MARK: - Unix socket 行协议服务器

final class UnixLineServer {
    private var listenFD: Int32 = -1
    private let handler: (String) -> String

    init(path: String, handler: @escaping (String) -> String) throws {
        self.handler = handler

        let dir = (path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(atPath: path)

        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw NSError(domain: "UnixLineServer", code: 1) }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        withUnsafeMutableBytes(of: &addr.sun_path) { ptr in
            path.withCString { cstr in
                strncpy(ptr.baseAddress!.assumingMemoryBound(to: CChar.self), cstr, 103)
            }
        }
        let bound = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                bind(fd, sa, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard bound == 0, listen(fd, 16) == 0 else {
            close(fd)
            throw NSError(domain: "UnixLineServer", code: 2)
        }
        listenFD = fd
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            self?.acceptLoop()
        }
    }

    private func acceptLoop() {
        while true {
            let cfd = accept(listenFD, nil, nil)
            if cfd < 0 { continue }
            DispatchQueue.global().async { [weak self] in
                self?.serve(cfd)
            }
        }
    }

    private func serve(_ cfd: Int32) {
        var buf = [UInt8]()
        var tmp = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = recv(cfd, &tmp, tmp.count, 0)
            if n <= 0 { close(cfd); return }
            var start = 0
            for i in 0..<n {
                if tmp[i] == 0x0A {
                    buf.append(contentsOf: tmp[start..<i])
                    let line = String(bytes: buf, encoding: .utf8) ?? ""
                    buf.removeAll(keepingCapacity: true)
                    let out = Array((handler(line) + "\n").utf8)
                    if !sendAll(cfd, out) { close(cfd); return }
                    start = i + 1
                }
            }
            if start < n { buf.append(contentsOf: tmp[start..<n]) }
            if buf.count > 16 * 1024 * 1024 { close(cfd); return } // 防失控
        }
    }

    private func sendAll(_ fd: Int32, _ data: [UInt8]) -> Bool {
        var off = 0
        while off < data.count {
            let n = data.withUnsafeBufferPointer { ptr in
                send(fd, ptr.baseAddress!.advanced(by: off), data.count - off, 0)
            }
            if n <= 0 { return false }
            off += n
        }
        return true
    }
}
