import AppKit
import CoreText
import Foundation
import CHtmlNative

/// 绘制指令列表 → CoreGraphics。
/// 同一份绘制器用于窗口(NSView.draw)与离屏位图(RenderTest/HnShot), 保证测试即所见。
public enum HNPainter {
    public static func draw(_ dl: hn_display_list, into cg: CGContext) {
        // 翻转坐标系(左上原点)里让文字正立: 基线定位用 CTLine
        cg.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
        guard let cmds = dl.cmds, dl.count > 0 else { return }
        for i in 0..<Int(dl.count) {
            let cmd = cmds[i]
            if cmd.kind == HN_CMD_RECT {
                drawRect(cmd, into: cg)
            } else if cmd.kind == HN_CMD_TEXT {
                drawText(cmd, into: cg)
            } else if cmd.kind == HN_CMD_IMAGE {
                drawImage(cmd, into: cg)
            } else if cmd.kind == HN_CMD_QUAD {
                drawQuad(cmd, into: cg)
            } else if cmd.kind == HN_CMD_CLIP_PUSH {
                cg.saveGState()
                let r = CGFloat(max(0, cmd.radius))
                cg.addPath(CGPath(roundedRect: rect(cmd), cornerWidth: r, cornerHeight: r, transform: nil))
                cg.clip()
            } else if cmd.kind == HN_CMD_CLIP_POP {
                cg.restoreGState()
            }
        }
    }

    // MARK: - 各指令

    static func rect(_ cmd: hn_cmd) -> CGRect {
        CGRect(x: CGFloat(cmd.x), y: CGFloat(cmd.y), width: CGFloat(cmd.w), height: CGFloat(cmd.h))
    }

    static func drawRect(_ cmd: hn_cmd, into cg: CGContext) {
        let rc = rect(cmd)
        let r = CGFloat(max(0, cmd.radius))
        let path = CGPath(roundedRect: rc, cornerWidth: r, cornerHeight: r, transform: nil)
        let hasFill = cmd.gradient != 0 || (cmd.fill & 0xFF) != 0
        let hasBorder = cmd.stroke_w > 0 && (cmd.stroke & 0xFF) != 0
        if !hasFill && !hasBorder { return }

        cg.saveGState()
        if cmd.shadow != 0 {
            // CG 阴影偏移在我们翻转后的上下文中 y 取反
            cg.setShadow(offset: CGSize(width: CGFloat(cmd.shadow_ox), height: -CGFloat(cmd.shadow_oy)),
                         blur: CGFloat(cmd.shadow_blur),
                         color: color(cmd.shadow_color).cgColor)
        }
        if hasFill {
            if cmd.gradient != 0 {
                let rad = CGFloat(cmd.grad_angle) * .pi / 180
                let dir = CGPoint(x: sin(rad), y: -cos(rad)) // 0deg=向上, 90deg=向右
                let diag = hypot(rc.width, rc.height) / 2
                let c = CGPoint(x: rc.midX, y: rc.midY)
                let start = CGPoint(x: c.x - dir.x * diag, y: c.y - dir.y * diag)
                let end = CGPoint(x: c.x + dir.x * diag, y: c.y + dir.y * diag)
                var from = color(cmd.grad_from)
                var to = color(cmd.grad_to)
                if let g = gradient(from: &from, to: &to) {
                    cg.addPath(path)
                    cg.saveGState()
                    cg.clip()
                    cg.drawLinearGradient(g, start: start, end: end, options: [])
                    cg.restoreGState()
                }
            } else {
                cg.setFillColor(color(cmd.fill).cgColor)
                cg.addPath(path)
                cg.fillPath()
            }
        }
        cg.restoreGState()

        if hasBorder {
            let inset = CGFloat(cmd.stroke_w) / 2
            cg.setStrokeColor(color(cmd.stroke).cgColor)
            cg.setLineWidth(CGFloat(cmd.stroke_w))
            cg.addPath(CGPath(
                roundedRect: rc.insetBy(dx: inset, dy: inset),
                cornerWidth: max(0, r - inset), cornerHeight: max(0, r - inset),
                transform: nil))
            cg.strokePath()
        }
    }

