import AppKit
import Foundation

/// 轻应用槽位 —— 把"它上次停在哪、多大"按**名字**记住。
///
/// KDE Plasmoid 那类桌面小组件的核心特征是**半固化**: 随用随消, 但消失后再
/// 回来时回到原处, 而不是每次都被重置到屏幕中央。缺了这一层, `ttl` 只是
/// "自动关掉", 不是 "桌面小组件"。
///
/// 与 HNStore 的分工:
///   `HNStore(<app id>)`     页面自己的数据(store.set/get 读写的那份)
///   `HNAppletStore(<槽位名>) 槽位的几何(运行时写, 页面不直接读)
/// 两者 id 空间不同: 同一个槽位可以被不同的 app id 打开(例如拿别的文件临时
/// 试看同一张卡片), 几何仍然跟着槽位走。
public final class HNAppletStore {

    public static let dir: URL = HNPaths.home.appendingPathComponent("applets", isDirectory: true)

    private let file: URL
    private let lock = NSLock()
    private var cache: [String: Any]

    public init(name: String) {
        let fm = FileManager.default
        try? fm.createDirectory(at: HNAppletStore.dir, withIntermediateDirectories: true)
        file = HNAppletStore.dir.appendingPathComponent("\(name).json")
        if let data = try? Data(contentsOf: file),
           let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            cache = obj
        } else {
            cache = [:]
        }
    }

    // MARK: - 读写

    /// 槽位几何, 存的是**绝对屏幕坐标**(Cocoa 约定: x 向右, y 向上, 原点在
    /// 全局屏幕空间左下)。
    ///
    /// 刻意不沿用清单 hn-x/hn-y 那套"相对主屏顶边、y 向下": 那个约定在多屏下
    /// 会丢精度 —— 小组件停在副屏时存的是"副屏顶边向下 N", 还原时却拿主屏
    /// 顶边去换算, 于是每次回来都往下挪一截(两屏顶边不等高时最明显)。
    /// 绝对坐标与屏幕布局无关, 这才是"回到原处"该有的性质。
    /// 展示层的换算由 `applets` op / CLI 负责(见 displayTopLeft)。
    public func geometry() -> (x: Double, y: Double, w: Double, h: Double)? {
        lock.lock(); defer { lock.unlock() }
        guard let x = cache["x"] as? Double, let y = cache["y"] as? Double,
              let w = cache["w"] as? Double, let h = cache["h"] as? Double,
              w.isFinite, h.isFinite, w > 0, h > 0,
              x.isFinite, y.isFinite else { return nil }
        return (x, y, w, h)
    }

    /// 换算成"左上原点、y 向下、相对主屏顶边"的展示坐标 —— 与 hn-x/hn-y 同
    /// 一套读法, 于是 `hn applet list` 的输出能和页面声明直接对照。
    public func displayTopLeft() -> (x: Double, y: Double, w: Double, h: Double)? {
        guard let g = geometry() else { return nil }
        let top = NSScreen.main?.visibleFrame.maxY ?? 1440
        return (g.x, top - g.y - g.h, g.w, g.h)
    }

    public func lastAppId() -> String? { lock.lock(); defer { lock.unlock() }; return cache["appId"] as? String }
    public func lastSeen() -> String? { lock.lock(); defer { lock.unlock() }; return cache["lastSeen"] as? String }

    /// 落盘。写前判有限: NSRect 在极少数情况下(窗口还没完成布局)会带 NaN,
    /// 而 JSONSerialization 遇到非有限浮点会整体抛错 —— 那样槽位会被静默
    /// 丢弃, 表现成"位置从来不记得"。
    public func save(x: Double, y: Double, w: Double, h: Double, appId: String) {
        guard x.isFinite, y.isFinite, w.isFinite, h.isFinite, w > 0, h > 0 else { return }
        lock.lock()
        cache["x"] = x; cache["y"] = y; cache["w"] = w; cache["h"] = h
        cache["appId"] = appId
        cache["lastSeen"] = ISO8601DateFormatter().string(from: Date())
        let snapshot = cache
        lock.unlock()
        // atomic: 宿主被 kill 时不留半个 JSON(那会让下次读取整份作废)
        if let data = try? JSONSerialization.data(withJSONObject: snapshot) {
            try? data.write(to: file, options: .atomic)
        }
    }

    /// 忘记这个槽位。返回是否存在过 —— 让调用方能区分"忘了"和"本来就没有"。
    @discardableResult
    public func remove() -> Bool {
        lock.lock(); defer { lock.unlock() }
        cache = [:]
        return (try? FileManager.default.removeItem(at: file)) != nil
    }

    // MARK: - 全槽位枚举(agent 用)

    public struct SlotInfo {
        public let name: String
        /// 绝对屏幕坐标(见 geometry())
        public var x: Double, y: Double, w: Double, h: Double
        public let appId: String?
        public let lastSeen: String?
        /// 此刻是否还开着(槽位是持久的, 应用是瞬时的 —— 两者正交)
        public var open: Bool

        public init(name: String, x: Double, y: Double, w: Double, h: Double,
                    appId: String?, lastSeen: String?, open: Bool) {
            self.name = name; self.x = x; self.y = y; self.w = w; self.h = h
            self.appId = appId; self.lastSeen = lastSeen; self.open = open
        }

        /// 展示用: 左上原点、y 向下、相对主屏顶边 —— 与 hn-x/hn-y 同一套读法
        public var displayTopLeft: (x: Double, y: Double, w: Double, h: Double) {
            let top = NSScreen.main?.visibleFrame.maxY ?? 1440
            return (x, top - y - h, w, h)
        }
    }

    public static func enumerate() -> [SlotInfo] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return [] }
        var out: [SlotInfo] = []
        for n in names where n.hasSuffix(".json") {
            let name = String(n.dropLast(5))
            let st = HNAppletStore(name: name)
            guard let g = st.geometry() else { continue }  // 残缺文件不算槽位
            out.append(SlotInfo(name: name, x: g.x, y: g.y, w: g.w, h: g.h,
                                appId: st.lastAppId(), lastSeen: st.lastSeen(), open: false))
        }
        return out.sorted { $0.name < $1.name }
    }

    public static func remove(named name: String) -> Bool {
        HNAppletStore(name: name).remove()
    }
}

extension HNEngine {

    /// 把槽位几何夹回当前屏幕集合内。
    ///
    /// 槽位是"上次停在哪"的记忆, 而屏幕是会变的: 拔掉那块外接显示器之后原坐标
    /// 可能整块落在视野外。不夹回去的症状是"打开后什么也看不到", 而用户不会往
    /// "上次放在副屏"这个方向排查。
    ///
    /// 判据是**中心点**而不是整个矩形: 小组件本来就该能边缘吸附/部分出界,
    /// 只有中心也看不见了才判定为不可达。
    static func clampAppletFrame(_ frame: NSRect) -> NSRect {
        let frames = NSScreen.screens.map(\.visibleFrame)
        guard !frames.isEmpty else { return frame }
        let union = frames.reduce(frames[0]) { NSUnionRect($0, $1) }
        guard !NSPointInRect(NSPoint(x: frame.midX, y: frame.midY), union) else { return frame }
        let home = NSScreen.main?.visibleFrame ?? frames[0]
        var f = frame
        f.origin = NSPoint(x: home.minX + 24, y: home.maxY - 24 - f.height)
        return f
    }
}
