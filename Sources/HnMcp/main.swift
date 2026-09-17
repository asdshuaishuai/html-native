#!/usr/bin/env swift
// hn-mcp — html-native MCP server (stdio, JSON-RPC 2.0)
//
// 把 hn 的应用能力暴露为 MCP 工具, 让 agent 直接创建/更新/销毁原生窗口,
// 而不用拼命令行。协议: MCP over stdio, 每行一个 JSON-RPC 消息。
//
// 工具集:
//   hn_open       打开应用(html 内容或文件路径, 可选 ttl/surface/尺寸/标题)
//   hn_update     热更新已有应用(窗口与状态保持)
//   hn_close      销毁应用
//   hn_list       列出运行中的应用
//   hn_persist    持久化为 .hnapp 离线包
//   hn_restore    从 .hnapp 恢复
//   hn_sys        读取系统数据片段(sys://cpu 等), 返回 HTML 片段
//   hn_shot       渲染截图(真实视图自绘, 无需屏幕权限)
//   hn_dom        内省 DOM 树(排查布局问题)
//   hn_dump       内省绘制指令列表
//
// 依赖: 与 hn CLI 同一个守护进程(Unix socket), 首次调用自动拉起。

import Foundation

// ---------- 与守护进程通信 ----------

let home = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent(".html-native", isDirectory: true)
let sockPath = home.appendingPathComponent("hn.sock").path

func rpc(_ obj: [String: Any], timeout: TimeInterval = 30) -> [String: Any]? {
    // 每次连接(简单可靠; 守护进程按行应答)
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { return nil }
    defer { close(fd) }

    var addr = sockaddr_un()
    addr.sun_family = sa_family_t(AF_UNIX)
    let pathBytes = Array(sockPath.utf8)
    guard pathBytes.count < MemoryLayout.size(ofValue: addr.sun_path) else { return nil }
    withUnsafeMutablePointer(to: &addr.sun_path) { p in
        p.withMemoryRebound(to: CChar.self, capacity: pathBytes.count) { dst in
            for (i, b) in pathBytes.enumerated() { dst[i] = CChar(bitPattern: b) }
        }
    }
    let connected = withUnsafePointer(to: &addr) { ap -> Bool in
        ap.withMemoryRebound(to: sockaddr.self, capacity: 1) { sp in
            connect(fd, sp, socklen_t(MemoryLayout<sockaddr_un>.size)) == 0
        }
    }
    guard connected else { return nil }

    guard let data = try? JSONSerialization.data(withJSONObject: obj),
          var line = String(data: data, encoding: .utf8) else { return nil }
    line += "\n"
    _ = line.withCString { write(fd, $0, strlen($0)) }

    var buf = [UInt8](repeating: 0, count: 1 << 20)
    var total = 0
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        let n = read(fd, &buf[total], buf.count - total - 1)
        if n <= 0 { break }
        total += n
        if buf[total - 1] == 0x0A { break }
    }
    guard total > 0, let parsed = try? JSONSerialization.jsonObject(with: Data(buf[0..<total])) as? [String: Any]
    else { return nil }
    return parsed
}

/// 确保守护进程在跑(hn CLI 自带拉起逻辑; 这里直接拉起二进制)
func ensureDaemon() {
    if rpc(["op": "ping"], timeout: 2) != nil { return }
    // 找 HnDaemon: 与 hn-mcp 同目录, 或 .build/debug
    let candidates = [
        Bundle.main.bundlePath.isEmpty ? nil : URL(fileURLWithPath: Bundle.main.bundlePath).deletingLastPathComponent(),
        URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent(".build/debug"),
    ].compactMap { $0 }
    for dir in candidates {
        let bin = dir.appendingPathComponent("HnDaemon")
        if FileManager.default.isExecutableFile(atPath: bin.path) {
            let p = Process()
            p.executableURL = bin
            p.standardOutput = FileHandle.nullDevice
            p.standardError = FileHandle.nullDevice
            try? p.run()
            // 等 socket 就绪
            for _ in 0..<40 {
                if rpc(["op": "ping"], timeout: 1) != nil { return }
                Thread.sleep(forTimeInterval: 0.15)
            }
            return
        }
    }
}

// ---------- 工具定义 ----------

struct Tool {
    let name: String
    let description: String
    let schema: [String: Any]
}

