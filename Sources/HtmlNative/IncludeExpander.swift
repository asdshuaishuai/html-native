import Foundation

/// `<include src="part.html">` 递归展开 —— 零构建的文件级组件化。
/// 人类开发者把界面拆成小文件即可复用, 无需任何打包器。
///
/// - 相对路径以 baseDir 解析
/// - 深度上限 6, 同文件链路循环时安全截断
/// - 文件缺失时内联一条可见的错误提示(而不是静默)
public enum IncludeExpander {
    public static let maxDepth = 6

    public static func expand(_ html: String, baseDir: URL? = nil) -> String {
        var seen: [String] = []
        return expandRec(html, baseDir: baseDir ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath), depth: 0, seen: &seen)
    }

    /// 完整预处理: 展开 <include> + 内联 <link rel="stylesheet" href="…">。
    /// 与网页生态对齐 —— HTML 文件自带样式引用, 无需命令行再传 --css。
    /// 返回处理后的 html 与合并后的 css(原 css 参数在前, link 在后)。
    public static func prepare(html: String, css: String?, baseDir: URL?) -> (html: String, css: String) {
        let dir = baseDir ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        var linkCss: [String] = []
        let expanded = inlineLinks(expand(html, baseDir: dir), baseDir: dir, cssOut: &linkCss)
        let joined = ([css ?? ""] + linkCss).filter { !$0.isEmpty }.joined(separator: "\n")
        return (expanded, joined)
    }

    /// 把 <link rel="stylesheet" href="x.css"> 替换为空(样式并入 cssOut); 仅本地文件
    static func inlineLinks(_ html: String, baseDir: URL, cssOut: inout [String]) -> String {
        var out = ""
        var rest = Substring(html)
        while let range = rest.range(of: "<link", options: .caseInsensitive) {
            // 确认是标签起始而非文本巧合(<linkxyz 之类)
            let after = rest[range.upperBound...]
            guard let c = after.first, c == " " || c == "\n" || c == "\t" || c == "\r" || c == ">" else {
                out += rest[..<range.upperBound]
                rest = rest[range.upperBound...]
                continue
            }
            out += rest[..<range.lowerBound]
            let tagRest = rest[range.lowerBound...]
            guard let close = tagRest.range(of: ">") else { out += tagRest; return out }
            let tag = String(tagRest[..<close.lowerBound])
            rest = tagRest[close.upperBound...]
            let rel = (attr(of: tag, "rel") ?? "").lowercased()
            guard rel.contains("stylesheet"), let href = attr(of: tag, "href") else {
                out += tag + ">"       // 保留非样式 link(如 icon)
                continue
            }
            let url = href.hasPrefix("/")
                ? URL(fileURLWithPath: href)
                : baseDir.appendingPathComponent(href)
            if let content = try? String(contentsOf: url, encoding: .utf8) {
                cssOut.append("/* \(href) */\n" + content)
            } else {
                cssOut.append("/* link 未找到: \(href) */")
            }
        }
        out += rest
        return out
    }

    static func expandRec(_ html: String, baseDir: URL, depth: Int, seen: inout [String]) -> String {
        guard depth < maxDepth else { return html }
        var out = ""
        var rest = Substring(html)
        while let range = rest.range(of: "<include") {
            out += rest[..<range.lowerBound]
            let tagRest = rest[range.lowerBound...]
            guard let close = tagRest.range(of: ">") else {
                out += tagRest
                return out
            }
            let tag = String(tagRest[..<close.lowerBound]) // <include src="..." ...
            rest = tagRest[close.upperBound...]

            guard let src = attr(of: tag, "src") else {
                out += "<div style=\"color:#ff5f57;font-size:11\">&lt;include 缺少 src&gt;</div>"
                continue
            }
            let url = src.hasPrefix("/")
                ? URL(fileURLWithPath: src)
                : baseDir.appendingPathComponent(src)
            let path = url.path
            if seen.contains(path) {
                out += "<div style=\"color:#ff5f57;font-size:11\">&lt;include 循环: \(src)&gt;</div>"
                continue
            }
            guard let content = try? String(contentsOfFile: path, encoding: .utf8) else {
                out += "<div style=\"color:#ff5f57;font-size:11\">&lt;include 未找到: \(src)&gt;</div>"
                continue
            }
            seen.append(path)
            out += expandRec(content, baseDir: url.deletingLastPathComponent(), depth: depth + 1, seen: &seen)
            seen.removeLast()
        }
        out += rest
        return out
    }

    /// 从标签字符串取属性值(单双引号均可)
    static func attr(of tag: String, _ name: String) -> String? {
        guard let r = tag.range(of: name + "=") else { return nil }
        var s = tag[r.upperBound...]
        while s.first == " " { s = s.dropFirst() }
        guard let q = s.first, q == "\"" || q == "'" else { return nil }
        s = s.dropFirst()
        guard let end = s.firstIndex(of: q) else { return nil }
        return String(s[..<end])
    }
}

/// 应用本地持久化 KV —— 每个应用一个 JSON 文件(~/.html-native/store/<id>.json),
/// 人类开发的应用无需任何后端即可保存状态。
public final class HNStore {
    public static let dir: URL = HNPaths.home.appendingPathComponent("store", isDirectory: true)

    private let file: URL
    private var cache: [String: String]
    private let lock = NSLock()

    public init(id: String) {
        let fm = FileManager.default
        try? fm.createDirectory(at: HNStore.dir, withIntermediateDirectories: true)
        file = HNStore.dir.appendingPathComponent("\(id).json")
        if let data = try? Data(contentsOf: file),
           let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: String] {
            cache = obj
        } else {
            cache = [:]
        }
    }

    public func get(_ key: String) -> String? {
        lock.lock(); defer { lock.unlock() }
        return cache[key]
    }

    public func set(_ key: String, _ value: String) {
        lock.lock()
        cache[key] = value
        let snapshot = cache
        lock.unlock()
        if let data = try? JSONSerialization.data(withJSONObject: snapshot) {
            try? data.write(to: file)
        }
    }

    public var count: Int {
        lock.lock(); defer { lock.unlock() }
        return cache.count
    }
}
