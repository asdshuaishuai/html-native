import AppKit
import CHtmlNative
import Foundation

/// .hnapp 胶囊 —— 一个文件装下"应用 + 它里面的全部数据 + agent 怎么驱动它"。
///
/// 参照 Capsule 的形态: 程序与其内容打成同一个可携带的文档, 而不是散落在运行
/// 时的多个私有目录里。落到 html-native 上, 一个胶囊包含:
///   文档    html / css
///   表面    表面类型与尺寸
///   数据    页面自己的 KV(原 ~/.html-native/store/<id>.json)
///   几何    槽位位置(原 ~/.html-native/applets/<名>.json)
///   指令    agent 的操作指令 + 运行期导出的能力图 + 操作历史
///
/// 为什么把 agent 指令当成一等公民: 这些持久化应用**绝大多数是 agent 造的**,
/// 而下一个(或另一个)agent 拿到文件时唯一需要的就是"这是什么、怎么驱动它"。
/// 少了这一段, 持久化出来的只是一张截图 —— 有界面没有语义, 谁也用不动它。
///
/// 兼容: v1 文件(只有 id/html/css/surface/title/w/h)照常还原, 缺的段按空处理。
public struct HNCapsule {

    /// 运行期导出的"怎么驱动它"能力图。全部来自文档与声明, 零副作用 ——
    /// 不做"逐个元素发一遍事件看谁响应"那种探测(那会改动页面状态)。
    public struct Capabilities {
        /// 文档里所有带 id 的元素(agent 的 --target 只能落在这些上)
        public var ids: [String] = []
        /// id → 它的 hx-trigger 声明(空串表示未声明, 按 click 处理)
        public var triggers: [String: String] = [:]
        /// 轮询元素: id → 间隔毫秒(来自 hx-trigger="every Ns")
        public var polls: [String: Int] = [:]
        /// 声明了 hn-lifecycle 的事件名
        public var lifecycle: [String] = []
        /// 占用的轻应用槽位名
        public var applet: String?
        /// 拖拽手柄元素(声明了 hn-drag 的链上元素)
        public var dragHandles: [String] = []
        /// 输入控件 id(页面有可填的东西)
        public var inputs: [String] = []
        /// 图片/矢量资源(字体之外的二进制依赖, 打包时要留意)
        public var media: [String] = []

        /// 导出一段人能读、agent 也能读的说明。刻意写成命令形态 —— 拿到这段的
        /// agent 不需要再读源码就知道该发什么。
        public func describe(id: String) -> String {
            var s: [String] = []
            s.append("应用 \(id): 表面由胶囊 surface 段声明; 用 hn dom/\(id) 看结构。")
            if !ids.isEmpty {
                s.append("可定位元素(--target): \(ids.joined(separator: " "))")
            }
            if !triggers.isEmpty {
                let t = triggers.map { "\($0.key)=\($0.value.isEmpty ? "click(默认)" : $0.value)" }
                    .joined(separator: ", ")
                s.append("事件触发: \(t)")
            }
            if !polls.isEmpty {
                let p = polls.map { "\($0.key) 每 \($0.value)ms" }.joined(separator: "; ")
                s.append("自动轮询: \(p) —— 不需要 agent 手动驱动")
            }
            if !inputs.isEmpty {
                s.append("可填控件: \(inputs.joined(separator: " "))"
                    + " —— 用 hn event/\(id) --kind input --target <id> --text <值>")
            }
            if !dragHandles.isEmpty {
                s.append("拖拽手柄: \(dragHandles.joined(separator: " "))")
            }
            if !lifecycle.isEmpty {
                s.append("生命周期: \(lifecycle.joined(separator: " "))"
                    + " —— 用 hn lifecycle/\(id) --kind hide 手动触发")
            }
            if let a = applet {
                s.append("轻应用槽位: \(a) —— 位置按此名记忆, hn applet remove \(a) 可忘掉")
            }
            return s.joined(separator: "\n")
        }
    }

    public let id: String
    public var title: String
    public var surface: String
    public var w: Double, h: Double
    public var html: String
    public var css: String

    // ---- 数据 ----
    public var store: [String: String]
    public var slot: HNAppletStore.SlotInfo?

    // ---- agent 操作指令 ----
    /// 自由文本指令: 这是什么、该怎么驱动它
    public var instructions: String
    /// 指令来源: "agent" = 调用方给的真指令; "derived" = 运行时按能力图导出
    public var instructionsSource: String
    /// 该应用经历过的可回放操作(open/update/event/lifecycle)
    public var ops: [[String: Any]]

    public var capabilities: Capabilities

    // MARK: - 导出

