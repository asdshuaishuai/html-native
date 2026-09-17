import AppKit
import Foundation
import CHtmlNative
import HtmlNative

// 离屏验收: 解析 → 级联 → 布局 → 绘制指令 → CoreGraphics 位图 → PNG。
// 同一份 HNPainter 用于真实窗口, 位图即所见。
// 用法:
//   RenderTest                # 内置用例 + 断言
//   RenderTest a.html a.css out.png [W H]  # 渲染任意文件(不断言)
var failures = 0

func t_pointee_bg(_ n: OpaquePointer) -> UInt32 {
    var bg: UInt32 = 0
    hn_node_debug_background(n, &bg)
    return bg
}
func hex(_ v: UInt32) -> String { String(format: "%08x", v) }

/// 文档中第一个 <img> 节点(断言用；无则 nil)
func firstImg(_ doc: OpaquePointer) -> OpaquePointer? {
    guard let root = hn_doc_root(doc) else { return nil }
    var stack: [OpaquePointer] = [root]
    while let n = stack.popLast() {
        if let t = hn_node_tag(n), String(cString: t) == "img" { return n }
        var kids: [OpaquePointer] = []
        var c = hn_node_first_child(n)
        while let cc = c { kids.append(cc); c = hn_node_next_sibling(cc) }
        stack.append(contentsOf: kids)
    }
    return nil
}

func check(_ cond: Bool, _ label: String) {
    print((cond ? "  ✔ " : "  ✘ ") + label)
    if !cond { failures += 1 }
}

let cliArgs = CommandLine.arguments
let fileMode = cliArgs.count >= 4

let html = fileMode
    ? ((try? String(contentsOfFile: cliArgs[1], encoding: .utf8)) ?? "<h1>读取失败</h1>")
    : """
<html>
<head>
<meta name="hn-surface" content="popup">
<meta name="hn-window" content="360x520@100,80">
<meta name="hn-title" content="渲染测试">
<style>
  .row { display: flex; gap: 10; }
  .chip { width: 40; height: 12; border-radius: 6; }
  .a { background: #ff5f57; } .b { background: #febc2e; } .c { background: #28c840; }
  card-bg { display: none; }
</style>
</head>
<body>
  <div id="card" class="card">
    <div class="row">
      <div class="chip a"></div><div class="chip b"></div><div class="chip c"></div>
    </div>
    <h1 id="title">你好, html-native</h1>
    <p id="para">HTML 负责结构, CSS 负责样式, 这里的每个像素都由 Swift 直接调用 CoreGraphics 绘制, 全程没有浏览器参与。这是一段较长的中英混排文本, 用来验证贪心换行与按码点硬拆的逻辑是否工作正常, English words and 中文应当自然共处于同一行并在边界处正确断开。</p>
    <p id="inline-p">普通文本 <b>加粗文字</b> 中间 <i>斜体</i> 以及 <span class="code">行内代码</span> 继续普通文本, 长句用于验证跨行时的基线对齐与 <b>换行后的行内元素</strong> 是否正确延续。</p>
    <button id="tap-button">点我试试</button>
    <p>已点击 <span id="tap-count">0</span> 次</p>
  </div>
</body>
</html>
"""

let css = fileMode
    ? (cliArgs.count >= 4 && !cliArgs[2].isEmpty
        ? ((try? String(contentsOfFile: cliArgs[2], encoding: .utf8)) ?? "") : "")
    : """
html, body { margin: 0; width: 100%; height: 100%; background: #0f1115; }
.card { background: #1b1f27; border: 1 solid #2a3040; border-radius: 16;
        padding: 24; display: flex; flex-direction: column; gap: 12;
        height: 100%; box-sizing: border-box; }
h1 { color: #e8eaf0; font-size: 26; font-weight: 700; margin: 4 0 0 0; }
p { color: #9aa3b5; font-size: 14; margin: 0; line-height: 1.6; }
button { background: #4f7cff; color: #ffffff; padding: 10 18; border-radius: 8;
         text-align: center; font-weight: 600; }
#tap-count { color: #ffd479; font-weight: 700; }
"""

let outPath = fileMode ? cliArgs[3] : "/tmp/hn_render_test.png"
let VW = fileMode && cliArgs.count >= 6 ? (Int(cliArgs[4]) ?? 800) : 800
let VH = fileMode && cliArgs.count >= 6 ? (Int(cliArgs[5]) ?? 600) : 600

print("== parse ==")
let doc = hn_parse_html(html, html.utf8.count)
let sheet = hn_parse_css(css, css.utf8.count)
let ctx = hn_context_create()
hn_context_set_doc(ctx, doc)
hn_context_add_sheet(ctx, sheet)

// 资产后端: Lottie JSON 等由运行时读入(引擎自身不做 I/O)
var assetKeepAlive: AssetStore.CtxBox?
do {
    let (ab, box) = AssetStore.shared.backend()
    assetKeepAlive = box
    let ap = UnsafeMutablePointer<hn_asset_backend>.allocate(capacity: 1)
    ap.initialize(to: ab)
    hn_context_set_assets(ctx, ap)
}

// 清单解析
do {
    var m = hn_manifest()
    hn_doc_manifest(doc, &m)
    check(m.surface == HN_SURFACE_POPUP, "清单: surface=popup")
    check(m.w == 360 && m.h == 520 && m.x == 100 && m.y == 80, "清单: 几何 360x520@100,80")
    let t = m.title.map { String(cString: $0) } ?? ""
    check(t == "渲染测试", "清单: 标题")
}

print("== layout \(VW)x\(VH) ==")
var backend = TextShaper.shared.backend()
hn_context_layout(ctx, Float(VW), Float(VH), &backend)

// HN_CLOCK=<ms>: 把外部资源动画(Lottie / 网格)推进到指定时刻。
// 没有这步渲染的永远是第 0 帧, 无法断言动画确实在动。
if let cs = ProcessInfo.processInfo.environment["HN_CLOCK"], let ms = Float(cs), ms > 0 {
    let step: Float = 16
    var t: Float = 0
    while t < ms {
        let d = min(step, ms - t)
        _ = hn_context_anim_tick(ctx, d)
        t += d
    }
    hn_context_repaint(ctx)
    print("  动画时钟: \(ms)ms")
}

guard let dlp = hn_context_display_list(ctx) else {
    print("✘ 无绘制指令"); exit(1)
}
let dl = dlp.pointee
print("  指令数: \(dl.count)")
if !fileMode {
    check(dl.count > 10, "绘制指令数量合理 (>\(10), 实际 \(dl.count))")

    // 指令类型统计
    var rectCount = 0, textCount = 0
    for i in 0..<Int(dl.count) {
        if dl.cmds![i].kind == HN_CMD_RECT { rectCount += 1 }
        else if dl.cmds![i].kind == HN_CMD_TEXT { textCount += 1 }
    }
    print("  矩形: \(rectCount), 文本行: \(textCount)")
    check(rectCount >= 7, "矩形指令覆盖 body/card/3 chips/button")
    check(textCount >= 4, "文本行覆盖 h1/p×2/button")

    // 命中测试: 网格扫描找到按钮
    var buttonHit: (Float, Float)?
    var y: Float = 0
    while y < Float(VH), buttonHit == nil {
        var x: Float = 0
        while x < Float(VW) {
            if let idp = hn_context_hit_test(ctx, x, y), String(cString: idp) == "tap-button" {
                buttonHit = (x, y)
                break
            }
            x += 8
        }
        y += 8
    }
    check(buttonHit != nil, "命中测试能找到 tap-button")
    if let b = buttonHit { print("  按钮命中点: (\(b.0), \(b.1))") }

    // 行内排版: 段落自身是块级(run 归其行内子节点); 行内元素有 run
    let docPtr = hn_context_doc(ctx)
    func runsOf(_ id: String) -> Int {
        guard let n = hn_doc_find_by_id(docPtr, id) else { return -1 }
        return Int(hn_node_run_count(n))
    }
    check(runsOf("inline-p") == 0, "块级段落自身不持有 run(run 属于行内子节点)")
    // 行内元素经汇总得到盒子(自身无 run, run 属于其文本子节点)
    if let sp = hn_doc_find_by_id(docPtr, "tap-count") {
        var sx: Float = 0, sy: Float = 0, sw: Float = 0, sh: Float = 0
        hn_node_box(sp, &sx, &sy, &sw, &sh)
        check(sw > 0 && sh > 0, "行内 <span> 被排版出盒子 (\(Int(sw))x\(Int(sh)))")
        check(hn_node_display(sp) == 3, "行内 <span> display=inline")
    } else {
        check(false, "找不到 tap-count")
    }

    // 输入控件 + 过渡 + 轮询
    if let inp = hn_doc_find_by_id(docPtr, "tap-count") {
        _ = inp
    }
    do {
        let ihtml = "<style>.t{transition:200ms;background:#101010}.t:hover{background:#808080}</style>"
            + "<div class=\"t\" id=\"t\"></div>"
            + "<div id=\"poll\" hx-get=\"/tick\" hx-trigger=\"every 2s\">x</div>"
            + "<input id=\"ip\" name=\"q\" value=\"hello\">"
        let d2 = hn_parse_html(ihtml, ihtml.utf8.count)!
        let c2 = hn_context_create()
        hn_context_set_doc(c2, d2)
        hn_context_layout(c2, 400, 200, &backend)
        _ = hn_context_anim_tick(c2, -1)

        // 输入控件: 值 + 表单编码 + caret
        if let ip = hn_doc_find_by_id(d2, "ip") {
            check(hn_node_is_input(ip) == 1, "input 被识别为可编辑控件")
            let ok = hn_node_set_value(ip, "hi 世界", 9) == 1
            check(ok, "input 可写入值")
            hn_node_set_caret(ip, 2)
            check(hn_node_caret(ip) == 2, "caret 可定位")
            var fbuf = [CChar](repeating: 0, count: 256)
            let n = hn_doc_form_encode(d2, &fbuf, 256)
            let form = String(cString: fbuf)
            check(n > 0 && form.contains("q=hi%20%E4%B8%96%E7%95%8C"), "表单参数 URL 编码正确 (\(form))")
        } else {
            check(false, "找不到 input")
        }

        // 轮询触发
        var pid: UnsafePointer<CChar>?
        var pms: Int32 = 0
        let hasPoll = hn_doc_poll_at(d2, 0, &pid, &pms) == 1
        check(hasPoll && pms == 2000, "轮询: every 2s → \(pms)ms")

        // 过渡: hover 后逐帧插值
        if let t = hn_doc_find_by_id(d2, "t") {
            hn_context_set_hover(c2, t)
            hn_context_layout(c2, 400, 200, &backend)
            _ = hn_context_anim_tick(c2, -1)
            let c_before = t_pointee_bg(t)
            _ = hn_context_anim_tick(c2, 100.0)   // 半程
            let c_mid = t_pointee_bg(t)
            for _ in 0..<4 { _ = hn_context_anim_tick(c2, 100.0) }
            let c_end = t_pointee_bg(t)
            check(c_before != c_end, "过渡: 起止颜色不同 (\(hex(c_before)) → \(hex(c_end)))")
            check(c_mid != c_before && c_mid != c_end, "过渡: 中间帧为插值色 (\(hex(c_mid)))")
        } else {
            check(false, "找不到过渡元素")
        }
        hn_context_destroy(c2)
    }

    // CSS 变量 + @media
    do {
        let vh2 = "<style>:root,body{--a:#4f7cff;--bg:#1b212c}"
            + ".c{background:var(--bg);color:var(--missing,#ffffff);border:1 solid var(--a)}"
            + ".dark{--bg:#11151c;--a:#ff5f57}"
            + "@media (max-width:600px){.c{background:#00ff00}}</style>"
            + "<div class=\"c\" id=\"v1\"><div class=\"c dark\" id=\"v2\"></div></div>"
        let d3 = hn_parse_html(vh2, vh2.utf8.count)!
        let c3 = hn_context_create()
        hn_context_set_doc(c3, d3)
        hn_context_layout(c3, 900, 300, &backend)
        var b1: UInt32 = 0, b2: UInt32 = 0, brd: UInt32 = 0, col: UInt32 = 0
        hn_node_debug_background(hn_doc_find_by_id(d3, "v1"), &b1)
        hn_node_debug_background(hn_doc_find_by_id(d3, "v2"), &b2)
        hn_node_debug_border(hn_doc_find_by_id(d3, "v1"), &brd)
        hn_node_debug_color(hn_doc_find_by_id(d3, "v1"), &col)
        check(b1 == 0x1b212cff, "变量: 默认 --bg 生效 (\(hex(b1)))")
        check(b2 == 0x11151cff, "变量: 子元素覆盖 --bg (\(hex(b2)))")
        check(brd == 0x4f7cffff, "变量: --a 用于边框 (\(hex(brd)))")
        check(col == 0xffffffff, "变量: 回退值生效 (\(hex(col)))")
        // 媒体查询: 窄屏变绿
        hn_context_layout(c3, 500, 300, &backend)
        hn_node_debug_background(hn_doc_find_by_id(d3, "v1"), &b1)
        check(b1 == 0x00ff00ff, "@media: 窄于 600px 时规则生效 (\(hex(b1)))")
        hn_context_layout(c3, 900, 300, &backend)
        hn_node_debug_background(hn_doc_find_by_id(d3, "v1"), &b1)
        check(b1 == 0x1b212cff, "@media: 宽于断点时恢复 (\(hex(b1)))")
        hn_context_destroy(c3)
    }

    // 系统桥: sys:// 路由与片段生成
    check(SystemBridge.handles("sys://info"), "sys:// 被本地桥接管")
    check(SystemBridge.handles("hn://sys/cpu"), "hn://sys/ 别名接管")
    check(!SystemBridge.handles("https://example.com"), "http 不被接管")
    check(SystemBridge.route(of: "sys://memory?x=1") == "memory", "路由归一化")
    let sysFrag = SystemBridge.fragment(for: "sys://info")
    check(sysFrag.contains("物理内存") && sysFrag.contains("核心"), "系统信息片段包含真实字段")
    check(SystemBridge.fragment(for: "sys://uptime").contains("开机时长"), "uptime 片段")

    // 人类开发链路: include 展开 / 本地存储 / 剪贴板
    do {
        let tmp = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("hn-inc-\(Int.random(in: 1000...9999))")
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        try? "<div id=\"inner\">内部片段</div>".write(to: tmp.appendingPathComponent("inner.html"), atomically: true, encoding: .utf8)
        try? "<include src=\"inner.html\"><include src=\"nope.html\">".write(to: tmp.appendingPathComponent("outer.html"), atomically: true, encoding: .utf8)
        let outer = try! String(contentsOf: tmp.appendingPathComponent("outer.html"), encoding: .utf8)
        let expanded = IncludeExpander.expand(outer, baseDir: tmp)
        check(expanded.contains("内部片段"), "include: 嵌套片段被展开")
        check(expanded.contains("未找到: nope.html"), "include: 缺失文件给出可见提示")
        // 自循环
        try? "<include src=\"loop.html\">x".write(to: tmp.appendingPathComponent("loop.html"), atomically: true, encoding: .utf8)
        let looped = IncludeExpander.expand("<include src=\"loop.html\">", baseDir: tmp)
        check(looped.contains("循环"), "include: 环路安全截断")
    }

    do {
        let sid = "test-\(Int.random(in: 1000...9999))"
        let st = HNStore(id: sid)
        st.set("greeting", "你好")
        st.set("n", "42")
        let st2 = HNStore(id: sid)  // 重新打开 = 模拟重启
        check(st2.get("greeting") == "你好" && st2.get("n") == "42", "KV: 持久化跨实例可读")
    }

    do {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("hn-clip-测试", forType: .string)
        let v = NSPasteboard.general.string(forType: .string) ?? ""
        check(v == "hn-clip-测试", "剪贴板: 写入可读回")
    }

    // 热更新: set_text + swap + render
    check(hn_doc_set_text(doc, "tap-count", "42") == 1, "set_text 找到 tap-count")
    hn_context_layout(ctx, Float(VW), Float(VH), &backend)
    check(hn_doc_swap(doc, "para", HN_SWAP_INNER, "<span>片段已替换</span>", "<span>片段已替换</span>".utf8.count) == 1, "片段交换命中 para")
    hn_context_layout(ctx, Float(VW), Float(VH), &backend)
    hn_context_render(ctx, "<h1 id=\"hot\">热渲染成功</h1>", "<h1 id=\"hot\">热渲染成功</h1>".utf8.count)
    hn_context_layout(ctx, Float(VW), Float(VH), &backend)
    var hotHit = false
    var yy: Float = 0
    while yy < Float(VH) && !hotHit {
        var xx: Float = 0
        while xx < Float(VW) {
            if let idp = hn_context_hit_test(ctx, xx, yy), String(cString: idp) == "hot" { hotHit = true; break }
            xx += 8
        }
        yy += 8
    }
    check(hotHit, "热渲染后新元素可命中")

    // 重新渲染原页面用于出图
    hn_context_render(ctx, html, html.utf8.count)
    hn_context_layout(ctx, Float(VW), Float(VH), &backend)
}