let tools: [Tool] = [
    Tool(name: "hn_open",
         description: "打开一个原生窗口应用。可传 html 内容或 file 路径(读文件)。id 已存在则热更新。ttl 秒后自动销毁(消息卡片语义)。",
         schema: [
            "type": "object",
            "properties": [
                "id": ["type": "string", "description": "应用标识(同 id 重复调用=热更新)"],
                "html": ["type": "string", "description": "HTML 内容(与 file 二选一)"],
                "file": ["type": "string", "description": "HTML 文件路径(与 html 二选一)"],
                "css": ["type": "string", "description": "CSS 文件路径(可选)"],
                "surface": ["type": "string", "enum": ["window", "popup", "layer"],
                            "description": "表面类型; 默认读 meta hn-surface"],
                "title": ["type": "string", "description": "窗口标题(覆盖 meta)"],
                "width": ["type": "number"], "height": ["type": "number"],
                "x": ["type": "number"], "y": ["type": "number"],
                "ttl": ["type": "number", "description": "存活秒数, 到期自动销毁"],
            ],
            "required": ["id"],
         ]),
    Tool(name: "hn_update",
         description: "热更新应用内容: 窗口与运行状态保持, 只替换文档。",
         schema: [
            "type": "object",
            "properties": [
                "id": ["type": "string"],
                "html": ["type": "string"],
                "file": ["type": "string"],
            ],
            "required": ["id"],
         ]),
    Tool(name: "hn_close",
         description: "销毁应用并关闭窗口。",
         schema: ["type": "object", "properties": ["id": ["type": "string"]], "required": ["id"]]),
    Tool(name: "hn_list",
         description: "列出当前运行中的应用(id / 表面 / 标题)。",
         schema: ["type": "object", "properties": [:]]),
    Tool(name: "hn_persist",
         description: "把应用持久化为 .hnapp 离线包(~/.html-native/apps/)。",
         schema: ["type": "object", "properties": ["id": ["type": "string"]], "required": ["id"]]),
    Tool(name: "hn_restore",
         description: "从 .hnapp 离线包恢复应用。",
         schema: ["type": "object", "properties": ["id": ["type": "string"]], "required": ["id"]]),
    Tool(name: "hn_sys",
         description: "读取系统数据片段(返回 HTML, 自带样式, 可直接换入页面或作为 tool 结果返回给用户看)。route 取值: info/cpu/memory/disk/battery/uptime/host。",
         schema: [
            "type": "object",
            "properties": ["route": ["type": "string", "description": "sys:// 路径, 如 cpu"]],
            "required": ["route"],
         ]),
    Tool(name: "hn_shot",
         description: "把 HTML 渲染成 PNG(真实视图自绘, 不需要屏幕录制权限)。传 live=true 会执行页面里的 sys:// 拉取。",
         schema: [
            "type": "object",
            "properties": [
                "file": ["type": "string", "description": "HTML 文件路径"],
                "css": ["type": "string", "description": "CSS 文件路径(可选)"],
                "out": ["type": "string", "description": "输出 PNG 路径"],
                "width": ["type": "number"], "height": ["type": "number"],
                "live": ["type": "boolean", "description": "执行 load/轮询拉取后再截图"],
                "hover": ["type": "string", "description": "注入悬停态的元素 id"],
                "scroll": ["type": "string", "description": "滚动注入, 格式 id,dy"],
            ],
            "required": ["file", "out"],
         ]),
    Tool(name: "hn_dom",
         description: "内省运行中应用的 DOM 树(含每个节点的盒坐标与文本片段数)。排查布局问题用: 文本节点坐标应与自身元素盒一致。",
         schema: ["type": "object", "properties": ["id": ["type": "string"]], "required": ["id"]]),
    Tool(name: "hn_dump",
         description: "内省运行中应用的绘制指令列表(实际要画什么)。同一条文字不应出现多次(重影)。",
         schema: ["type": "object", "properties": ["id": ["type": "string"]], "required": ["id"]]),

    /* 下面三个是"驱动"能力。此前 MCP 只能开窗口 + 读界面, 无法点击、
       无法执行脚本 —— 也就是"看得见但动不了", 自动化只能做展示不能做交互。 */
    Tool(name: "hn_event",
         description: """
向运行中的应用注入一个交互事件(等价于用户操作)。返回三态: 已处理(有处理器)
/ 已派发但无人监听 / 目标未找到。kind 取值: click dblclick mousedown mouseup
mousemove mouseenter mouseleave keydown keyup focus blur input change submit scroll。
target 给元素 id(推荐); 或给 x/y 坐标按命中测试定位。
""",
         schema: ["type": "object", "properties": [
             "id": ["type": "string", "description": "应用标识"],
             "kind": ["type": "string", "description": "事件类型, 如 click / keydown"],
             "target": ["type": "string", "description": "目标元素 id"],
             "x": ["type": "number"], "y": ["type": "number"],
             "key": ["type": "string", "description": "键名, 如 Enter / ArrowLeft"],
             "modifiers": ["type": "integer",
                           "description": "修饰键位掩码: 1=shift 2=ctrl 4=alt 8=cmd"],
             "text": ["type": "string", "description": "input 事件的新值"],
         ], "required": ["id", "kind"]]),

    Tool(name: "hn_eval",
         description: """
在该应用的 JS 上下文里执行一段脚本并返回结果。
native 与 webkit 两个渲染器都可用 —— 用来读运行时状态、触发页面逻辑、
或在断言里取计算值(比解析 DOM 文本可靠)。
""",
         schema: ["type": "object", "properties": [
             "id": ["type": "string", "description": "应用标识"],
             "js": ["type": "string", "description": "要执行的 JS 表达式"],
         ], "required": ["id", "js"]]),

    Tool(name: "hn_text",
         description: "取运行中应用里某个元素的文本内容(比读整棵 DOM 省 token)。",
         schema: ["type": "object", "properties": [
             "id": ["type": "string"],
             "element": ["type": "string", "description": "元素 id"],
         ], "required": ["id", "element"]]),
]