    /// 从运行中的应用打一个胶囊。
    /// - Parameter instructions: agent 写的操作指令; 传空则按能力图自动导出
    ///   (来源标为 derived, 让调用方知道该补一段更好的)。
    public static func make(app: HNApp, instructions: String = "") -> HNCapsule {
        let m = app.manifestForCapsule()
        let caps = Capabilities.derive(from: app)
        let title = app.window.title.isEmpty ? app.label : app.window.title
        let src: String
        let text: String
        if !instructions.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            text = instructions
            src = "agent"
        } else {
            text = caps.describe(id: app.id)
            src = "derived"
        }
        let slotInfo = app.applet.flatMap { name -> HNAppletStore.SlotInfo? in
            let st = HNAppletStore(name: name)
            guard let g = st.geometry() else { return nil }
            return HNAppletStore.SlotInfo(name: name, x: g.x, y: g.y, w: g.w, h: g.h,
                                          appId: st.lastAppId(), lastSeen: st.lastSeen(),
                                          open: false)
        }
        return HNCapsule(
            id: app.id, title: title, surface: app.surface.rawValue,
            w: Double(app.host.asView.bounds.width), h: Double(app.host.asView.bounds.height),
            html: app.html, css: app.css ?? "",
            store: HNStore(id: app.id).snapshot(), slot: slotInfo,
            instructions: text, instructionsSource: src,
            ops: HNEngine.shared.opLog(for: app.id),
            capabilities: caps, manifestTitle: m.title, manifestLifecycle: m.lifecycle)
    }

    public func toJSON() -> [String: Any] {
        var d: [String: Any] = [
            "format": "hnapp",
            "version": 2,
            "id": id, "title": title, "surface": surface,
            "w": w, "h": h,
            "html": html, "css": css,
            "store": store,
            "instructions": instructions,
            "instructionsSource": instructionsSource,
            "ops": ops,
            "capabilities": capabilities.toJSON(),
        ]
        if let s = slot {
            d["slot"] = ["name": s.name, "x": s.x, "y": s.y, "w": s.w, "h": s.h,
                         "appId": s.appId ?? "", "lastSeen": s.lastSeen ?? ""]
        }
        if !manifestTitle.isEmpty { d["manifestTitle"] = manifestTitle }
        if !manifestLifecycle.isEmpty { d["manifestLifecycle"] = manifestLifecycle }
        return d
    }

    /// 序列化成单文件内容。刻意保持**纯 JSON 文本**: 人类可直接 diff、
    /// agent 可直接读、任何编辑器都能打开 —— 二进制容器在这里没有收益,
    /// 而可读性是这类"文档式应用"的核心卖点。
    public func serialize() throws -> Data {
        try JSONSerialization.data(withJSONObject: toJSON(), options: [.prettyPrinted, .sortedKeys])
    }

    // MARK: - 导入

    public static func load(from url: URL) -> HNCapsule? {
        guard let data = try? Data(contentsOf: url),
              let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let html = obj["html"] as? String else { return nil }
        let id = obj["id"] as? String ?? url.deletingPathExtension().lastPathComponent
        let caps: Capabilities = (obj["capabilities"] as? [String: Any]).flatMap { Capabilities.fromJSON($0) }
            ?? Capabilities()
        var slot: HNAppletStore.SlotInfo?
        if let s = obj["slot"] as? [String: Any],
           let x = s["x"] as? Double, let y = s["y"] as? Double,
           let w = s["w"] as? Double, let h = s["h"] as? Double, w > 0, h > 0 {
            slot = HNAppletStore.SlotInfo(name: s["name"] as? String ?? id,
                                          x: x, y: y, w: w, h: h,
                                          appId: s["appId"] as? String,
                                          lastSeen: s["lastSeen"] as? String, open: false)
        }
        var cw = 0.0, ch = 0.0
        if let v = obj["w"] as? Double, v > 0 { cw = v }
        if let v = obj["h"] as? Double, v > 0 { ch = v }
        return HNCapsule(
            id: id,
            title: obj["title"] as? String ?? "",
            surface: obj["surface"] as? String ?? "window",
            w: cw, h: ch,
            html: html, css: obj["css"] as? String ?? "",
            store: (obj["store"] as? [String: String]) ?? [:],
            slot: slot,
            instructions: obj["instructions"] as? String ?? "",
            instructionsSource: obj["instructionsSource"] as? String ?? "agent",
            ops: (obj["ops"] as? [[String: Any]]) ?? [],
            capabilities: caps,
            manifestTitle: obj["manifestTitle"] as? String ?? "",
            manifestLifecycle: obj["manifestLifecycle"] as? String ?? "")
    }

    /// 该胶囊的版本(v1 老格式没有 version 字段)
    public let version: Int
    /// 清单里声明的标题/生命周期(v1 没有, 从 html 里重新解析即可)
    let manifestTitle: String
    let manifestLifecycle: String

    private init(id: String, title: String, surface: String, w: Double, h: Double,
                 html: String, css: String, store: [String: String],
                 slot: HNAppletStore.SlotInfo?, instructions: String,
                 instructionsSource: String, ops: [[String: Any]],
                 capabilities: Capabilities, manifestTitle: String = "",
                 manifestLifecycle: String = "") {
        self.id = id; self.title = title; self.surface = surface
        self.w = w; self.h = h; self.html = html; self.css = css
        self.store = store; self.slot = slot
        self.instructions = instructions; self.instructionsSource = instructionsSource
        self.ops = ops; self.capabilities = capabilities
        self.manifestTitle = manifestTitle; self.manifestLifecycle = manifestLifecycle
        self.version = 2
    }
}

