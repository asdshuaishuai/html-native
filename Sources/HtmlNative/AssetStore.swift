import Foundation
import CHtmlNative

/// 外部资产读取(Lottie JSON 等)。引擎不做磁盘/网络 I/O —— 与文本/图片
/// 后端同样是依赖倒置: 运行时决定字节从哪来(本地文件 / 内存 / 远端下载),
/// 引擎只解析拿到的内容。这让同一份 Lottie 求值代码在三个平台完全一致。
public final class AssetStore {
    public static let shared = AssetStore()

    /// 相对路径按 root 解析(默认进程工作目录, 与 ImageStore 一致)
    public var root: URL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)

    /// 内存资产: 宿主/脚本可先把内容放进来(避免落盘, 也便于测试)
    private var memory: [String: NSData] = [:]
    private let lock = NSLock()
    /// 已加载文件的字节。引擎按指针读取并**就地解析**(会写入缓冲区),
    /// 因此: (1) 缓冲必须长期存活, (2) 用 NSData 而非 Data —— 它的 bytes
    /// 指针在对象存活期间稳定, 而 Data 的 withUnsafeBytes 指针只在闭包内有效。
    private var loaded: [String: NSData] = [:]

    public func resolve(_ path: String) -> String {
        if path.hasPrefix("/") { return path }
        return root.appendingPathComponent(path).path
    }

    public func put(_ path: String, _ data: Data) {
        lock.lock()
        memory[path] = data as NSData
        loaded[path] = data as NSData
        lock.unlock()
    }

    public func put(_ path: String, _ text: String) {
        put(path, Data(text.utf8))
    }

    /// 让某路径的缓存失效(热更新 Lottie 文件时调用)
    public func invalidate(_ path: String) {
        lock.lock()
        loaded.removeValue(forKey: path)
        lock.unlock()
    }

    func data(_ path: String) -> NSData? {
        lock.lock()
        if let d = memory[path] { lock.unlock(); return d }
        if let d = loaded[path] { lock.unlock(); return d }
        lock.unlock()
        guard let d = FileManager.default.contents(atPath: resolve(path)) else { return nil }
        let ns = d as NSData
        lock.lock()
        loaded[path] = ns
        lock.unlock()
        return ns
    }

    /// 供 ctx 持有的盒对象(闭包不可捕获, 经 Unmanaged 指回)
    public final class CtxBox {
        let store: AssetStore
        init(store: AssetStore) { self.store = store }
    }

    public func backend() -> (hn_asset_backend, CtxBox) {
        let box = CtxBox(store: self)
        var b = hn_asset_backend()
        b.ctx = Unmanaged.passUnretained(box).toOpaque()
        b.load = { ctx, path, len in
            guard let ctx, let path else { return nil }
            let box = Unmanaged<CtxBox>.fromOpaque(ctx).takeUnretainedValue()
            let key = String(cString: path)
            guard let d = box.store.data(key), d.length > 0 else { return nil }
            len?.pointee = d.length
            return d.bytes.assumingMemoryBound(to: CChar.self)
        }
        return (b, box)
    }
}
