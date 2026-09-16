import Foundation
import JavaScriptCore

/// 原生 JavaScript 运行时（JavaScriptCore，系统自带框架，**不是 WebKit**）。
///
/// 定位：HTML 是结构与样式的载体；JS 只作为**交互业务能力的补充** ——
/// 需要计算、状态机、复杂条件逻辑时用它，而这些恰恰是纯声明式表达吃力的部分。
///
/// 设计原则：
/// 1. **引擎保持纯 C**：JS 运行时在运行时层，引擎不知道 JS 存在。
///    `hncore`（三平台二进制）依旧零依赖。
/// 2. **DOM 变更回到引擎**：JS 通过桥调用引擎的 C API 改 DOM，
///    布局/绘制仍是同一条管线 —— 所以 JS 改的东西与 HTML 写的完全等价。
/// 3. **受控的 API 面**：只暴露必要能力（查询/改属性/改文本/插片段/注册处理器），
///    不做完整 DOM 实现（那是浏览器的工作，也不是这里的定位）。
/// 4. **可关闭**：`hn-js="off"` 或没有 `<script>` 时零开销（不创建 JSContext）。
public final class HNJSRuntime {

    /// DOM 操作与查询的回调集合（由视图注入，桥到引擎 C API）
    public struct Bridge {
        /// 按 id 找元素，返回不透明句柄（0 = 未找到）
        public var findById: (String) -> UInt
        /// 按选择器找（仅支持 #id / .class / tag 的简单形式）
        public var findAll: (String) -> [UInt]
        /// 读属性（不存在返回 nil）
        public var getAttr: (UInt, String) -> String?
        /// 写属性
        public var setAttr: (UInt, String, String) -> Bool
        /// 读文本内容
        public var getText: (UInt) -> String
        /// 写文本内容
        public var setText: (UInt, String) -> Bool
        /// 插入 HTML 片段（handle, html, mode）；mode="create" 时建游离容器并返回其句柄
        public var insertHTML: (UInt, String, String) -> UInt
        /// 移除子节点
        public var removeChild: (UInt, UInt) -> Bool
        /// 元素标签名
        public var tagOf: (UInt) -> String
        /// 读样式值（computed style 的常用项）
        public var getStyle: (UInt, String) -> String?
        /// 写内联样式（并入 style 属性）
        public var setStyle: (UInt, String, String) -> Bool
        /// 触发请求（hx 语义复用：url, method, body, targetId, swap）
        public var fetch: (String, String, String, String, String) -> Void
        /// 元数据
        public var localStorageGet: (String) -> String?
        public var localStorageSet: (String, String) -> Void
        public var log: (String) -> Void
        public var reload: () -> Void
    }

    private let context: JSContext
    public let bridge: Bridge
    /// JS 执行时的异常（宿主可展示，便于调试）
    public private(set) var lastError: String?

    public init(bridge: Bridge) {
        self.bridge = bridge
        self.context = JSContext()!
        context.exceptionHandler = { [weak self] _, exc in
            let msg = exc?.toString() ?? "unknown"
            self?.lastError = msg
            bridge.log("[js error] \(msg)")
        }
        installGlobals()
    }

    // MARK: - 桥接

    /// 元素句柄 0 视为 null
    private func handleValue(_ v: JSValue?) -> UInt {
        guard let v, !v.isUndefined, !v.isNull else { return 0 }
        return UInt(v.toUInt32())
    }