    static func drawText(_ cmd: hn_cmd, into cg: CGContext) {
        guard let tp = cmd.text, cmd.text_len > 0 else { return }
        let s = String(decoding: UnsafeBufferPointer(
            start: UnsafeRawPointer(tp).assumingMemoryBound(to: UInt8.self),
            count: Int(cmd.text_len)), as: UTF8.self)
        guard !s.isEmpty else { return }
        let f = cmd.font
        let attr: [NSAttributedString.Key: Any] = [
            .font: TextShaper.shared.nsfont(f),
            .kern: CGFloat(f.letter_spacing),
            .foregroundColor: color(cmd.fill),
        ]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attr))
        cg.textPosition = CGPoint(x: CGFloat(cmd.tx), y: CGFloat(cmd.baseline))
        CTLineDraw(line, cg)
    }

    /// 四边形填充(3D 投影后的面片): 逐顶点建路径后填充。
    /// 抗锯齿由 CGContext 负责; 顶点顺序为顺时针, 无需额外排序。
    static func drawQuad(_ cmd: hn_cmd, into cg: CGContext) {
        /* C 的 float qx[4] 在 Swift 里导入为元组, 用 withUnsafePointer 取连续内存 */
        var pts: [CGPoint] = []
        withUnsafePointer(to: cmd.qx) { px in
            withUnsafePointer(to: cmd.qy) { py in
                px.withMemoryRebound(to: Float.self, capacity: 4) { ax in
                    py.withMemoryRebound(to: Float.self, capacity: 4) { ay in
                        for i in 0..<4 {
                            pts.append(CGPoint(x: CGFloat(ax[i]), y: CGFloat(ay[i])))
                        }
                    }
                }
            }
        }
        cg.saveGState()
        cg.beginPath()
        cg.move(to: pts[0])
        for i in 1..<4 { cg.addLine(to: pts[i]) }
        cg.closePath()
        cg.setFillColor(color(cmd.fill).cgColor)
        cg.fillPath()
        cg.restoreGState()
    }

    static func drawImage(_ cmd: hn_cmd, into cg: CGContext) {
        guard let tp = cmd.text, cmd.text_len > 0 else { return }
        let src = String(decoding: UnsafeBufferPointer(
            start: UnsafeRawPointer(tp).assumingMemoryBound(to: UInt8.self),
            count: Int(cmd.text_len)), as: UTF8.self)
        guard let img = ImageStore.shared.image(src) else { return }
        let rc = rect(cmd)
        let r = CGFloat(max(0, cmd.radius))

        cg.saveGState()
        if r > 0 {
            cg.addPath(CGPath(roundedRect: rc, cornerWidth: r, cornerHeight: r, transform: nil))
            cg.clip()
        }
        // NSImage 绘制走 AppKit 上下文, 需要包一层并告知翻转
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: cg, flipped: true)
        img.draw(in: rc,
                 from: .zero,
                 operation: .sourceOver,
                 fraction: 1,
                 respectFlipped: true,
                 hints: [.interpolation: NSImageInterpolation.high])
        NSGraphicsContext.restoreGraphicsState()
        cg.restoreGState()
    }

    // MARK: - 颜色

    static func color(_ c: hn_color) -> NSColor {
        NSColor(srgbRed: CGFloat((c >> 24) & 0xFF) / 255.0,
                green: CGFloat((c >> 16) & 0xFF) / 255.0,
                blue: CGFloat((c >> 8) & 0xFF) / 255.0,
                alpha: CGFloat(c & 0xFF) / 255.0)
    }

    static func gradient(from: inout NSColor, to: inout NSColor) -> CGGradient? {
        var fr: CGFloat = 0, fg: CGFloat = 0, fb: CGFloat = 0, fa: CGFloat = 1
        var tr: CGFloat = 0, tg: CGFloat = 0, tb: CGFloat = 0, ta: CGFloat = 1
        from.usingColorSpace(.sRGB)?.getRed(&fr, green: &fg, blue: &fb, alpha: &fa)
        to.usingColorSpace(.sRGB)?.getRed(&tr, green: &tg, blue: &tb, alpha: &ta)
        let comps: [CGFloat] = [fr, fg, fb, fa, tr, tg, tb, ta]
        return CGGradient(colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                          colorComponents: comps,
                          locations: [0, 1],
                          count: 2)
    }
}