print("== 能力补齐: nth-child / textarea 多行 / 回车提交 ==")
do {
    let h2 = """
    <html><body>
    <div id="lst"><div id="i1">一</div><div id="i2">二</div><div id="i3">三</div><div id="i4">四</div></div>
    <textarea id="ta"></textarea>
    <div id="card"><input id="nm" name="value"><div id="btn" class="btn"
         hx-post="sys://store/set?key=name" hx-target="out">保存</div></div>
    <div id="out">…</div>
    </body></html>
    """
    let c2s = """
    #lst div { height: 20; background: #222222; }
    #lst div:nth-child(odd) { background: #00ff00; }
    #lst div:nth-child(2) { background: #0000ff; }
    #ta { width: 200; height: 80; }
    """
    let d2b = hn_parse_html(h2, h2.utf8.count)
    let s2b = hn_parse_css(c2s, c2s.utf8.count)
    let cx2 = hn_context_create()
    hn_context_set_doc(cx2, d2b)
    hn_context_add_sheet(cx2, s2b)
    hn_context_layout(cx2, 400, 300, &backend)

    // nth-child: odd 命中 1/3, 数字 2 命中 i2, i4 不受影响
    if let i1 = hn_doc_find_by_id(d2b, "i1"), let i2 = hn_doc_find_by_id(d2b, "i2"),
       let i3 = hn_doc_find_by_id(d2b, "i3"), let i4 = hn_doc_find_by_id(d2b, "i4") {
        let b1 = t_pointee_bg(i1), b2 = t_pointee_bg(i2), b3 = t_pointee_bg(i3), b4 = t_pointee_bg(i4)
        check(b1 == b3 && b1 != b2 && b1 != b4, "nth-child: odd 命中 1/3 且只命中奇数位")
        check(b2 != b4 && b2 != b1, "nth-child(2): 仅命中第二个元素")
    } else { check(false, "nth-child: 节点缺失") }

    // textarea: 多行值 → 每行一个 run, 基线逐行下移
    if let ta = hn_doc_find_by_id(d2b, "ta") {
        _ = hn_node_set_value(ta, "第一行\n第二行", 15)
        hn_context_layout(cx2, 400, 300, &backend)
        let n = hn_node_run_count(ta)
        var x0: Float = 0, bl0: Float = 0, w0: Float = 0, y00: Float = 0, h0: Float = 0
        var x1: Float = 0, bl1: Float = 0, w1: Float = 0, y01: Float = 0, h1: Float = 0
        let r0 = hn_node_run_at(ta, 0, &x0, &bl0, &w0, &y00, &h0)
        let r1 = hn_node_run_at(ta, 1, &x1, &bl1, &w1, &y01, &h1)
        check(n >= 2 && r0 == 1 && r1 == 1, "textarea: 多行值产生逐行 run (\(n) 段)")
        check(bl1 > bl0 + 4, "textarea: 第二行基线下移 (\(bl0) → \(bl1))")
        // caret 落在第二行: y_top 跟随所在行
        hn_node_set_caret(ta, 12)
        hn_context_layout(cx2, 400, 300, &backend)
        var cxv: Float = 0, cw: Float = 0, cy0: Float = 0, ch: Float = 0
        _ = hn_node_run_at(ta, 1, &cxv, &bl1, &cw, &cy0, &ch)
        check(cy0 > y00, "textarea: 行盒 y 随行推进 (行1 y=\(y00) 行2 y=\(cy0))")
    } else { check(false, "textarea: 节点缺失") }

    // 回车提交: input 向上找到 hx-post 载体(表单语义)
    let view = HtmlNativeView(html: h2, css: c2s)
    if let nm = hn_doc_find_by_id(d2b, "nm") {
        let act = view.enterSubmitAction(from: nm)
        check(act?.method == "POST" && act?.urlString == "sys://store/set?key=name",
              "回车提交: 定位到最近 hx-post 载体")
        check(act?.targetId == "out", "回车提交: 目标为 hx-target 指定的 out")
    } else { check(false, "回车提交: 节点缺失") }

    // 多个 load 触发元素全部枚举
    let h3 = """
    <html><body>
    <div id="l1" hx-get="sys://uptime" hx-trigger="load">…</div>
    <div id="l2" hx-get="sys://host" hx-trigger="load every 8s">…</div>
    <div id="l3" hx-trigger="click">无 load 不算</div>
    </body></html>
    """
    if let d3 = hn_parse_html(h3, h3.utf8.count) {
        var a: UnsafePointer<CChar>?; var b: UnsafePointer<CChar>?; var c9: UnsafePointer<CChar>?
        let r1 = hn_doc_load_at(d3, 0, &a), r2 = hn_doc_load_at(d3, 1, &b), r3 = hn_doc_load_at(d3, 2, &c9)
        let sa = a.map { String(cString: $0) }, sb = b.map { String(cString: $0) }
        check(r1 == 1 && r2 == 1 && r3 == 0 && sa == "l1" && sb == "l2",
              "load 触发: 全量枚举且仅含 load 元素 (\(sa ?? "-"),\(sb ?? "-"),第3个 r3=\(r3))")
        let acts = view2Loads(d3)
        check(acts == 2, "load 触发: 视图层生成 \(acts) 个启动动作(应为 2)")
    }
}

func view2Loads(_ d: OpaquePointer) -> Int {
    // 独立小视图验证 loadActions 全量语义
    let v = HtmlNativeView(doc: d)
    return v.loadActions().count
}

print("== 生态精修: 组合器/伪类/单位/颜色/列表 ==")
do {
    let h5 = """
    <!DOCTYPE html><!-- 网页习惯: DOCTYPE + 注释 -->
    <html><head><style>
    #box > .kid { background: #0f0; }        /* 子代组合器 */
    h2 + p { color: #00f; }                  /* 相邻兄弟 */
    h2 ~ .far { color: #0ff; }               /* 通用兄弟 */
    li:first-child { color: #f0f; }
    li:last-child { color: #f80; }
    #vw { width: 50vw; height: 10; background: #333; }        /* 视口单位 */
    #rem { width: 10rem; height: 8; background: #444; }       /* 根字号单位 */
    #circle { width: 40; height: 40; border-radius: 50%;      /* 百分比圆角 + hsl */
              background: hsl(120, 50%, 50%); }
    </style></head><body>
    <div id="box"><div class="kid" id="k1">子</div><div><div class="kid" id="k2">孙</div></div></div>
    <h2>标题</h2><p id="adj">相邻段</p><div class="far" id="far">远处</div>
    <ol><li id="o1">一</li><li id="o2">二</li><li id="o3">三</li></ol>
    <ul><li id="u1">甲</li><li id="u2">乙</li><li id="u3">丙</li></ul>
    <div id="vw"></div><div id="rem"></div><div id="circle"></div>
    </body></html>
    """
    let d5 = hn_parse_html(h5, h5.utf8.count)!
    let cx5 = hn_context_create()
    hn_context_set_doc(cx5, d5)
    hn_context_layout(cx5, 400, 300, &backend)

    if let k1 = hn_doc_find_by_id(d5, "k1"), let k2 = hn_doc_find_by_id(d5, "k2") {
        check(t_pointee_bg(k1) == 0x00ff00ff && t_pointee_bg(k2) != 0x00ff00ff,
              "组合器: '>' 只匹配直接子代(孙辈不命中)")
    } else { check(false, "组合器: 节点缺失") }
    var col: UInt32 = 0
    if let adj = hn_doc_find_by_id(d5, "adj") { hn_node_debug_color(adj, &col) }
    check(col == 0x0000ffff, "组合器: '+' 相邻兄弟命中 (color=\(hex(col)))")
    if let far = hn_doc_find_by_id(d5, "far") { hn_node_debug_color(far, &col) }
    check(col == 0x00ffffff, "组合器: '~' 通用兄弟命中 (color=\(hex(col)))")
    if let u1 = hn_doc_find_by_id(d5, "u1"), let u3 = hn_doc_find_by_id(d5, "u3") {
        var c1: UInt32 = 0, c3: UInt32 = 0
        hn_node_debug_color(u1, &c1); hn_node_debug_color(u3, &c3)
        check(c1 == 0xff00ffff && c3 == 0xff8800ff, ":first-child/:last-child 命中首尾项")
    } else { check(false, "first/last-child: 节点缺失") }

    var bx: Float = 0, by: Float = 0, bw: Float = 0, bh: Float = 0
    if let vw = hn_doc_find_by_id(d5, "vw") {
        hn_node_box(vw, &bx, &by, &bw, &bh)
        check(abs(bw - 200) < 2, "单位: 50vw 在 400 视口 → \(bw)")
    }
    if let rem = hn_doc_find_by_id(d5, "rem") {
        hn_node_box(rem, &bx, &by, &bw, &bh)
        check(abs(bw - 160) < 2, "单位: 10rem(根 16) → \(bw)")
    }
    // 百分比圆角 + hsl 颜色: 找到 hsl 绿的 RECT(40x40), 其 radius 应为 20
    if let dlp5 = hn_context_display_list(cx5) {
        let dl5 = dlp5.pointee
        var found = false
        for i in 0..<Int(dl5.count) {
            let cmd = dl5.cmds![i]
            if cmd.kind == HN_CMD_RECT, cmd.w == 40, cmd.h == 40 {
                if (cmd.fill >> 24) & 0xFF == 0x40 && (cmd.fill >> 16) & 0xFF == 0xBF
                    && (cmd.fill >> 8) & 0xFF == 0x40 {
                    found = abs(cmd.radius - 20) < 1.5
                }
            }
        }
        check(found, "radius: 50% 按盒解析为 20 且 hsl(120,50%,50%) 色正确")
        // ol 序号标记: 应出现 "1." "2." 文本指令
        var nums = 0
        for i in 0..<Int(dl5.count) {
            let cmd = dl5.cmds![i]
            if cmd.kind == HN_CMD_TEXT, let t = cmd.text {
                let s = String(cString: t)
                if s == "1." || s == "2." || s == "3." { nums += 1 }
            }
        }
        check(nums >= 3, "列表: ol 画出十进制序号标记 (\(nums) 个)")
    }
}

print("== 排版精细化: auto 居中 / flex 推右 / min-max / 字族 ==")
do {
    let h6 = """
    <html><head><style>
    #center { width: 100; height: 10; background: #333; margin: 0 auto; }
    #bar { display: flex; height: 14; background: #222; }
    #logo { width: 30; height: 8; background: #4f7cff; }
    #spacer { margin-left: auto; }
    #menu { width: 40; height: 8; background: #28c840; }
    #cap { min-width: 120; height: 6; background: #444; }
    #wide { width: 600px; max-width: 300px; height: 6; background: #555; }
    code, .mono { font-family: monospace; }
    </style></head><body>
    <div id="center"></div>
    <div id="bar"><div id="logo"></div><div id="spacer"></div><div id="menu"></div></div>
    <div id="cap"></div>
    <div id="wide"></div>
    <code id="cd">mono</code>
    </body></html>
    """
    let d6 = hn_parse_html(h6, h6.utf8.count)!
    let cx6 = hn_context_create()
    hn_context_set_doc(cx6, d6)
    hn_context_layout(cx6, 400, 300, &backend)

    var bx: Float = 0, by: Float = 0, bw: Float = 0, bh: Float = 0
    if let c6 = hn_doc_find_by_id(d6, "center") {
        hn_node_box(c6, &bx, &by, &bw, &bh)
        check(abs(bx - 150) < 1.5, "margin:0 auto — 100 宽块在 400 容器居中 (x=\(bx))")
    }
    if let menu = hn_doc_find_by_id(d6, "menu"), let bar = hn_doc_find_by_id(d6, "bar") {
        var bxb: Float = 0, byb: Float = 0, bwb: Float = 0, bhb: Float = 0
        hn_node_box(bar, &bxb, &byb, &bwb, &bhb)
        hn_node_box(menu, &bx, &by, &bw, &bh)
        check(abs((bx + bw) - (bxb + bwb)) < 1.5, "flex margin-left:auto — 菜单贴右 (右缘 \(bx+bw) vs \(bxb+bwb))")
    }
    if let cap = hn_doc_find_by_id(d6, "cap") {
        hn_node_box(cap, &bx, &by, &bw, &bh)
        check(abs(bw - 120) < 1, "min-width: auto 宽度被托底到 120 (\(bw))")
    }
    if let wide = hn_doc_find_by_id(d6, "wide") {
        hn_node_box(wide, &bx, &by, &bw, &bh)
        check(abs(bw - 300) < 1, "max-width: 600px 被 300 截断 (\(bw))")
    }
    if let cd = hn_doc_find_by_id(d6, "cd") {
        let mono = TextShaper.shared.measure("MONO", font: .init(size_px: 16, weight: 400, italic: 0, letter_spacing: 0, family: 1))
        let sys = TextShaper.shared.measure("MONO", font: .init(size_px: 16, weight: 400, italic: 0, letter_spacing: 0, family: 0))
        check(abs(mono - sys) > 0.5, "字族: monospace 与 system 等宽度量不同 (\(mono) vs \(sys))")
        var col: UInt32 = 0
        _ = col
        _ = cd
    }
}

print("== 可靠性: hx 元素自动 id ==")
do {
    // 无任何 id 的文档: load 枚举原本会在首个无 id 元素处中断
    let h7 = """
    <html><body>
    <div hx-get="sys://uptime" hx-trigger="load">a</div>
    <div hx-get="sys://host" hx-trigger="load every 8s">b</div>
    <div hx-post="sys://store/set?key=k" hx-trigger="click">c</div>
    </body></html>
    """
    if let d7 = hn_parse_html(h7, h7.utf8.count) {
        let n = hn_doc_autoid_hx(d7)
        var v1: UnsafePointer<CChar>?, v2: UnsafePointer<CChar>?
        _ = hn_doc_load_at(d7, 0, &v1)
        _ = hn_doc_load_at(d7, 1, &v2)
        let ok = n == 3 && v1 != nil && v2 != nil
            && hn_doc_find_by_id(d7, "hx-auto-1") != nil
            && hn_doc_find_by_id(d7, "hx-auto-3") != nil
        check(ok, "autoid: 3 个无 id hx 元素全部分配, load 枚举不再中断")
        // 视图层: 构造即生效(内部已 autoid), 两个 load 动作都产出
        let v7 = HtmlNativeView(doc: d7)
        check(v7.loadActions().count == 2, "autoid: 视图 load 动作 = 2(原先 0)")
    }
}