    private func installGlobals() {
        let b = bridge

        // 单一派发器：JS 侧把 [操作名, 参数...] 打包成数组传入。
        // 这样规避 JavaScriptCore "block 形参个数必须与调用一致"的限制，
        // 任意元数的操作都能用同一条通道。
        let dispatch: @convention(block) (JSValue) -> JSValue = { [weak self] arg in
            guard let self else { return JSValue(undefinedIn: JSContext()) }
            let arr = arg.toArray() as? [Any] ?? []
            let op = (arr.first as? String) ?? ""
            let a = arr.count > 1 ? String(describing: arr[1]) : ""
            let a2 = arr.count > 2 ? String(describing: arr[2]) : ""
            let a3 = arr.count > 3 ? String(describing: arr[3]) : ""
            let a4 = arr.count > 4 ? String(describing: arr[4]) : ""
            let ctx = self.context
            func numOrNull(_ h: UInt) -> JSValue {
                h == 0 ? JSValue(nullIn: ctx) : JSValue(object: Int(h), in: ctx)
            }
            func handle(_ idx: Int) -> UInt {
                guard arr.count > idx else { return 0 }
                if let n = arr[idx] as? NSNumber { return UInt(truncating: n) }
                if let s = arr[idx] as? String, let v = UInt(s) { return v }
                return 0
            }
            switch op {
            case "queryId":   return numOrNull(b.findById(a))
            case "queryAll":
                return JSValue(object: b.findAll(a).map { Int($0) }, in: ctx)
            case "create":    return numOrNull(b.insertHTML(0, a, "create"))
            case "getAttr":
                guard let v = b.getAttr(handle(1), a2) else { return JSValue(nullIn: ctx) }
                return JSValue(object: v, in: ctx)
            case "setAttr":   return JSValue(bool: b.setAttr(handle(1), a2, a3), in: ctx)
            case "getText":   return JSValue(object: b.getText(handle(1)), in: ctx)
            case "setText":   return JSValue(bool: b.setText(handle(1), a2), in: ctx)
            case "setHTML":   return JSValue(bool: b.insertHTML(handle(1), a2, a3) != 0, in: ctx)
            case "removeChild": return JSValue(bool: b.removeChild(handle(1), handle(2)), in: ctx)
            case "tagOf":     return JSValue(object: b.tagOf(handle(1)), in: ctx)
            case "getStyle":
                guard let v = b.getStyle(handle(1), a2) else { return JSValue(nullIn: ctx) }
                return JSValue(object: v, in: ctx)
            case "setStyle":  return JSValue(bool: b.setStyle(handle(1), a2, a3), in: ctx)
            case "request":
                b.fetch(a, a2.isEmpty ? "GET" : a2, a3, a4, arr.count > 5 ? String(describing: arr[5]) : "inner")
                return JSValue(undefinedIn: ctx)
            case "storeGet":
                guard let v = b.localStorageGet(a) else { return JSValue(nullIn: ctx) }
                return JSValue(object: v, in: ctx)
            case "storeSet":  b.localStorageSet(a, a2); return JSValue(undefinedIn: ctx)
            case "log":       b.log(a); return JSValue(undefinedIn: ctx)
            case "reload":    b.reload(); return JSValue(undefinedIn: ctx)
            default:          return JSValue(undefinedIn: ctx)
            }
        }
        context.setObject(dispatch, forKeyedSubscript: "__hnCall" as NSString)

        // JS 侧的薄封装(shim 通过 __hnCall 访问引擎)
        context.evaluateScript(Self.shim)
    }