// ---------- 工具实现 ----------

func readMaybeFile(_ inline: Any?, _ path: Any?) -> String? {
    if let s = inline as? String, !s.isEmpty { return s }
    if let p = path as? String, let s = try? String(contentsOfFile: p, encoding: .utf8) { return s }
    return nil
}

func callTool(_ name: String, _ args: [String: Any]) -> String {
    switch name {
    case "hn_open":
        guard let id = args["id"] as? String else { return "错误: 缺少 id" }
        guard let html = readMaybeFile(args["html"], args["file"]) else {
            return "错误: 需要 html 或 file"
        }
        var req: [String: Any] = ["op": "open", "id": id, "html": html]
        if let css = args["css"] as? String { req["css"] = css }
        if let s = args["surface"] as? String { req["surface"] = s }
        if let t = args["title"] as? String { req["title"] = t }
        if let w = args["width"] as? Double { req["w"] = w }
        if let h = args["height"] as? Double { req["h"] = h }
        if let x = args["x"] as? Double { req["x"] = x }
        if let y = args["y"] as? Double { req["y"] = y }
        if let ttl = args["ttl"] as? Double { req["ttl"] = ttl }
        let r = rpc(req) ?? [:]
        if let err = r["error"] as? String { return "失败: \(err)" }
        let title = r["title"] as? String ?? ""
        return "已打开: \(id)\(title.isEmpty ? "" : " (\(title))")"

    case "hn_update":
        guard let id = args["id"] as? String else { return "错误: 缺少 id" }
        guard let html = readMaybeFile(args["html"], args["file"]) else {
            return "错误: 需要 html 或 file"
        }
        let r = rpc(["op": "update", "id": id, "html": html]) ?? [:]
        if let err = r["error"] as? String { return "失败: \(err)" }
        return "已热更新: \(id)"

    case "hn_close":
        guard let id = args["id"] as? String else { return "错误: 缺少 id" }
        _ = rpc(["op": "close", "id": id])
        return "已销毁: \(id)"

    case "hn_list":
        let r = rpc(["op": "list"]) ?? [:]
        guard let apps = r["apps"] as? [[String: Any]] else { return "无响应" }
        if apps.isEmpty { return "(无运行中的应用)" }
        return apps.map { a in
            "\(a["id"] as? String ?? "?")  \(a["surface"] as? String ?? "?")  \(a["title"] as? String ?? "")"
        }.joined(separator: "\n")

    case "hn_persist":
        guard let id = args["id"] as? String else { return "错误: 缺少 id" }
        let r = rpc(["op": "persist", "id": id]) ?? [:]
        if let p = r["path"] as? String { return "已持久化: \(p)" }
        return "失败: \(r["error"] as? String ?? "?")"

    case "hn_restore":
        guard let id = args["id"] as? String else { return "错误: 缺少 id" }
        let r = rpc(["op": "restore", "id": id]) ?? [:]
        if let err = r["error"] as? String { return "失败: \(err)" }
        return "已恢复: \(id)"

    case "hn_sys":
        let route = args["route"] as? String ?? "info"
        // 本地直接生成片段(无需守护进程)
        return SystemBridgeFragment.fragment(for: "sys://" + route)

    case "hn_shot":
        guard let file = args["file"] as? String, let out = args["out"] as? String else {
            return "错误: 需要 file 与 out"
        }
        var argv = [file, out]
        if let css = args["css"] as? String, !css.isEmpty { argv.insert(css, at: 1) }
        var extra: [String] = []
        if let w = args["width"] as? Double { extra.append(String(Int(w))) }
        if let h = args["height"] as? Double { extra.append(String(Int(h))) }
        if args["live"] as? Bool == true { extra.append("--live") }
        if let hv = args["hover"] as? String { extra.append(contentsOf: ["--hover", hv]) }
        if let sc = args["scroll"] as? String { extra.append(contentsOf: ["--scroll", sc]) }
        let bin = findBinary("HnShot")
        guard let bin else { return "错误: 找不到 HnShot(需先 swift build)" }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: bin)
        p.arguments = argv + extra
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        try? p.run(); p.waitUntilExit()
        let outStr = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        if p.terminationStatus == 0 { return "已输出: \(out)" }
        return "失败: \(outStr)"

    case "hn_dom", "hn_dump":
        guard let id = args["id"] as? String else { return "错误: 缺少 id" }
        let op = name == "hn_dom" ? "dom" : "dump"
        let r = rpc(["op": op, "id": id]) ?? [:]
        if let lines = r["dom"] as? [String] { return lines.joined(separator: "\n") }
        if let cmds = r["cmds"] as? [[String: Any]] {
            return "指令数: \(cmds.count)\n" + cmds.map { c -> String in
                let k = c["k"] as? String ?? "?"
                let x = c["x"] as? Int ?? 0, y = c["y"] as? Int ?? 0
                let s = c["s"] as? String ?? ""
                return k == "t" ? "TEXT x=\(x) y=\(y) \(s)"
                                : "\(k) x=\(x) y=\(y) w=\(c["w"] as? Int ?? 0) h=\(c["h"] as? Int ?? 0)"
            }.joined(separator: "\n")
        }
        return "无响应(应用可能未运行)"

    case "hn_event":
        guard let id = args["id"] as? String, let kind = args["kind"] as? String else {
            return "错误: 缺少 id 或 kind"
        }
        var req: [String: Any] = ["op": "event", "id": id, "kind": kind]
        if let t = args["target"] as? String { req["target"] = t }
        if let k = args["key"] as? String { req["key"] = k }
        if let m = args["modifiers"] as? Int { req["modifiers"] = m }
        if let t = args["text"] as? String { req["text"] = t }
        if let x = args["x"] as? Double { req["x"] = x }
        if let y = args["y"] as? Double { req["y"] = y }
        let r = rpc(req) ?? [:]
        guard r["ok"] as? Bool == true else {
            return "失败: \(r["error"] ?? "?")"
        }
        /* 把三态讲清楚: "没人监听"与"处理了但没阻止冒泡"是两件不同的事,
           前者通常意味着 target 选错了或页面没挂处理器。 */
        let d = r["detail"] as? [String: Any] ?? [:]
        if d["noTarget"] as? Bool == true {
            return "目标未找到: 给 target(元素 id) 或 x/y 坐标"
        }
        let handled = d["handled"] as? Bool ?? false
        let hx = d["hx"] as? Bool ?? false
        if handled { return "已处理: \(kind) (JS 处理器被调用)" }
        if hx { return "已处理: \(kind) (触发 hx 行为)" }
        return "已派发但无人监听: \(kind)"

    case "hn_eval":
        guard let id = args["id"] as? String, let js = args["js"] as? String else {
            return "错误: 缺少 id 或 js"
        }
        let r = rpc(["op": "eval", "id": id, "js": js]) ?? [:]
        guard r["ok"] as? Bool == true else { return "失败: \(r["error"] ?? "?")" }
        return String(describing: r["value"] ?? "(nil)")

    case "hn_text":
        guard let id = args["id"] as? String, let el = args["element"] as? String else {
            return "错误: 缺少 id 或 element"
        }
        let r = rpc(["op": "text", "id": id, "element": el]) ?? [:]
        guard r["ok"] as? Bool == true else { return "失败: \(r["error"] ?? "?")" }
        return r["text"] as? String ?? "(空)"

    default:
        return "未知工具: \(name)"
    }
}

