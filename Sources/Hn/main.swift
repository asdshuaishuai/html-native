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

/// 完整用法。这条输出是 **AI agent 的主要说明书** —— 它靠 `hn` 的
/// 输出来学会怎么驱动界面, 所以必须把命令、参数、与"能拿到什么"列全,
/// 而不是一句"见源码注释"。
func usage() {
    print("""
    hn — html-native 命令行(agent 的系统入口)

    窗口与表面
      hn open <id> <file.html> [--css a.css] [--surface window|popup|layer]
              [--title "T"] [--w W] [--h H] [--x X] [--y Y] [--ttl 秒]
      hn update <id> <file.html>    用新 HTML 整体替换(样式保留)
      hn dev <file.html>            开发模式: 保存即热更新
      hn close <id>                 销毁窗口
      hn list                       列出所有运行中的应用
      hn persist <id> <文件.json>   把该应用的本地存储落盘
      hn restore <id> <文件.json>   从文件恢复
      hn syscard                    打开系统信息卡
      hn ping                       探测宿主(并自动拉起)

    读取界面(agent 感知)
      hn dom <id> [--depth N]       打印 DOM 树(class + 深度)
      hn text <id> --element <sel>  取某元素的文本内容
      hn dump <id>                  打印绘制指令列表(display list)
      hn anim <id>                  列出该应用正在播放的动画

    驱动交互(agent 操作)
      hn event <id> --text click --target <元素id>
              --text 可用: click dblclick mousedown mouseup mousemove
                           mouseenter mouseleave keydown keyup focus blur
                           input change submit scroll
              可配: --x --y(坐标) --key "Enter" --key "ArrowLeft" --mods 8
                   (1=shift 2=ctrl 4=alt 8=cmd) --target 定位元素
      hn eval <id> --js <表达式>    在该应用的 JS 上下文里求值并打印结果
                                    (native 与 webkit 渲染器都可用)

    stdin 也可直接喂 HTML:  echo '<h1>hi</h1>' | hn open myapp -

    宿主: ~/.html-native/hn.sock (常驻进程, 不存在时自动拉起)
    """)
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
    var target: String?
    var key: String?
    var mods: Int?
    var text: String?
    var kind: String?      /* event 的事件类型 */
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
    case "--target": i += 1; o.target = i < args.count ? args[i] : nil
    case "--key": i += 1; o.key = i < args.count ? args[i] : nil
    case "--mods": i += 1; o.mods = i < args.count ? Int(args[i]) : nil
    case "--text": i += 1; o.text = i < args.count ? args[i] : nil
    /* --id 之前不存在: 所有未识别参数按位置落入 id/file, 于是
       "hn eval --id evtest --text ..." 里的 "--id" 被当成应用 id,
       daemon 收到 id="--id" → 报"未找到应用: --id"。这类失败很难自查,
       因为命令看上去完全正确。 */
    case "--id": i += 1; o.id = i < args.count ? args[i] : nil
    case "--element": i += 1; o.target = i < args.count ? args[i] : nil
    case "--js": i += 1; o.message = i < args.count ? args[i] : nil
    case "--kind": i += 1; o.kind = i < args.count ? args[i] : nil
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
        } else if k == "q", let pts = c["q"] as? [Int], pts.count >= 8 {
            print(String(format: "QUAD (%d,%d)(%d,%d)(%d,%d)(%d,%d) fill=%08X",
                         pts[0], pts[1], pts[2], pts[3], pts[4], pts[5], pts[6], pts[7],
                         (c["fill"] as? Int) ?? 0))
        } else {
            print(String(format: "%@ x=%4d y=%4d w=%4d h=%4d", k, x, y, w, h))
        }
    }

case "event":
    /* 事件类型两种写法都接受:
         hn event <id> click            (位置参数)
         hn event <id> --kind click     (显式)
       之前只有位置参数一种, 而 --text 又恰好是"输入事件的新值"参数,
       于是 "hn event <id> --text click" 会被解析成 kind 缺失而打出用法 ——
       命令看着完全正确却失败, 属于很难自查的那类。 */
    guard let id = o.id, let kind = o.kind ?? o.file else { usage(); exit(2) }
    var req: [String: Any] = ["op": "event", "id": id, "kind": kind]
    if let t = o.target { req["target"] = t }
    if let k = o.key { req["key"] = k }
    if let m = o.mods { req["modifiers"] = m }
    if let tx = o.text { req["text"] = tx }
    if let x = o.w, let y = o.h { req["x"] = x; req["y"] = y }
    let r = rpc(req) ?? [:]
    if let c = r["consumed"] as? Bool {
        /* 区分"没人监听"和"处理了但没阻止冒泡" —— 两者对判断有没有生效差别很大 */
        let d = r["detail"] as? [String: Any] ?? [:]
        let handled = d["handled"] as? Bool ?? false
        let hx = d["hx"] as? Bool ?? false
        let noTarget = d["noTarget"] as? Bool ?? false
        if noTarget { print("目标未找到: --target <元素id> 或给 --x/--y"); exit(1) }
        if handled || hx || c {
            let via = hx && !handled ? "(hx 行为)" : "(JS 处理器)"
            print("事件已处理: \(kind) \(via)")
        } else {
            print("事件已派发但无人监听: \(kind)")
        }
    } else { print("失败: \(r["error"] ?? "?")"); exit(1) }

case "eval":
    guard let id = o.id, let js = o.message else { usage(); exit(2) }
    let r = rpc(["op": "eval", "id": id, "js": js]) ?? [:]
    if let v = r["value"] { print(v) } else { print("失败: \(r["error"] ?? "?")"); exit(1) }

case "lifecycle":
    /* 事件类型走 --kind(与 event 一致); o.message 从未被 --message 填充,
       用它会导致命令永远打印用法。 */
    guard let id = o.id, let k = o.kind ?? o.message else { usage(); exit(2) }
    let r = rpc(["op": "lifecycle", "id": id, "kind": k]) ?? [:]
    if let d = r["dispatched"] as? Bool {
        print(d ? "已派发: \(k)" : "该页面未声明 hn-lifecycle=\"\(k)\"")
    } else { print("失败: \(r["error"] ?? "?")"); exit(1) }

case "anim":
    guard let id = o.id else { usage(); exit(2) }
    let r = rpc(["op": "anim", "id": id]) ?? [:]
    if let lines = r["anim"] as? [String] { for l in lines { print(l) } }
    else { print("失败"); exit(1) }

case "text":
    guard let id = o.id, let el = o.target else { usage(); exit(2) }
    let r = rpc(["op": "text", "id": id, "element": el]) ?? [:]
    if let t = r["text"] as? String { print(t) } else { print("失败"); exit(1) }

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
