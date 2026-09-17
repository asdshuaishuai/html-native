import AppKit
import CHtmlNative
import Foundation
import WebKit

/// 渲染宿主契约 —— HNEngine/宿主只依赖这一层, 具体渲染器可替换。
/// 两个实现: HtmlNativeView(自研引擎) 与 HNWebKitHost(WKWebView 兜底)。
public protocol HNWebHost: AnyObject {
    /// 作为 NSView 挂进窗口
    var asView: NSView { get }
    /// 本地 KV 隔离标识
    var storeId: String { get set }
    /// 全量渲染(首次与热更新共用)
    func render(_ html: String, css: String?)
    /// load 触发动作(native 由宿主发起; webkit 由注入脚本自理 → 返回空)
    func loadActions() -> [HxAction]
    func performHx(_ action: HxAction)
    func startPolling()
    func stopPolling()
    /// 内省(排查用): webkit 渲染器无引擎 DOM, 返回说明行
    func dumpDisplayList() -> [[String: Any]]
    func dumpDOM() -> [String]
    /// 首次绘制回调(窗口底色取 body 背景: 两条路径时机不同)
    var onFirstPaint: (() -> Void)? { get set }
    /// body 背景色(#rrggbb), 取不到返回 nil
    func bodyBackgroundHex() -> UInt32?
    /// 引擎上下文(仅 native 渲染器存在)。
    /// 用可选类型 + 独立命名: native 视图已有一个非可选的 engineContext 供
    /// 宿主/工具直接使用; 这里是"需要判空"的那一份 —— 若让 webkit 用一个
    /// 假指针(如 OpaquePointer(bitPattern: 1))去满足非空签名, 调用方任何
    /// "顺手调引擎 API" 都会解引用非法地址而崩溃。
    var engineContextOrNil: OpaquePointer? { get }

    /// 同步执行一段 JS 并返回结果的字符串形式; 该应用无 JS 环境返回 nil。
    /// **两个渲染器都必须实现**: native 走 JavaScriptCore(引擎仍是纯 C,
    /// 与 JS 隔离), webkit 走 WKWebView。少一个就会让 agent 的 eval 能力
    /// 只在某个渲染器上可用 —— 表现为"同一条 daemon 命令时好时坏"。
    func evalSync(_ js: String) -> Any?
}

/// sys:// 桥对象 —— 必须在 WKWebView 创建**之前**注册到 config,
/// 因为 WKWebView 会复制一份 configuration, 创建后再注册会丢失。
/// sys:// 请求的脚本消息桥。
/// 说明: WKWebView 的**异步** XMLHttpRequest 对自定义 scheme 不稳定
/// (同步可通, 异步 onerror), 因此兜底路径改用 WKScriptMessageHandler —
/// 脚本 postMessage 请求, 原生应答后再回调脚本, 语义与 native 完全一致。
final class HNWebKitBridge: NSObject, WKScriptMessageHandler {
    static let name = "hnSys"
    weak var host: HNWebKitHost?

    func userContentController(_ uc: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let host,
              let dict = message.body as? [String: Any],
              let url = dict["url"] as? String else { return }
        let body = dict["body"] as? String ?? ""
        let id = dict["id"] as? String ?? ""
        host.respond(requestId: id, url: url, body: body)
    }
}

/// WebKit 兜底渲染器: 系统 WKWebView。
///
/// 用途: 引擎暂不支持的构造(grid / absolute / canvas / svg / video)。
/// 由页面 `<meta name="hn-renderer" content="webkit">` 显式声明启用,
/// 绝不静默切换。窗口生命周期(ttl/热更新/持久化/list/dev watch)与
/// native 路径完全一致 —— 换的是渲染层, 不是应用模型。
public final class HNWebKitHost: NSObject, HNWebHost, WKNavigationDelegate {

    public var asView: NSView { webView }
    public var storeId: String = "default"
    public var onFirstPaint: (() -> Void)?

    private let webView: WKWebView
    private var bridge: HNWebKitBridge!
    private var pendingHTML: String = ""
    private var didFirstPaint = false

    /// sys:// 在 WebKit 里通过自定义 scheme 桥接(浏览器内核不允许未注册 scheme)

