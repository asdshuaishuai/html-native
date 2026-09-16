import AppKit
import Foundation

/// 渲染器选择: 同一份 HTML/CSS 可走两条渲染路径。
///
/// - `native`(默认): 我们的 C99 引擎 → 原生渲染, 零 WebKit 依赖
/// - `webkit`(显式声明): 系统 WKWebView 兜底, 用于引擎暂不支持的构造
///   (grid / position:absolute / canvas / svg / video 等)
///
/// 设计原则: **绝不静默切换**。必须由页面显式声明, 否则同一文件在不同
/// 机器上会渲染出不同结果——那是灾难, 不是兜底。
public enum HNRenderer: String {
    case native
    case webkit

    /// 从页面 meta 读取渲染器声明; 未声明默认 native
    public static func declared(in html: String) -> HNRenderer {
        guard let content = metaContent("hn-renderer", in: html) else { return .native }
        return HNRenderer(rawValue: content.lowercased().trimmingCharacters(in: .whitespaces)) ?? .native
    }

    /// 取 <meta name="X" content="Y"> 的 Y(大小写不敏感, 单双引号均可)
    static func metaContent(_ name: String, in html: String) -> String? {
        let pattern = "<meta[^>]*name\\s*=\\s*[\"']\(name)[\"'][^>]*>"
        guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]),
              let m = re.firstMatch(in: html, range: NSRange(html.startIndex..., in: html)),
              let tagRange = Range(m.range, in: html) else { return nil }
        return attribute("content", in: String(html[tagRange]))
    }

    static func attribute(_ name: String, in tag: String) -> String? {
        let pattern = "\(name)\\s*=\\s*[\"']([^\"']*)[\"']"
        guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]),
              let m = re.firstMatch(in: tag, range: NSRange(tag.startIndex..., in: tag)),
              m.numberOfRanges > 1, let r = Range(m.range(at: 1), in: tag) else { return nil }
        return String(tag[r])
    }
}

/// CSS 归一化: 我们的引擎接受"免单位数值"(`padding: 16` 视作 16px),
/// 但标准 CSS 会忽略它。兜底到 WebKit 前必须补齐单位,
/// 否则同一份文件在两条路径下会长得不一样。
public enum HNCSSNormalizer {

    /// 需要补 px 的裸数字属性。line-height 不在内: 裸数字是倍数, 标准合法
    static let lengthProps: Set<String> = [
        "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "top", "right", "bottom", "left", "gap", "row-gap", "column-gap",
        "border-width", "border-radius", "font-size", "letter-spacing",
        "flex-basis", "border", "box-shadow", "text-indent", "outline-width",
    ]

    /// 行内 style / 样式表统一入口。仅在声明区(`prop: value`)内改写;
    /// 注释、字符串、函数参数(calc/var/gradient)内的数字不动。
    public static func normalize(_ css: String) -> String {
        var out = ""
        var i = css.startIndex
        var depth = 0                 // `{}` 深度: >0 表示在声明块内
        var inComment = false
        var quote: Character?
        var decl = ""                 // 当前累积的声明文本

        func flushDecl() {
            out += rewriteDeclaration(decl)
            decl = ""
        }

        while i < css.endIndex {
            let ch = css[i]
            let next = css.index(after: i)
            let hasNext = next < css.endIndex

            if inComment {
                if ch == "*" && hasNext && css[next] == "/" { inComment = false; i = css.index(after: next); continue }
                i = next; continue
            }
            if let q = quote {
                decl.append(ch)
                if ch == q { quote = nil }
                i = next; continue
            }
            if ch == "/" && hasNext && css[next] == "*" {
                // 注释前的声明先落盘
                if depth > 0 { flushDecl() } else { out += decl; decl = "" }
                inComment = true; i = css.index(after: next); continue
            }
            if ch == "\"" || ch == "'" { quote = ch; decl.append(ch); i = next; continue }

            if ch == "{" {
                out += decl; decl = ""
                depth += 1
                out.append(ch)
                i = next; continue
            }
            if ch == "}" {
                if depth > 0 { flushDecl() }
                else { out += decl; decl = "" }
                depth = max(0, depth - 1)
                out.append(ch)
                i = next; continue
            }
            if ch == ";" {
                if depth > 0 { flushDecl() } else { out += decl; decl = "" }
                out.append(ch)
                i = next; continue
            }
            decl.append(ch)
            i = next
        }
        out += decl
        return out
    }

    /// 单条声明: `prop: value` → 按属性决定是否补单位
    static func rewriteDeclaration(_ decl: String) -> String {
        guard let colon = decl.firstIndex(of: ":") else { return decl }
        let name = decl[..<colon].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let rawValue = String(decl[decl.index(after: colon)...])
        guard lengthProps.contains(name) else { return decl }
        let head = String(decl[..<decl.index(after: colon)])
        return head + normalizeValue(rawValue)
    }

    /// 值内逐 token 补单位。分隔符(空格/逗号)在括号外时断 token,
    /// 括号内的函数参数不拆(calc/var/gradient/rgba)。
    static func normalizeValue(_ value: String) -> String {
        var out = ""
        var token = ""
        var i = value.startIndex
        var paren = 0
        while i < value.endIndex {
            let ch = value[i]
            if ch == "(" { paren += 1 }
            else if ch == ")" { paren = max(0, paren - 1) }
            let isSep = (ch == " " || ch == "\t" || ch == ",") && paren == 0
            if isSep {
                out += normalizeToken(token) + String(ch)
                token = ""
            } else {
                token.append(ch)
            }
            i = value.index(after: i)
        }
        out += normalizeToken(token)
        return out
    }

    static func normalizeToken(_ tok: String) -> String {
        guard !tok.isEmpty else { return tok }
        // 值前面可能带逗号(如 0, 8 这种多值); 只在尾部无单位时补
        let lower = tok.lowercased()
        let keywords: Set<String> = ["auto", "inherit", "initial", "none", "normal",
                                     "solid", "dashed", "dotted", "transparent", "currentcolor"]
        if keywords.contains(lower) { return tok }
        if tok.contains("(") || tok.hasPrefix("#") { return tok }
        // 已有单位后缀
        for u in ["px", "em", "rem", "vw", "vh", "pt", "%", "s", "ms", "deg", "fr", "ch", "vmin", "vmax"] {
            if lower.hasSuffix(u) { return tok }
        }
        // 尾部标点(逗号/斜杠)先剥离再判断
        var core = tok
        var suffix = ""
        while let last = core.last, last == "," || last == "/" {
            suffix = String(last) + suffix
            core.removeLast()
        }
        guard let d = Double(core) else { return tok }
        _ = d
        // 0 在长度上下文里补 px 更稳妥(box-shadow 偏移/边框等都能接受 0px)
        return core + "px" + suffix
    }
}