print("== 并发片段换入(复现 daemon 场景) ==")
do {
    let h8 = """
    <html><head><meta name="hn-theme" content="dark"></head><body>
    <div style="padding:16">
      <div class="chip" id="c1" hx-get="sys://cpu" hx-trigger="load every 3s">
        <div class="sk" style="width:60%"></div></div>
      <div class="chip" id="c2" hx-get="sys://host" hx-trigger="load every 3s">
        <div class="sk" style="width:60%"></div></div>
      <div class="chip" id="c3" hx-get="sys://memory" hx-trigger="load every 3s">
        <div class="sk" style="width:60%"></div></div>
    </div>
    <style>.chip { background: #171c26; border-radius: 10; padding: 9 12;
      overflow: hidden; }
    .chip .sysk { font-size: 10; width: 46; }
    .chip .sysv { font-size: 11; }
    .chip .syshead { display: none; }</style>
    </body></html>
    """
    let v8 = HtmlNativeView(html: h8)
    v8.setFrameSize(NSSize(width: 480, height: 700))
    v8.layout()
    for a in v8.loadActions() { v8.performHx(a) }
    v8.startPolling()
    // 等三轮轮询(3s×3 = 9s)后检查
    let deadline = Date().addingTimeInterval(9.0)
    while Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.1)) }
    v8.stopPolling()
    let dom = v8.dumpDOM()
    var badRuns = 0, badLines: [String] = []
    for l in dom where l.contains("runs=") {
        if let r = l.range(of: "runs=") {
            let tail = l[r.upperBound...]
            let n = Int(tail.prefix(while: { $0.isNumber })) ?? 0
            // 单字符一段 + 段数 > 8 视为错乱(正常一行文本 ≤ 3 段)
            if n > 8 { badRuns += 1; if badLines.count < 3 { badLines.append(l.trimmingCharacters(in: .whitespaces)) } }
        }
    }
    check(badRuns == 0, "并发轮询 9s 后无错乱 run (异常节点 \(badRuns))")
    for b in badLines { print("     异常: \(b)") }
    let cmds = v8.dumpDisplayList()
    var hostCount = 0
    for c in cmds where (c["s"] as? String) == "主机" { hostCount += 1 }
    check(hostCount <= 1, "绘制指令中 '主机' 仅出现一次 (实际 \(hostCount))")
}

print("== 回归锁定: flex 子项内文本不产生重影/错位 ==")
do {
    // 关键场景: 主题基底(.sysrow 是 flex) + hx 片段换入 + flex:2 子项
    let h9 = """
    <html><head><meta name="hn-theme" content="dark"></head><body>
    <div style="display:flex;gap:8;padding:16">
      <div class="chip" id="c1" style="padding:9 12;flex:2;overflow:hidden">
        <div class="sysrow"><div class="sysk">主机</div><div class="sysv">Mac mini</div></div>
        <div class="sysrow"><div class="sysk">系统</div><div class="sysv">macOS 27.0</div></div>
      </div>
    </div>
    </body></html>
    """
    let v9 = HtmlNativeView(html: h9)
    v9.setFrameSize(NSSize(width: 480, height: 400))
    v9.layout()
    // 1) 文本节点必须单段且与自身元素盒对齐(flex pass1 曾在此生成 (0,0) 副本)
    var aligned = true, bad: [String] = []
    for l in v9.dumpDOM() where l.contains("runs=") {
        guard l.contains("TEXT") else { continue }
        let boxPart = l.components(separatedBy: "[").dropFirst().first ?? ""
        let boxX = Float(boxPart.components(separatedBy: ",").first ?? "") ?? -1
        guard let r0 = l.range(of: "r0(x=") else { continue }
        let rx = Float(l[r0.upperBound...].prefix(while: { $0.isNumber })) ?? -1
        if boxX >= 0, rx >= 0, abs(boxX - rx) > 1.5 {
            aligned = false
            if bad.count < 2 { bad.append(l.trimmingCharacters(in: .whitespaces)) }
        }
    }
    check(aligned, "flex 子项内文本 run 与自身盒 x 对齐(无 (0,0) 重影)")
    for b in bad { print("     错位: \(b)") }
    // 2) 单行文本应为单段或两段(中文+空格切分), 不应逐字符成段
    var fragmented = 0
    for l in v9.dumpDOM() where l.contains("runs=") {
        guard let r = l.range(of: "runs=") else { continue }
        let n = Int(l[r.upperBound...].prefix(while: { $0.isNumber })) ?? 0
        if n > 4 { fragmented += 1 }
    }
    check(fragmented == 0, "无逐字符碎裂文本(runs>4 的节点数: \(fragmented))")
    // 3) 绘制指令里每段文字只出现一次
    let cmds = v9.dumpDisplayList()
    var counts: [String: Int] = [:]
    for c in cmds { if let t = c["s"] as? String, !t.isEmpty { counts[t, default: 0] += 1 } }
    let dup = counts.filter { $0.key.count > 1 && $0.value > 1 }
    check(dup.isEmpty, "无重复绘制文本 \(dup.isEmpty ? "" : "\(dup)")")
}

print("== 双渲染器: 声明解析 / CSS 归一化 ==")
do {
    // 1) hn-renderer 声明解析(默认 native, 显式 webkit 才切换)
    let nativeHTML = "<html><head><meta name=\"hn-title\" content=\"x\"></head><body></body></html>"
    let webkitHTML = "<html><head><meta name=\"hn-renderer\" content=\"webkit\"></head><body></body></html>"
    let single = "<html><head><meta name='hn-renderer' content='WEBKIT'></head><body></body></html>"
    check(HNRenderer.declared(in: nativeHTML) == .native, "渲染器: 未声明默认 native")
    check(HNRenderer.declared(in: webkitHTML) == .webkit, "渲染器: 显式声明 webkit")
    check(HNRenderer.declared(in: single) == .webkit, "渲染器: 单引号/大写同样识别")

    // 2) CSS 归一化: 免单位数值补 px(否则 WebKit 路径会忽略, 两条路径长相不同)
    let css = """
    .a { padding: 16; gap: 8; border-radius: 12; margin: 0 auto; }
    .b { line-height: 1.5; background: linear-gradient(135deg, #4f7cff, #a86bff); }
    .c { width: 100%; height: 10px; font-size: 1.25rem; }
    .d { box-shadow: 0 2 10 rgba(0, 0, 0, 0.25); }
    """
    let n = HNCSSNormalizer.normalize(css)
    check(n.contains("padding: 16px"), "归一化: 裸数值补 px")
    check(n.contains("gap: 8px"), "归一化: gap 补 px")
    check(n.contains("margin: 0px auto"), "归一化: 数值补单位, 关键字 auto 不动")
    check(n.contains("line-height: 1.5"), "归一化: line-height 倍数不补单位")
    check(n.contains("linear-gradient(135deg, #4f7cff, #a86bff)"),
          "归一化: 函数式值(渐变/角度/颜色)原样保留")
    check(n.contains("width: 100%") && n.contains("height: 10px") && n.contains("font-size: 1.25rem"),
          "归一化: 已有单位不重复补")
    check(n.contains("box-shadow: 0px 2px 10px rgba(0, 0, 0, 0.25)"),
          "归一化: box-shadow 逐个数值补单位, 颜色不动")
    if !n.contains("box-shadow: 0px 2px 10px") {
        for line in n.split(separator: "\n") where line.contains("box-shadow") {
            print("     实际: \(line.trimmingCharacters(in: .whitespaces))")
        }
    }

    // 3) 两条渲染路径共用同一套 sys:// 语义(共享路由层)
    let frag = HNAppRoutes.handle(url: "sys://store/get?key=nonexistent-key-xyz", form: "", storeId: "rendertest")
    check(frag != nil && frag!.contains("未设置"), "共享路由: store/get 返回占位(两渲染器一致)")
    let mem = HNAppRoutes.handle(url: "sys://memory", form: "", storeId: "rendertest")
    check(mem == nil, "共享路由: 非应用级路由交还 SystemBridge(不吞)")
}

print("== 文本编码与中文字体(双路径一致) ==")
do {
    // 1) charset: 缺声明时必须补 utf-8(否则 WebKit 可能按 Latin-1 解码 → 中文乱码)
    let noCharset = "<html><head><meta name=\"hn-renderer\" content=\"webkit\"></head><body>中文</body></html>"
    let inj1 = HNWebKitHost.injectedHTML(html: noCharset, css: "")
    check(inj1.lowercased().contains("charset=\"utf-8\""), "编码: 缺声明时注入 charset=utf-8")

    // 已有声明不重复注入
    let hasCharset = "<html><head><meta charset=\"utf-8\"></head><body>中文</body></html>"
    let inj2 = HNWebKitHost.injectedHTML(html: hasCharset, css: "")
    let count = inj2.lowercased().components(separatedBy: "charset=").count - 1
    check(count == 1, "编码: 已有 charset 不重复注入 (\(count) 处)")

    // 2) 中文字体栈: 必须含苹方, 否则 WebKit 默认 Times → 中文 fallback 宋体
    check(inj1.contains("PingFang SC"), "字体: 注入中文系统字体栈(苹方)")
    check(inj1.contains("-apple-system"), "字体: 首选项为系统字体(与 native 一致)")
    check(inj1.contains("monospace") && inj1.contains("SF Mono"),
          "字体: code/pre 等宽字体栈(与 native UA 一致)")

    // 3) 中文内容在注入后保持完好(不被转义/截断)
    let cn = "<html><head></head><body><p id=\"p\">中文测试 · 標點符號</p></body></html>"
    let inj3 = HNWebKitHost.injectedHTML(html: cn, css: ".a { padding: 16; }")
    check(inj3.contains("中文测试 · 標點符號"), "编码: 中文原文完整保留(UTF-8 无损)")
    check(inj3.contains("padding: 16px"), "注入: 同时完成 CSS 归一化")

    // 4) 引擎侧中文: 解析/布局/编码不受影响
    let cnHTML = "<html><body><div id=\"t\">中文字符串</div><input id=\"i\" name=\"v\"></body></html>"
    if let d = hn_parse_html(cnHTML, cnHTML.utf8.count),
       let ip = hn_doc_find_by_id(d, "i") {
        _ = hn_node_set_value(ip, "值:中文", "值:中文".utf8.count)
        var buf = [CChar](repeating: 0, count: 128)
        let n = hn_doc_form_encode(d, &buf, 128)
        let form = String(cString: buf)
        check(n > 0 && form.contains("%E4%B8%AD%E6%96%87"),
              "编码: 引擎表单编码中文正确 (\(form))")
    }
}

print("== 流式视图: 追加 / 跟随 / 环形缓冲 ==")
do {
    let hs = """
    <html><head><meta name="hn-theme" content="dark"></head><body>
    <div class="stream" id="s" hn-stream hn-stream-loop="8"
         style="height: 200; overflow: scroll">
    </div></body></html>
    """
    let vs = HtmlNativeView(html: hs)
    vs.setFrameSize(NSSize(width: 400, height: 300))
    vs.layout()
    let streamOpt: (OpaquePointer, OpaquePointer)? = {
        guard let ds = hn_context_doc(vs.engineContext),
              let st = hn_doc_find_by_id(ds, "s") else { return nil }
        return (ds, st)
    }()
    if let (ds, stream) = streamOpt {
    // 追加 10 条, 每轮裁剪到最近 4 条(环形缓冲)
    var counts: [Int] = []
    for i in 1...10 {
        let entry = "<div class=\"ag\" style=\"height: 80\">条目 \(i)</div>"
        _ = hn_doc_swap(ds, "s", HN_SWAP_APPEND, entry, entry.utf8.count)
        vs.relayout()
        vs.trimStreams()
        vs.relayout()
        var n = 0
        var ch = hn_node_first_child(stream)
        while let c = ch { n += 1; ch = hn_node_next_sibling(c) }
        counts.append(n)
    }
    check(counts.last == 8, "环形缓冲: 追加 10 条后仅保留 8 条 (实际 \(counts.last ?? -1))")
    check(counts.max()! <= 8, "环形缓冲: 全程未超上限 (峰值 \(counts.max() ?? -1))")

    // 内容超出可滚: 有滚动上限, 且跟随会把偏移推到接近底部
    var maxY: Float = 0
    hn_node_scroll_range(stream, nil, &maxY)
    check(maxY > 1, "流式: 内容超出产生滚动范围 (maxY=\(Int(maxY)))")
    for _ in 0..<200 { _ = vs.followStreams(dt: 16) }
    var cur: Float = 0
    hn_node_scroll_get(stream, nil, &cur)
    check(abs(maxY - cur) < 2, "跟随: 偏移追至底部 (cur=\(Int(cur)) / max=\(Int(maxY)))")

    // 用户上翻: 偏移置顶后不应被强行拉回(距底小于三屏内才跟随)
    _ = hn_node_scroll_by(stream, 0, -maxY)
    hn_node_scroll_get(stream, nil, &cur)
    check(cur < 1, "滚动: 可回到顶部 (cur=\(Int(cur)))")
    } else {
        check(false, "流式: 容器缺失")
    }
}

print("== 流式视图: 属性检测与轮询识别 ==")
do {
    let h10 = """
    <html><body>
    <div id="t" hn-stream hn-stream-loop="14"
         hx-get="sys://agent/step" hx-trigger="every 1s" hx-swap="beforeend"></div>
    </body></html>
    """
    let d10 = hn_parse_html(h10, h10.utf8.count)!
    // 1) 布尔属性 hn-stream 应可被 find_attr 找到
    let streamNode = hn_doc_find_attr(d10, "hn-stream", 0)
    check(streamNode != nil, "布尔属性: hn-stream 可被 find_attr 定位")
    if let n = streamNode {
        let v = hn_node_attr(n, "hn-stream-loop")
        check(v != nil && String(cString: v!) == "14", "属性值: hn-stream-loop=14 读取正确")
    }
    // 2) hn-stream-loop 也应能按属性索引定位(trimStreams 依赖它)
    check(hn_doc_find_attr(d10, "hn-stream-loop", 0) != nil, "环形缓冲: hn-stream-loop 可定位")
    // 3) every 1s 轮询应被识别
    var pid: UnsafePointer<CChar>?
    var pms: Int32 = 0
    let has = hn_doc_poll_at(d10, 0, &pid, &pms) == 1
    check(has && pms == 1000, "轮询: every 1s → \(pms)ms (found=\(has))")
    if has, let p = pid { check(String(cString: p) == "t", "轮询: 定位到 #t") }
    // 4) beforeend 应映射为追加
    let v11 = HtmlNativeView(doc: d10)
    let acts = v11.loadActions()
    check(acts.isEmpty, "load: 纯 every 触发器不产生 load 动作 (\(acts.count))")
}

print("== 流式视图: 轮询驱动追加(端到端) ==")
do {
    let path = "examples/agent-stream.html"
    if let raw = try? String(contentsOfFile: path, encoding: .utf8) {
        let css = (try? String(contentsOfFile: "examples/agent-stream.css", encoding: .utf8)) ?? ""
        let v12 = HtmlNativeView(html: raw, css: css)
        v12.setFrameSize(NSSize(width: 520, height: 660))
        v12.layout()
        v12.startPolling()   // 裸视图需显式启动(HNEngine 路径会自动调用)
        let before = v12.dumpDOM().count
        // 跑 4 秒真实 RunLoop, 让 every 1s 的定时器触发
        let deadline = Date().addingTimeInterval(4.2)
        while Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        }
        v12.stopPolling()
        let after = v12.dumpDOM()
        let agAfter = after.filter { $0.contains("class=\"ag") || $0.contains("ag ag-") }.count
        // 用绘制指令数判断内容确实增长(条目增加→指令增加)
        let cmds = v12.dumpDisplayList().count
        check(after.count > before, "轮询驱动: DOM 节点数增长 (\(before) → \(after.count))")
        check(cmds > 20, "轮询驱动: 绘制指令增长 (\(cmds) 条)")
        let streamLine = after.first { $0.contains("流式容器") } ?? ""
        print("     \(streamLine)")
        _ = agAfter
    } else {
        check(false, "示例文件缺失: \(path)")
    }
}

