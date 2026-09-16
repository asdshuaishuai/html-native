import AppKit
import Foundation
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

        case "close":
            guard let id = obj["id"] as? String else { return resp(false, ["error": "id required"]) }
            DispatchQueue.main.sync { HNEngine.shared.close(id: id) }
            DevWatcher.stop(id: id)
            return resp(true, ["id": id])

        case "eval":
            guard let id = obj["id"] as? String, let js = obj["js"] as? String else {
                return resp(false, ["error": "id/js required"])
            }
            var value: Any = ""
            DispatchQueue.main.sync {
                if let app = HNEngine.shared.app(id: id) {
                    if let wk = app.host as? HNWebKitHost {
                        value = wk.evalSync(js) ?? "(nil)"
                    } else {
                        value = "(该应用使用 native 渲染器, 无 JS 上下文)"
                    }
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
                    ["id": $0.id, "surface": $0.surface.rawValue, "title": $0.title]
                }
            }
            return resp(true, ["apps": items])

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
