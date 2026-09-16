import Foundation
import HtmlNative
import IOKit

// hn — html-native 系统入口(agent/工具用)。
//   hn open <id> [file.html|-] [--css f] [--surface window|popup|layer]
//      [--title t] [--w N] [--h N] [--x N --y N]    打开或热更新应用
//   hn update <id> [file.html|-]                     热更新(窗口保持)
//   hn close <id>                                    销毁
//   hn list                                          列出运行中的应用
//   hn persist <id>                                  持久化为 .hnapp 包
//   hn restore <id>                                  从包恢复
//   hn ping
//
// 宿主未运行时自动拉起 HnDaemon(常驻, 不占 Dock)。

var args = Array(CommandLine.arguments.dropFirst())
guard let cmd = args.first else { usage(); exit(2) }
args.removeFirst()

func usage() {
    print("用法: hn open|update|close|list|persist|restore|ping ... (见源码注释)")
}

struct Opts {
    var id: String?
    var file: String?
    var css: String?
    var surface: String?
    var title: String?
    var w: Double?
    var h: Double?
    var x: Double?
    var y: Double?
    var ttl: Double?
    var message: String?
}
var o = Opts()
var i = 0
while i < args.count {
    let a = args[i]
    switch a {
    case "--css": i += 1; o.css = i < args.count ? args[i] : nil
    case "--surface": i += 1; o.surface = i < args.count ? args[i] : nil
    case "--title": i += 1; o.title = i < args.count ? args[i] : nil
    case "--w": i += 1; o.w = i < args.count ? Double(args[i]) : nil
    case "--h": i += 1; o.h = i < args.count ? Double(args[i]) : nil
    case "--x": i += 1; o.x = i < args.count ? Double(args[i]) : nil
    case "--y": i += 1; o.y = i < args.count ? Double(args[i]) : nil
    case "--ttl": i += 1; o.ttl = i < args.count ? Double(args[i]) : nil
    default:
        if o.id == nil { o.id = a } else if o.file == nil { o.file = a }
    }
    i += 1
}

func readHTML(_ file: String?) -> String? {
    if let file, file != "-" {
        return try? String(contentsOfFile: file, encoding: .utf8)
    }
    if isatty(STDIN_FILENO) == 0 {
        let data = FileHandle.standardInput.readDataToEndOfFile()
        if !data.isEmpty { return String(data: data, encoding: .utf8) }
    }
    return nil
}

// ---- 宿主连接(自动拉起) ----

func connectOrSpawn() -> Int32? {
    if let fd = connectHost() { return fd }
    spawnDaemon()
    for _ in 0..<40 { // 最多等 2s
        usleep(50_000)
        if let fd = connectHost() { return fd }
    }
    return nil
}

func connectHost() -> Int32? {
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { return nil }
    var addr = sockaddr_un()
    addr.sun_family = sa_family_t(AF_UNIX)
    let path = "~/.html-native/hn.sock" // 由 CLI 直接拼 HOME, 避免 import AppKit
    let home = FileManager.default.homeDirectoryForCurrentUser.path
    let sock = home + "/.html-native/hn.sock"
    _ = path
    withUnsafeMutableBytes(of: &addr.sun_path) { ptr in
        sock.withCString { cstr in
            strncpy(ptr.baseAddress!.assumingMemoryBound(to: CChar.self), cstr, 103)
        }
    }
    let ok = withUnsafePointer(to: &addr) { ptr in
        ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
            connect(fd, sa, socklen_t(MemoryLayout<sockaddr_un>.size))
        }
    }
    return ok == 0 ? fd : nil
}

func spawnDaemon() {
    let exe = CommandLine.arguments[0] as NSString
    let daemon = (exe.deletingLastPathComponent as NSString).appendingPathComponent("HnDaemon")
    guard FileManager.default.fileExists(atPath: daemon) else {
        FileHandle.standardError.write("找不到宿主二进制: \(daemon)\n".data(using: .utf8)!)
        return
    }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: daemon)
    p.standardOutput = FileHandle.nullDevice
    try? p.run()
}

func rpc(_ obj: [String: Any]) -> [String: Any]? {
    guard let fd = connectOrSpawn() else {
        FileHandle.standardError.write("无法连接宿主(~/.html-native/hn.sock)\n".data(using: .utf8)!)
        return nil
    }
    defer { close(fd) }
    guard let data = try? JSONSerialization.data(withJSONObject: obj),
          let line = String(data: data, encoding: .utf8) else { return nil }
    var payload = Array(line.utf8) + [0x0A]
    let n = payload.count
    payload.withUnsafeMutableBufferPointer { ptr in
        _ = send(fd, ptr.baseAddress, n, 0)
    }
    // 读一行响应
    var buf = [UInt8]()
    var tmp = [UInt8](repeating: 0, count: 65536)
    while true {
        let n = recv(fd, &tmp, tmp.count, 0)
        if n <= 0 { break }
        for b in tmp[0..<n] {
            buf.append(b)
            if b == 0x0A {
                let s = String(bytes: buf, encoding: .utf8) ?? ""
                return (try? JSONSerialization.jsonObject(with: Data(s.utf8))) as? [String: Any]
            }
        }
    }
    return nil
}

// ---- 命令分发 ----