extension HNCapsule.Capabilities {
    func toJSON() -> [String: Any] {
        [
            "ids": ids, "triggers": triggers, "polls": polls,
            "lifecycle": lifecycle, "applet": applet ?? "",
            "dragHandles": dragHandles, "inputs": inputs, "media": media,
        ]
    }

    static func fromJSON(_ d: [String: Any]) -> HNCapsule.Capabilities {
        var c = HNCapsule.Capabilities()
        c.ids = (d["ids"] as? [String]) ?? []
        c.triggers = (d["triggers"] as? [String: String]) ?? [:]
        c.polls = (d["polls"] as? [String: Int]) ?? [:]
        c.lifecycle = (d["lifecycle"] as? [String]) ?? []
        c.applet = (d["applet"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        c.dragHandles = (d["dragHandles"] as? [String]) ?? []
        c.inputs = (d["inputs"] as? [String]) ?? []
        c.media = (d["media"] as? [String]) ?? []
        return c
    }

    /// 走一遍 DOM 把"能怎么驱动它"导出来。零副作用: 只读属性和结构。
    public static func derive(from app: HNApp) -> HNCapsule.Capabilities {
        var c = HNCapsule.Capabilities()
        guard let ctx = app.view?.engineContextOrNil, let doc = hn_context_doc(ctx) else {
            // webkit 路径没有 C 引擎文档; 至少把清单声明带出来
            var m = hn_manifest()
            if let d = hn_parse_html(app.html, app.html.utf8.count) {
                hn_doc_manifest(d, &m)
                c.lifecycle = (m.lifecycle.map { String(cString: $0) } ?? "")
                    .split(separator: " ").map(String.init)
                c.applet = m.applet.map { String(cString: $0) }
            }
            return c
        }
        var m = hn_manifest()
        hn_doc_manifest(doc, &m)
        c.lifecycle = (m.lifecycle.map { String(cString: $0) } ?? "")
            .split(separator: " ").map(String.init)
        c.applet = m.applet.map { String(cString: $0) }

        var stack: [OpaquePointer] = hn_doc_root(doc).map { [$0] } ?? []
        var seen = Set<OpaquePointer>()
        while let n = stack.popLast() {
            if seen.contains(n) { continue }
            seen.insert(n)
            if let tagp = hn_node_tag(n) {
                let tag = String(cString: tagp)
                let id = hn_node_attr(n, "id").map { String(cString: $0) } ?? ""
                let trig = hn_node_attr(n, "hx-trigger").map { String(cString: $0).lowercased() } ?? ""
                if !id.isEmpty {
                    c.ids.append(id)
                    if !trig.isEmpty { c.triggers[id] = trig }
                    if tag == "input" || tag == "textarea" || tag == "select" { c.inputs.append(id) }
                    if tag == "img" { c.media.append(id) }
                    if hn_node_ancestor_with_attr(n, "hn-drag") != nil { c.dragHandles.append(id) }
                    // hx-trigger="every 5s" → 轮询
                    if let r = trig.range(of: #"every\s+(\d+)\s*(ms|s)?"#,
                                          options: .regularExpression) {
                        let seg = String(trig[r])
                        let num = seg.components(separatedBy: CharacterSet.decimalDigits.inverted)
                            .filter { !$0.isEmpty }.first.flatMap(Int.init) ?? 0
                        if num > 0 {
                            c.polls[id] = seg.contains("ms") ? num : num * 1000
                        }
                    }
                }
            }
            var ch = hn_node_first_child(n)
            while let k = ch { stack.append(k); ch = hn_node_next_sibling(k) }
        }
        c.ids = Array(c.ids.uniqued())
        return c
    }
}

extension Array where Element: Hashable {
    func uniqued() -> [Element] {
        var s = Set<Element>()
        return filter { s.insert($0).inserted }
    }
}

extension HNApp {
    /// 读一遍文档清单(打胶囊用; 标题/生命周期在胶囊里要单独存一份,
    /// 因为 restore 时可能只想改表面参数而不重写 html)。
    func manifestForCapsule() -> (title: String, lifecycle: String) {
        guard let ctx = view?.engineContextOrNil, let doc = hn_context_doc(ctx) else { return ("", "") }
        var m = hn_manifest()
        hn_doc_manifest(doc, &m)
        return (m.title.map { String(cString: $0) } ?? "",
                m.lifecycle.map { String(cString: $0) } ?? "")
    }
}
