import AppKit
import Foundation
import CHtmlNative

/// 图片加载与缓存: 为引擎提供固有尺寸回调, 为绘制器提供 NSImage。
/// 相对路径按 root 解析(默认进程工作目录)。
public final class ImageStore {
    public static let shared = ImageStore()

    public var root: URL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    private var cache: [String: NSImage] = [:]
    private let lock = NSLock()

    public func resolve(_ src: String) -> String {
        if src.hasPrefix("/") { return src }
        return root.appendingPathComponent(src).path
    }

    public func image(_ src: String) -> NSImage? {
        let path = resolve(src)
        lock.lock()
        if let hit = cache[path] {
            lock.unlock()
            return hit
        }
        lock.unlock()
        guard let img = NSImage(contentsOfFile: path) else { return nil }
        lock.lock()
        cache[path] = img
        lock.unlock()
        return img
    }

    /// 供 ctx 持有的盒对象(backend 闭包不可捕获, 经 Unmanaged 指回)
    public final class CtxBox {
        let store: ImageStore
        init(store: ImageStore) { self.store = store }
    }

    public func backend() -> (hn_image_backend, CtxBox) {
        let box = CtxBox(store: self)
        var b = hn_image_backend()
        b.ctx = Unmanaged.passUnretained(box).toOpaque()
        b.size = { ctx, path, w, h in
            guard let ctx, let path else { return 0 }
            let box = Unmanaged<CtxBox>.fromOpaque(ctx).takeUnretainedValue()
            guard let img = box.store.image(String(cString: path)) else { return 0 }
            let pw = img.representations.first?.pixelsWide ?? Int(img.size.width)
            let ph = img.representations.first?.pixelsHigh ?? Int(img.size.height)
            guard pw > 0, ph > 0 else { return 0 }
            w?.pointee = Float(pw)
            h?.pointee = Float(ph)
            return 1
        }
        return (b, box)
    }
}
