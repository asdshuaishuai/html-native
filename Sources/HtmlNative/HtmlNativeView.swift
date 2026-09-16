import AppKit
import Foundation
import CHtmlNative

/// htmx 风格的声明式交互动作(由元素的 hx-* 属性解析而来)。
public struct HxAction {
    public let method: String        // "GET" / "POST"
    public let urlString: String     // 相对 baseURL 解析
    public let targetId: String      // 片段换入的目标元素 id
    public let swap: hn_swap_mode
    public let sourceId: String?     // 触发元素 id
}

/// html-native 原生视图: 一份 HTML 文档 + 常驻原生渲染。
///
/// 三种更新通道(全部无需 JS, 全部热更新):
///  - render(_:): 整体替换文档内容(agent 推送 UI 的原语)
///  - applyFragment(_:targetId:swap:): htmx 片段交换
///  - setText(_:onElement:): 局部文本更新
public final class HtmlNativeView: NSView, HNWebHost {
    private var ctx: OpaquePointer!
    private var backend = TextShaper.shared.backend()

    /// 相对 hx-get/hx-post URL 的基地址
    public var baseURL: URL?
    /// 本地 KV 存储的应用标识(持久化到 ~/.html-native/store/<id>.json)
    public var storeId: String = "default"
    /// 首次绘制回调(协议要求; native 路径首帧即可取底色)
    public var onFirstPaint: (() -> Void)?
    public var asView: NSView { self }
    var store: HNStore { HNStore(id: storeId) }
    /// 相对图片路径的解析根(默认进程工作目录)
    public var imageRoot: URL? {
        didSet { ImageStore.shared.root = imageRoot ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath) }
    }
    /// 自定义请求通道(默认走 URLSession); 返回 HTML 片段
    public var hxTransport: ((HxAction, @escaping (String?) -> Void) -> Void)?
    /// 非 hx 元素被点击时回调(元素 id, 可能向上冒泡)
    public var onClickUnhandled: ((String?) -> Void)?
    /// 输入值变化回调(元素 id, 新值)
    public var onInput: ((String, String) -> Void)?

    private var imageBox: ImageStore.CtxBox?
    private var imageBackendPtr: UnsafeMutablePointer<hn_image_backend>?
    private var hoverTracking: NSTrackingArea?
    private var currentHoverNode: OpaquePointer?
    private var caretTimer: Timer?
    private var pollTimers: [Timer] = []
    private var animTimer: Timer?
    private var lastFrameTime: CFTimeInterval = 0
    private var caretOn = true
    private var focusedInput: OpaquePointer?

    /// 当前聚焦的输入控件 id
    public var focusedInputId: String? {
        guard let ctx, let n = focusedInput else { return nil }
        _ = ctx
        return hn_node_attr(n, "id").map { String(cString: $0) }
    }

    /// 表单参数(URL 编码): 反映所有控件的当前值
    public func formEncoded() -> String {
        guard let ctx, let doc = hn_context_doc(ctx) else { return "" }
        var cap = 4096
        var buf = [CChar](repeating: 0, count: cap)
        let n = hn_doc_form_encode(doc, &buf, cap)
        if n >= cap - 1 {
            cap = Int(n) + 64
            buf = [CChar](repeating: 0, count: cap)
            _ = hn_doc_form_encode(doc, &buf, cap)
        }
        return String(cString: buf)
    }

    /// 以编程方式设置某控件值 + 聚焦
    @discardableResult
    public func setInputValue(_ text: String, onElement id: String) -> Bool {
        guard let ctx, let doc = hn_context_doc(ctx),
              let node = hn_doc_find_by_id(doc, id),
              hn_node_is_input(node) == 1 else { return false }
        _ = hn_node_set_value(node, text, text.utf8.count)
        focusInput(node)
        relayout()
        return true
    }

    public convenience init(html: String, css: String? = nil) {
        self.init(doc: hn_parse_html(html, html.utf8.count), css: css)
    }

    /// 从已解析的 hn 文档构造(引擎/宿主路径: 清单可在构造前先从 doc 读出)
    public init(doc: OpaquePointer, css: String? = nil) {
        super.init(frame: .zero)
        ctx = hn_context_create()
        _ = hn_doc_autoid_hx(doc)   // hx 元素缺 id 时自动分配(load/poll/click 可靠工作)
        // set_doc 内部按 hn-theme 装载设计令牌基座(引擎侧实现, 三平台同源)
        hn_context_set_doc(ctx, doc)
        // 作者样式排最后 → 同特异性即可覆盖主题令牌
        if let css, !css.isEmpty {
            hn_context_add_sheet(ctx, hn_parse_css(css, css.utf8.count))
        }
        // 图片后端须长期存活: 堆分配结构体 + ctx 盒对象都由视图持有
        let (backend, box) = ImageStore.shared.backend()
        imageBox = box
        let ptr = UnsafeMutablePointer<hn_image_backend>.allocate(capacity: 1)
        ptr.initialize(to: backend)
        imageBackendPtr = ptr
        hn_context_set_images(ctx, ptr)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    deinit {
        caretTimer?.invalidate()
        pollTimers.forEach { $0.invalidate() }
        animTimer?.invalidate()
        if let ctx { hn_context_destroy(ctx) }
        if let p = imageBackendPtr {
            p.deinitialize(count: 1)
            p.deallocate()
        }
    }

    override public var isFlipped: Bool { true }
    override public var acceptsFirstResponder: Bool { true }

    // MARK: - 布局与绘制

    override public func layout() {
        super.layout()
        relayout()
    }

    public func relayout() {
        guard let ctx, bounds.width > 1, bounds.height > 1 else { return }
        var backend = self.backend
        hn_context_layout(ctx, Float(bounds.width), Float(bounds.height), &backend)
        // 首帧初始化动画基准值(不产生动画), 然后按需启动帧循环
        _ = hn_context_anim_tick(ctx, -1)
        kickAnimationLoop()
        needsDisplay = true
    }

    /// 若样式声明了 transition, 启动帧循环驱动插值
    private func kickAnimationLoop() {
        guard let ctx else { return }
        if animTimer != nil { return }
        guard hn_context_anim_tick(ctx, 0) == 1 else { return }
        lastFrameTime = CACurrentMediaTime()
        animTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            guard let self, let ctx = self.ctx else { return }
            let now = CACurrentMediaTime()
            let dt = (now - self.lastFrameTime) * 1000.0
            self.lastFrameTime = now
            let stillActive = hn_context_anim_tick(ctx, Float(dt)) == 1
            self.needsDisplay = true
            if !stillActive {
                self.animTimer?.invalidate()
                self.animTimer = nil
            }
        }
    }

    override public func draw(_ dirtyRect: NSRect) {
        guard let ctx, let dlp = hn_context_display_list(ctx),
              let cg = NSGraphicsContext.current?.cgContext else { return }
        HNPainter.draw(dlp.pointee, into: cg)
    }

    // MARK: - 热更新通道

    /// 整体替换文档内容, 样式表保留 —— agent 现场生成 UI 直接推入
    /// 协议实现: css 已在构造期注入, 热更新只需新 html
    public func render(_ html: String, css: String?) { render(html) }

    /// 取 body 背景色(协议实现)
    public func bodyBackgroundHex() -> UInt32? {
        guard let ctx, let doc = hn_context_doc(ctx), let body = hn_doc_body(doc) else { return nil }
        var col: UInt32 = 0
        hn_node_debug_background(body, &col)
        return (col & 0xFF) < 2 ? nil : col
    }

    public func render(_ html: String) {
        guard let ctx else { return }
        hn_context_render(ctx, html, html.utf8.count)
        if let doc = hn_context_doc(ctx) { _ = hn_doc_autoid_hx(doc) }
        relayout()
    }

    /// 局部文本更新(如时钟/计数器)
    public func setText(_ text: String, onElement id: String) {
        guard let ctx, let doc = hn_context_doc(ctx) else { return }
        hn_doc_set_text(doc, id, text)
        relayout()
    }

    /// htmx 片段交换后重新布局
    public func applyFragment(_ html: String, targetId: String, swap: hn_swap_mode) {
        guard let ctx, let doc = hn_context_doc(ctx) else { return }
        hn_doc_swap(doc, targetId, swap, html, html.utf8.count)
        relayout()
        if ProcessInfo.processInfo.environment["HN_LOG_SWAP"] != nil {
            let dom = dumpDOM()
            let header = "\n=== swap → \(targetId) (\(html.utf8.count) bytes) ===\n"
            let body = dom.joined(separator: "\n") + "\n"
            if let fh = FileHandle(forWritingAtPath: "/tmp/hn-swap.log") {
                fh.seekToEndOfFile()
                fh.write((header + body).data(using: .utf8)!)
                fh.closeFile()
            } else {
                try? (header + body).write(toFile: "/tmp/hn-swap.log", atomically: true, encoding: .utf8)
            }
        }
    }

    // MARK: - 交互

    override public func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = hoverTracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
                               options: [.mouseEnteredAndExited, .mouseMoved, .activeInActiveApp],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        hoverTracking = t
    }

    /// hover → :hover 伪类重新匹配 + cursor 更新
    private func updateHover(at p: NSPoint) {
        guard let ctx else { return }
        let node = hn_context_hit_node(ctx, Float(p.x), Float(p.y))
        if node != currentHoverNode {
            currentHoverNode = node
            hn_context_set_hover(ctx, node)
            relayout() // 重新级联(:hover 生效)
        }
        if hn_node_cursor(node) == 1 {
            NSCursor.pointingHand.set()
        } else {
            NSCursor.arrow.set()
        }
    }

    override public func mouseMoved(with event: NSEvent) {
        updateHover(at: convert(event.locationInWindow, from: nil))
    }

    override public func mouseEntered(with event: NSEvent) {
        updateHover(at: convert(event.locationInWindow, from: nil))
    }

    override public func mouseExited(with event: NSEvent) {
        if let ctx, currentHoverNode != nil {
            currentHoverNode = nil
            hn_context_set_hover(ctx, nil)
            relayout()
        }
        NSCursor.arrow.set()
    }

    override public func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        // hn-drag: 该元素(或祖先)声明为拖拽手柄 → 交给系统移动窗口
        if let ctx, let node = hn_context_hit_node(ctx, Float(p.x), Float(p.y)) {
            if hn_node_ancestor_with_attr(node, "hn-drag") != nil {
                window?.performDrag(with: event)
                return
            }
        }
        // 输入控件: 点击即聚焦
        if let ctx, let node = hn_context_hit_node(ctx, Float(p.x), Float(p.y)) {
            if hn_node_is_input(node) == 1 {
                hn_context_set_active(ctx, node)
                focusInput(node)
                relayout()
                return
            }
            hn_context_set_active(ctx, node)
            relayout()
        }
        if let action = hxAction(at: p) {
            performHx(action)
            return
        }
        onClickUnhandled?(hitTestId(at: p))
    }

    override public func mouseUp(with event: NSEvent) {
        if let ctx {
            hn_context_set_active(ctx, nil)
            relayout()
        }
    }

    // MARK: - 输入(键盘编辑)

    private func focusInput(_ node: OpaquePointer?) {
        guard let ctx else { return }
        focusedInput = node
        hn_context_set_focus(ctx, node)
        if node != nil { startCaretBlink() } else { stopCaretBlink() }
    }

    private func startCaretBlink() {
        caretTimer?.invalidate()
        caretOn = true
        caretTimer = Timer.scheduledTimer(withTimeInterval: 0.53, repeats: true) { [weak self] _ in
            guard let self, let ctx = self.ctx else { return }
            self.caretOn.toggle()
            hn_context_set_caret_visible(ctx, self.caretOn ? 1 : 0)
            hn_context_repaint(ctx)
            self.needsDisplay = true
        }
    }

    private func stopCaretBlink() {
        caretTimer?.invalidate()
        caretTimer = nil
        if let ctx { hn_context_set_caret_visible(ctx, 0) }
    }

    override public func keyDown(with event: NSEvent) {
        guard let ctx, let node = focusedInput, hn_node_is_input(node) == 1 else {
            // Tab 在控件间移动焦点
            if event.keyCode == 48 { advanceFocus(backward: event.modifierFlags.contains(.shift)); return }
            super.keyDown(with: event)
            return
        }
        if event.keyCode == 48 { advanceFocus(backward: event.modifierFlags.contains(.shift)); return }

        var value = ""
        var vlen = 0
        if let v = hn_node_value(node, &vlen), vlen > 0 {
            value = String(decoding: UnsafeBufferPointer(
                start: UnsafeRawPointer(v).assumingMemoryBound(to: UInt8.self), count: vlen), as: UTF8.self)
        }
        var caret = Int(hn_node_caret(node))
        let bytes = Array(value.utf8)
        var changed = false

        if event.keyCode == 51 { // Backspace
            if caret > 0 {
                let cut = utf8PrevIndex(bytes, caret)
                value = String(decoding: bytes[0..<cut] + bytes[caret...], as: UTF8.self)
                caret = cut
                changed = true
            }
        } else if event.keyCode == 117 { // Delete
            if caret < bytes.count {
                let cut = utf8NextIndex(bytes, caret)
                value = String(decoding: bytes[0..<cut] + bytes[caret...], as: UTF8.self)
                changed = true
            }
        } else if event.keyCode == 36 || event.keyCode == 76 { // Return: textarea 换行 / input 提交
            let tag = hn_node_tag(node).map { String(cString: $0) } ?? ""
            if tag == "textarea" {
                value = String(decoding: bytes[0..<caret] + Array("\n".utf8) + bytes[caret...], as: UTF8.self)
                caret += 1
                changed = true
            } else {
                // 表单体验: input 上回车 → 触发最近携带 hx-post/hx-get 的祖先(如"保存"按钮所在容器)
                submitFromEnter(node)
                return
            }
        } else if let chars = event.characters, !chars.isEmpty {
            let printable = chars.unicodeScalars.filter { $0.value >= 32 && $0.value != 127 }
            if !printable.isEmpty {
                let ins = String(String.UnicodeScalarView(printable))
                value = String(decoding: bytes[0..<caret] + Array(ins.utf8) + bytes[caret...], as: UTF8.self)
                caret += ins.utf8.count
                changed = true
            }
        }
        _ = event.modifierFlags

        if changed {
            _ = hn_node_set_value(node, value, value.utf8.count)
            hn_node_set_caret(node, Int32(caret))
            caretOn = true
            hn_context_set_caret_visible(ctx, 1)
            relayout()
            if let idp = hn_node_attr(node, "id") {
                onInput?(String(cString: idp), value)
            }
        }
    }

    public override func becomeFirstResponder() -> Bool { true }
    public override func resignFirstResponder() -> Bool {
        focusInput(nil)
        relayout()
        return true
    }

    private func utf8PrevIndex(_ b: [UInt8], _ i: Int) -> Int {
        var k = i - 1
        while k > 0 && (b[k] & 0xC0) == 0x80 { k -= 1 }
        return k
    }
    private func utf8NextIndex(_ b: [UInt8], _ i: Int) -> Int {
        var k = i + 1
        while k < b.count && (b[k] & 0xC0) == 0x80 { k += 1 }
        return k
    }

    /// Tab 在输入控件间轮换焦点
    public func advanceFocus(backward: Bool) {
        guard let ctx, let doc = hn_context_doc(ctx) else { return }
        var inputs: [OpaquePointer] = []
        var i: Int32 = 0
        while let n = hn_doc_input_at(doc, i) {
            inputs.append(n)
            i += 1
        }
        guard !inputs.isEmpty else { return }
        var idx = 0
        if let cur = focusedInput, let f = inputs.firstIndex(where: { $0 == cur }) {
            idx = (f + (backward ? -1 : 1) + inputs.count) % inputs.count
        } else if backward {
            idx = inputs.count - 1
        }
        focusInput(inputs[idx])
        relayout()
    }

    /// 滚轮: 找点位下最深的 overflow 容器滚动(纯绘制偏移, 不重排)
    override public func scrollWheel(with event: NSEvent) {
        guard let ctx else { return }
        let p = convert(event.locationInWindow, from: nil)
        guard let scroller = hn_context_scrollable_at(ctx, Float(p.x), Float(p.y)) else { return }
        let dy = event.scrollingDeltaY
        if dy != 0, hn_node_scroll_by(scroller, 0, Float(-dy)) == 1 {
            hn_context_repaint(ctx)
            needsDisplay = true
        }
    }

    public func hitTestId(at p: NSPoint) -> String? {
        guard let ctx else { return nil }
        guard let idp = hn_context_hit_test(ctx, Float(p.x), Float(p.y)) else { return nil }
        return String(cString: idp)
    }

    /// 解析点按位置对应的 htmx 动作(向上冒泡找 hx-get/hx-post)
    public func hxAction(at p: NSPoint) -> HxAction? {
        guard let ctx else { return nil }
        guard let node = hn_context_hit_node(ctx, Float(p.x), Float(p.y)) else { return nil }
        guard let src = hn_node_ancestor_with_attr(node, "hx-get")
                ?? hn_node_ancestor_with_attr(node, "hx-post") else { return nil }
        return hxAction(for: src)
    }

    /// 从携带 hx-* 的元素节点构建动作
    func hxAction(for src: OpaquePointer) -> HxAction? {
        let getURL = hn_node_attr(src, "hx-get").map(String.init)
        let postURL = hn_node_attr(src, "hx-post").map(String.init)
        guard let urlString = postURL ?? getURL, !urlString.isEmpty else { return nil }

        var targetId: String
        if let t = hn_node_attr(src, "hx-target").map(String.init), !t.isEmpty, t != "this" {
            targetId = t
        } else if let id = hn_node_attr(src, "id").map(String.init) {
            targetId = id
        } else {
            return nil
        }

        let swapStr = hn_node_attr(src, "hx-swap").map(String.init) ?? "innerHTML"
        let swap: hn_swap_mode =
            swapStr == "outerHTML" ? HN_SWAP_OUTER :
            (swapStr == "append" || swapStr == "beforeend") ? HN_SWAP_APPEND :
            (swapStr == "prepend" || swapStr == "afterbegin") ? HN_SWAP_PREPEND :
            HN_SWAP_INNER

        return HxAction(method: postURL != nil ? "POST" : "GET",
                        urlString: urlString,
                        targetId: targetId,
                        swap: swap,
                        sourceId: hn_node_attr(src, "id").map(String.init))
    }

    /// 回车提交(表单语义): 自身 → 祖先 → 最近容器子树内第一个 hx-post/get 载体
    public func enterSubmitAction(from node: OpaquePointer) -> HxAction? {
        guard let src = hn_node_form_carrier(node) else { return nil }
        return hxAction(for: src)
    }

    func submitFromEnter(_ node: OpaquePointer) {
        guard let action = enterSubmitAction(from: node) else { return }
        performHx(action)
    }

    /// 执行一次 htmx 动作: 拉取片段并换入目标
    public func performHx(_ action: HxAction) {
        if let hxTransport {
            hxTransport(action) { [weak self] html in
                guard let self, let html else { return }
                self.applyFragment(html, targetId: action.targetId, swap: action.swap)
            }
            return
        }
        // 系统集成: sys:// 由本地桥直接应答(零网络)
        if SystemBridge.handles(action.urlString) {
            let target = action.targetId, swap = action.swap
            if let frag = handleAppSys(action, form: formEncoded()) {   // 应用级路由
                applyFragment(frag, targetId: target, swap: swap)
                return
            }
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                let frag = SystemBridge.fragment(for: action.urlString)
                DispatchQueue.main.async {
                    self?.applyFragment(frag, targetId: target, swap: swap)
                }
            }
            return
        }
        let url = URL(string: action.urlString, relativeTo: baseURL) ?? baseURL
        guard let url else { return }
        var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 10)
        if action.method == "POST" {
            req.httpMethod = "POST"
            req.setValue("application/x-www-form-urlencoded; charset=utf-8",
                         forHTTPHeaderField: "Content-Type")
            req.httpBody = formEncoded().data(using: .utf8)
        } else {
            let q = formEncoded()
            if !q.isEmpty, var comps = URLComponents(url: url, resolvingAgainstBaseURL: false) {
                comps.query = (comps.query.map { $0 + "&" } ?? "") + q
                if let u = comps.url { req = URLRequest(url: u, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 10) }
            }
        }
        URLSession.shared.dataTask(with: req) { [weak self] data, _, _ in
            guard let self, let data, let html = String(data: data, encoding: .utf8) else { return }
            DispatchQueue.main.async {
                self.applyFragment(html, targetId: action.targetId, swap: action.swap)
            }
        }.resume()
    }

    /// 供截图/宿主工具: 引擎上下文句柄(hover/scroll/repaint 等只读或状态注入用途)
    public var engineContext: OpaquePointer { ctx }

    /// 内省: DOM 树 + 布局盒 + run 数(排查节点位置错乱)
    public func dumpDOM() -> [String] {
        guard let ctx, let doc = hn_context_doc(ctx) else { return [] }
        var out: [String] = []
        func walk(_ n: OpaquePointer, _ depth: Int) {
            if depth > 14 { return }
            var line = String(repeating: "  ", count: depth)
            if let tag = hn_node_tag(n) {
                line += String(cString: tag)
                if let idp = hn_node_attr(n, "id") { line += "#" + String(cString: idp) }
            } else {
                var vlen = 0
                if let v = hn_node_value(n, &vlen), vlen > 0 {
                    let buf = UnsafeBufferPointer(start: UnsafeRawPointer(v).assumingMemoryBound(to: UInt8.self), count: Int(vlen))
                    let full = String(decoding: buf, as: UTF8.self)
                    let preview = String(full.prefix(24))
                    line += "TEXT(\"" + preview + "\")"
                } else { line += "TEXT" }
            }
            var x: Float = 0, y: Float = 0, w: Float = 0, h: Float = 0
            hn_node_box(n, &x, &y, &w, &h)
            line += String(format: " [%.0f,%.0f %.0fx%.0f]", x, y, w, h)
            let rc = hn_node_run_count(n)
            if rc > 0 {
                var rx: Float = 0, rb: Float = 0, rw: Float = 0, ryt: Float = 0, rh: Float = 0
                _ = hn_node_run_at(n, 0, &rx, &rb, &rw, &ryt, &rh)
                line += " runs=\(rc) r0(x=\(Int(rx)) bl=\(Int(rb)))"
            }
            out.append(line)
            var ch = hn_node_first_child(n)
            var guardCount = 0
            while let c = ch, guardCount < 4000 { walk(c, depth + 1); ch = hn_node_next_sibling(c); guardCount += 1 }
        }
        if let root = hn_doc_root(doc) { walk(root, 0) }
        return out
    }

    /// 内省: 当前绘制指令列表(排查 live 窗口实际画了什么)
    public func dumpDisplayList() -> [[String: Any]] {
        guard let ctx, let dlp = hn_context_display_list(ctx) else { return [] }
        let dl = dlp.pointee
        var out: [[String: Any]] = []
        for i in 0..<Int(dl.count) {
            guard let cmds = dl.cmds else { break }
            let c = cmds[i]
            var d: [String: Any] = [
                "k": c.kind == HN_CMD_TEXT ? "t" : (c.kind == HN_CMD_RECT ? "r" : "x"),
            ]
            if c.kind == HN_CMD_TEXT {
                d["x"] = Int(c.tx); d["y"] = Int(c.baseline)
            } else {
                d["x"] = Int(c.x); d["y"] = Int(c.y)
                d["w"] = Int(c.w); d["h"] = Int(c.h)
            }
            if c.kind == HN_CMD_TEXT, let tp = c.text, c.text_len > 0 {
                d["s"] = String(decoding: UnsafeBufferPointer(
                    start: UnsafeRawPointer(tp).assumingMemoryBound(to: UInt8.self),
                    count: Int(c.text_len)), as: UTF8.self)
            }
            out.append(d)
        }
        return out
    }

    /// 解析 hx-trigger="every Ns" 并启动轮询(重复调用会先清旧定时器)
    public func startPolling() {
        stopPolling()
        guard let ctx, let doc = hn_context_doc(ctx) else { return }
        var i: Int32 = 0
        while true {
            var id: UnsafePointer<CChar>?
            var ms: Int32 = 0
            guard hn_doc_poll_at(doc, i, &id, &ms) == 1, let idp = id else { break }
            let target = String(cString: idp)
            let interval = TimeInterval(ms) / 1000.0
            let t = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in
                guard let self, let ctx = self.ctx else { return }
                // 重新解析当前 DOM 上的属性(内容可能已被热更新)
                guard let doc2 = hn_context_doc(ctx) else { return }
                var id2: UnsafePointer<CChar>?
                var ms2: Int32 = 0
                guard hn_doc_poll_at(doc2, i, &id2, &ms2) == 1, let p2 = id2 else { return }
                let tid = String(cString: p2)
                guard let node = hn_doc_find_by_id(doc2, tid) else { return }
                let get = hn_node_attr(node, "hx-get").map { String(cString: $0) }
                let post = hn_node_attr(node, "hx-post").map { String(cString: $0) }
                guard let url = post ?? get else { return }
                let swapStr = hn_node_attr(node, "hx-swap").map { String(cString: $0) } ?? "innerHTML"
                let swap: hn_swap_mode =
                    swapStr == "outerHTML" ? HN_SWAP_OUTER :
                    (swapStr == "append" || swapStr == "beforeend") ? HN_SWAP_APPEND :
                    (swapStr == "prepend" || swapStr == "afterbegin") ? HN_SWAP_PREPEND : HN_SWAP_INNER
                var tgt = hn_node_attr(node, "hx-target").map { String(cString: $0) } ?? "this"
                if tgt == "this" { tgt = tid }
                self.performHx(HxAction(method: post != nil ? "POST" : "GET", urlString: url,
                                        targetId: tgt, swap: swap, sourceId: tid))
            }
            pollTimers.append(t)
            i += 1
        }
    }

    public func stopPolling() {
        pollTimers.forEach { $0.invalidate() }
        pollTimers.removeAll()
    }

    /// 应用级 sys:// 路由: 依赖视图状态(store), 与纯读的 SystemBridge 互补。
    /// 返回 nil 表示交给 SystemBridge。
    func handleAppSys(_ action: HxAction, form encodedForm: String) -> String? {
        // 共享路由层: webkit 兜底渲染器走同一实现, 保证两条路径语义一致
        let form = action.method == "POST" ? encodedForm : ""
        return HNAppRoutes.handle(url: action.urlString, form: form, storeId: storeId)
    }

    static func queryParams(_ url: String) -> [String: String] {
        guard let q = url.firstIndex(of: "?") else { return [:] }
        var out: [String: String] = [:]
        for pair in url[q...].dropFirst().split(separator: "&") {
            let kv = pair.split(separator: "=", maxSplits: 1)
            guard kv.count == 2 else { continue }
            out[String(kv[0])] = String(kv[1]).removingPercentEncoding ?? String(kv[1])
        }
        return out
    }

    static func formBody(_ action: HxAction, encoded: String) -> [String: String] {
        let q = action.method == "POST" ? encoded : ""
        var out: [String: String] = [:]
        for pair in q.split(separator: "&") {
            let kv = pair.split(separator: "=", maxSplits: 1)
            guard kv.count == 2 else { continue }
            let k = String(kv[0]).removingPercentEncoding ?? String(kv[0])
            let v = String(kv[1]).removingPercentEncoding ?? String(kv[1])
            out[k] = v
        }
        return out
    }

    func postNotification(title: String, body: String) {
        let note = NSUserNotification()
        note.title = title
        note.informativeText = body
        note.soundName = NSUserNotificationDefaultSoundName
        NSUserNotificationCenter.default.deliver(note)
    }

    func escapeHtml(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }

    /// 页面启动时要自动发起的 hx-trigger="load" 动作(文档内全部 load 元素)
    public func loadActions() -> [HxAction] {
        guard let ctx, let doc = hn_context_doc(ctx) else { return [] }
        var acts: [HxAction] = []
        var idx: Int32 = 0
        while true {
            var idp: UnsafePointer<CChar>?
            guard hn_doc_load_at(doc, idx, &idp) == 1 else { break }
            guard let p = idp else { idx += 1; continue }   // 无 id 元素跳过, 不中断枚举
            let id = String(cString: p)
            if let node = hn_doc_find_by_id(doc, id),
               let get = hn_node_attr(node, "hx-get").map(String.init) {
                let swapStr = hn_node_attr(node, "hx-swap").map(String.init) ?? "innerHTML"
                let swap: hn_swap_mode = swapStr == "outerHTML" ? HN_SWAP_OUTER : HN_SWAP_INNER
                acts.append(HxAction(method: "GET", urlString: get, targetId: id, swap: swap, sourceId: id))
            }
            idx += 1
        }
        return acts
    }
}