switch cmd {
case "new":
    guard let name = o.id ?? o.file else { usage(); exit(2) }
    let direct = URL(fileURLWithPath: name)
    var isDir: ObjCBool = false
    let dir = FileManager.default.fileExists(atPath: name, isDirectory: &isDir) && isDir.boolValue
        ? direct
        : URL(fileURLWithPath: ".").appendingPathComponent(name)
    let app = dir.appendingPathComponent("app.html")
    let dirName = dir.lastPathComponent
    let ok = Scaffold.make(at: dir, name: dirName)
    if ok {
        print("已创建: \(dir.path)")
        print("  开发: hn dev \(app.path) --css \(dir.appendingPathComponent("app.css").path)")
        print("  运行: hn open \(dirName) \(app.path) --css \(dir.appendingPathComponent("app.css").path)")
    } else { exit(1) }

case "dev":
    guard let file = o.file ?? o.id else { usage(); exit(2) }
    var obj: [String: Any] = ["op": "dev", "path": file]
    if let v = o.id, v != file { obj["id"] = v }
    if let v = o.css { obj["css"] = v }
    if let v = o.surface { obj["surface"] = v }
    let r = rpc(obj) ?? [:]
    if (r["ok"] as? Bool) == true {
        print("开发模式: \(r["id"] ?? "?") 监听 \(file)")
        print("编辑文件并保存, 窗口即时热更新; Ctrl-C 退出后用 hn close <id> 关闭。")
    } else {
        print("失败: \(r["error"] as? String ?? "?")"); exit(1)
    }

case "syscard":
    let r = rpc(["op": "open", "id": "syscard", "html": SysCard.html,
                 "surface": "popup", "w": 300.0, "h": 430.0]) ?? [:]
    if (r["ok"] as? Bool) == true { print("系统信息卡已打开: syscard (hn close syscard 销毁)") }
    else { print("失败: \(r["error"] as? String ?? "?")"); exit(1) }

case "ping":
    let r = rpc(["op": "ping"]) ?? [:]
    print(r["engine"] as? String ?? "no response")

case "open", "update":
    guard cmd == "update" || o.id != nil || true else { exit(2) }
    guard let html = readHTML(o.file) else {
        FileHandle.standardError.write("缺少 hn 编码(文件或 stdin)\n".data(using: .utf8)!)
        exit(2)
    }
    var obj: [String: Any] = ["op": cmd, "html": html]
    if let v = o.id { obj["id"] = v }
    if cmd == "open" {
        if let v = o.css, let s = try? String(contentsOfFile: v, encoding: .utf8) { obj["css"] = s }
        if let v = o.surface { obj["surface"] = v }
        if let v = o.title { obj["title"] = v }
        if let v = o.w { obj["w"] = v }
        if let v = o.h { obj["h"] = v }
        if let v = o.x { obj["x"] = v }
        if let v = o.y { obj["y"] = v }
        if let v = o.ttl { obj["ttl"] = v }
    }
    let r = rpc(obj) ?? [:]
    if (r["ok"] as? Bool) == true {
        let id = r["id"] as? String ?? o.id ?? "?"
        let created = r["created"] as? Bool ?? false
        if cmd == "update" { print("已热更新: \(id)") }
        else { print(created ? "已打开: \(id)" : "已热更新: \(id)") }
    } else {
        print("失败: \(r["error"] as? String ?? "unknown")")
        exit(1)
    }

case "close":
    guard let id = o.id else { usage(); exit(2) }
    _ = rpc(["op": "close", "id": id])
    print("已销毁: \(id)")

case "list":
    let r = rpc(["op": "list"]) ?? [:]
    if let apps = r["apps"] as? [[String: Any]] {
        if apps.isEmpty { print("(无运行中的应用)") }
        for a in apps {
            print("\(a["id"] ?? "?")\t\(a["surface"] ?? "?")\t\(a["title"] ?? "")")
        }
    } else {
        print("无响应"); exit(1)
    }

case "dump":
    guard let id = o.id else { usage(); exit(2) }
    guard let r = rpc(["op": "dump", "id": id]),
          let cmds = r["cmds"] as? [[String: Any]] else { print("无响应"); exit(1) }
    print("指令数: \(cmds.count)")
    for c in cmds {
        let k = c["k"] as? String ?? "?"
        let x = c["x"] as? Int ?? 0, y = c["y"] as? Int ?? 0
        let w = c["w"] as? Int ?? 0, h = c["h"] as? Int ?? 0
        if k == "t" {
            print(String(format: "t x=%4d y=%4d %@", x, y, (c["s"] as? String) ?? ""))
        } else {
            print(String(format: "%@ x=%4d y=%4d w=%4d h=%4d", k, x, y, w, h))
        }
    }

case "eval":
    guard let id = o.id, let js = o.message ?? o.file else { usage(); exit(2) }
    let r = rpc(["op": "eval", "id": id, "js": js]) ?? [:]
    if let v = r["value"] { print(v) } else { print("失败: \(r["error"] ?? "?")"); exit(1) }

case "dom":
    guard let id = o.id else { usage(); exit(2) }
    guard let r = rpc(["op": "dom", "id": id]), let lines = r["dom"] as? [String] else { print("无响应"); exit(1) }
    for l in lines { print(l) }

case "persist":
    guard let id = o.id else { usage(); exit(2) }
    let r = rpc(["op": "persist", "id": id]) ?? [:]
    if let path = r["path"] as? String { print("已持久化: \(path)") }
    else { print("失败: \(r["error"] as? String ?? "?")"); exit(1) }

case "restore":
    guard let id = o.id else { usage(); exit(2) }
    let r = rpc(["op": "restore", "id": id]) ?? [:]
    if (r["ok"] as? Bool) == true { print("已恢复: \(id)") }
    else { print("失败: \(r["error"] as? String ?? "?")"); exit(1) }

default:
    usage()
    exit(2)
}