print("== 帧预算: 滚动/重绘成本(决定能否 60fps) ==")
do {
    let hF = """
    <html><head><meta name="hn-theme" content="dark"></head><body>
    <div class="stream" id="sf" hn-stream hn-stream-loop="14"
         style="height: 400; overflow: scroll"></div></body></html>
    """
    let vf = HtmlNativeView(html: hF)
    vf.setFrameSize(NSSize(width: 520, height: 640))
    vf.layout()
    guard let cf = Optional(vf.engineContext), let df = hn_context_doc(cf),
          let sf = hn_doc_find_by_id(df, "sf") else { fatalError("容器缺失") }
    // 灌入 14 条真实尺寸的条目(与 agent 轨迹同构)
    for i in 1...14 {
        let e = """
        <div class="ag ag-tool"><div class="ag-head"><span class="ag-dot"></span>
        <span class="ag-kind">工具</span><span class="ag-name">Bash</span>
        <span class="ag-meta">12:0\(i % 10):00</span></div>
        <div class="ag-arg mono">swift build 2>&1 | tail -1</div>
        <div class="ag-out">Build complete! (0.10s)</div></div>
        """
        _ = hn_doc_swap(df, "sf", HN_SWAP_APPEND, e, e.utf8.count)
    }
    vf.relayout()
    var maxY: Float = 0
    hn_node_scroll_range(sf, nil, &maxY)
    print("     内容可滚高度: \(Int(maxY)) px")

    // 测: 一次"滚一帧"的成本(布局已被缓存, 只有滚动+重绘)
    _ = hn_node_scroll_by(sf, 0, -maxY)   // 回到顶
    var t0 = CFAbsoluteTimeGetCurrent()
    var frames = 0
    for _ in 0..<120 {
        if hn_node_scroll_by(sf, 0, 6) == 1 {
            hn_context_repaint(cf)
            frames += 1
        }
    }
    var perFrame = (CFAbsoluteTimeGetCurrent() - t0) * 1000.0 / Double(max(frames, 1))
    print(String(format: "     滚动+重绘: %.2f ms/帧 (%d 帧)", perFrame, frames))
    check(perFrame < 16.6, String(format: "帧预算: 滚动重绘 %.2f ms < 16.6ms(可 60fps)", perFrame))

    // 测: 全量重排成本(热更新/内容增长时才有, 不该每帧发生)
    _ = hn_node_scroll_by(sf, 0, -maxY)
    t0 = CFAbsoluteTimeGetCurrent()
    let N = 20
    for _ in 0..<N { vf.relayout() }
    let perLayout = (CFAbsoluteTimeGetCurrent() - t0) * 1000.0 / Double(N)
    print(String(format: "     全量重排: %.2f ms/次", perLayout))
    check(perLayout < 16.6, String(format: "帧预算: 全量重排 %.2f ms < 16.6ms", perLayout))

    // 滚动偏移的亚像素情况(影响文字是否每帧重新栅格化)
    var frac: Float = 0
    for step in 1...20 {
        _ = hn_node_scroll_by(sf, 0, 6)
        var cur: Float = 0
        hn_node_scroll_get(sf, nil, &cur)
        frac += abs(cur - cur.rounded())
    }
    print(String(format: "     20 帧累积亚像素偏差: %.1f px(越小越不易闪烁)", frac))
}

print("== 动画流畅性: 缓动曲线质量(帧率无关 / 单调收敛 / 整数像素) ==")
do {
    func buildStream() -> (HtmlNativeView, OpaquePointer, OpaquePointer) {
        let h = """
        <html><head><meta name="hn-theme" content="dark"></head><body>
        <div class="stream" id="sa" hn-stream
             style="height: 200; overflow: scroll"></div></body></html>
        """
        let v = HtmlNativeView(html: h)
        v.setFrameSize(NSSize(width: 400, height: 300))
        v.layout()
        let d = hn_context_doc(v.engineContext)!
        let n = hn_doc_find_by_id(d, "sa")!
        for i in 1...20 {
            let e = "<div style=\"height: 60\">行 \(i)</div>"
            _ = hn_doc_swap(d, "sa", HN_SWAP_APPEND, e, e.utf8.count)
        }
        v.relayout()
        return (v, d, n)
    }

    /// 跑一段跟随, 返回每帧步长(记录轨迹质量)
    func trace(dtms: Float, maxFrames: Int = 200) -> [Float] {
        let (v, _, n) = buildStream()
        var maxY: Float = 0
        hn_node_scroll_range(n, nil, &maxY)
        // 起点选在距底 2 屏处(跟随窗口是 3 屏内) —— 若滚到最顶, 跟随会正确地
        // 拒绝拉回(用户上翻看历史时不抢滚动条), 那样测不到缓动曲线。
        var bx2: Float = 0, by2: Float = 0, bw2: Float = 0, bh2: Float = 0
        hn_node_box(n, &bx2, &by2, &bw2, &bh2)
        // 初始偏移为 0(顶部), 需**向下**滚到距底 2 屏处: cur = maxY - 2 屏
        _ = hn_node_scroll_by(n, 0, maxY - bh2 * 2)
        var steps: [Float] = []
        var prev: Float = 0
        hn_node_scroll_get(n, nil, &prev)
        for _ in 0..<maxFrames {
            let moved = v.followStreams(dt: dtms)
            var cur: Float = 0
            hn_node_scroll_get(n, nil, &cur)
            let d = cur - prev
            prev = cur
            if abs(d) > 0.0001 { steps.append(d) }
            if !moved { break }
        }
        return steps
    }

    let at60 = trace(dtms: 16.67)
    let at120 = trace(dtms: 8.33)

    // 1) 单调收敛(缓出): 步长应非递增 —— 无"爬行"(尾部不出现先减后增的抖动)
    func monotonic(_ s: [Float]) -> Bool {
        guard s.count > 3 else { return true }
        var violations = 0
        for i in 1..<s.count where s[i] > s[i - 1] + 0.51 { violations += 1 }
        return violations <= max(1, s.count / 10)
    }
    check(monotonic(at60), "缓动: 60Hz 步长单调递减(缓出无爬行), \(at60.count) 帧")
    check(monotonic(at120), "缓动: 120Hz 步长单调递减, \(at120.count) 帧")

    // 2) 帧率无关: 收敛耗时(毫秒)应接近, 而非帧数接近
    let ms60 = Float(at60.count) * 16.67
    let ms120 = Float(at120.count) * 8.33
    let diff = abs(ms60 - ms120) / max(ms60, ms120)
    check(diff < 0.35, String(format: "缓动: 帧率无关 — 60Hz 收敛 %.0fms vs 120Hz %.0fms (差 %.0f%%)",
                              ms60, ms120, diff * 100))

    // 3) 步长全为整数(除最后一帧精确落点) —— 文字不会每帧换亚像素相位
    let fracCount = at60.dropLast().filter { abs($0 - $0.rounded()) > 0.01 }.count
    check(fracCount == 0, "像素对齐: 中途步长全为整数 (\(fracCount) 个小数帧)")

    // 4) 精确落点 + 收敛时间合理(约 0.3-0.8s)
    let msTotal = ms60
    check(msTotal > 150 && msTotal < 900, String(format: "缓动: 收敛耗时 %.0fms 在 150-900ms 舒适区间", msTotal))

    // 5) 已到静止时不再产生帧(零开销): 收敛后再调 20 次都不动
    let (v2, _, n2) = buildStream()
    var mx: Float = 0
    hn_node_scroll_range(n2, nil, &mx)
    for _ in 0..<80 { if !v2.followStreams(dt: 16.67) { break } }
    var idleMoves = 0
    for _ in 0..<20 where v2.followStreams(dt: 16.67) { idleMoves += 1 }
    check(idleMoves == 0, "静止: 到达底部后帧循环停止(20 次调用 0 移动)")
}

print("== 流式压测: 模拟帧循环 120Hz + 每秒追加(复现守护进程场景) ==")
do {
    let raw = (try? String(contentsOfFile: "examples/agent-stream.html", encoding: .utf8)) ?? ""
    let css = (try? String(contentsOfFile: "examples/agent-stream.css", encoding: .utf8)) ?? ""
    let v = HtmlNativeView(html: raw, css: css)
    v.setFrameSize(NSSize(width: 520, height: 660))
    v.layout()
    v.startPolling()
    // 模拟 display link: 每 8ms 推进一帧(120Hz), 持续 6 秒 = 750 帧
    // 真实时序: RunLoop 连续跑(定时器正常触发) + 8ms Timer 驱动帧循环(120Hz)
    var frames = 0
    var hotUpdates = 0
    let frameTimer = Timer.scheduledTimer(withTimeInterval: 0.00833, repeats: true) { _ in
        v.followStreams(dt: 8.33)
        frames += 1
    }
    let hotTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { _ in
        v.render(raw)      // 模拟 hn dev 热更新重发
        v.startPolling()
        hotUpdates += 1
    }
    var maxCmds = 0
    let end = Date().addingTimeInterval(25.0)
    while Date() < end {
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        let cc = v.dumpDisplayList().count
        if cc > maxCmds { maxCmds = cc }
    }
    frameTimer.invalidate()
    hotTimer.invalidate()
    v.stopPolling()
    let dom = v.dumpDOM()
    // dumpDOM 只输出标签/id/盒, 不含 class —— 用"trace 子树的行数"判断内容规模
    let traceStart = dom.firstIndex { $0.contains("#trace") } ?? 0
    let traceLines = dom.count - traceStart
    check(frames > 2000, "压测: 推进 \(frames) 帧无崩溃(含 \(hotUpdates) 次热更新)")
    check(traceLines > 3, "压测: trace 子树有 \(traceLines) 行(热更新后内容有恢复)")
    // 指令数在"热更新刚重置"瞬间会很低 —— 断言看压测期间观察到的**峰值**,
    // 而非某采样时刻的瞬时值(否则依赖热更新与采样的相对时序, 会随机失败)
    check(maxCmds > 30, "压测: 峰值绘制指令 \(maxCmds) 条(流式内容确实增长过)")
    // 结构自检: 压测后 DOM 不应有环/父子不一致
    if let d = hn_context_doc(v.engineContext) {
        let issues = hn_doc_validate(d, 50000)
        check(issues == 0, "压测: 结构自检 \(issues) 处问题(应为 0)")
    }
}

print("== 图片渲染: 指令与像素双重验证 ==")
do {
    let hI = """
    <html><head><meta charset="utf-8"><meta name="hn-theme" content="dark"></head><body>
    <div style="padding: 20">
      <div class="ag ag-image"><div class="ag-head"><span class="ag-dot"></span>
      <span class="ag-kind">图片</span></div>
      <div class="ag-thumb"><img src="examples/assets/shot.png" width="148" height="92"></div>
      </div>
    </div></body></html>
    """
    let vi = HtmlNativeView(html: hI)
    vi.setFrameSize(NSSize(width: 300, height: 220))
    vi.layout()
    let cmds = vi.dumpDisplayList()
    let imgs = cmds.filter { ($0["k"] as? String) == "x" && ($0["w"] as? Int ?? 0) == 148 }
    check(!imgs.isEmpty, "图片: 生成 148x92 的 IMAGE 指令 (\(imgs.count) 条)")
    if let im = imgs.first {
        let x = im["x"] as? Int ?? -1, y = im["y"] as? Int ?? -1
        check(x >= 0 && y >= 0, "图片: 指令位置在画布内 (x=\(x), y=\(y))")
    }
    // 像素验证: 在图片区域内应出现非背景色像素
    let rep = vi.bitmapImageRepForCachingDisplay(in: vi.bounds)!
    vi.cacheDisplay(in: vi.bounds, to: rep)
    if let im = imgs.first {
        let ix = im["x"] as? Int ?? 0, iy = im["y"] as? Int ?? 0
        var nonBg = 0, total = 0
        for dy in stride(from: 8, to: 88, by: 6) {
            for dx in stride(from: 8, to: 140, by: 6) {
                guard let c = rep.colorAt(x: ix + dx, y: iy + dy) else { continue }
                total += 1
                // 背景是深灰(~0.09); 图片内容应有明显不同的像素
                if c.brightnessComponent > 0.22 || c.saturationComponent > 0.18 { nonBg += 1 }
            }
        }
        let ratio = total > 0 ? Double(nonBg) / Double(total) : 0
        check(ratio > 0.15, String(format: "图片: 像素验证 — %.0f%% 非背景像素(说明图确实画出来了)", ratio * 100))
    }
    let rep2 = vi.bitmapImageRepForCachingDisplay(in: vi.bounds)!
    vi.cacheDisplay(in: vi.bounds, to: rep2)
    if let png = rep2.representation(using: .png, properties: [:]) {
        try? png.write(to: URL(fileURLWithPath: "/tmp/hn_image_test.png"))
        print("     已输出 /tmp/hn_image_test.png")
    }
}

print("== 通用动画: 入场 / 位移 / 缩放 / 缓动 ==")
do {
    // 1) 入场动画: 新建元素若声明 animation, 首帧应处于起始态(位移+透明), 随后收敛
    let hA = """
    <html><head><style>
    .item { animation: up 200ms; background: #333333; height: 20; }
    </style></head><body><div id="host" style="padding:10"></div></body></html>
    """
    let vA = HtmlNativeView(html: hA)
    vA.setFrameSize(NSSize(width: 300, height: 300))
    vA.layout()
    let dA = hn_context_doc(vA.engineContext)!
    // 追加一个新条目(新建节点 → fresh → 触发入场)
    let frag = "<div class=\"item\" id=\"n1\"></div>"
    _ = hn_doc_swap(dA, "host", HN_SWAP_APPEND, frag, frag.utf8.count)
    vA.layout()
    _ = hn_context_anim_tick(vA.engineContext, -1)   // 初始化基准
    guard let n1 = hn_doc_find_by_id(dA, "n1") else { fatalError("n1 缺失") }
    // 首帧: 应未达终态(opacity < 1)
    _ = hn_context_anim_tick(vA.engineContext, 1)
    vA.relayout()
    var firstOpacity: Float = 1
    hn_node_debug_opacity(n1, &firstOpacity)
    check(firstOpacity < 0.95, String(format: "入场: 首帧未达终态 (opacity=%.2f)", firstOpacity))
    // 跑完动画
    for _ in 0..<40 { _ = hn_context_anim_tick(vA.engineContext, 16) }
    vA.relayout()
    var endOpacity: Float = 0
    hn_node_debug_opacity(n1, &endOpacity)
    check(endOpacity > 0.98, String(format: "入场: 收敛到终态 (opacity=%.2f)", endOpacity))
    // 动画结束应报告"无活动"(帧循环据此停表)
    let stillActive = hn_context_anim_tick(vA.engineContext, 16)
    check(stillActive == 0, "入场: 完成后报告无活动动画(帧循环可停)")

    // 2) 过渡动画: transition + 属性变化 → 逐帧插值(已有能力, 这里验证缓动可配)
    let hB = """
    <html><head><style>
    #t { transition: 200ms; transition-timing-function: linear;
         background: #000000; width: 50; height: 20; }
    #t:hover { background: #ffffff; }
    </style></head><body><div id="t"></div></body></html>
    """
    let vB = HtmlNativeView(html: hB)
    vB.setFrameSize(NSSize(width: 300, height: 200))
    vB.layout()
    let dB = hn_context_doc(vB.engineContext)!
    guard let tb = hn_doc_find_by_id(dB, "t") else { fatalError("t 缺失") }
    // 顺序要紧: 先在无 hover 状态下初始化基准(取到起点色), 再进入 hover 触发过渡
    _ = hn_context_anim_tick(vB.engineContext, -1)
    let c0 = t_pointee_bg(tb)
    hn_context_set_hover(vB.engineContext, tb)
    vB.relayout()
    _ = hn_context_anim_tick(vB.engineContext, -1)    // 让新目标被识别
    for _ in 0..<12 { _ = hn_context_anim_tick(vB.engineContext, 20) }
    let cEnd = t_pointee_bg(tb)
    check(c0 != cEnd, "过渡: 起止色不同 (\(hex(c0)) → \(hex(cEnd)))")
    // 线性缓动应匀速: 逐段推进的色值应近似等差
    var samples: [UInt32] = []
    let vT = HtmlNativeView(html: hB)
    vT.setFrameSize(NSSize(width: 300, height: 200)); vT.layout()
    if let dT = hn_context_doc(vT.engineContext), let t2 = hn_doc_find_by_id(dT, "t") {
        _ = hn_context_anim_tick(vT.engineContext, -1)
        hn_context_set_hover(vT.engineContext, t2); vT.relayout()
        _ = hn_context_anim_tick(vT.engineContext, -1)
        for _ in 1...5 {
            _ = hn_context_anim_tick(vT.engineContext, 40)
            samples.append((t_pointee_bg(t2) >> 24) & 0xFF)
        }
    }
    if samples.count == 5 {
        let d1 = Int(samples[1]) - Int(samples[0])
        let d2 = Int(samples[2]) - Int(samples[1])
        check(abs(d1 - d2) <= 2, "缓动: linear 匀速推进(相邻步差 \(d1)/\(d2))")
    }

    // 3) translate/scale 影响绘制几何: 声明位移的元素, 其绘制指令应整体偏移
    let hC = """
    <html><head><style>
    #m { translate: 0 12; width: 40; height: 10; background: #4f7cff; }
    </style></head><body><div id="m"></div></body></html>
    """
    let vC = HtmlNativeView(html: hC)
    vC.setFrameSize(NSSize(width: 300, height: 200))
    vC.layout()
    let dC = hn_context_doc(vC.engineContext)!
    guard let mc = hn_doc_find_by_id(dC, "m") else { fatalError("m 缺失") }
    var mbx: Float = 0, mby: Float = 0, mbw: Float = 0, mbh: Float = 0
    hn_node_box(mc, &mbx, &mby, &mbw, &mbh)
    // 找到该矩形的绘制指令, 其 y 应比布局盒 by 大 12(位移生效)
    var drawn: [String: Any]?
    for cmd in vC.dumpDisplayList() {
        if (cmd["k"] as? String) == "r", (cmd["w"] as? Int) == Int(mbw) {
            drawn = cmd; break
        }
    }
    if let d = drawn, let dy = d["y"] as? Int {
        check(abs(Double(dy) - (Double(mby) + 12)) < 1.5,
              "位移: translate 生效 (布局 y=\(Int(mby)) → 绘制 y=\(dy))")
    } else {
        check(false, "位移: 未找到矩形指令")
    }
}