func findBinary(_ name: String) -> String? {
    let cwd = FileManager.default.currentDirectoryPath
    let candidates = [
        "\(cwd)/.build/debug/\(name)",
        "\(cwd)/.build/arm64-apple-macosx/debug/\(name)",
        "\(cwd)/dist/\(name)",
    ]
    return candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
}

// SystemBridge 是 Swift 运行时里的类型; MCP server 独立进程,
// 因此这里内联一份最小片段(与运行时保持同款类名, 由 hn-theme 皮肤化)。
enum SystemBridgeFragment {
    static func fragment(for url: String) -> String {
        let route = url.replacingOccurrences(of: "sys://", with: "").split(separator: "?").first.map(String.init) ?? "info"
        switch route {
        case "cpu":
            let cores = ProcessInfo.processInfo.activeProcessorCount
            return row("核心", "\(cores) 核")
        case "memory":
            let total = ProcessInfo.processInfo.physicalMemory / (1024 * 1024 * 1024)
            return row("物理内存", "\(total) GB")
        case "host":
            return row("主机", Host.current().localizedName ?? "Mac")
        case "uptime":
            var boot = timeval(); var len = MemoryLayout<timeval>.size
            sysctlbyname("kern.boottime", &boot, &len, nil, 0)
            let up = Int(Date().timeIntervalSince1970 - TimeInterval(boot.tv_sec))
            return row("开机时长", "\(up / 3600) 小时 \((up % 3600) / 60) 分")
        default:
            let v = ProcessInfo.processInfo.operatingSystemVersion
            return row("系统", "macOS \(v.majorVersion).\(v.minorVersion)")
                + row("架构", archName())
        }
    }
    static func archName() -> String {
        #if arch(arm64)
        return "Apple Silicon (arm64)"
        #else
        return "Intel (x86_64)"
        #endif
    }
    static func row(_ k: String, _ v: String) -> String {
        "<div class=\"sysrow\"><div class=\"sysk\">\(k)</div><div class=\"sysv\">\(v)</div></div>"
    }
}