    /// JS 侧的薄封装：让 `el.textContent = 'x'` / `el.classList.add('y')` 这类
    /// 常见写法可用。全部转发到 __hn 桥，最终由引擎改 DOM。
    static let shim = #"""
    (function () {
      function call() {
        var a = Array.prototype.slice.call(arguments);
        return __hnCall(a);
      }
      // 显式取全局对象并挂载 API(不要依赖隐式全局赋值: 读未声明变量会抛错)
      var G = (typeof globalThis !== 'undefined') ? globalThis : this;
      G.document = G.document || {};
      function wrap(h) {
        if (h === null || h === undefined) return null;
        var o = {
          __h: h,
          get id() { return call('getAttr', h, 'id') || ''; },
          set id(v) { call('setAttr', h, 'id', String(v)); },
          get tagName() { return String(call('tagOf', h) || '').toUpperCase(); },
          getAttribute: function (n) { var v = call('getAttr', h, String(n)); return (v === null || v === undefined) ? null : v; },
          setAttribute: function (n, v) { call('setAttr', h, String(n), String(v)); },
          hasAttribute: function (n) { var v = call('getAttr', h, String(n)); return v !== null && v !== undefined; },
          removeAttribute: function (n) { call('setAttr', h, String(n), ''); },
          get textContent() { return call('getText', h); },
          set textContent(v) { call('setText', h, String(v)); },
          get innerHTML() { return call('getText', h); },
          set innerHTML(v) { call('setHTML', h, String(v), 'inner'); },
          insertAdjacentHTML: function (pos, html) {
            var m = (pos === 'afterbegin') ? 'prepend' : (pos === 'beforeend') ? 'append' : 'inner';
            call('setHTML', h, String(html), m);
          },
          appendChild: function (child) {
            if (child && child.__html !== undefined) call('setHTML', h, String(child.__html), 'append');
            return child;
          },
          removeChild: function (child) { call('removeChild', h, child && child.__h); },
          style: null,
          classList: {
            _get: function () { var c = call('getAttr', h, 'class'); return c ? String(c).split(/\s+/).filter(Boolean) : []; },
            _set: function (arr) { call('setAttr', h, 'class', arr.join(' ')); },
            add: function (c) { var s = this._get(); if (s.indexOf(c) < 0) { s.push(c); this._set(s); } },
            remove: function (c) { this._set(this._get().filter(function (x) { return x !== c; })); },
            contains: function (c) { return this._get().indexOf(c) >= 0; },
            toggle: function (c) { this.contains(c) ? this.remove(c) : this.add(c); }
          },
          __ev: {},
          addEventListener: function (type, fn) {
            (this.__ev[type] = this.__ev[type] || []).push(fn);
            return true;
          },
          removeEventListener: function (type, fn) {
            var a = this.__ev[type]; if (!a) return;
            var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1);
          },
          dispatch: function (type, ev) {
            var a = this.__ev[type] || [];
            for (var i = 0; i < a.length; i++) {
              try { a[i].call(this, ev || { type: type }); }
              catch (e) { call('log', 'handler error: ' + e); }
            }
            return a.length;
          },
          __html: undefined
        };
        // style 用代理实现属性式读写
        o.style = (typeof Proxy !== 'undefined') ? new Proxy({}, {
          get: function (_, k) { return call('getStyle', h, String(k)); },
          set: function (_, k, v) { call('setStyle', h, String(k), String(v)); return true; }
        }) : {};
        return o;
      }

      var doc = G.document;
      doc.__getElementById = function (id) { return wrap(call('queryId', String(id))); };
      doc.getElementById = doc.__getElementById;
      doc.querySelectorAll = function (sel) {
        var arr = call('queryAll', String(sel)) || [];
        var out = [];
        for (var i = 0; i < arr.length; i++) out.push(wrap(arr[i]));
        return out;
      };
      doc.querySelector = function (sel) {
        var a = doc.querySelectorAll(sel);
        return a.length ? a[0] : null;
      };
      doc.createElement = function (tag) { return wrap(call('create', String(tag))); };
      doc.createHTML = function (html) { return { __html: String(html) }; };

      // 高层业务 API: 请求(复用 hx 语义)、本地存储、日志
      G.hn = {
        request: function (url, method, body, target, swap) {
          call('request', String(url), method ? String(method) : 'GET',
               body === undefined ? '' : String(body),
               target === undefined ? '' : String(target),
               swap ? String(swap) : 'inner');
        },
        get: function (k) { var v = call('storeGet', String(k)); return (v === null || v === undefined) ? null : v; },
        set: function (k, v) { call('storeSet', String(k), String(v)); },
        log: function (m) { call('log', String(m)); }
      };
      G.console = G.console || { log: function () {
        var p = []; for (var i = 0; i < arguments.length; i++) p.push(String(arguments[i]));
        call('log', p.join(' '));
      } };
      G.window = G;
      G.__hnWrap = wrap;

      // 引擎事件派发入口(原生侧命中测试后调用)
      function __hnDispatchTo(id, type, detail) {
        var el = document.getElementById(id);
        if (!el) return 0;
        var ev = detail || {}; ev.type = type; ev.target = el;
        return el.dispatch(type, ev);
      }
      G.__hnDispatchTo = __hnDispatchTo;
    })();
    """#

    /// 执行页面里的 <script> 内容。
    /// 在 DOM 就绪后调用一次；脚本内的 DOM 变更会经桥回到引擎。
    @discardableResult
    public func run(_ script: String) -> Bool {
        lastError = nil
        context.evaluateScript(script)
        return lastError == nil
    }

    /// 调试: 求值表达式并返回结果描述(测试/排查用)
    public func probe(_ expr: String) -> String? {
        lastError = nil
        let v = context.evaluateScript(expr)
        if let e = lastError { return "error: " + e }
        return v?.toString()
    }

    /// 派发事件到 JS 处理器（引擎侧命中测试后调用）
    public func dispatch(event: String, elementId: String, detail: [String: Any] = [:]) {
        let det = (try? JSONSerialization.data(withJSONObject: detail))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        let js = "window.__hnDispatchTo(" + jsString(elementId) + ", " + jsString(event) + ", " + det + ")"
        context.evaluateScript(js)
    }

    private func jsString(_ s: String) -> String {
        let escaped = s
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n")
        return "\"\(escaped)\""
    }
}
