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
        /// 真实 HTTP 请求(Promise 风格): 方法/URL/头/体 → (status, headers, bodyText) 或错误
        public var fetchRequest: (String, String, [String: String], String?,
                                  @escaping (Int, [String: String], String) -> Void,
                                  @escaping (Int, [String: String], String) -> Void) -> Void
        /// 元数据
        public var localStorageGet: (String) -> String?
        public var localStorageSet: (String, String) -> Void
        public var log: (String) -> Void
        /// JS 调用 preventDefault/stopPropagation → 中止继续冒泡
        public var preventDefault: () -> Void
        public var reload: () -> Void
        /// 写网格顶点(handle, 扁平 x/y 数组, cols, rows) —— 应用层自定义 rig 用。
        /// 引擎只做"网格 + 贴图"合成; 骨骼/物理/口型同步由脚本逐帧驱动。
        public var setMeshVerts: (UInt, [Float], Int, Int) -> Bool
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
            case "setMesh":
                // 参数布局: [op, handle, 顶点串, cols, rows]
                // (arr[1] 是 handle —— 用 handle(1) 取; 顶点从 arr[2] 起)
                // 顶点用**字符串**而非嵌套数组传递: JS→Swift 的数组桥接对
                // 嵌套数组不可靠(toArray() 只做一层平坦化), 用字符串最稳,
                // 也避免了大网格(128x128 = 3 万个数)的逐元素桥接开销。
                var verts: [Float] = []
                if arr.count > 2, let s = arr[2] as? String, !s.isEmpty {
                    var idx = s.startIndex
                    while idx < s.endIndex {
                        guard let comma = s[idx...].firstIndex(of: ",") else {
                            if let v = Float(s[idx...]) { verts.append(v) }
                            break
                        }
                        if let v = Float(s[idx..<comma]) { verts.append(v) }
                        if comma == s.index(before: s.endIndex) { break }
                        idx = s.index(after: comma)
                    }
                }
                let cols = arr.count > 3 ? (Int(String(describing: arr[3])) ?? 0) : 0
                let rows = arr.count > 4 ? (Int(String(describing: arr[4])) ?? 0) : 0
                return JSValue(bool: b.setMeshVerts(handle(1), verts, cols, rows), in: ctx)
            case "request":
                b.fetch(a, a2.isEmpty ? "GET" : a2, a3, a4, arr.count > 5 ? String(describing: arr[5]) : "inner")
                return JSValue(undefinedIn: ctx)
            case "storeGet":
                guard let v = b.localStorageGet(a) else { return JSValue(nullIn: ctx) }
                return JSValue(object: v, in: ctx)
            case "storeSet":  b.localStorageSet(a, a2); return JSValue(undefinedIn: ctx)
            case "fetch":
                // 参数: url, method, headersJSON, body, callbackId
                let url = a
                let method = a2.isEmpty ? "GET" : a2
                var hdrs: [String: String] = [:]
                if let d = a3.data(using: .utf8),
                   let o = try? JSONSerialization.jsonObject(with: d) as? [String: String] {
                    hdrs = o
                }
                let body: String? = a4.isEmpty ? nil : a4
                let cbId = arr.count > 5 ? String(describing: arr[5]) : ""
                b.fetchRequest(method, url, hdrs, body, { status, respHeaders, text in
                    self.resolveFetch(cbId, ok: true, status: status, headers: respHeaders, body: text)
                }, { status, respHeaders, err in
                    // 错误也带状态码/响应头 —— JS 侧需要区分 404 与网络故障
                    self.resolveFetch(cbId, ok: false, status: status, headers: respHeaders, body: err)
                })
                return JSValue(undefinedIn: ctx)
            case "timerAdd":
                guard let ms = Double(a) else { return JSValue(object: 0, in: ctx) }
                let repeat_ = arr.count > 2 ? ((arr[2] as? Bool) ?? (arr[2] as? NSNumber)?.boolValue ?? false) : false
                return JSValue(object: Int(self.addTimer(ms: ms, repeats: repeat_)), in: ctx)
            case "timerClear":
                if let n = Int(a) { self.clearTimer(n) }
                return JSValue(undefinedIn: ctx)
            case "preventDefault": b.preventDefault(); return JSValue(undefinedIn: ctx)
            case "stopPropagation": b.preventDefault(); return JSValue(undefinedIn: ctx)
            case "log":       b.log(a); return JSValue(undefinedIn: ctx)
            case "reload":    b.reload(); return JSValue(undefinedIn: ctx)
            default:          return JSValue(undefinedIn: ctx)
            }
        }
        context.setObject(dispatch, forKeyedSubscript: "__hnCall" as NSString)

        // JS 侧的薄封装(shim 通过 __hnCall 访问引擎)
        context.evaluateScript(Self.shim)
        if let e = lastError {
            FileHandle.standardError.write("[hn-js] shim 加载失败: \(e)\n".data(using: .utf8)!)
        }
        #if DEBUG
        let chk = context.evaluateScript("typeof window")
        if chk?.toString() != "object" {
            FileHandle.standardError.write("[hn-js] shim 未生效: typeof window=\(chk?.toString() ?? "nil")\n".data(using: .utf8)!)
        }
        #endif
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

      /* setTimeout / setInterval: JavaScriptCore 是裸引擎, 这两个属于宿主能力,
         不内置。这里桥到原生定时器(runtime 用 Timer 实现), 让 JS 能写常规异步代码。
         注意: 缺失会在**调用时**抛异常 —— 若 shim 里用到, 会导致后续定义全部不执行
         (本项目踩过: hn.fetch 里的 setTimeout 让 window 都变得未定义)。 */
      G.setTimeout = function (fn, ms) {
        var id = String(call('timerAdd', String(ms || 0), false));
        __timers[id] = fn;
        return id;
      };
      G.setInterval = function (fn, ms) {
        var id = String(call('timerAdd', String(ms || 0), true));
        __timers[id] = fn;
        return id;
      };
      /* 注意: 不能写 `G.a = G.b = function (id) {...}` —— 链式赋值在语句位置
         会被解析为"匿名函数声明"(SyntaxError), 导致整个 shim 加载失败。
         必须用圆括号包住函数表达式。 */
      G.clearTimeout = (G.clearInterval = function (id) {
        if (id === undefined || id === null) return;
        delete __timers[String(id)];
        call('timerClear', String(id));
      });
      G.__timers = G.__timers || {};
      var __timers = G.__timers;
      /* 原生侧定时器触发时回调此函数 */
      G.__hnTimerFire = function (id) {
        var fn = __timers[String(id)];
        if (fn) { try { fn(); } catch (e) { call('log', 'timer error: ' + e); } }
      };
      /* 包装对象按 handle 缓存 —— 必须!
         若每次 getElementById 都新建包装, 那么 addEventListener 注册在
         临时对象上, 之后查询拿到的是另一个空对象 → 监听器静默丢失。 */
      var __wrapCache = {};
      function wrap(h) {
        if (h === null || h === undefined) return null;
        var key = String(h);
        if (__wrapCache[key]) return __wrapCache[key];
        var o = {
          __h: h,
          get id() { return call('getAttr', h, 'id') || ''; },
          set id(v) { call('setAttr', h, 'id', String(v)); },
          get tagName() { return String(call('tagOf', h) || '').toUpperCase(); },
          getAttribute: function (n) { var v = call('getAttr', h, String(n)); return (v === null || v === undefined) ? null : v; },
          setAttribute: function (n, v) { call('setAttr', h, String(n), String(v)); },
          hasAttribute: function (n) { var v = call('getAttr', h, String(n)); return v !== null && v !== undefined; },
          removeAttribute: function (n) { call('setAttr', h, String(n), ''); },
          /* 网格变形: 应用层自定义 rig(骨骼/物理/口型)逐帧写顶点。
             verts 是 2*(cols+1)*(rows+1) 个数字(x,y 交替), 元素盒局部坐标。
             写入后该元素改走网格绘制(贴图 = 元素的 src)。 */
          setMeshVerts: function (verts, cols, rows) {
            return !!call('setMesh', h, Array.prototype.join.call(verts, ','), cols, rows);
          },
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
        __wrapCache[key] = o;
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
      /* hn.fetch(url, options) → Promise<{ok,status,headers,text(),json()}>
         options: { method, headers, body } —— 语义对齐 Web 的 fetch(子集)
         iOS/macOS 侧走 URLSession; 回调经 __hnFetchDone 回到 Promise */
      var __fetchSeq = 0, __fetchPending = {};
      G.__hnFetchDone = function (id, ok, status, headers, body) {
        var p = __fetchPending[id];
        if (!p) return;
        delete __fetchPending[id];
        var res = {
          ok: ok, status: status, headers: headers || {},
          text: function () { return body; },
          json: function () { try { return JSON.parse(body); } catch (e) { return null; } }
        };
        if (ok) { p.resolve(res); }
        else {
          var err = new Error(body || ('HTTP ' + status));
          err.status = status; err.response = res;
          p.reject(err);
        }
      };
      G.hn = {
        request: function (url, method, body, target, swap) {
          call('request', String(url), method ? String(method) : 'GET',
               body === undefined ? '' : String(body),
               target === undefined ? '' : String(target),
               swap ? String(swap) : 'inner');
        },
        get: function (k) { var v = call('storeGet', String(k)); return (v === null || v === undefined) ? null : v; },
        set: function (k, v) { call('storeSet', String(k), String(v)); },
        log: function (m) { call('log', String(m)); },
        fetch: function (url, options) {
          options = options || {};
          var method = (options.method || 'GET').toUpperCase();
          var headers = options.headers || {};
          var body = options.body === undefined || options.body === null ? '' : String(options.body);
          var id = 'f' + (++__fetchSeq);
          return new Promise(function (resolve, reject) {
            __fetchPending[id] = { resolve: resolve, reject: reject };
            var hj = '{}';
            try { hj = JSON.stringify(headers); } catch (e) { hj = '{}'; }
            call('fetch', String(url), method, hj, body, id);
            G.setTimeout(function () {
              if (__fetchPending[id]) {
                delete __fetchPending[id];
                reject(new Error('fetch timeout: ' + url));
              }
            }, 20000);
          });
        }
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
        var ev = detail || {};
        ev.type = type;
        ev.target = el;
        ev.defaultPrevented = false;
        ev._stopped = false;
        ev.preventDefault = function () { ev.defaultPrevented = true; call('preventDefault'); };
        ev.stopPropagation = function () { ev._stopped = true; call('preventDefault'); };
        /* 常用只读字段(DOM 习惯): 按键、坐标、修饰键 */
        ev.key = ev.key || '';
        ev.keyCode = ev.keyCode || 0;
        ev.clientX = ev.x || 0;
        ev.clientY = ev.y || 0;
        ev.shiftKey = !!ev.shift; ev.ctrlKey = !!ev.ctrl;
        ev.altKey = !!ev.alt; ev.metaKey = !!ev.meta;
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

    // MARK: - 定时器(桥给 JS 的 setTimeout/setInterval)

    private var timers: [Int: Timer] = [:]
    private var timerSeq = 0
    private let timerLock = NSLock()

    func addTimer(ms: Double, repeats: Bool) -> Int {
        timerLock.lock()
        timerSeq += 1
        let id = timerSeq
        timerLock.unlock()
        let t = Timer.scheduledTimer(withTimeInterval: max(ms, 1) / 1000.0, repeats: repeats) {
            [weak self] tm in
            guard let self else { return }
            if !repeats { self.clearTimer(id) }
            // 回调 JS(主线程; Timer 已在主 RunLoop)
            self.context.evaluateScript("window.__hnTimerFire(" + String(id) + ")")
        }
        timerLock.lock(); timers[id] = t; timerLock.unlock()
        return id
    }

    func clearTimer(_ id: Int) {
        timerLock.lock()
        let t = timers.removeValue(forKey: id)
        timerLock.unlock()
        t?.invalidate()
    }

    /// 完成一次 fetch: 回调 JS 侧的 then/catch
    func resolveFetch(_ cbId: String, ok: Bool, status: Int, headers: [String: String], body: String) {
        guard !cbId.isEmpty else { return }
        let hdrJSON = (try? JSONSerialization.data(withJSONObject: headers))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        let js = "window.__hnFetchDone(" + jsString(cbId) + ", " + (ok ? "true" : "false")
            + ", " + String(status) + ", " + hdrJSON + ", " + jsString(body) + ")"
        DispatchQueue.main.async { [weak self] in
            self?.context.evaluateScript(js)
        }
    }

    /// 派发事件到 JS 处理器（引擎侧命中测试后调用）
    public func dispatch(event: String, elementId: String, detail: [String: Any] = [:]) {
        let det = (try? JSONSerialization.data(withJSONObject: detail))
            .flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        let js = "window.__hnDispatchTo(" + jsString(elementId) + ", " + jsString(event) + ", " + det + ")"
        let r = context.evaluateScript(js)
    }

    /// Swift 字符串 → JS 字符串字面量。
    ///
    /// 必须转义**所有**行终止符与控制字符: 只处理 \n 是不够的 ——
    /// 响应体/用户输入里的 \r 会让整个字面量提前终止(SyntaxError: Unexpected EOF),
    /// 表现为"事件或网络回调偶发失效", 且难以定位。
    /// U+2028/U+2029 在早期 ES 里也是行终止符, 一并处理。
    private func jsString(_ s: String) -> String {
        var out = "\""
        out.reserveCapacity(s.utf8.count + 8)
        for scalar in s.unicodeScalars {
            switch scalar {
            case "\\": out += "\\\\"
            case "\"": out += "\\\""
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            case "\u{08}": out += "\\b"
            case "\u{0C}": out += "\\f"
            case "\u{2028}": out += "\\u2028"
            case "\u{2029}": out += "\\u2029"
            default:
                if scalar.value < 0x20 {
                    out += String(format: "\\u%04x", scalar.value)
                } else {
                    out.unicodeScalars.append(scalar)
                }
            }
        }
        return out + "\""
    }
}
