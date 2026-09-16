import AppKit
import Foundation

/// 应用级 sys:// 路由 —— 两个渲染器(native / webkit 兜底)共用同一套语义。
///
/// 契约: 输入请求(URL + 表单编码体) → 输出 HTML 片段。
/// 这样"同一份 HTML 在两条渲染路径下行为一致"是结构保证, 而不是两套实现碰巧一样。
public enum HNAppRoutes {

    /// 处理应用级路由; 非应用级(如 sys://cpu)返回 nil, 交由 SystemBridge 处理
    public static func handle(url: String, form encodedForm: String, storeId: String) -> String? {
        let route = SystemBridge.route(of: url)
        let query = queryParams(url)
        let body = formParams(encodedForm)

        switch route {
        case let r where r.hasPrefix("store/"):
            let key = query["key"] ?? body["key"] ?? ""
            guard !key.isEmpty else {
                return "<span style=\"color:#ff5f57\">store 需要 ?key=</span>"
            }
            let st = HNStore(id: storeId)
            if route == "store/get" {
                let v = st.get(key) ?? ""
                return "<span class=\"store-v\">\(v.isEmpty ? "未设置" : escape(v))</span>"
            }
            if route == "store/set" {
                let v = body["value"] ?? body[key] ?? ""
                st.set(key, v)
                return "<span class=\"store-v\">\(v.isEmpty ? "已清空" : "已保存: \(escape(v))")</span>"
            }
            if route == "store/count" { return "<span>\(st.count)</span>" }
            return nil

        case "clipboard/get":
            let v = NSPasteboard.general.string(forType: .string) ?? ""
            return "<span>\(v.isEmpty ? "(剪贴板为空)" : escape(String(v.prefix(500))))</span>"

        case "clipboard/set":
            let v = body["text"] ?? ""
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(v, forType: .string)
            return "<span style=\"color:#3ecf6f\">已写入剪贴板 (\(v.count) 字)</span>"

        case "notify":
            let title = body["title"] ?? "html-native"
            let message = body["body"] ?? body["text"] ?? ""
            postNotification(title: title, body: message)
            return "<span style=\"color:#3ecf6f\">已发送通知</span>"

        case "open":
            let u = body["url"] ?? query["url"] ?? ""
            if let url = URL(string: u) { NSWorkspace.shared.open(url) }
            return "<span>\(u.isEmpty ? "缺少 url" : "已打开 \(escape(u))")</span>"

        default:
            return nil
        }
    }

    // MARK: - 工具

    public static func queryParams(_ url: String) -> [String: String] {
        guard let q = url.firstIndex(of: "?") else { return [:] }
        return parsePairs(String(url[url.index(after: q)...]), sep: "&")
    }

    public static func formParams(_ encoded: String) -> [String: String] {
        parsePairs(encoded, sep: "&")
    }

    static func parsePairs(_ s: String, sep: Character) -> [String: String] {
        var out: [String: String] = [:]
        for pair in s.split(separator: sep) {
            let kv = pair.split(separator: "=", maxSplits: 1)
            guard kv.count == 2 else { continue }
            let k = String(kv[0]).removingPercentEncoding ?? String(kv[0])
            let v = String(kv[1]).removingPercentEncoding ?? String(kv[1])
            out[k] = v
        }
        return out
    }

    public static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }

    public static func postNotification(title: String, body: String) {
        let note = NSUserNotification()
        note.title = title
        note.informativeText = body
        note.soundName = NSUserNotificationDefaultSoundName
        NSUserNotificationCenter.default.deliver(note)
    }
}