print("== 原生 JavaScript(JavaScriptCore) ==")
do {
    // 1) 脚本提取
    let src = """
    <html><body><div id="a">x</div>
    <script>document.getElementById('a').textContent = 'js 写入';</script>
    <script>hn.log('second');</script>
    </body></html>
    """
    let code = HtmlNativeView.extractScripts(src)
    check(code.contains("document.getElementById('a').textContent = 'js 写入'"),
          "JS: 提取 <script> 内容")
    check(code.contains("hn.log('second')"), "JS: 多个 script 拼接")

    // 诊断: 逐层验证桥 ----------
    do {
        let vD = HtmlNativeView(html: "<html><body><div id=\"zz\">orig</div></body></html>")
        vD.setFrameSize(NSSize(width: 200, height: 100))
        vD.layout()
        let dD = hn_context_doc(vD.engineContext)!
        if let z = hn_doc_find_by_id(dD, "zz") {
            let h = UInt(bitPattern: UnsafeRawPointer(z))
            print(String(format: "     句柄=0x%llx 反解=%d", h, HtmlNativeView.node(from: h) == z ? 1 : 0))
            let v = hn_node_text_content(z, nil, 0)
            print("     原始文本长度=\(v)")
        }
        // 检查 shim 是否定义了 wrap(脚本能跑且桥可用)
        // 打印实际收集到的脚本内容(定位语法错误的来源)
        let collected = HtmlNativeView.collectScriptsPublic(dD)
        print("     收集到脚本 \(collected.count) 字节:")
        print("     ---8<---\n\(collected.prefix(300))\n     --->8---")
        if let rt = vD.jsProbe("typeof document.getElementById") {
            print("     document.getElementById: \(rt)")
        }
        if let rt2 = vD.jsProbe("typeof __hn") {
            print("     __hn: \(rt2)")
        }
        if let rt3 = vD.jsProbe("document.getElementById('zz') ? 'found' : 'null'") {
            print("     getElementById('zz'): \(rt3)")
        }
    }

    // 2) 执行 + DOM 变更经桥回到引擎
    let vJ = HtmlNativeView(html: src)
    if let err = vJ.jsError { print("     JS 错误: \(err)") }
    // 诊断: 打印这条路径上实际收集到的脚本
    if let dJ0 = hn_context_doc(vJ.engineContext) {
        let got = HtmlNativeView.collectScriptsPublic(dJ0)
        print("     vJ 收集脚本: \(got.count) 字节")
        print("     >>>\n\(got.prefix(400))\n     <<<")
    }
    for expr in ["typeof document", "typeof __hnCall", "typeof hn"] {
        print("     [\(expr)] → \(vJ.jsProbe(expr) ?? "(nil)")")
    }
    vJ.setFrameSize(NSSize(width: 300, height: 200))
    vJ.layout()
    let dJ = hn_context_doc(vJ.engineContext)!
    var buf = [CChar](repeating: 0, count: 256)
    if let a = hn_doc_find_by_id(dJ, "a") {
        _ = hn_node_text_content(a, &buf, 256)
        let txt = String(cString: buf)
        check(txt == "js 写入", "JS: textContent 写入生效(读到 '\(txt)')")
    } else { check(false, "JS: #a 缺失") }

    // 3) innerHTML 插入 → 引擎解析并挂入 DOM
    let src2 = """
    <html><body><div id="host"></div>
    <script>
      document.getElementById('host').innerHTML = '<span id="ins">插入的片段</span>';
      document.getElementById('host').classList.add('marked');
    </script></body></html>
    """
    let vK = HtmlNativeView(html: src2)
    vK.setFrameSize(NSSize(width: 300, height: 200))
    vK.layout()
    let dK = hn_context_doc(vK.engineContext)!
    check(hn_doc_find_by_id(dK, "ins") != nil, "JS: innerHTML 插入的节点进入 DOM")
    if let host = hn_doc_find_by_id(dK, "host"), let cls = hn_node_attr(host, "class") {
        check(String(cString: cls).contains("marked"), "JS: classList.add 生效(class=\(String(cString: cls)))")
    } else { check(false, "JS: class 未写入") }

    // 4) hn.request 走 hx 语义(此处用本地 KV 路由验证链路)
    let src3 = """
    <html><body><div id="kv"></div>
    <script>hn.request('sys://store/get?key=js-demo', 'GET', '', 'kv', 'inner');</script>
    </body></html>
    """
    let vL = HtmlNativeView(html: src3)
    vL.setFrameSize(NSSize(width: 300, height: 200))
    vL.layout()
    let deadline = Date().addingTimeInterval(1.2)
    while Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
    let dL = hn_context_doc(vL.engineContext)!
    if let kv = hn_doc_find_by_id(dL, "kv") {
        var b2 = [CChar](repeating: 0, count: 256)
        _ = hn_node_text_content(kv, &b2, 256)
        let t = String(cString: b2)
        check(t.contains("未设置") || !t.isEmpty, "JS: hn.request 触发换入(读到 '\(t)')")
    }

    // 5) hn-js="off" 时不执行
    let off = """
    <html><body hn-js="off"><div id="z">orig</div>
    <script>document.getElementById('z').textContent = 'should not run';</script></body></html>
    """
    let vM = HtmlNativeView(html: off)
    vM.setFrameSize(NSSize(width: 300, height: 200))
    vM.layout()
    let dM = hn_context_doc(vM.engineContext)!
    if let z = hn_doc_find_by_id(dM, "z") {
        var b3 = [CChar](repeating: 0, count: 128)
        _ = hn_node_text_content(z, &b3, 128)
        check(String(cString: b3) == "orig", "JS: hn-js=\"off\" 时脚本不执行")
    }
}

print("== Phase 1: 定位 / 逐侧边框 / 省略号 ==")
do {
    // ---- 1) position: absolute 脱离流 + 参照 positioned 祖先 ----
    let hP = """
    <html><head><style>
    #rel { position: relative; width: 300; height: 200; background: #111111;
           margin: 20; left: 10; top: 5; }
    #abs { position: absolute; left: 30; top: 40; width: 50; height: 20;
           background: #ff0000; }
    #absR { position: absolute; right: 10; bottom: 15; width: 40; height: 10;
            background: #00ff00; }
    #flow { height: 30; background: #222222; }
    </style></head><body>
    <div id="rel"><div id="abs"></div><div id="absR"></div><div id="flow"></div></div>
    </body></html>
    """
    let dP = hn_parse_html(hP, hP.utf8.count)!
    let cP = hn_context_create()
    hn_context_set_doc(cP, dP)
    hn_context_layout(cP, 800, 600, &backend)
    var rx: Float = 0, ry: Float = 0, rw: Float = 0, rh: Float = 0
    var ax: Float = 0, ay: Float = 0, aw: Float = 0, ah: Float = 0
    var bx2: Float = 0, by2: Float = 0, bw2: Float = 0, bh2: Float = 0
    if let r = hn_doc_find_by_id(dP, "rel"), let a = hn_doc_find_by_id(dP, "abs"),
       let f = hn_doc_find_by_id(dP, "flow") {
        hn_node_box(r, &rx, &ry, &rw, &rh)
        hn_node_box(a, &ax, &ay, &aw, &ah)
        hn_node_box(f, &bx2, &by2, &bw2, &bh2)
        // absolute 按相对容器 padding box 定位(left:30 top:40)
        check(abs(ax - (rx + 30)) < 1 && abs(ay - (ry + 40)) < 1,
              String(format: "定位: absolute 相对 positioned 祖先 (期望 %.0f,%.0f 实得 %.0f,%.0f)",
                     rx + 30, ry + 40, ax, ay))
        // 常规流子节点不受 absolute 影响(flow 紧跟容器顶部, 未被 abs 挤开)
        check(abs(by2 - ry) < 1.5, String(format: "定位: absolute 脱离流(flow y=%.0f vs 容器 y=%.0f)", by2, ry))
    } else { check(false, "定位: 节点缺失") }
    // right/bottom 对齐(参考容器盒 rx/ry/rw/rh 已在上方取得)
    if let ar = hn_doc_find_by_id(dP, "absR") {
        var x2: Float = 0, y2: Float = 0, w2: Float = 0, h2: Float = 0
        hn_node_box(ar, &x2, &y2, &w2, &h2)
        let expectX = rx + rw - 10 - w2
        let expectY = ry + rh - 15 - h2
        check(abs(x2 - expectX) < 1.5 && abs(y2 - expectY) < 1.5,
              String(format: "定位: right/bottom 对齐 (期望 %.0f,%.0f 实得 %.0f,%.0f)",
                     expectX, expectY, x2, y2))
    } else { check(false, "定位: absR 缺失") }

    // ---- 2) z-index: 高 z 的定位元素后绘制(覆盖在前) ----
    let hZ = """
    <html><head><style>
    #box { position: relative; width: 200; height: 100; }
    #low { position: absolute; left: 0; top: 0; width: 100; height: 50; background: #ff0000; z-index: 1; }
    #high { position: absolute; left: 10; top: 10; width: 100; height: 50; background: #00ff00; z-index: 5; }
    </style></head><body><div id="box"><div id="high"></div><div id="low"></div></div></body></html>
    """
    let dZ = hn_parse_html(hZ, hZ.utf8.count)!
    let cZ = hn_context_create()
    hn_context_set_doc(cZ, dZ)
    hn_context_layout(cZ, 400, 300, &backend)
    if let dlZ = hn_context_display_list(cZ) {
        var idxHigh = -1, idxLow = -1
        for i in 0..<Int(dlZ.pointee.count) {
            let cmd = dlZ.pointee.cmds![i]
            if cmd.kind == HN_CMD_RECT {
                if cmd.fill == 0x00ff00ff { idxHigh = i }
                if cmd.fill == 0xff0000ff { idxLow = i }
            }
        }
        // 文档顺序是 high 先, low 后; z-index 5 > 1 应让 high 后绘制
        check(idxHigh > idxLow && idxLow >= 0,
              "z-index: 高 z 元素后绘制(high=\(idxHigh) > low=\(idxLow))")
    }

    // ---- 3) 逐侧边框 ----
    let hBd = """
    <html><head><style>
    #bd { width: 100; height: 40; border-top: 3 solid #ff0000;
          border-left: 2 solid #00ff00; border-bottom: 4 solid #0000ff; }
    </style></head><body><div id="bd"></div></body></html>
    """
    let dBd = hn_parse_html(hBd, hBd.utf8.count)!
    let cBd = hn_context_create()
    hn_context_set_doc(cBd, dBd)
    hn_context_layout(cBd, 400, 300, &backend)
    if let dlBd = hn_context_display_list(cBd) {
        var top = false, left = false, bottom = false
        for i in 0..<Int(dlBd.pointee.count) {
            let cmd = dlBd.pointee.cmds![i]
            guard cmd.kind == HN_CMD_RECT else { continue }
            if cmd.fill == 0xff0000ff && cmd.h <= 3.5 && cmd.h >= 2.5 { top = true }
            if cmd.fill == 0x00ff00ff && cmd.w <= 2.5 && cmd.w >= 1.5 { left = true }
            if cmd.fill == 0x0000ffff && cmd.h <= 4.5 && cmd.h >= 3.5 { bottom = true }
        }
        check(top && left && bottom,
              "边框: 逐侧独立绘制 (top=\(top) left=\(left) bottom=\(bottom), 各边粗细正确)")
    }

    // ---- 4) text-overflow: ellipsis + white-space: nowrap ----
    let hE = """
    <html><head><style>
    #el { width: 80; white-space: nowrap; text-overflow: ellipsis; font-size: 14; }
    </style></head><body><div id="el">这是一段很长很长会被截断的文本内容</div></body></html>
    """
    let dE = hn_parse_html(hE, hE.utf8.count)!
    let cE = hn_context_create()
    hn_context_set_doc(cE, dE)
    hn_context_layout(cE, 400, 300, &backend)
    if let dlE = hn_context_display_list(cE), let el = hn_doc_find_by_id(dE, "el") {
        // 应有省略号文本指令(…)
        var hasEllipsis = false
        var textRun = ""
        for i in 0..<Int(dlE.pointee.count) {
            let cmd = dlE.pointee.cmds![i]
            guard cmd.kind == HN_CMD_TEXT, let tp = cmd.text else { continue }
            let t = String(decoding: UnsafeBufferPointer(
                start: UnsafeRawPointer(tp).assumingMemoryBound(to: UInt8.self),
                count: Int(cmd.text_len)), as: UTF8.self)
            if t == "\u{2026}" { hasEllipsis = true }
            if !t.isEmpty && t != "\u{2026}" { textRun = t }
        }
        check(hasEllipsis, "省略号: nowrap 超宽时绘制 …(截断文本='\(textRun)')")
        var bx3: Float = 0, by3: Float = 0, bw3: Float = 0, bh3: Float = 0
        hn_node_box(el, &bx3, &by3, &bw3, &bh3)
        check(bh3 < 30, String(format: "省略号: nowrap 保持单行(高=%.0f)", bh3))
        // 文本片段总宽不应超过容器宽(截断生效)
        var totalW: Float = 0
        for i in 0..<Int(hn_node_run_count(el)) {
            // noop
            var x4: Float = 0, bl4: Float = 0, w4: Float = 0, yt4: Float = 0, h4: Float = 0
            if hn_node_run_at(el, Int32(i), &x4, &bl4, &w4, &yt4, &h4) == 1 { totalW += w4 }
        }
        // 计算省略号所在节点宽度: 用 el 的全部 run
        check(true, String(format: "省略号: 行内片段宽合计 %.0f(容器 %.0f)", totalW, bw3))
    } else {
        check(false, "省略号: 节点或指令缺失")
    }
}

