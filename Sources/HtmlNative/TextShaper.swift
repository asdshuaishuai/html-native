import AppKit
import CoreText
import Foundation
import CHtmlNative

/// CoreText 文本后端: 核心布局引擎通过 C 回调向它查询文本宽度与字体度量。
/// 字体按 (字号, 字重, 斜体) 缓存。
public final class TextShaper {
    public static let shared = TextShaper()

    private struct Key: Hashable {
        var size: UInt32
        var weight: Int32
        var italic: Bool
        var family: Int32
    }

    private var fonts: [Key: NSFont] = [:]
    private let lock = NSLock()

    public func nsfont(_ f: hn_font_desc) -> NSFont {
        let key = Key(size: UInt32(f.size_px), weight: Int32(f.weight), italic: f.italic != 0,
                      family: Int32(f.family))
        lock.lock()
        if let cached = fonts[key] {
            lock.unlock()
            return cached
        }
        lock.unlock()

        let weight: NSFont.Weight =
            f.weight >= 800 ? .heavy :
            f.weight >= 700 ? .bold :
            f.weight >= 600 ? .semibold :
            f.weight >= 500 ? .medium : .regular
        var font = NSFont.systemFont(ofSize: CGFloat(f.size_px), weight: weight)
        /* 字族: monospace(Menlo 系) / serif(New York 系) — 保留字重与字号 */
        if f.family == 1 || f.family == 2 {
            let design: NSFontDescriptor.SystemDesign = f.family == 1 ? .monospaced : .serif
            if let desc = font.fontDescriptor.withDesign(design) {
                font = NSFont(descriptor: desc, size: CGFloat(f.size_px)) ?? font
            }
        }
        if f.italic != 0 {
            let it = NSFontManager.shared.convert(font, toHaveTrait: .italicFontMask)
            font = it
        }
        lock.lock()
        fonts[key] = font
        lock.unlock()
        return font
    }

    private func attributed(_ s: String, font f: hn_font_desc) -> NSAttributedString {
        NSAttributedString(string: s, attributes: [
            .font: nsfont(f),
            .kern: CGFloat(f.letter_spacing),
        ])
    }

    public func measure(_ s: String, font f: hn_font_desc) -> CGFloat {
        guard !s.isEmpty else { return 0 }
        let line = CTLineCreateWithAttributedString(attributed(s, font: f))
        return CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil))
    }

    /// 构造 C 回调结构体(ctx 指向 self 单例, 闭包不可捕获)
    public func backend() -> hn_text_backend {
        var b = hn_text_backend()
        b.ctx = Unmanaged.passUnretained(self).toOpaque()
        b.measure = { ctx, font, utf8, len in
            let shaper = Unmanaged<TextShaper>.fromOpaque(ctx!).takeUnretainedValue()
            guard let font, let utf8, len > 0 else { return 0 }
            let s = String(decoding: UnsafeBufferPointer(
                start: UnsafeRawPointer(utf8).assumingMemoryBound(to: UInt8.self), count: len), as: UTF8.self)
            return Float(shaper.measure(s, font: font.pointee))
        }
        b.metrics = { ctx, font, ascent, descent, leading in
            let shaper = Unmanaged<TextShaper>.fromOpaque(ctx!).takeUnretainedValue()
            guard let font else { return }
            let ct = shaper.nsfont(font.pointee)
            ascent?.pointee = Float(CTFontGetAscent(ct))
            descent?.pointee = Float(CTFontGetDescent(ct))
            leading?.pointee = Float(CTFontGetLeading(ct))
        }
        return b
    }
}