    public override init() {
        let cfg = WKWebViewConfiguration()
        // 让页面脚本可用(注入 htmx-lite 需要)
        let prefs = WKWebpagePreferences()
        prefs.allowsContentJavaScript = true
        cfg.defaultWebpagePreferences = prefs
        // 消息桥: 创建 WKWebView 之前注册(脚本 postMessage → 原生应答)
        let b = HNWebKitBridge()
        let ucc = WKUserContentController()
        ucc.add(b, name: HNWebKitBridge.name)
        cfg.userContentController = ucc
        self.bridge = b
        self.webView = WKWebView(frame: .zero, configuration: cfg)
        super.init()
        b.host = self
        webView.navigationDelegate = self
        webView.setValue(false, forKey: "drawsBackground")   // 透明底, 与主题背景融合
        if #available(macOS 12.0, *) { webView.underPageBackgroundColor = .clear }
    }

    // MARK: - 渲染

    public func render(_ html: String, css: String?) {
        // 与 native 路径同源: 主题令牌取自 C 引擎(单一事实来源), 再整体归一化(补单位)
        let themeName = HNRenderer.metaContent("hn-theme", in: html)
        let themeCSS = themeName.flatMap { n -> String? in
            guard let p = hn_theme_css(n) else { return nil }
            let base = hn_theme_base_css().map { String(cString: $0) } ?? ""
            return String(cString: p) + "\n" + base
        }
        let composed = [themeCSS, css].compactMap { $0 }.joined(separator: "\n")
        let normalizedCSS = HNCSSNormalizer.normalize(composed)
        let styled = Self.inject(html: html, css: normalizedCSS)
        pendingHTML = styled
        // 必须用 loadFileURL(file:// 源): loadHTMLString 的文档源无法访问
        // 已注册的自定义 scheme(XMLHttpRequest 会直接 onerror)
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("html-native-runtime", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let doc = dir.appendingPathComponent("page-\(UUID().uuidString).html")
        do {
            try styled.write(to: doc, atomically: true, encoding: .utf8)
            webView.loadFileURL(doc, allowingReadAccessTo: dir)
        } catch {
            // 兜底: 退回 loadHTMLString(自定义 scheme 不可用, 但页面仍能显示)
            webView.loadHTMLString(styled, baseURL: nil)
        }
    }

    /// 把 charset / 字体栈 / css / htmx-lite 注入文档, 使两条渲染路径表现一致。
    ///
    /// 三件事:
    /// 1. `charset=utf-8`: 缺声明时 WebKit 可能按 Latin-1 解码 → 中文乱码
    /// 2. 系统字体栈(含中文): WebKit 默认 Times, 中文会 fallback 到宋体;
    ///    native 用系统字体(中文=苹方)。补齐后两边字形一致
    /// 3. 作者 css + htmx-lite 运行时
    /// 测试/内省用: 暴露注入后的最终 HTML
    public static func injectedHTML(html: String, css: String) -> String {
        inject(html: html, css: HNCSSNormalizer.normalize(css))
    }

    static func inject(html: String, css: String) -> String {
        var out = html
        var injection = ""

        // 1) charset: 缺失时补在最前(HTML 规范要求前 1024 字节内)
        if !out.lowercased().contains("charset=") {
            injection += "<meta charset=\"utf-8\">\n"
        }
        // 2) 字体栈: 与 native 对齐(系统 sans + 苹方; 等宽给 code/pre)
        injection += """
        <style>
        \(fontStack)
        </style>
        <style>
        \(css)
        </style>
        <script>
        \(htmxLiteJS)
        </script>
        """
        if let r = out.range(of: "</head>", options: .caseInsensitive) {
            out.replaceSubrange(r, with: injection + "</head>")
        } else if let r = out.range(of: "<body", options: .caseInsensitive) {
            out.insert(contentsOf: injection, at: r.lowerBound)
        } else {
            out = injection + out
        }
        return out
    }

    // MARK: - HNWebHost(webkit 侧由注入脚本自理, 宿主层保持空实现)

    public func loadActions() -> [HxAction] { [] }
    public func performHx(_ action: HxAction) {}
    public func startPolling() {}
    public func stopPolling() {}

    public func dumpDisplayList() -> [[String: Any]] {
        [["k": "note", "s": "webkit 兜底渲染器: 无引擎绘制指令(渲染由 WKWebView 承担)"]]
    }

    public func dumpDOM() -> [String] {
        ["webkit 兜底渲染器: DOM 在 WKWebView 内, 请用 Safari 开发者工具检查"]
    }

    public var engineContextOrNil: OpaquePointer? { nil }

    public func evalSync(_ js: String) -> Any? {
        var result: Any?
        var done = false
        webView.evaluateJavaScript(js) { v, _ in result = v; done = true }
        let deadline = Date().addingTimeInterval(1.5)
        while !done && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        return result
    }

    public func bodyBackgroundHex() -> UInt32? {
        var hex: UInt32?
        var done = false
        webView.evaluateJavaScript("getComputedStyle(document.body).backgroundColor") { v, _ in
            if let s = v as? String { hex = Self.parseCSSColor(s) }
            done = true
        }
        // 同步等一小段(仅首帧调用一次)
        let deadline = Date().addingTimeInterval(0.35)
        while !done && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        return hex
    }

    /// 同步执行 JS(内省/验证用): 跑一小段 RunLoop 等回调
    /// "rgb(15, 18, 24)" / "rgba(15,18,24,1)" → 0xRRGGBBAA
    static func parseCSSColor(_ s: String) -> UInt32? {
        guard let open = s.firstIndex(of: "("), let close = s.lastIndex(of: ")") else { return nil }
        let body = s[s.index(after: open)..<close]
        let parts = body.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        guard parts.count >= 3 else { return nil }
        func comp(_ i: Int) -> UInt32 {
            let v = Double(parts[i]) ?? 0
            return UInt32(max(0, min(255, v)))
        }
        let a: UInt32 = parts.count >= 4 ? UInt32(max(0, min(255, (Double(parts[3]) ?? 1) * 255))) : 255
        return (comp(0) << 24) | (comp(1) << 16) | (comp(2) << 8) | a
    }

    // MARK: - WKNavigationDelegate

    public func webView(_ w: WKWebView, didFinish n: WKNavigation!) {
        guard !didFirstPaint else { return }
        didFirstPaint = true
        onFirstPaint?()
    }

    // MARK: - sys:// 桥(由 HNWebKitBridge 调用)

    /// 应答脚本请求: 调共享路由层拿片段, 再回注到页面
    func respond(requestId: String, url: String, body: String) {
        let fragment = HNAppRoutes.handle(url: url, form: body, storeId: storeId)
            ?? SystemBridge.fragment(for: url)
        let js = "window.__hnResolve(" + jsString(requestId) + ", " + jsString(fragment) + ")"
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    /// Swift 字符串 → JS 字面量(安全转义)
    func jsString(_ s: String) -> String {
        var out = "\""
        for ch in s.unicodeScalars {
            switch ch {
            case "\\": out += "\\\\"
            case "\"": out += "\\\""
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default:
                if ch.value < 0x20 { out += String(format: "\\u%04x", ch.value) }
                else { out.unicodeScalars.append(ch) }
            }
        }
        return out + "\""
    }



    // MARK: - 注入的 htmx-lite 运行时

    /// 与 native 侧语义对齐的最小实现: hx-get/post、hx-target、hx-swap、
    /// hx-trigger(click | load | every Ns)、表单参数携带、sys:// 桥。
    /// 字体栈: 与 native(NSFont.systemFont → 中文苹方)对齐, 避免 WebKit
    /// 默认 Times 导致中文 fallback 成宋体、两条路径字形不一致。
    /// 只设字体, 不碰盒模型/间距(WebKit 自带 UA 样式已足够, 叠加会翻倍)。
    static let fontStack = """
    html { font-family: -apple-system, BlinkMacSystemFont, "SF Pro Text",
           "PingFang SC", "Hiragino Sans GB", "Heiti SC",
           "Microsoft YaHei", sans-serif; }
    code, pre, kbd, samp, tt { font-family: "SF Mono", Menlo, Monaco,
           "Courier New", monospace; }
    """

    /// 用 Swift 原始字符串(#""")嵌入 JS: 反斜杠原样保留, 避免转义错位
    static let htmxLiteJS = #"""
    (function () {
      // sys:// 请求走原生消息桥(异步 XHR 对自定义 scheme 不可靠)
      var pending = {};
      var seq = 0;
      window.__hnResolve = function (id, html) {
        var p = pending[id];
        if (!p) return;
        delete pending[id];
        p(html);
      };
      function hnRequest(url, method, body) {
        return new Promise(function (resolve, reject) {
          var id = 'r' + (++seq);
          pending[id] = resolve;
          try {
            window.webkit.messageHandlers.hnSys.postMessage({
              id: id, url: url, body: body || '', method: method || 'GET'
            });
          } catch (e) {
            delete pending[id];
            reject(e);
          }
          setTimeout(function () {
            if (pending[id]) { delete pending[id]; reject(new Error('timeout')); }
          }, 8000);
        });
      }
      function formData() {
        var out = [];
        document.querySelectorAll('input,textarea').forEach(function (el) {
          var n = el.getAttribute('name') || el.id;
          if (!n) return;
          out.push(encodeURIComponent(n) + '=' + encodeURIComponent(el.value || ''));
        });
        return out.join('&');
      }
      function intervalOf(trig) {
        // 解析 "every 3s" / "every 500ms", 不依赖正则
        var i = trig.indexOf('every');
        if (i < 0) return 0;
        var rest = trig.substring(i + 5).trim();
        var num = '';
        for (var k = 0; k < rest.length; k++) {
          var c = rest.charAt(k);
          if (c >= '0' && c <= '9') num += c; else break;
        }
        if (!num) return 0;
        var n = parseInt(num, 10);
        var after = rest.substring(num.length).trim();
        return after.indexOf('ms') === 0 ? n : n * 1000;
      }
      function triggerOf(el) { return (el.getAttribute('hx-trigger') || 'click').toLowerCase(); }
      function request(el) {
        var get = el.getAttribute('hx-get'), post = el.getAttribute('hx-post');
        var url = post || get;
        if (!url) return;
        var target = el.getAttribute('hx-target') || 'this';
        var tEl = (target === 'this') ? el : document.getElementById(target);
        if (!tEl) return;
        var swap = (el.getAttribute('hx-swap') || 'innerHTML').toLowerCase();
        var body = null;
        if (post) body = formData();
        else {
          var q = formData();
          if (q) url += (url.indexOf('?') >= 0 ? '&' : '?') + q;
        }
        hnRequest(url, post ? 'POST' : 'GET', body).then(function (html) {
          if (swap === 'outerhtml') tEl.outerHTML = html;
          else if (swap === 'append' || swap === 'beforeend') tEl.insertAdjacentHTML('beforeend', html);
          else if (swap === 'prepend' || swap === 'afterbegin') tEl.insertAdjacentHTML('afterbegin', html);
          else tEl.innerHTML = html;
        }).catch(function (e) { tEl.textContent = '(请求失败: ' + url + ')'; });
      }
      function start() {
        document.querySelectorAll('[hx-trigger]').forEach(function (el) {
          var trig = triggerOf(el);
          if (trig.indexOf('load') >= 0) request(el);
          var ms = intervalOf(trig);
          if (ms > 0) setInterval(function () { request(el); }, ms);
        });
        // 点击(默认触发): 冒泡委托
        document.addEventListener('click', function (ev) {
          var el = ev.target && ev.target.closest ? ev.target.closest('[hx-get],[hx-post]') : null;
          if (!el) return;
          ev.preventDefault();
          request(el);
        });
        // 回车提交: 自身 → 祖先 → 最近容器的第一个 hx-post/get 载体(与 native 语义一致)
        document.addEventListener('keydown', function (ev) {
          if (ev.key !== 'Enter') return;
          var el = ev.target;
          if (!el || el.tagName !== 'INPUT') return;
          var carrier = el.closest('[hx-post],[hx-get]');
          if (!carrier) {
            var p = el.parentElement;
            for (var i = 0; i < 5 && p && !carrier; i++, p = p.parentElement) {
              carrier = p.querySelector('[hx-post],[hx-get]');
            }
          }
          if (carrier) { ev.preventDefault(); request(carrier); }
        });
      }
      if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
      else start();
    })();
    """#
}