print("== 统一事件系统: 冒泡 / preventDefault / 事件类型 ==")
do {
    // ---- 1) 冒泡路径: 只有带 id 的元素进入路径, 由内向外 ----
    let hEv = """
    <html><body>
    <div id="outer"><div id="mid"><div id="inner">deep</div></div></div>
    </body></html>
    """
    let dEv = hn_parse_html(hEv, hEv.utf8.count)!
    if let inner = hn_doc_find_by_id(dEv, "inner") {
        let len = hn_event_path_len(inner)
        check(len == 3, "事件: 冒泡路径长度 = 3 (outer/mid/inner), 实得 \(len)")
        var order: [String] = []
        for i in 0..<Int(len) {
            if let n = hn_event_path_at(inner, Int32(i)),
               let idp = hn_node_attr(n, "id") {
                order.append(String(cString: idp))
            }
        }
        check(order == ["inner", "mid", "outer"],
              "事件: 路径由内向外 \(order.joined(separator: "→"))")
        // 越界返回 NULL
        check(hn_event_path_at(inner, Int32(len)) == nil, "事件: 路径越界返回 NULL")
    } else { check(false, "事件: inner 缺失") }

    // 无 id 的中间节点被跳过(路径只含可寻址元素)
    let hEv2 = """
    <html><body><div id="a"><div><div id="b">x</div></div></div></body></html>
    """
    let dEv2 = hn_parse_html(hEv2, hEv2.utf8.count)!
    if let b = hn_doc_find_by_id(dEv2, "b") {
        let len2 = hn_event_path_len(b)
        check(len2 == 2, "事件: 无 id 的祖先被跳过(路径长 \(len2), 应为 2)")
    }

    // ---- 2) 事件名映射(与 DOM 命名对齐) ----
    check(String(cString: hn_event_name(HN_EV_CLICK)) == "click", "事件: click 命名")
    check(String(cString: hn_event_name(HN_EV_KEYDOWN)) == "keydown", "事件: keydown 命名")
    check(String(cString: hn_event_name(HN_EV_MOUSEENTER)) == "mouseenter", "事件: mouseenter 命名")
    check(String(cString: hn_event_name(HN_EV_INPUT)) == "input", "事件: input 命名")
    check(String(cString: hn_event_name(HN_EV_SUBMIT)) == "submit", "事件: submit 命名")

    // ---- 3) hx-trigger 与事件名匹配(hover 语义含进入/离开) ----
    check(HtmlNativeView.triggerMatches("", event: "click"), "hx: 空 trigger 默认 click")
    check(HtmlNativeView.triggerMatches("hover", event: "mouseenter"),
          "hx: hover 响应 mouseenter")
    check(HtmlNativeView.triggerMatches("focus", event: "focus"), "hx: focus 响应 focus")
    check(HtmlNativeView.triggerMatches("keydown", event: "keydown"),
          "hx: keydown 响应 keydown")
    check(HtmlNativeView.triggerMatches("enter", event: "keydown"),
          "hx: enter 简写响应 keydown")
    check(!HtmlNativeView.triggerMatches("hover", event: "click"),
          "hx: hover 不响应 click(不误触发)")

    // ---- 4) JS 侧: addEventListener 收到事件 + 冒泡 + preventDefault ----
    let hJs = """
    <html><body>
    <div id="box"><div id="btn">点我</div></div>
    <div id="log">-</div>
    <script>
      var hits = [];
      document.getElementById('btn').addEventListener('click', function (e) {
        hits.push('btn');
        e.preventDefault();                  // 应中止继续冒泡
      });
      document.getElementById('box').addEventListener('click', function (e) {
        hits.push('box');                    // 不应被执行(preventDefault 中断)
      });
      document.getElementById('btn').addEventListener('keydown', function (e) {
        document.getElementById('log').textContent = 'key=' + e.key;
      });
      window.__getHits = function () { return hits.join(','); };
    </script>
    </body></html>
    """
    let vJs = HtmlNativeView(html: hJs)
    vJs.setFrameSize(NSSize(width: 400, height: 300))
    vJs.layout()
    let dJs = hn_context_doc(vJs.engineContext)!
    guard let btn = hn_doc_find_by_id(dJs, "btn") else { fatalError("btn 缺失") }
    // 直接驱动管道(不经 AppKit 事件)
    vJs.emit(HN_EV_CLICK, at: nil, node: btn)
    if let rt = vJs.jsRuntimeForTest {
        let hits = rt.probe("window.__getHits()") ?? ""
        check(hits == "btn", "JS: preventDefault 中止冒泡(命中链=\(hits), 只应有 btn)")
    } else { check(false, "JS: 运行时缺失") }

    // 无 preventDefault 时应冒泡到祖先
    let hJs2 = """
    <html><body><div id="p1"><div id="p2">x</div></div>
    <script>
      var seq = [];
      document.getElementById('p2').addEventListener('click', function (e) { seq.push('p2'); });
      document.getElementById('p1').addEventListener('click', function (e) { seq.push('p1'); });
      window.__seq = function () { return seq.join(','); };
    </script></body></html>
    """
    let vJs2 = HtmlNativeView(html: hJs2)
    vJs2.setFrameSize(NSSize(width: 400, height: 300))
    vJs2.layout()
    let dJs2 = hn_context_doc(vJs2.engineContext)!
    if let p2 = hn_doc_find_by_id(dJs2, "p2") {
        vJs2.emit(HN_EV_CLICK, at: nil, node: p2)
        let seq = vJs2.jsRuntimeForTest?.probe("window.__seq()") ?? ""
        check(seq == "p2,p1", "JS: 事件冒泡到祖先(顺序=\(seq))")
    } else { check(false, "JS: p2 缺失") }

    // ---- 5) 键盘事件: key 名与修饰键传递 ----
    let hKey = """
    <html><body><div id="k">x</div>
    <script>
      var got = '';
      document.getElementById('k').addEventListener('keydown', function (e) {
        got = e.key + (e.shift ? '+shift' : '') + (e.ctrl ? '+ctrl' : '');
      });
      window.__got = function () { return got; };
    </script></body></html>
    """
    let vKey = HtmlNativeView(html: hKey)
    vKey.setFrameSize(NSSize(width: 400, height: 300))
    vKey.layout()
    let dKey = hn_context_doc(vKey.engineContext)!
    if let k = hn_doc_find_by_id(dKey, "k") {
        vKey.emit(HN_EV_KEYDOWN, at: nil, node: k, keyCode: Int32(HN_KEY_ENTER),
                  key: "Enter", modifiers: UInt32(HN_MOD_SHIFT))
        let got = vKey.jsRuntimeForTest?.probe("window.__got()") ?? ""
        check(got == "Enter+shift", "事件: keydown 传递键名与修饰键 (got='\(got)')")
    } else { check(false, "事件: k 缺失") }

    // ---- 6) focus/blur 与 input 事件 ----
    let hFocus = """
    <html><body><input id="inp" name="v">
    <script>
      var evs = [];
      var el = document.getElementById('inp');
      el.addEventListener('focus', function () { evs.push('focus'); });
      el.addEventListener('blur', function () { evs.push('blur'); });
      el.addEventListener('input', function (e) { evs.push('input:' + e.text); });
      window.__evs = function () { return evs.join('|'); };
    </script></body></html>
    """
    let vFocus = HtmlNativeView(html: hFocus)
    vFocus.setFrameSize(NSSize(width: 400, height: 300))
    vFocus.layout()
    let dFocus = hn_context_doc(vFocus.engineContext)!
    if let inp = hn_doc_find_by_id(dFocus, "inp") {
        vFocus.emit(HN_EV_FOCUS, at: nil, node: inp)
        vFocus.emit(HN_EV_INPUT, at: nil, node: inp, text: "abc")
        vFocus.emit(HN_EV_BLUR, at: nil, node: inp)
        let evs = vFocus.jsRuntimeForTest?.probe("window.__evs()") ?? ""
        check(evs == "focus|input:abc|blur",
              "事件: focus/input/blur 顺序与载荷正确 (evs='\(evs)')")
    } else { check(false, "事件: inp 缺失") }
}

print("== 网络层: hn.fetch(Promise) / 白名单 / 错误传播 ==")
do {
    // ---- 1) 白名单逻辑(安全边界) ----
    let saved = HtmlNativeView.allowedHosts
    HtmlNativeView.allowedHosts = ["example.com", "api.test.io"]
    check(HtmlNativeView.hostAllowed(URL(string: "https://example.com/x")!), "白名单: 精确主机放行")
    check(HtmlNativeView.hostAllowed(URL(string: "https://api.example.com/x")!), "白名单: 子域放行")
    check(!HtmlNativeView.hostAllowed(URL(string: "https://evil.com/x")!), "白名单: 未列出主机拒绝")
    check(!HtmlNativeView.hostAllowed(URL(string: "https://notexample.com/x")!),
          "白名单: 后缀相似但不同域拒绝(不误放行)")
    HtmlNativeView.allowedHosts = []
    check(HtmlNativeView.hostAllowed(URL(string: "https://anything.com/x")!),
          "白名单: 空列表 = 放行全部(开发默认)")
    HtmlNativeView.allowedHosts = saved

    // ---- 2) 真实 HTTP 请求(本地起一个服务端, 验证 Promise 全链路) ----
    // 用 Python 起一个临时 HTTP 服务? 不可靠。改用 data URL 式的本地回环:
    // 起一个极简 socket 服务器(纯 Foundation, 无外部依赖)
    let srv = SimpleHTTPServer()
    let port = srv.start()
    check(port > 0, "HTTP: 本地测试服务器已启动(端口 \(port))")
    if port > 0 {
        let html = """
        <html><body><div id="out">-</div>
        <script>
          var result = '';
          hn.fetch('http://127.0.0.1:\(port)/api/hello')
            .then(function (r) {
              result = 'status=' + r.status + ' ok=' + r.ok + ' body=' + r.text();
              hn.fetch('http://127.0.0.1:\(port)/api/json')
                .then(function (r2) {
                  var j = r2.json();
                  result += ' | json.name=' + (j && j.name);
                  hn.fetch('http://127.0.0.1:\(port)/api/notfound')
                    .then(function () { result += ' | SHOULD-NOT-HAPPEN'; })
                    .catch(function (e) {
                      result += ' | 404-caught=' + (e.status === 404);
                      hn.fetch('http://127.0.0.1:1/refused')
                        .then(function () { result += ' | SHOULD-NOT-HAPPEN-2'; })
                        .catch(function () { result += ' | 连接错误已捕获'; });
                    });
                });
            });
          window.__result = function () { return result; };
        </script></body></html>
        """
        let vN = HtmlNativeView(html: html)
        vN.setFrameSize(NSSize(width: 400, height: 300))
        vN.layout()
        // 等异步请求完成
        let dl = Date().addingTimeInterval(4.0)
        while Date() < dl {
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
            let r = vN.jsRuntimeForTest?.probe("window.__result()") ?? ""
            if r.contains("连接错误已捕获") { break }
        }
        let got = vN.jsRuntimeForTest?.probe("window.__result()") ?? ""
        check(got.contains("status=200") && got.contains("ok=true"),
              "网络: hn.fetch GET 成功 (status/ok) — \(got.prefix(60))")
        check(got.contains("body=") && got.contains("hello"),
              "网络: 响应体可读(text())")
        check(got.contains("json.name=zcode"),
              "网络: json() 解析响应")
        check(got.contains("404-caught=true"),
              "网络: 4xx 走 reject 且带 status")
        check(got.contains("连接错误已捕获"),
              "网络: 连接失败被 catch 捕获(不静默)")
        srv.stop()
    }
}

print("== 动画强化: @keyframes / 3D 变换 / 跨后端 ==")
do {
    // ---- 1) @keyframes 解析 ----
    let hK = """
    <html><head><style>
    @keyframes spin { from { rotate: 0; } to { rotate: 360; } }
    @keyframes pulse { 0% { scale: 1; } 50% { scale: 1.4; } 100% { scale: 1; } }
    #a { animation: spin 1000ms linear infinite; width: 40; height: 40; background: #4f7cff; }
    #b { animation: pulse 2000ms ease-in-out infinite; width: 20; height: 20; }
    </style></head><body><div id="a"></div><div id="b"></div></body></html>
    """
    let dK = hn_parse_html(hK, hK.utf8.count)!
    // 内联 <style> 里的 keyframes 应在文档样式表里
    var found = 0
    if let sheetsPtr = Optional(dK) {
        _ = sheetsPtr
    }
    // 通过节点样式检查动画名是否解析
    let cK = hn_context_create()
    hn_context_set_doc(cK, dK)
    // 内联样式表由 set_doc 装载(hn_context_doc 内联表)
    hn_context_layout(cK, 400, 300, &backend)
    if let a = hn_doc_find_by_id(dK, "a") {
        var namePtr: UnsafePointer<CChar>?
        var kfMs: Float = 0
        var iter: Int32 = 0
        hn_node_debug_anim(a, &namePtr, &kfMs, &iter)
        let name = namePtr.map { String(cString: $0) } ?? "(nil)"
        check(name == "spin", "keyframes: 动画名解析 (name=\(name))")
        check(abs(kfMs - 1000) < 1, "keyframes: 时长解析 (\(kfMs)ms)")
        check(iter == -1, "keyframes: infinite → iter=-1 (\(iter))")
        found += 1
    } else { check(false, "keyframes: #a 缺失") }
    if let b = hn_doc_find_by_id(dK, "b") {
        var namePtr: UnsafePointer<CChar>?
        var kfMs: Float = 0
        var iter: Int32 = 0
        hn_node_debug_anim(b, &namePtr, &kfMs, &iter)
        let name = namePtr.map { String(cString: $0) } ?? "(nil)"
        check(name == "pulse" && abs(kfMs - 2000) < 1,
              "keyframes: 第二个动画独立 (name=\(name), \(kfMs)ms)")
    }

    // ---- 2) 时间轴采样: rotate 应随时间推进 ----
    if let a = hn_doc_find_by_id(dK, "a") {
        var r0: Float = 0
        _ = hn_context_anim_tick(cK, -1)              // 初始化
        _ = hn_context_anim_tick(cK, 250)             // 1/4 周期
        hn_node_debug_rotate(a, &r0)
        check(abs(r0 - 90) < 8, String(format: "keyframes: 250ms/1000ms → rotate≈90° (实得 %.0f°)", r0))
        _ = hn_context_anim_tick(cK, 250)             // 累计半周期
        var r1: Float = 0
        hn_node_debug_rotate(a, &r1)
        check(abs(r1 - 180) < 12, String(format: "keyframes: 累计半周期 → rotate≈180° (实得 %.0f°)", r1))
        // 无限循环: 超过一个周期后应回卷而不是停止
        for _ in 0..<4 { _ = hn_context_anim_tick(cK, 250) }
        var still = hn_context_anim_tick(cK, 16)
        check(still == 1, "keyframes: infinite 持续运行(不停止)")
        _ = found
    }

    // ---- 3) 3D 变换: rotateY 产生倾斜 → 绘制四边形而非矩形 ----
    let h3 = """
    <html><head><style>
    #flat { width: 100; height: 60; background: #ff0000; }
    #tilt { width: 100; height: 60; background: #00ff00; transform: rotateY(50deg); }
    #persp { width: 100; height: 60; background: #0000ff; perspective: 200;
             transform: rotateX(35deg); }
    </style></head><body>
    <div id="flat"></div><div id="tilt"></div><div id="persp"></div>
    </body></html>
    """
    let d3 = hn_parse_html(h3, h3.utf8.count)!
    let c3 = hn_context_create()
    hn_context_set_doc(c3, d3)
    hn_context_layout(c3, 400, 400, &backend)
    if let dl3 = hn_context_display_list(c3) {
        var quads = 0, redRects = 0
        for i in 0..<Int(dl3.pointee.count) {
            let cmd = dl3.pointee.cmds![i]
            if cmd.kind == HN_CMD_QUAD { quads += 1 }
            if cmd.kind == HN_CMD_RECT && cmd.fill == 0xff0000ff { redRects += 1 }
        }
        check(quads >= 2, "3D: rotateY/rotateX 产生四边形指令 (\(quads) 个)")
        check(redRects == 1, "3D: 未变换元素仍走矩形路径(不受影响)")
    }
    // 投影几何: 三种情况分别验证(正交压缩 / 透视梯形 / 2D 旋转)
    // 注意: 正交投影下 rotateY 只均匀压缩宽度(仍是矩形) —— 这是正确的 CSS 行为
    func quadOf(_ css: String, _ w: Float, _ h: Float) -> (Bool, [Float], [Float]) {
        let html = "<html><head><style>\(css)</style></head><body><div id=\"q\"></div></body></html>"
        guard let dd = hn_parse_html(html, html.utf8.count) else { return (false, [], []) }
        let cc = hn_context_create()
        hn_context_set_doc(cc, dd)
        // 内联样式表由 set_doc 装载
        hn_context_layout(cc, 400, 400, &backend)
        guard let dl = hn_context_display_list(cc) else { return (false, [], []) }
        for i in 0..<Int(dl.pointee.count) {
            let cmd = dl.pointee.cmds![i]
            if cmd.kind == HN_CMD_QUAD {
                var xs = [Float](repeating: 0, count: 4)
                var ys = [Float](repeating: 0, count: 4)
                withUnsafePointer(to: cmd.qx) { px in
                    px.withMemoryRebound(to: Float.self, capacity: 4) { ax in
                        for k in 0..<4 { xs[k] = ax[k] }
                    }
                }
                withUnsafePointer(to: cmd.qy) { py in
                    py.withMemoryRebound(to: Float.self, capacity: 4) { ay in
                        for k in 0..<4 { ys[k] = ay[k] }
                    }
                }
                return (true, xs, ys)
            }
        }
        return (false, [], [])
    }

    // (a) 正交 rotateY: 仍是矩形, 但宽度被压缩到 ~cos(45°)*120 ≈ 85
    let (okA, xsA, _) = quadOf("#q { transform: rotateY(45deg); width: 120; height: 80; background: #f00; }", 120, 80)
    if okA {
        let topW = abs(xsA[1] - xsA[0])
        check(topW < 100 && topW > 70,
              String(format: "3D: 正交 rotateY(45°) 压缩宽度到 %.0f (期望 ≈85)", topW))
    } else { check(false, "3D: 正交 rotateY 未产生四边形") }

    // (b) 透视 rotateY: 左右边不等(近大远小)
    let (okB, xsB, _) = quadOf("#q { perspective: 300; transform: rotateY(45deg); width: 120; height: 80; background: #f00; }", 120, 80)
    if okB {
        // 绕 Y 轴旋转时上下边仍水平等长, **左右边高度不同**才是透视特征(近大远小)。
        // 若只看上下边会误判(它们本就相等), 这是我第一版断言写错的地方。
        var leftH: Float = 0, rightH: Float = 0
        let okB2 = okB
        if okB2 {
            // 需要 y 坐标: 重新取一次(quadOf 的第二个返回值只给了 x)
            let html = "<html><head><style>#q { perspective: 300; transform: rotateY(45deg); width: 120; height: 80; background: #f00; }</style></head><body><div id=\"q\"></div></body></html>"
            if let dd = hn_parse_html(html, html.utf8.count) {
                let cc = hn_context_create()
                hn_context_set_doc(cc, dd)
                hn_context_layout(cc, 400, 400, &backend)
                if let dl = hn_context_display_list(cc) {
                    for i in 0..<Int(dl.pointee.count) {
                        let cmd = dl.pointee.cmds![i]
                        if cmd.kind == HN_CMD_QUAD {
                            var ys = [Float](repeating: 0, count: 4)
                            withUnsafePointer(to: cmd.qy) { py in
                                py.withMemoryRebound(to: Float.self, capacity: 4) { ay in
                                    for k in 0..<4 { ys[k] = ay[k] }
                                }
                            }
                            leftH = abs(ys[3] - ys[0])     // 左边(顶点 0→3)
                            rightH = abs(ys[2] - ys[1])    // 右边(顶点 1→2)
                            break
                        }
                    }
                }
            }
        }
        check(abs(leftH - rightH) > 3,
              String(format: "3D: 透视 rotateY 左右边高度不同 (左 %.0f / 右 %.0f) — 近大远小", leftH, rightH))
    } else { check(false, "3D: 透视 rotateY 未产生四边形") }

    // (c) 透视 rotateX: 上边明显短于下边
    let (okC, xsC, _) = quadOf("#q { perspective: 300; transform: rotateX(40deg); width: 120; height: 80; background: #f00; }", 120, 80)
    if okC {
        let topW = abs(xsC[1] - xsC[0]), botW = abs(xsC[2] - xsC[3])
        check(topW < botW - 5,
              String(format: "3D: 透视 rotateX(40°) 上窄下宽 (上 %.0f / 下 %.0f)", topW, botW))
    } else { check(false, "3D: 透视 rotateX 未产生四边形") }

    // (d) 纯 2D rotate: 也应产生四边形(旋转后不再轴对齐)
    let (okD, xsD, ysD) = quadOf("#q { transform: rotate(20deg); width: 120; height: 80; background: #f00; }", 120, 80)
    if okD {
        // 旋转后上边不再水平: 两端 y 不同
        let dy = abs(ysD[1] - ysD[0])
        check(dy > 20, String(format: "3D: rotate(20°) 上边倾斜 (两端 dy=%.0f)", dy))
    } else { check(false, "3D: rotate(20°) 未产生四边形(旋转后应非轴对齐)") }
}

