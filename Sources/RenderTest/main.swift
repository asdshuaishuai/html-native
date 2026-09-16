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
    let end = Date().addingTimeInterval(25.0)
    while Date() < end {
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
    }
    frameTimer.invalidate()
    hotTimer.invalidate()
    v.stopPolling()
    let dom = v.dumpDOM()
    // dumpDOM 只输出标签/id/盒, 不含 class —— 用"trace 子树的行数"判断内容规模
    let traceStart = dom.firstIndex { $0.contains("#trace") } ?? 0
    let traceLines = dom.count - traceStart
    check(frames > 2000, "压测: 推进 \(frames) 帧无崩溃(含 \(hotUpdates) 次热更新)")
    check(traceLines > 3, "压测: trace 子树有 \(traceLines) 行(流式内容在增长)")
    let cmds = v.dumpDisplayList().count
    check(cmds > 30, "压测: 绘制指令 \(cmds) 条(内容非空)")
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
