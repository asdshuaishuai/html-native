import AppKit
import Foundation
import CHtmlNative
import HtmlNative

// HnShot — 真实 UI 截图(视图自绘, 非屏幕捕获, 无需任何权限):
//   HnShot file.html file.css out.png [W H] [--hover id] [--scroll id,dy] [--css2 f]
// --hover  : 注入 :hover 状态(展示交互效果)
// --scroll : 滚动指定 overflow 容器(展示滚动裁剪)
// 图片相对路径默认按进程工作目录解析。

let args = CommandLine.arguments
// 两种形式: HnShot file.html file.css out.png … / HnShot file.html out.png …(css 省略)
guard args.count >= 3 else {
    print("用法: HnShot file.html [file.css] out.png [W H] [--hover id] [--scroll id,dy] [--live]")
    exit(2)
}
var htmlPath = args[1]
var cssPath: String?
var outPath: String
var firstOpt: Int
if args.count >= 4 && !args[2].hasPrefix("-") && (args[2] as NSString).pathExtension.lowercased() != "png" {
    cssPath = args[2]
    outPath = args[3]
    firstOpt = 4
} else {
    outPath = args[2]
    firstOpt = 3
}
var waitSecs = 3.0
var hoverId: String?
var liveMode = false
var scrollId: String?
var scrollDy: Float = 120
var W = 860.0, H = 580.0
var i = firstOpt
while i < args.count {
    switch args[i] {
    case "--hover": i += 1; hoverId = i < args.count ? args[i] : nil
    case "--live": liveMode = true
    case "--wait": i += 1; if i < args.count { waitSecs = Double(args[i]) ?? waitSecs }
    case "--scroll":
        i += 1
        if i < args.count, let sc = args[i].range(of: ",") {
            scrollId = String(args[i][..<sc.lowerBound])
            scrollDy = Float(args[i][sc.upperBound...]) ?? 120
        }
    default:
        if i == firstOpt { W = Double(args[i]) ?? W }
        if i == firstOpt + 1 { H = Double(args[i]) ?? H }
    }
    i += 1
}

guard let htmlRaw = try? String(contentsOfFile: htmlPath, encoding: .utf8) else {
    print("读取失败: \(htmlPath)"); exit(1)
}
let prepared = IncludeExpander.prepare(
    html: htmlRaw,
    css: cssPath.flatMap { try? String(contentsOfFile: $0, encoding: .utf8) },
    baseDir: URL(fileURLWithPath: htmlPath).deletingLastPathComponent())
let html = prepared.html
let css = prepared.css

_ = NSApplication.shared // AppKit 基础设施(cacheDisplay 需要)

// 离屏创建真实视图(不上屏)
let doc = hn_parse_html(html, html.utf8.count)!
let view = HtmlNativeView(doc: doc, css: css)
view.setFrameSize(NSSize(width: W, height: H))
view.layout()

// live 模式: 执行页面的 load/轮询拉取(sys:// 等), 等待片段换入
let ectx = view.engineContext
if liveMode {
    for action in view.loadActions() { view.performHx(action) }
    view.startPolling()
    // 跑主 RunLoop 等待异步片段到达(不能 Thread.sleep, 会堵住主队列);
    // 3s 覆盖 sys:// 双采样阻塞与全部 load 片段换入
    let deadline = Date().addingTimeInterval(waitSecs)
    while Date() < deadline {
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
    }
}
if let hid = hoverId, let node = hn_doc_find_by_id(hn_context_doc(ectx), hid) {
    hn_context_set_hover(ectx, node)
    view.relayout()
}
if let sid = scrollId, let node = hn_doc_find_by_id(hn_context_doc(ectx), sid) {
    if hn_node_scroll_by(node, 0, scrollDy) == 1 {
        hn_context_repaint(ectx)
    }
}

// 调试: HN_DUMP=1 时转储绘制指令
if ProcessInfo.processInfo.environment["HN_DUMP"] != nil {
    var dl = hn_display_list()
    if let ptr = hn_context_display_list(ectx) { dl = ptr.pointee }
    FileHandle.standardError.write("cmds=\(dl.count)\n".data(using: .utf8)!)
    for i in 0..<Int(dl.count) {
        let c = dl.cmds![i]
        if c.kind == HN_CMD_TEXT {
            let s = String(decoding: UnsafeBufferPointer(start: UnsafeRawPointer(c.text!).assumingMemoryBound(to: UInt8.self), count: Int(c.text_len)), as: UTF8.self)
            FileHandle.standardError.write("  TEXT '\(s)' x=\(c.tx) y=\(c.baseline)\n".data(using: .utf8)!)
        } else if c.kind == HN_CMD_RECT {
            FileHandle.standardError.write("  RECT (\(c.x),\(c.y) \(c.w)x\(c.h))\n".data(using: .utf8)!)
        }
    }
}

// 2x 像素密度捕获
let pw = Int(W) * 2
let ph = Int(H) * 2
let rep = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: pw,
    pixelsHigh: ph,
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: NSColorSpaceName.deviceRGB,
    bitmapFormat: NSBitmapImageRep.Format.alphaFirst,
    bytesPerRow: 0,
    bitsPerPixel: 32
)!
rep.size = NSSize(width: W, height: H)
view.cacheDisplay(in: view.bounds, to: rep)

guard let png = rep.representation(using: NSBitmapImageRep.FileType.png, properties: [:]) else {
    print("PNG 编码失败"); exit(1)
}
try! png.write(to: URL(fileURLWithPath: outPath))
print("已输出: \(outPath) (\(Int(W))x\(Int(H)) @2x)")