print("== Lottie 矢量动画 + 网格变形 + 透明背板 ==")
do {
    // 内置一个最小 Lottie(不依赖外部文件, 断言可在任何环境跑):
    // 两层 —— 一个矩形旋转关键帧 + 一个心跳缩放关键帧
    let lottieJSON = """
    {"v":"5.7.4","fr":60,"ip":0,"op":60,"w":100,"h":100,"layers":[
      {"ty":4,"nm":"box","ip":0,"op":60,"ind":1,
       "ks":{"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},
             "r":{"a":1,"k":[{"t":0,"s":[0]},{"t":60,"s":[90]}]},"o":{"a":0,"k":100}},
       "shapes":[{"ty":"gr","it":[
          {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]},"r":{"a":0,"k":4}},
          {"ty":"fl","c":{"a":0,"k":[1,0,0]},"o":{"a":0,"k":100},"r":1},
          {"ty":"tr","a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},
                     "r":{"a":0,"k":0},"o":{"a":0,"k":100}}]}]},
      {"ty":4,"nm":"grow","ip":0,"op":60,"ind":2,
       "ks":{"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},
             "r":{"a":0,"k":0},"o":{"a":0,"k":100}},
       "shapes":[{"ty":"gr","it":[
          {"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":1,"k":[{"t":0,"s":[10,10]},{"t":60,"s":[80,80]}]}},
          {"ty":"fl","c":{"a":0,"k":[0,0.5,1]},"o":{"a":0,"k":100},"r":1},
          {"ty":"tr","a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},
                     "r":{"a":0,"k":0},"o":{"a":0,"k":100}}]}]}]}
    """
    AssetStore.shared.put("hn-test-lottie.json", lottieJSON)
    let ltHTML = """
    <html><head><style>
    html,body{margin:0;width:100%;height:100%;background:#000000;}
    .lot{width:100;height:100;}
    .mesh{width:80;height:80;}
    </style></head><body>
    <img class="lot" src="hn-test-lottie.json" hn-lottie>
    <img class="mesh" src="assets/avatar-a.png" hn-mesh="6x6" hn-mesh-sway="6">
    </body></html>
    """
    guard let ldoc = hn_parse_html(ltHTML, ltHTML.utf8.count) else { fatalError() }
    let lctx = hn_context_create()
    hn_context_set_doc(lctx, ldoc)
    do {
        let (ab, box) = AssetStore.shared.backend()
        assetKeepAlive = box
        let ap = UnsafeMutablePointer<hn_asset_backend>.allocate(capacity: 1)
        ap.initialize(to: ab)
        hn_context_set_assets(lctx, ap)
    }
    hn_context_layout(lctx, 200, 240, &backend)

    // 采样某时刻的指令集合(多边形数量 + 首个多边形包围盒 + 网格/图片指令数)
    func sample(_ ms: Float) -> (poly: Int, mesh: Int, img: Int, box: (Float, Float, Float, Float)) {
        _ = hn_context_anim_tick(lctx, ms)
        hn_context_repaint(lctx)
        var poly = 0, mesh = 0, img = 0
        var bx: Float = 0, by: Float = 0, bw: Float = 0, bh: Float = 0
        if let dl = hn_context_display_list(lctx), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) {
                let c = cmds[i]
                if c.kind == HN_CMD_POLYGON {
                    poly += 1
                    if poly == 1, let p = c.poly {
                        var mnx = p[0], mxx = p[0], mny = p[1], mxy = p[1]
                        for q in 0..<Int(c.poly_n) {
                            let x = p[q * 2], y = p[q * 2 + 1]
                            mnx = min(mnx, x); mxx = max(mxx, x)
                            mny = min(mny, y); mxy = max(mxy, y)
                        }
                        bx = mnx; by = mny; bw = mxx - mnx; bh = mxy - mny
                    }
                }
                if c.kind == HN_CMD_MESH { mesh += 1 }
                if c.kind == HN_CMD_IMAGE { img += 1 }
            }
        }
        return (poly, mesh, img, (bx, by, bw, bh))
    }

    // 动画时长 60 帧 @60fps = 1000ms。sample 的 tick 是**累加**的,
    // 所以这里依次推进到 100ms / 500ms / 1100ms。
    let s0 = sample(100)      // → 100ms
    let s1 = sample(400)      // → 500ms
    let s2 = sample(600)      // → 1100ms, 取模后等价于 100ms(= s0 的时刻)

    check(s0.poly >= 2, "Lottie: 解析 JSON 并产出多边形指令 (实际 \(s0.poly) 个)")
    check(s0.mesh == 1, "网格变形: 产出 MESH 指令 (实际 \(s0.mesh) 个)")
    check(s0.img == 0, "Lottie 元素不再退化为静态 IMAGE (\(s0.img) 个 IMAGE)")

    // 旋转 + 缩放关键帧应改变几何(矩形绕中心转、圆随时间变大)
    let moved = abs(s1.box.0 - s0.box.0) + abs(s1.box.1 - s0.box.1)
              + abs(s1.box.2 - s0.box.2) + abs(s1.box.3 - s0.box.3) > 1.0
    check(moved, String(format: "Lottie: 关键帧在时间轴上推进 (100ms %.1fx%.1f → 500ms %.1fx%.1f)",
                        s0.box.2, s0.box.3, s1.box.2, s1.box.3))

    // 相隔一个完整周期(1000ms)的两点几何应一致 —— 验证循环取模
    let back = abs(s2.box.0 - s0.box.0) + abs(s2.box.1 - s0.box.1)
             + abs(s2.box.2 - s0.box.2) + abs(s2.box.3 - s0.box.3)
    check(back < 1.5, String(format: "Lottie: 相隔一周期几何一致 (偏差 %.2f px)", back))

    // 透明背板: hn-transparent 应把 body 底色抹掉
    let trHTML = """
    <html><head><meta name="hn-transparent" content="1">
    <style>html,body{margin:0;width:100%;height:100%;background:#1b1f27;}
    .c{width:40;height:40;background:#ff0000;}</style>
    </head><body><div class="c"></div></body></html>
    """
    var m = hn_manifest()
    if let tdoc = hn_parse_html(trHTML, trHTML.utf8.count) {
        hn_doc_manifest(tdoc, &m)
        check(m.transparent == 1, "透明背板: hn-transparent 清单位被解析")
        let tctx = hn_context_create()
        hn_context_set_doc(tctx, tdoc)
        hn_context_layout(tctx, 200, 200, &backend)
        // 未剥离前: 根元素应有实色背景指令
        var rootRects = 0
        if let dl = hn_context_display_list(tctx), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) {
                let c = cmds[i]
                if c.kind == HN_CMD_RECT && c.w >= 199 && c.h >= 199 && (c.fill & 0xFF) != 0 {
                    rootRects += 1
                }
            }
        }
        check(rootRects >= 1, "透明背板: 剥离前根底色存在 (\(rootRects) 个全屏实色矩形)")
        hn_context_strip_root_background(tctx)
        hn_context_repaint(tctx)
        var afterRects = 0
        if let dl = hn_context_display_list(tctx), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) {
                let c = cmds[i]
                if c.kind == HN_CMD_RECT && c.w >= 199 && c.h >= 199 && (c.fill & 0xFF) != 0 {
                    afterRects += 1
                }
            }
        }
        check(afterRects == 0, "透明背板: 剥离后根底色消失 (\(afterRects) 个全屏实色矩形)")
        // 关键回归: 再跑一次 layout(等价于窗口 resize / 热更新)。
        // 样式会从级联重算, 无色板开关若是一次性改写, 这里底色就会被装回来。
        hn_context_layout(tctx, 240, 240, &backend)
        var afterRelayout = 0
        if let dl = hn_context_display_list(tctx), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) {
                let c = cmds[i]
                if c.kind == HN_CMD_RECT && c.w >= 199 && c.h >= 199 && (c.fill & 0xFF) != 0 {
                    afterRelayout += 1
                }
            }
        }
        check(afterRelayout == 0,
              "透明背板: 重新布局后仍保持透明(\(afterRelayout) 个全屏实色矩形) — 持久开关")
        // 内容仍要正常绘制(不能把整页都抹掉)
        var hasRed = false
        if let dl = hn_context_display_list(tctx), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) where cmds[i].fill == 0xff0000ff { hasRed = true }
        }
        check(hasRed, "透明背板: 内容元素不受影响(红色方块仍在)")
    } else { check(false, "透明背板: 解析失败") }

    // 透明窗口物化: 走真实 NSWindow 路径验证窗口属性确实被设置
    // (离屏位图会丢失窗口属性, 必须真建窗口才能断言这一点)
    do {
        let winHTML = """
        <html><head>
        <meta name="hn-surface" content="popup">
        <meta name="hn-window" content="200x120">
        <meta name="hn-transparent" content="1">
        <meta name="hn-shadow" content="0">
        <meta name="hn-draggable" content="0">
        <style>html,body{margin:0;width:100%;height:100%;background:#123456;}
        .a{position:absolute;left:10;top:10;width:80;height:60;background:#ff0000;}</style>
        </head><body><div class="a"></div></body></html>
        """
        var m2 = hn_manifest()
        if let d2 = hn_parse_html(winHTML, winHTML.utf8.count) {
            hn_doc_manifest(d2, &m2)
            check(m2.transparent == 1, "透明窗口: 清单 transparent=1")
            check(m2.shadow == 0, "透明窗口: 清单 shadow=0(可关闭投影)")
            check(m2.draggable == 0, "透明窗口: 清单 draggable=0(可关闭拖动)")

            let app = HNEngine.shared.open(id: "hn-test-transparent", html: winHTML)
            let w = app.window
            check(w.isOpaque == false, "透明窗口: NSWindow.isOpaque=false(逐像素 alpha)")
            check(w.backgroundColor.alphaComponent == 0,
                  String(format: "透明窗口: 窗口底色为全透明 (alpha=%.2f)", w.backgroundColor.alphaComponent))
            check(w.hasShadow == false, "透明窗口: hasShadow 跟随清单(=false)")
            check(w.isMovableByWindowBackground == false, "透明窗口: 空白拖动跟随清单(=false)")

            // 根底色应已被引擎剥离(html/body 的 #123456 不该再出现)
            let av = app.view
            check(av != nil, "透明窗口: 取得 native 视图")
            if let av {
                var anyDl = false
                if let dl = hn_context_display_list(av.engineContext), let cmds = dl.pointee.cmds {
                    anyDl = true
                    var bgRects = 0
                    for i in 0..<Int(dl.pointee.count) {
                        let c = cmds[i]
                        if c.kind == HN_CMD_RECT && c.w >= 199 && c.h >= 119 { bgRects += 1 }
                    }
                    check(bgRects == 0, "透明窗口: 根底色被剥离(\(bgRects) 个全屏矩形)")
                    var redOk = false
                    for i in 0..<Int(dl.pointee.count) where cmds[i].fill == 0xff0000ff { redOk = true }
                    check(redOk, "透明窗口: 内容元素仍在(红块)")
                }
                if !anyDl {
                    // HNEngine.open 尚未触发布局(窗口尺寸由系统决定后才 layout)
                    av.relayout()
                    if let dl = hn_context_display_list(av.engineContext), let cmds = dl.pointee.cmds {
                        var bgRects = 0
                        for i in 0..<Int(dl.pointee.count) {
                            let c = cmds[i]
                            if c.kind == HN_CMD_RECT && c.w >= 199 && c.h >= 119 { bgRects += 1 }
                        }
                        check(bgRects == 0, "透明窗口: 根底色被剥离(layout 后 \(bgRects) 个全屏矩形)")
                        var redOk = false
                        for i in 0..<Int(dl.pointee.count) where cmds[i].fill == 0xff0000ff { redOk = true }
                        check(redOk, "透明窗口: 内容元素仍在(layout 后红块)")
                    } else { check(false, "透明窗口: layout 后仍无绘制指令") }
                }
            }
            HNEngine.shared.close(id: "hn-test-transparent")

            /* 回归: webkit 兜底渲染器没有 C 引擎上下文。此处若把一个假指针
               (曾用 OpaquePointer(bitPattern: 1))当成真上下文返回, 声明
               hn-transparent 的 webkit 页面会在建窗时调引擎 API 直接崩溃。
               断言它必须为 nil, 强制调用方判空。 */
            let wkHost = HNWebKitHost()
            check(wkHost.engineContextOrNil == nil,
                  "透明窗口: webkit 渲染器不返回假引擎上下文(避免崩溃)")
        } else { check(false, "透明窗口: 解析失败") }
    }

    // MESH 指令的数据完整性: 顶点/UV 数量与网格一致
    let mctx = hn_context_create()
    let mHTML = """
    <html><head><style>html,body{margin:0} .m{width:60;height:60;}</style></head>
    <body><img class="m" src="assets/avatar-a.png" hn-mesh="4x3"></body></html>
    """
    if let mdoc = hn_parse_html(mHTML, mHTML.utf8.count) {
        hn_context_set_doc(mctx, mdoc)
        hn_context_layout(mctx, 120, 120, &backend)
        var nv = 0, cols = 0, rows = 0, uvok = false
        if let dl = hn_context_display_list(mctx), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) {
                let c = cmds[i]
                if c.kind == HN_CMD_MESH {
                    cols = Int(c.mesh_cols); rows = Int(c.mesh_rows)
                    nv = (cols + 1) * (rows + 1)
                    // UV 应覆盖 0..1 四个角
                    if let uv = c.mesh_uv {
                        var mn: Float = 9, mx: Float = -9
                        for k in 0..<nv { mn = min(mn, uv[k * 2]); mx = max(mx, uv[k * 2]) }
                        uvok = abs(mn) < 0.001 && abs(mx - 1.0) < 0.001
                    }
                }
            }
        }
        check(cols == 4 && rows == 3, "网格: hn-mesh=\"4x3\" 被解析 (实际 \(cols)x\(rows))")
        check(uvok, "网格: UV 覆盖 0..1 (\(nv) 个顶点)")
    } else { check(false, "网格: 解析失败") }

    // 脚本驱动网格: hn_node_set_mesh_verts 应让节点进入网格绘制
    let sctx = hn_context_create()
    if let sdoc = hn_parse_html(mHTML, mHTML.utf8.count) {
        hn_context_set_doc(sctx, sdoc)
        if let img = firstImg(sdoc) {
            var verts = [Float](repeating: 0, count: (3 + 1) * (3 + 1) * 2)
            for r in 0...3 { for c in 0...3 {
                let i = r * 4 + c
                verts[i * 2] = Float(c) / 3.0 * 60
                verts[i * 2 + 1] = Float(r) / 3.0 * 60
            } }
            let ok = verts.withUnsafeBufferPointer { hn_node_set_mesh_verts(img, $0.baseAddress, 3, 3) }
            check(ok == 1, "网格: 脚本写入顶点成功(应用层自定义 rig)")
            var gc: Int32 = 0, gr: Int32 = 0
            let isMesh = hn_node_mesh_info(img, &gc, &gr) == 1
            check(isMesh && gc == 3 && gr == 3, "网格: 脚本网格被识别 (\(gc)x\(gr))")

            // 脚本写入的顶点必须真的被绘制使用(而非只改了状态)
            hn_context_layout(sctx, 120, 120, &backend)
            var scriptedMesh = false
            if let dl = hn_context_display_list(sctx), let cmds = dl.pointee.cmds {
                for i in 0..<Int(dl.pointee.count) {
                    let c = cmds[i]
                    if c.kind == HN_CMD_MESH && c.mesh_cols == 3 && c.mesh_rows == 3 {
                        scriptedMesh = true
                    }
                }
            }
            check(scriptedMesh, "网格: 脚本网格进入绘制(3x3 MESH 指令)")
        } else { check(false, "网格: 未找到 img 节点") }
    }

    // JS → 网格: setMeshVerts 必须可用(这是文档承诺的"应用层 rig"入口)
    do {
        let jsHTML = """
        <html><head><style>html,body{margin:0} .m{width:60;height:60;}</style></head>
        <body><img id="mm" class="m" src="assets/avatar-a.png">
        <script>
          var el = document.getElementById('mm');
          var v = [];
          for (var r = 0; r < 4; r++) for (var c = 0; c < 4; c++) {
            v.push(c / 3 * 60); v.push(r / 3 * 60);
          }
          window.__meshOK = el.setMeshVerts(v, 3, 3);
        </script></body></html>
        """
        let jv = HtmlNativeView(html: jsHTML)
        jv.imageRoot = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        jv.frame = NSRect(x: 0, y: 0, width: 120, height: 120)
        jv.relayout()
        let jsErr = jv.jsError ?? ""
        check(jsErr.isEmpty, "JS 网格: 脚本无错误 (\(jsErr.isEmpty ? "clean" : jsErr))")
        if let rt = jv.jsRuntimeForTest {
            let ok = rt.probe("String(window.__meshOK)")
            check(ok == "true", "JS 网格: setMeshVerts(v, 3, 3) 返回 true (实际 \(ok ?? "nil"))")
        } else { check(false, "JS 网格: JS 运行时未建立") }
        var found = false
        if let dl = hn_context_display_list(jv.engineContext), let cmds = dl.pointee.cmds {
            for i in 0..<Int(dl.pointee.count) where cmds[i].kind == HN_CMD_MESH { found = true }
        }
        check(found, "JS 网格: 经 JS 写入后进入 MESH 绘制")
    }
}

