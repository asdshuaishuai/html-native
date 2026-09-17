import AppKit
import CoreVideo
import Foundation
import QuartzCore
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
        didSet {
            let root = imageRoot ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            ImageStore.shared.root = root
            // 资产(Lottie JSON)与图片用同一个根: 相对路径的解析口径必须一致,
            // 否则 <img src="a.png"> 与 hn-lottie="a.json" 会去不同的地方找文件。
            AssetStore.shared.root = root
        }
    }
    /// 自定义请求通道(默认走 URLSession); 返回 HTML 片段
    public var hxTransport: ((HxAction, @escaping (String?) -> Void) -> Void)?

    /// 允许访问的网络主机(安全边界)。
    /// 空数组 = 允许全部(开发默认); 非空时只放行列出的主机。
    /// 建议生产应用显式声明, 避免应用代码随意外发数据。
    public static var allowedHosts: [String] = []

    /// 请求超时(秒)
    public static var requestTimeout: TimeInterval = 15

    /// 主机是否被允许(含子域匹配)
    public static func hostAllowed(_ url: URL) -> Bool {
        if allowedHosts.isEmpty { return true }
        guard let host = url.host?.lowercased() else { return false }
        for a in allowedHosts {
            let allow = a.lowercased()
            if host == allow || host.hasSuffix("." + allow) { return true }
        }
        return false
    }
    /// 非 hx 元素被点击时回调(元素 id, 可能向上冒泡)
    public var onClickUnhandled: ((String?) -> Void)?
    /// 输入值变化回调(元素 id, 新值)
    public var onInput: ((String, String) -> Void)?

    private var imageBox: ImageStore.CtxBox?
    private var imageBackendPtr: UnsafeMutablePointer<hn_image_backend>?
    private var assetBox: AssetStore.CtxBox?
    private var assetBackendPtr: UnsafeMutablePointer<hn_asset_backend>?
    private var jsRuntime: HNJSRuntime?
    /// 测试用: 暴露 JS 运行时以驱动断言
    public var jsRuntimeForTest: HNJSRuntime? { jsRuntime }

    /// JS 侧调用 preventDefault 时置位(中止继续派发)
    fileprivate var jsPrevented = false
    /// 最近一次完整 HTML(供 arena 压缩重建)
    private var fullHTML: String = ""
    /// swap 计数(达到阈值时触发 arena 压缩)
    private var swapCount: Int = 0
    /// 最近一次脚本错误(便于排查)
    public var jsError: String?
    private var hoverTracking: NSTrackingArea?
    private var currentHoverNode: OpaquePointer?
    private var caretTimer: Timer?
    private var pollTimers: [Timer] = []
    /// 帧驱动: 优先 CADisplayLink(macOS 14+, 与刷新率对齐), 回退 CVDisplayLink。
    /// 绝不用 Timer —— 它与 vsync 不同相, 60Hz 定时器配 120Hz 屏只能隔帧更新, 产生抖动。
    private var frameDriverActive = false
    private var cvLink: CVDisplayLink?
    private var fallbackTimer: Timer?
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
        fullHTML = html
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
        fullHTML = ""   // 由 render()/convenience init 设置
        // 图片后端须长期存活: 堆分配结构体 + ctx 盒对象都由视图持有
        let (backend, box) = ImageStore.shared.backend()
        imageBox = box
        let ptr = UnsafeMutablePointer<hn_image_backend>.allocate(capacity: 1)
        ptr.initialize(to: backend)
        imageBackendPtr = ptr
        hn_context_set_images(ctx, ptr)
        // 资产后端: Lottie JSON 等外部数据文件。引擎不做磁盘 I/O,
        // 字节由运行时提供(同一份 Lottie 在三个平台由同一段 C 代码求值)。
        let (ab, abox) = AssetStore.shared.backend()
        assetBox = abox
        let aptr = UnsafeMutablePointer<hn_asset_backend>.allocate(capacity: 1)
        aptr.initialize(to: ab)
        assetBackendPtr = aptr
        hn_context_set_assets(ctx, aptr)
        runPageScripts()
    }

    // MARK: - 原生 JavaScript(JavaScriptCore)

    /// 从当前文档收集并执行 <script>。JS 只作为交互业务的补充:
    /// 引擎仍是纯 C, JS 的 DOM 变更也走同一条布局/绘制管线。
    ///
    /// 从 DOM 取(而非原始 html 字符串): 这样构造路径(html/doc)与热更新统一,
    /// 且 <include>/片段换入带来的脚本同样会被执行。
    func runPageScripts() {
        guard let ctx, let doc = hn_context_doc(ctx) else { return }
        let code = Self.collectScripts(doc)
        let disabled = Self.attrWalk(doc, "hn-js") == "off"
        guard !code.isEmpty, !disabled else { jsRuntime = nil; return }
        let rt = HNJSRuntime(bridge: makeJSBridge())
        jsRuntime = rt
        rt.run(code)
        jsError = rt.lastError
        if let err = rt.lastError {
            FileHandle.standardError.write("[hn-js] 脚本错误: \(err)\n".data(using: .utf8)!)
        }
        relayout()
    }

    /// 组织 JS → 引擎的桥: 每个 DOM 操作都落到既有的 C API 上
    func makeJSBridge() -> HNJSRuntime.Bridge {
        weak var weakSelf = self
        return HNJSRuntime.Bridge(
            findById: { id in
                guard let self = weakSelf, let ctx = self.ctx,
                      let doc = hn_context_doc(ctx), let n = hn_doc_find_by_id(doc, id) else { return 0 }
                return UInt(bitPattern: UnsafeRawPointer(n))
            },
            findAll: { sel in
                guard let self = weakSelf, let ctx = self.ctx, let doc = hn_context_doc(ctx) else { return [] }
                var out: [UInt] = []
                let s = sel.trimmingCharacters(in: .whitespaces)
                if s.hasPrefix("#") {
                    if let n = hn_doc_find_by_id(doc, String(s.dropFirst())) {
                        out.append(UInt(bitPattern: UnsafeRawPointer(n)))
                    }
                } else if s.hasPrefix(".") {
                    let cls = String(s.dropFirst())
                    var i: Int32 = 0
                    while let n = hn_doc_find_attr(doc, "class", i) {
                        i += 1
                        if let cv = hn_node_attr(n, "class"), String(cString: cv).contains(cls) {
                            out.append(UInt(bitPattern: UnsafeRawPointer(n)))
                        }
                    }
                } else {
                    var i: Int32 = 0
                    while let n = hn_doc_find_attr(doc, "id", i) {
                        i += 1
                        if let tv = hn_node_tag(n), String(cString: tv) == s {
                            out.append(UInt(bitPattern: UnsafeRawPointer(n)))
                        }
                    }
                }
                return out
            },
            getAttr: { h, name in
                guard let n = Self.node(from: h) else { return nil }
                return hn_node_attr(n, name).map { String(cString: $0) }
            },
            setAttr: { h, name, val in
                guard let n = Self.node(from: h) else { return false }
                let ok = hn_node_set_attr(n, name, val) == 1
                /* DOM 变更后必须重排 —— 否则 JS 的属性/类名改动不会出现在屏幕上
                   (表现为"JS 执行成功但界面没反应", 排查成本很高) */
                if ok { weakSelf?.relayout() }
                return ok
            },
            getText: { h in
                guard let n = Self.node(from: h) else { return "" }
                var buf = [CChar](repeating: 0, count: 4096)
                _ = hn_node_text_content(n, &buf, 4096)
                return String(cString: buf)
            },
            setText: { h, txt in
                guard let n = Self.node(from: h) else { return false }
                let ok = hn_node_set_text_content(n, txt, txt.utf8.count) == 1
                if ok { weakSelf?.relayout() }   /* 同上: 文本变更需重排才可见 */
                return ok
            },
            insertHTML: { h, html, mode in
                guard let self = weakSelf else { return 0 }
                if mode == "create" {
                    // 游离容器: 挂在一个不可见的根容器下, 插到目标时再搬
                    guard let ctx = self.ctx, let doc = hn_context_doc(ctx),
                          let holder = hn_doc_find_by_id(doc, "__hn_detached") else { return 0 }
                    guard let el = hn_node_append_element(holder, html) else { return 0 }
                    return UInt(bitPattern: UnsafeRawPointer(el))
                }
                guard let self = weakSelf, let ctx = self.ctx, let doc = hn_context_doc(ctx),
                      let target = hn_doc_find_by_id(doc, Self.idOf(h)) ?? Self.node(from: h).map({ $0 }) else { return 0 }
                let m: hn_swap_mode = mode == "append" ? HN_SWAP_APPEND
                    : mode == "prepend" ? HN_SWAP_PREPEND : HN_SWAP_INNER
                let ok = hn_node_insert_html(target, html, html.utf8.count, m) == 1
                if ok { self.relayout() }
                return ok ? UInt(bitPattern: UnsafeRawPointer(target)) : 0
            },
            removeChild: { parent, child in
                guard let p = Self.node(from: parent), let c = Self.node(from: child) else { return false }
                let ok = hn_node_remove_child(p, c) == 1
                if ok { weakSelf?.relayout() }
                return ok
            },
            tagOf: { h in
                guard let n = Self.node(from: h), let t = hn_node_tag(n) else { return "" }
                return String(cString: t)
            },
            getStyle: { _, _ in nil },
            setStyle: { h, prop, val in
                guard let n = Self.node(from: h) else { return false }
                let existing = hn_node_attr(n, "style").map { String(cString: $0) } ?? ""
                var decls = existing
                if let r = decls.range(of: "\(prop):") {
                    let rest = decls[r.upperBound...]
                    if let semi = rest.firstIndex(of: ";") {
                        decls.replaceSubrange(r.lowerBound...semi, with: "\(prop): \(val);")
                    } else {
                        decls.replaceSubrange(r.lowerBound..., with: "\(prop): \(val)")
                    }
                } else {
                    if !decls.isEmpty && !decls.hasSuffix(";") { decls += ";" }
                    decls += " \(prop): \(val)"
                }
                let ok = hn_node_set_attr(n, "style", decls) == 1
                if ok { weakSelf?.relayout() }
                return ok
            },
            fetch: { url, method, body, target, swap in
                guard let self = weakSelf else { return }
                let m: hn_swap_mode = swap == "append" ? HN_SWAP_APPEND
                    : swap == "prepend" ? HN_SWAP_PREPEND
                    : swap == "outer" ? HN_SWAP_OUTER : HN_SWAP_INNER
                self.performHx(HxAction(method: method, urlString: url,
                                        targetId: target, swap: m, sourceId: nil))
            },
            fetchRequest: { method, urlStr, headers, body, done, fail in
                guard let url = URL(string: urlStr) else {
                    fail(0, [:], "invalid URL: \(urlStr)"); return
                }
                guard let scheme = url.scheme?.lowercased(),
                      scheme == "http" || scheme == "https" else {
                    fail(0, [:], "unsupported scheme: \(url.scheme ?? "none")"); return
                }
                guard Self.hostAllowed(url) else {
                    fail(0, [:], "host not allowed: \(url.host ?? "?")")
                    return
                }
                var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData,
                                     timeoutInterval: Self.requestTimeout)
                req.httpMethod = method
                for (k, v) in headers { req.setValue(v, forHTTPHeaderField: k) }
                if let body, !body.isEmpty, method != "GET" {
                    req.httpBody = body.data(using: .utf8)
                    if headers["Content-Type"] == nil {
                        req.setValue("text/plain; charset=utf-8", forHTTPHeaderField: "Content-Type")
                    }
                }
                URLSession.shared.dataTask(with: req) { data, resp, err in
                    if let err {
                        fail(0, [:], err.localizedDescription); return
                    }
                    let http = resp as? HTTPURLResponse
                    let status = http?.statusCode ?? 0
                    var rh: [String: String] = [:]
                    for (k, v) in http?.allHeaderFields ?? [:] {
                        if let ks = k as? String, let vs = v as? String { rh[ks] = vs }
                    }
                    let text = data.flatMap { String(data: $0, encoding: .utf8) } ?? ""
                    // 2xx/3xx 视为成功; 4xx/5xx 走 reject(与 fetch 语义一致)
                    if status >= 200 && status < 400 {
                        done(status, rh, text)
                    } else {
                        /* HTTP 错误也带状态码与响应头: JS 侧 e.status 才能判 404/500 */
                        fail(status, rh, text.isEmpty ? "HTTP \(status)" : text)
                    }
                }.resume()
            },
            localStorageGet: { k in weakSelf?.store.get(k) },
            localStorageSet: { k, v in weakSelf?.store.set(k, v) },
            log: { msg in FileHandle.standardError.write("[hn-js] \(msg)\n".data(using: .utf8)!) },
            preventDefault: { weakSelf?.jsPrevented = true },
            reload: { weakSelf?.relayout() },
            setMeshVerts: { h, verts, cols, rows in
                guard let self = weakSelf, let n = Self.node(from: h) else { return false }
                let ok = verts.withUnsafeBufferPointer {
                    hn_node_set_mesh_verts(n, $0.baseAddress, Int32(cols), Int32(rows))
                }
                if ok == 1 { self.relayout() }
                return ok == 1
            }
        )
    }

    /// handle → 引擎节点指针(句柄就是指针位模式)
    public static func node(from handle: UInt) -> OpaquePointer? {
        guard handle != 0 else { return nil }
        return OpaquePointer(bitPattern: handle)
    }

    static func idOf(_ handle: UInt) -> String {
        guard let n = node(from: handle), let idp = hn_node_attr(n, "id") else { return "" }
        return String(cString: idp)
    }

    /// 调试: 在当前 JS 上下文里求值(仅测试用)
    public func jsProbe(_ expr: String) -> String? {
        jsRuntime?.probe(expr)
    }

    /// 遍历 DOM 收集 <script> 节点的文本内容
    public static func collectScriptsPublic(_ doc: OpaquePointer) -> String { collectScripts(doc) }

    static func collectScripts(_ doc: OpaquePointer) -> String {
        var codes: [String] = []
        var stack: [OpaquePointer] = []
        if let root = hn_doc_root(doc) { stack.append(root) }
        var guardCount = 0
        while let n = stack.popLast(), guardCount < 20000 {
            guardCount += 1
            if let tag = hn_node_tag(n), String(cString: tag) == "script" {
                var buf = [CChar](repeating: 0, count: 65536)
                let len = hn_node_text_content(n, &buf, 65536)
                if len > 0 { codes.append(String(cString: buf)) }
            }
            // 逆序入栈: 出栈即文档顺序(脚本按页面出现次序执行)
            var kids: [OpaquePointer] = []
            var ch = hn_node_first_child(n)
            while let c = ch { kids.append(c); ch = hn_node_next_sibling(c) }
            for c in kids.reversed() { stack.append(c) }
        }
        return codes.joined(separator: "\n;\n")
    }

    /// 从 DOM 读任意元素的属性值(取第一个命中的)
    static func attrWalk(_ doc: OpaquePointer, _ name: String) -> String? {
        var idx: Int32 = 0
        while let n = hn_doc_find_attr(doc, name, idx) {
            idx += 1
            if let v = hn_node_attr(n, name) { return String(cString: v) }
        }
        return nil
    }

    /// 抽取页面里所有 <script> 的内容(拼接执行; 保留供测试与工具使用)
    public static func extractScripts(_ html: String) -> String {
        var out: [String] = []
        var rest = Substring(html)
        while let open = rest.range(of: "<script", options: .caseInsensitive) {
            guard let gt = rest[open.upperBound...].firstIndex(of: ">") else { break }
            let afterOpen = rest.index(after: gt)
            guard let close = rest.range(of: "</script", options: .caseInsensitive, range: afterOpen..<rest.endIndex) else { break }
            // 跳过带 src 的外链(不支持的用法: 显式忽略而非静默错乱)
            let attrs = rest[open.upperBound..<gt]
            if !attrs.lowercased().contains("src=") {
                out.append(String(rest[afterOpen..<close.lowerBound]))
            }
            rest = rest[close.upperBound...]
        }
        return out.joined(separator: "\n;\n")
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    deinit {
        caretTimer?.invalidate()
        pollTimers.forEach { $0.invalidate() }
        stopFrameDriver()
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
        kickTickLoop()
        needsDisplay = true
    }

    // MARK: - 帧驱动(display link)

    /// 按需启动帧循环: 有过渡动画或流式跟随才跑, 静止界面零开销。
    private func kickTickLoop() {
        guard let ctx else { return }
        if frameDriverActive { return }
        let hasStreams = hn_context_doc(ctx).flatMap { hn_doc_find_attr($0, "hn-stream", 0) } != nil
        let probe = hn_context_anim_tick(ctx, 0)
        guard hasStreams || probe == 1 else { return }
        lastFrameTime = CACurrentMediaTime()
        startFrameDriver()
    }

    private func startFrameDriver() {
        guard !frameDriverActive else { return }
        frameDriverActive = true
        /* 驱动策略: CVDisplayLink 优先(与刷新率对齐, 无需屏幕关联),
           失败则退回 Timer(60fps)。
           注: CADisplayLink 在 accessory 激活策略 + 无屏幕关联时**不触发回调**
           (实测 caLink 创建成功但 onDisplayTick 零次), 因此不作为首选。 */
        var link: CVDisplayLink?
        if CVDisplayLinkCreateWithActiveCGDisplays(&link) == kCVReturnSuccess, let link {
            let cb: CVDisplayLinkOutputCallback = { _, _, _, _, _, context in
                guard let context else { return kCVReturnSuccess }
                let view = Unmanaged<HtmlNativeView>.fromOpaque(context).takeUnretainedValue()
                DispatchQueue.main.async { view.frameStep() }
                return kCVReturnSuccess
            }
            CVDisplayLinkSetOutputCallback(link, cb, Unmanaged.passUnretained(self).toOpaque())
            if CVDisplayLinkStart(link) == kCVReturnSuccess {
                cvLink = link
                return
            }
        }
        /* 兜底: 主 RunLoop 定时器(60fps)。
           在 CVDisplayLink 不可用的环境(无活动显示器/沙箱)也保证动画能跑。 */
        let t = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            self?.frameStep()
        }
        RunLoop.main.add(t, forMode: .common)
        fallbackTimer = t
    }

    private func stopFrameDriver() {
        frameDriverActive = false
        if let l = cvLink { CVDisplayLinkStop(l) }
        cvLink = nil
        fallbackTimer?.invalidate()
        fallbackTimer = nil
    }

    /// 一帧: 推进过渡插值 + 流式跟随 → 请求重绘。两者都空闲则停表。
    private func frameStep() {
        guard frameDriverActive, let ctx else { return }
        let now = CACurrentMediaTime()
        // dt 上限 50ms: 窗口被遮挡/休眠后恢复时, 避免一次性跳变
        let dt = Float(min(now - lastFrameTime, 0.05) * 1000.0)
        lastFrameTime = now
        guard dt > 0 else { return }
        let animActive = hn_context_anim_tick(ctx, dt) == 1
        let streamActive = followStreams(dt: dt)
        /* 动画推进改的是**样式字段**, 而 display list 是绘制阶段生成的 ——
           只标 needsDisplay 会重画旧指令(界面看着不动)。
           必须重新生成绘制指令(repaint 只重跑绘制, 不重算样式/布局,
           所以不会把动画插值结果覆盖掉)。 */
        if animActive {
            hn_context_repaint(ctx)
        }
        needsDisplay = true
        if !animActive && !streamActive { stopFrameDriver() }
    }

    // MARK: - 流式视图(agent 轨迹 / 日志 / 对话)

    /// 声明 `hn-stream` 的容器 = 流式视图: 内容增长时自动平滑跟随底部。
    /// 这是聊天/日志视图的 "tail -f" 语义, 由运行时提供, 页面只需声明属性。
    ///
    /// 跟随条件: 当前视口距底部不超过约 3 屏。用户主动向上翻看历史时,
    /// 不再强行拉回底部 —— 与真实聊天应用的行为一致。
    /// 返回 1 表示本帧仍在移动(帧循环需继续)。
    @discardableResult
    public func followStreams(dt: Float) -> Bool {
        guard let ctx, let doc = hn_context_doc(ctx), dt > 0 else { return false }
        let tau: Float = 0.10        // 时间常数(秒): 约 0.3s 收敛到静止
        var moving = false
        var idx: Int32 = 0
        while let node = hn_doc_find_attr(doc, "hn-stream", idx) {
            idx += 1
            var maxY: Float = 0
            hn_node_scroll_range(node, nil, &maxY)
            var cur: Float = 0
            hn_node_scroll_get(node, nil, &cur)
            var bx: Float = 0, by: Float = 0, bw: Float = 0, bh: Float = 0
            hn_node_box(node, &bx, &by, &bw, &bh)
            let gap = maxY - cur
            if gap < -0.5 {
                // 内容收缩(环形裁剪): 直接归位, 不缓动 —— 这是布局变化不是运动
                if hn_node_scroll_by(node, 0, gap) == 1 {
                    hn_context_repaint(ctx)
                    moving = true
                }
                continue
            }
            guard gap > 0.5 else { continue }
            // 用户主动上翻时不抢滚动条: 距底超过三屏就放手
            guard gap <= max(bh, 120) * 3 else { continue }

            // 帧率无关的真指数逼近: step = gap·(1 − e^(−dt/τ))
            // 不用线性近似(gap·dt·c: 帧率一变速度就变), 也不设最小步长
            // (那会让收尾阶段一直"爬行", 观感发滞)。
            let k = 1 - exp(-dt / 1000.0 / tau)
            var step = gap * k
            // 取整到整数像素: 中途每帧的偏移都是整数, 文字不会在不同亚像素相位
            // 反复重新栅格化(否则长文本滚动会发虚/抖动)。逼近是反馈回路, 会自纠偏差;
            // 最后一帧直接落到精确目标(整数中间帧 + 精确落点)。
            let rounded = step.rounded()
            step = rounded == 0 ? gap : rounded
            if hn_node_scroll_by(node, 0, step) == 1 {
                hn_context_repaint(ctx)
                moving = true
            }
        }
        return moving
    }

    /// 流式视图环形缓冲: `hn-stream-loop="14"` 只保留最近 14 条元素子节点。
    /// 让内容可以无限追加(agent 持续输出)而内存与视觉都收敛 —— 旧条目滚出视野即回收。
    public func trimStreams() {
        guard let ctx, let doc = hn_context_doc(ctx) else { return }
        var trimmed = false
        var idx: Int32 = 0
        // 先收集再摘除: 摘除会改变 DOM, 边遍历边改会漏项/错位
        var targets: [(OpaquePointer, Int32)] = []
        while let node = hn_doc_find_attr(doc, "hn-stream-loop", idx) {
            idx += 1
            guard let v = hn_node_attr(node, "hn-stream-loop"),
                  let keep = Int32(String(cString: v)), keep > 0 else { continue }
            targets.append((node, keep))
        }
        for (node, keep) in targets {
            if hn_node_trim_children(node, keep) > 0 { trimmed = true }
        }
        // 摘除改变了内容高度: 必须重排后再让任何代码读盒/滚动范围
        if trimmed { relayout() }
    }

    override public func draw(_ dirtyRect: NSRect) {
        guard let ctx, let dlp = hn_context_display_list(ctx),
              let cg = NSGraphicsContext.current?.cgContext else { return }
        /* 透明背板: 先清成全透明, 再由绘制指令画出内容。
           这样 html 里不画背景的地方就真的透出桌面/下层窗口
           (配合 window.isOpaque=false 与 backgroundColor=.clear)。
           非透明模式也清一次: 避免重绘时残留上一次的像素(拖影)。 */
        cg.saveGState()
        cg.setBlendMode(.copy)
        cg.setFillColor(NSColor.clear.cgColor)
        cg.fill(dirtyRect)
        cg.restoreGState()
        HNPainter.draw(dlp.pointee, into: cg)
    }

    /// 是否透明背板(由 hn-transparent 声明; 影响窗口配置)
    public var wantsTransparentBackdrop: Bool {
        guard let ctx, let doc = hn_context_doc(ctx) else { return false }
        var m = hn_manifest()
        hn_doc_manifest(doc, &m)
        return m.transparent == 1
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
        fullHTML = html
        hn_context_render(ctx, html, html.utf8.count)
        if let doc = hn_context_doc(ctx) { _ = hn_doc_autoid_hx(doc) }
        runPageScripts()
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
        _ = hn_doc_swap(doc, targetId, swap, html, html.utf8.count)
        // 换入片段若自带 hx-* 且缺 id, 补一个(不重启轮询 —— 重启会打断正在跑的定时器,
        // 导致"每轮都重启、swap 永不完成"的死循环)。
        if let d = hn_context_doc(ctx), let node = hn_doc_find_by_id(d, targetId) {
            _ = hn_doc_autoid_hx(d)
            _ = node
        }
        // 流式视图: 环形裁剪(内部含重排) → 帧循环接管跟随
        trimStreams()
        relayout()
        // arena 压缩: 片段追加会在 arena 上累积旧节点(环形裁剪只从链上摘除),
        // 每 100 次换入做一次全量重解析, 回收已移除节点的内存
        swapCount += 1
        if swapCount % 100 == 0, !fullHTML.isEmpty {
            hn_context_compact(ctx, fullHTML, fullHTML.utf8.count)
            if let doc = hn_context_doc(ctx) { _ = hn_doc_autoid_hx(doc) }
            relayout()
        }

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
        // 统一管道: 移动 / 进入 / 离开
        if node != currentHoverNode {
            if let old = currentHoverNode {
                _ = emit(HN_EV_MOUSELEAVE, at: p, node: old)
            }
            if let new = node {
                _ = emit(HN_EV_MOUSEENTER, at: p, node: new)
            }
        }
        _ = emit(HN_EV_MOUSEMOVE, at: p, node: node)
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

    // MARK: - 统一事件管道

    /// 事件分发的唯一出口。
    ///
    /// 设计: 引擎给出事件数据 + 冒泡路径(hn_event_path_at), 运行时沿路径
    /// 由内向外派发; 两个消费者共享同一来源:
    ///   1. JS 的 addEventListener(经 HNJSRuntime.dispatch)
    ///   2. hx-trigger 声明的行为(hover/focus/keydown 等)
    /// 任一层调用 preventDefault() 即中止继续派发(与 DOM 语义一致)。
    ///
    /// 返回 true 表示事件被消费(调用方不必再做默认处理)。
    @discardableResult
    public func emit(_ kind: hn_event_kind, at p: NSPoint?, node target: OpaquePointer?,
              keyCode: Int32 = 0, key: String? = nil, modifiers: UInt32 = 0,
              repeat isRepeat: Bool = false, delta: Float = 0, text: String? = nil) -> Bool {
        guard let ctx else { return false }
        let hit = target ?? (p.flatMap { hn_context_hit_node(ctx, Float($0.x), Float($0.y)) })
        guard let hit else { return false }

        let name = String(cString: hn_event_name(kind))
        let detail: [String: Any] = [
            "x": p.map { Double($0.x) } ?? 0,
            "y": p.map { Double($0.y) } ?? 0,
            "keyCode": Int(keyCode),
            "key": key ?? "",
            "shift": (modifiers & UInt32(HN_MOD_SHIFT)) != 0,
            "ctrl": (modifiers & UInt32(HN_MOD_CTRL)) != 0,
            "alt": (modifiers & UInt32(HN_MOD_ALT)) != 0,
            "meta": (modifiers & UInt32(HN_MOD_META)) != 0,
            "repeat": isRepeat,
            "delta": Double(delta),
            "text": text ?? "",
        ]

        // 沿冒泡路径由内向外派发
        let depth = hn_event_path_len(hit)
        var consumed = false
        for i in 0..<Int(depth) {
            guard let n = hn_event_path_at(hit, Int32(i)),
                  let idp = hn_node_attr(n, "id") else { continue }
            let id = String(cString: idp)

            // 消费者 1: JS 处理器(返回值 = 注册的处理器数; preventDefault 会置标志)
            if let rt = jsRuntime {
                jsPrevented = false
                rt.dispatch(event: name, elementId: id, detail: detail)
                if jsPrevented { consumed = true; break }
            }

            // 消费者 2: hx-trigger 声明的行为
            let trig = hn_node_attr(n, "hx-trigger").map { String(cString: $0).lowercased() } ?? ""
            if Self.triggerMatches(trig, event: name) {
                if let act = hxActionForNode(n) {
                    performHx(act)
                    consumed = true
                }
            }
        }
        return consumed
    }

    /// hx-trigger 与事件名的匹配(含 hx 的 hover 语义: 进入也触发)
    public static func triggerMatches(_ trig: String, event: String) -> Bool {
        if trig.isEmpty { return event == "click" }        // 默认 click
        switch event {
        case "click":
            return trig.contains("click") || trig.contains("load") == false && trig.isEmpty
        case "mouseenter":
            return trig.contains("hover") || trig.contains("mouseenter")
        case "mouseleave":
            return trig.contains("hover-out") || trig.contains("mouseleave")
        case "keydown":
            return trig.contains("keydown") || trig.contains("enter")
        case "focus":
            return trig.contains("focus")
        case "blur":
            return trig.contains("blur")
        case "scroll":
            return trig.contains("scroll")
        case "submit":
            return trig.contains("submit")
        case "change":
            return trig.contains("change")
        default:
            return trig.contains(event)
        }
    }

    /// 由节点直接构造 hx 动作(事件管道内用; 不经命中测试)
    func hxActionForNode(_ node: OpaquePointer) -> HxAction? {
        hxAction(for: node)
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
        // 统一事件管道: mousedown → click(冒泡), JS 与 hx-trigger 共享同一来源
        _ = emit(HN_EV_MOUSEDOWN, at: p, node: nil, modifiers: Self.modMask(event))
        if emit(HN_EV_CLICK, at: p, node: nil, modifiers: Self.modMask(event)) {
            return
        }
        onClickUnhandled?(hitTestId(at: p))
    }

    override public func mouseUp(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        _ = emit(HN_EV_MOUSEUP, at: p, node: nil, modifiers: Self.modMask(event))
        if let ctx {
            hn_context_set_active(ctx, nil)
            relayout()
        }
    }

    /// 修饰键 → 引擎掩码
    static func modMask(_ e: NSEvent) -> UInt32 {
        var m: UInt32 = 0
        if e.modifierFlags.contains(.shift) { m |= UInt32(HN_MOD_SHIFT) }
        if e.modifierFlags.contains(.control) { m |= UInt32(HN_MOD_CTRL) }
        if e.modifierFlags.contains(.option) { m |= UInt32(HN_MOD_ALT) }
        if e.modifierFlags.contains(.command) { m |= UInt32(HN_MOD_META) }
        return m
    }

    // MARK: - 输入(键盘编辑)

    private func focusInput(_ node: OpaquePointer?) {
        // focus/blur 事件(切换时派发到旧/新目标)
        if focusedInput != node {
            if let old = focusedInput {
                _ = emit(HN_EV_BLUR, at: nil, node: old)
            }
            if let new = node {
                _ = emit(HN_EV_FOCUS, at: nil, node: new)
            }
        }
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
        guard let ctx else { super.keyDown(with: event); return }
        // 统一管道: keydown 先派发(JS/hx 可消费, 如 Escape 关闭弹窗、快捷键)
        let kc = Self.hnKeyCode(event)
        let keyName = Self.keyName(event)
        let mods = Self.modMask(event)
        let hitForKeys = focusedInput ?? hn_context_hit_node(ctx, -1, -1)
        if emit(HN_EV_KEYDOWN, at: nil, node: hitForKeys, keyCode: kc,
                key: keyName, modifiers: mods, repeat: event.isARepeat) {
            return
        }
        guard let node = focusedInput, hn_node_is_input(node) == 1 else {
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
                let id = String(cString: idp)
                // input 事件: 值每次变化都派发(元素级冒泡)
                _ = emit(HN_EV_INPUT, at: nil, node: node, text: value)
                onInput?(id, value)
            }
        }
    }

    override public func keyUp(with event: NSEvent) {
        _ = emit(HN_EV_KEYUP, at: nil, node: focusedInput,
                 keyCode: Self.hnKeyCode(event), key: Self.keyName(event),
                 modifiers: Self.modMask(event))
    }

    /// 平台键码 → 引擎无关键码
    static func hnKeyCode(_ e: NSEvent) -> Int32 {
        switch e.keyCode {
        case 36, 76: return Int32(HN_KEY_ENTER)
        case 53:     return Int32(HN_KEY_ESC)
        case 48:     return Int32(HN_KEY_TAB)
        case 51:     return Int32(HN_KEY_BACKSPACE)
        case 117:    return Int32(HN_KEY_DELETE)
        case 123:    return Int32(HN_KEY_LEFT)
        case 124:    return Int32(HN_KEY_RIGHT)
        case 125:    return Int32(HN_KEY_DOWN)
        case 126:    return Int32(HN_KEY_UP)
        case 115:    return Int32(HN_KEY_HOME)
        case 119:    return Int32(HN_KEY_END)
        case 116:    return Int32(HN_KEY_PAGEUP)
        case 121:    return Int32(HN_KEY_PAGEDOWN)
        case 49:     return Int32(HN_KEY_SPACE)
        default:     return 0
        }
    }

    /// 键名(与 DOM KeyboardEvent.key 对齐)
    static func keyName(_ e: NSEvent) -> String {
        switch e.keyCode {
        case 36, 76: return "Enter"
        case 53:     return "Escape"
        case 48:     return "Tab"
        case 51:     return "Backspace"
        case 117:    return "Delete"
        case 123:    return "ArrowLeft"
        case 124:    return "ArrowRight"
        case 125:    return "ArrowDown"
        case 126:    return "ArrowUp"
        case 115:    return "Home"
        case 119:    return "End"
        case 116:    return "PageUp"
        case 121:    return "PageDown"
        case 49:     return " "
        default:     return e.charactersIgnoringModifiers ?? ""
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
        if !Self.hostAllowed(url) {
            FileHandle.standardError.write("[hn] 主机不在白名单: \(url.host ?? "?") — 请求被拒\n".data(using: .utf8)!)
            return
        }
        var req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData,
                             timeoutInterval: Self.requestTimeout)
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
    /// 协议实现: 需要判空的那一份(native 恒有值)
    public var engineContextOrNil: OpaquePointer? { ctx }

    /// 内省: 指定 id 的文本内容(排查"内容对不对")
    public func textOf(id: String) -> String? {
        guard let ctx, let doc = hn_context_doc(ctx),
              let n = hn_doc_find_by_id(doc, id) else { return nil }
        var buf = [CChar](repeating: 0, count: 65536)
        let len = hn_node_text_content(n, &buf, 65536)
        _ = len
        return String(cString: buf)
    }

    /// 内省: DOM 树 + 布局盒 + run 数(排查节点位置错乱)
    public func dumpDOM() -> [String] {
        guard let ctx, let doc = hn_context_doc(ctx) else { return [] }
        var out: [String] = []
        // 诊断头两行: 轮询元素与流式容器(判断"流不流动")。
        // 注意: 这里的遍历有固定栈上限, 只做有限次抽样, 不遍历整棵树。
        var polls: [String] = []
        for i: Int32 in 0..<8 {
            var idp: UnsafePointer<CChar>?
            var ms: Int32 = 0
            guard hn_doc_poll_at(doc, i, &idp, &ms) == 1 else { break }
            polls.append("\(idp.map { String(cString: $0) } ?? "?")@\(ms)ms")
        }
        out.append("· 轮询: \(polls.isEmpty ? "(无)" : polls.joined(separator: ", "))")
        var streams: [String] = []
        for i: Int32 in 0..<4 {
            guard let n = hn_doc_find_attr(doc, "hn-stream", i) else { break }
            var maxY: Float = 0
            hn_node_scroll_range(n, nil, &maxY)
            let loop = hn_node_attr(n, "hn-stream-loop").map { String(cString: $0) } ?? "-"
            streams.append("loop=\(loop) maxY=\(Int(maxY))")
        }
        out.append("· 流式容器: \(streams.isEmpty ? "(无)" : streams.joined(separator: ", "))")
        func walk(_ n: OpaquePointer, _ depth: Int) {
            if depth > 24 { return }   // 遮罩/弹窗常在深层嵌套
            var line = String(repeating: "  ", count: depth)
            if let tag = hn_node_tag(n) {
                line += String(cString: tag)
                if let idp = hn_node_attr(n, "id") { line += "#" + String(cString: idp) }
                /* class 必须显示: 样式问题排查基本都从类名定位 */
                if let clsp = hn_node_attr(n, "class") {
                    let cls = String(cString: clsp)
                    if !cls.isEmpty { line += "." + cls.replacingOccurrences(of: " ", with: ".") }
                }
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
                "k": c.kind == HN_CMD_TEXT ? "t"
                   : (c.kind == HN_CMD_RECT ? "r"
                   : (c.kind == HN_CMD_QUAD ? "q" : "x")),
            ]
            if c.kind == HN_CMD_TEXT {
                d["x"] = Int(c.tx); d["y"] = Int(c.baseline)
            } else if c.kind == HN_CMD_QUAD {
                /* 四边形: 顶点是关键信息(3D 投影结果) */
                var pts: [Int] = []
                withUnsafePointer(to: c.qx) { px in
                    withUnsafePointer(to: c.qy) { py in
                        px.withMemoryRebound(to: Float.self, capacity: 4) { ax in
                            py.withMemoryRebound(to: Float.self, capacity: 4) { ay in
                                for k in 0..<4 { pts.append(Int(ax[k])); pts.append(Int(ay[k])) }
                            }
                        }
                    }
                }
                d["q"] = pts
                d["fill"] = Int(c.fill)
                d["x"] = pts[0]; d["y"] = pts[1]
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
            // 定时器锁定 **id**, 不用索引重查 —— 索引会随 DOM 变化(片段换入)而失准,
            // 表现为"轮询只跑一轮就静默停住"。按 id 查找则始终指向同一逻辑目标。
            let tid = String(cString: idp)
            let interval = TimeInterval(ms) / 1000.0
            let t = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in
                guard let self, let ctx = self.ctx else { return }
                // 重新解析当前 DOM 上的属性(内容可能已被热更新)
                guard let doc2 = hn_context_doc(ctx) else { return }
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