// ---------- MCP / JSON-RPC 主循环 ----------

func send(_ obj: [String: Any]) {
    guard let data = try? JSONSerialization.data(withJSONObject: obj) else { return }
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
}

func reply(_ id: Any?, _ result: Any) {
    send(["jsonrpc": "2.0", "id": id ?? NSNull(), "result": result])
}

func replyError(_ id: Any?, _ code: Int, _ message: String) {
    send(["jsonrpc": "2.0", "id": id ?? NSNull(), "error": ["code": code, "message": message]])
}

ensureDaemon()

while let line = readLine(strippingNewline: true) {
    guard !line.isEmpty,
          let obj = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
          let method = obj["method"] as? String else { continue }
    let id = obj["id"]
    let params = obj["params"] as? [String: Any] ?? [:]

    switch method {
    case "initialize":
        reply(id, [
            "protocolVersion": "2024-11-05",
            "capabilities": ["tools": [:]],
            "serverInfo": ["name": "html-native", "version": "0.1.0"],
        ])
    case "notifications/initialized":
        continue
    case "tools/list":
        reply(id, ["tools": tools.map { ["name": $0.name, "description": $0.description,
                                          "inputSchema": $0.schema] }])
    case "tools/call":
        let name = params["name"] as? String ?? ""
        let args = params["arguments"] as? [String: Any] ?? [:]
        let text = callTool(name, args)
        reply(id, ["content": [["type": "text", "text": text]], "isError": false])
    case "ping":
        reply(id, [:])
    default:
        if id != nil { replyError(id, -32601, "method not found: \(method)") }
    }
}