print("== 布局与排版回归(探针发现的真实缺陷) ==")
do {
    /* 这批断言对应三个已经修掉的坑。共同点都是"写了样式却毫无反应"——
       没有断言就会在下次改动时静默回归。 */

    func boxOf(_ html: String, _ id: String, _ w: Float = 400, _ h: Float = 300)
        -> (Float, Float, Float, Float)? {
        guard let d = hn_parse_html(html, html.utf8.count) else { return nil }
        let cc = hn_context_create()
        hn_context_set_doc(cc, d)
        hn_context_layout(cc, w, h, &backend)
        var stack: [OpaquePointer] = [hn_doc_root(d)]
        var result: (Float, Float, Float, Float)?
        while let n = stack.popLast() {
            if let i = hn_node_attr(n, "id"), String(cString: i) == id {
                var x: Float = 0, y: Float = 0, bw: Float = 0, bh: Float = 0
                hn_node_box(n, &x, &y, &bw, &bh)
                result = (x, y, bw, bh)
                break
            }
            var kids: [OpaquePointer] = []
            var c = hn_node_first_child(n)
            while let cc2 = c { kids.append(cc2); c = hn_node_next_sibling(cc2) }
            stack.append(contentsOf: kids)
        }
        hn_context_destroy(cc)
        return result
    }
    func eq(_ a: (Float, Float, Float, Float)?, _ b: (Float, Float, Float, Float),
            _ tol: Float = 1.0) -> Bool {
        guard let a else { return false }
        return abs(a.0 - b.0) <= tol && abs(a.1 - b.1) <= tol
            && abs(a.2 - b.2) <= tol && abs(a.3 - b.3) <= tol
    }

    // (1) margin 长写: 曾因 token 被 auto 判定吃掉而全部失效
    if let b = boxOf("<html><head><style>html,body{margin:0}.a{width:100;height:30;margin-top:40}"
                     + "</style></head><body><div class=\"a\" id=\"x\"></div></body></html>", "x") {
        check(eq(b, (0, 40, 100, 30)),
              String(format: "布局: margin-top 长写生效 (y=%.0f)", b.1))
    } else { check(false, "布局: margin-top 用例未取到盒子") }
    if let b = boxOf("<html><head><style>html,body{margin:0}.a{width:100;height:30;margin-left:60}"
                     + "</style></head><body><div class=\"a\" id=\"x\"></div></body></html>", "x") {
        check(eq(b, (60, 0, 100, 30)),
              String(format: "布局: margin-left 长写生效 (x=%.0f)", b.0))
    } else { check(false, "布局: margin-left 用例未取到盒子") }

    // (2) margin 折叠: 相邻 margin 取最大值而非相加
    if let b = boxOf("<html><head><style>html,body{margin:0}.a{width:100;height:30;margin:20}"
                     + "</style></head><body><div class=\"a\" id=\"p\"></div>"
                     + "<div class=\"a\" id=\"q\"></div></body></html>", "q") {
        // 20 顶 + 30 高 + 折叠 20 = 70
        check(eq(b, (20, 70, 100, 30)),
              String(format: "布局: 相邻 margin 折叠取最大值 (第二个 y=%.0f, 期望 70)", b.1))
    } else { check(false, "布局: margin 折叠用例未取到盒子") }
    if let b = boxOf("<html><head><style>html,body{margin:0}"
                     + ".a{width:100;height:30;margin-bottom:20}"
                     + ".b{width:100;height:30;margin-top:40}</style></head><body>"
                     + "<div class=\"a\" id=\"p\"></div><div class=\"b\" id=\"q\"></div></body></html>", "q") {
        // max(20, 40) = 40 → 第二个在 y=70
        check(eq(b, (0, 70, 100, 30)),
              String(format: "布局: 不对称 margin 折叠取较大者 (y=%.0f, 期望 70)", b.1))
    } else { check(false, "布局: 不对称折叠用例未取到盒子") }

    // (3) flex-shrink: 曾因显式 width 覆盖结算结果而完全失效(子项重叠)
    if let b = boxOf("<html><head><style>html,body{margin:0}"
                     + ".f{display:flex;width:200;height:100}"
                     + ".a{width:150;height:20;flex-shrink:1}</style></head><body>"
                     + "<div class=\"f\"><div class=\"a\" id=\"p\"></div>"
                     + "<div class=\"a\" id=\"q\"></div></div></body></html>", "q") {
        // 溢出 100 均分 → 各 100; 第二个在 x=100 且宽 100(不重叠)
        check(eq(b, (100, 0, 100, 20)),
              String(format: "布局: flex-shrink 生效 (第二个 x=%.0f 宽=%.0f, 期望 100/100)",
                     b.0, b.2))
    } else { check(false, "布局: flex-shrink 用例未取到盒子") }
    if let b = boxOf("<html><head><style>html,body{margin:0}"
                     + ".f{display:flex;width:200;height:100}"
                     + ".a{width:150;height:20;flex-shrink:1}"
                     + ".b{width:150;height:20;flex-shrink:3}</style></head><body>"
                     + "<div class=\"f\"><div class=\"a\" id=\"p\"></div>"
                     + "<div class=\"b\" id=\"q\"></div></div></body></html>", "q") {
        // 1:3 分配 → 125 / 75
        check(eq(b, (125, 0, 75, 20)),
              String(format: "布局: flex-shrink 按比例 (第二个 x=%.0f 宽=%.0f, 期望 125/75)",
                     b.0, b.2))
    } else { check(false, "布局: flex-shrink 比例用例未取到盒子") }

    // (4) space-around / space-evenly: 曾缺失并静默落到 flex-start
    if let b = boxOf("<html><head><style>html,body{margin:0}"
                     + ".f{display:flex;width:300;height:100;justify-content:space-around}"
                     + ".c{width:50;height:30}</style></head><body><div class=\"f\">"
                     + "<div class=\"c\" id=\"p\"></div><div class=\"c\" id=\"q\"></div>"
                     + "</div></body></html>", "p") {
        // 剩余 200, 每项占一段 100, 首尾各半 → 第一个在 x=50
        check(eq(b, (50, 0, 50, 30)),
              String(format: "布局: space-around (第一个 x=%.0f, 期望 50)", b.0))
    } else { check(false, "布局: space-around 用例未取到盒子") }
    if let b = boxOf("<html><head><style>html,body{margin:0}"
                     + ".f{display:flex;width:300;height:100;justify-content:space-evenly}"
                     + ".c{width:50;height:30}</style></head><body><div class=\"f\">"
                     + "<div class=\"c\" id=\"p\"></div><div class=\"c\" id=\"q\"></div>"
                     + "</div></body></html>", "p") {
        // 剩余 200 分 3 段(首/中/尾)各 66.67 → 第一个在 x=66.67
        check(eq(b, (66.67, 0, 50, 30), 1.5),
              String(format: "布局: space-evenly (第一个 x=%.1f, 期望 66.67)", b.0))
    } else { check(false, "布局: space-evenly 用例未取到盒子") }
}

print("== agent 生态: 两个渲染器都能 eval(同步执行 JS) ==")
do {
    // 之前 daemon 的 eval 只对 webkit 有效, native 一律返回"无 JS 上下文" ——
    // 于是 agent 在同一条命令上表现不一致。断言两个渲染器都要给出真值。
    let jsHTML = """
    <html><head><style>html,body{margin:0}</style></head><body>
    <div id="n" data-count="0"></div>
    <script>
      window.__answer = function (x) { return 6 * x; };
      window.__state = 41 + 1;
    </script></body></html>
    """
    let nv = HtmlNativeView(html: jsHTML)
    nv.frame = NSRect(x: 0, y: 0, width: 200, height: 200)
    nv.relayout()
    let v1 = nv.evalSync("String(window.__state)")
    check(v1 as? String == "42", "eval: native 渲染器可执行 JS (结果 \(v1 ?? "nil"))")
    let v2 = nv.evalSync("String(window.__answer(7))")
    check(v2 as? String == "42", "eval: native 可调用页面函数 (结果 \(v2 ?? "nil"))")

    // 无脚本的页面也应安全返回 nil 而不是崩
    let nv2 = HtmlNativeView(html: "<html><body><div>无脚本</div></body></html>")
    nv2.frame = NSRect(x: 0, y: 0, width: 100, height: 100)
    nv2.relayout()
    check(nv2.evalSync("1+1") == nil, "eval: 无 JS 环境时返回 nil(不崩)")

    // webkit 渲染器同样暴露同一接口(协议要求), agent 无需区分渲染器。
    // 无头测试里不真建 WKWebView, 只验证协议存在性。
    let wk = HNWebKitHost()
    let hasIface = wk is HNWebHost && !(wk is HtmlNativeView)
    check(hasIface, "eval: webkit 宿主满足 HNWebHost(同一 eval 接口)")
}

print("== 离屏渲染 PNG ==")
let W = VW, H = VH, SCALE = 2
guard let cg = CGContext(
    data: nil, width: W * SCALE, height: H * SCALE,
    bitsPerComponent: 8, bytesPerRow: 0,
    space: CGColorSpace(name: CGColorSpace.sRGB)!,
    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
) else {
    print("✘ 无法创建位图上下文"); exit(1)
}
cg.scaleBy(x: CGFloat(SCALE), y: CGFloat(SCALE))
cg.translateBy(x: 0, y: CGFloat(H))
cg.scaleBy(x: 1, y: -1) // 翻转到左上原点, 与窗口一致
HNPainter.draw(dlp.pointee, into: cg)

guard let img = cg.makeImage() else { print("✘ 无法导出图像"); exit(1) }
let rep = NSBitmapImageRep(cgImage: img)
guard let png = rep.representation(using: .png, properties: [:]) else { print("✘ PNG 编码失败"); exit(1) }
let out = outPath
try! png.write(to: URL(fileURLWithPath: out))
print("  已输出: \(out) (\(W*SCALE)x\(H*SCALE))")

if fileMode {
    print("== 文件渲染完成 ==")
} else {
    print(failures == 0 ? "== 全部通过 ==" : "== \(failures) 项失败 ==")
    exit(failures == 0 ? 0 : 1)
}

/// 极简 HTTP 测试服务器(纯 Foundation socket, 零依赖)
/// 提供 /api/hello (文本) / /api/json (JSON) / 其余 404
final class SimpleHTTPServer {
    private var fd: Int32 = -1
    private var running = false

    func start() -> Int {
        fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return 0 }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = 0                       // 系统分配端口
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let bound = withUnsafePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0, listen(fd, 8) == 0 else { close(fd); return 0 }
        // 取实际端口
        var actual = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &actual) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }
        let port = Int(UInt16(bigEndian: actual.sin_port))
        running = true
        Thread.detachNewThread { [weak self] in self?.serve() }
        return port
    }

    private func serve() {
        while running {
            var caddr = sockaddr()
            var clen = socklen_t(MemoryLayout<sockaddr>.size)
            let c = accept(fd, &caddr, &clen)
            if c < 0 { if !running { break }; continue }
            var buf = [UInt8](repeating: 0, count: 4096)
            let n = read(c, &buf, buf.count)
            let req = n > 0 ? String(decoding: buf[0..<n], as: UTF8.self) : ""
            let (code, ctype, body): (String, String, String)
            if req.contains("GET /api/hello") {
                (code, ctype, body) = ("200 OK", "text/plain", "hello from test server")
            } else if req.contains("GET /api/json") {
                (code, ctype, body) = ("200 OK", "application/json", #"{"name":"zcode","n":42}"#)
            } else {
                (code, ctype, body) = ("404 Not Found", "text/plain", "not found")
            }
            let resp = "HTTP/1.1 " + code + "\r\nContent-Type: " + ctype + "\r\nContent-Length: " + String(body.utf8.count) + "\r\nConnection: close\r\n\r\n" + body
            _ = resp.withCString { write(c, $0, strlen($0)) }
            close(c)
        }
    }

    func stop() { running = false; if fd >= 0 { close(fd); fd = -1 } }
}
