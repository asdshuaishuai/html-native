import Foundation
import HtmlNative

/// hn new — 项目脚手架: 生成一个可直接运行的最小应用。
/// 展示布局/变量主题/输入/本地存储/系统集成, 是人类开发者的起点。
enum Scaffold {
    static func make(at dir: URL, name: String) -> Bool {
        let fm = FileManager.default
        let app = dir.appendingPathComponent("app.html")
        let css = dir.appendingPathComponent("app.css")
        let head = dir.appendingPathComponent("head.html")
        let readme = dir.appendingPathComponent("README.md")
        guard !fm.fileExists(atPath: app.path) else {
            FileHandle.standardError.write("已存在: \(app.path)\n".data(using: .utf8)!)
            return false
        }
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        try? appHTML(name: name).write(to: app, atomically: true, encoding: .utf8)
        try? appCSS().write(to: css, atomically: true, encoding: .utf8)
        try? headHTML().write(to: head, atomically: true, encoding: .utf8)
        try? readmeMD(name: name).write(to: readme, atomically: true, encoding: .utf8)
        return true
    }

    // MARK: - 模板

    static func appHTML(name: String) -> String {
        """
        <html><head>
        <meta name="hn-surface" content="window">
        <meta name="hn-window" content="420x520">
        <meta name="hn-title" content="\(name)">
        <include src="head.html">
        </head><body>
          <div class="wrap">
            <h1>\(name)</h1>
            <p class="sub">用 HTML 写界面, 原生渲染, 无浏览器。</p>

            <!-- 本地存储: 重启后仍在 -->
            <div class="card">
              <div class="k">你的名字</div>
              <div class="row">
                <input id="name-input" name="value" placeholder="输入后点保存">
                <div class="btn" hx-post="sys://store/set?key=name" hx-target="name-out">保存</div>
              </div>
              <div class="v" id="name-out"><span hx-get="sys://store/get?key=name"
                    hx-trigger="load">…</span></div>
            </div>

            <!-- 系统信息: sys:// 本地应答 -->
            <div class="card" id="syscard" hx-get="sys://info" hx-trigger="load">
              <div class="k">系统信息(点击刷新)</div>
              <div class="sub">加载中…</div>
            </div>

            <div class="foot">编辑本文件, 保存即热更新(hn dev 模式)。</div>
          </div>
        </body></html>
        """
    }

    static func headHTML() -> String {
        """
        <!-- 被 app.html 的 <include> 引入: 零构建的文件级组件 -->
        <style>
        :root, body { --accent: #4f7cff; --bg: #12151c; --card: #1b212c; }
        </style>
        """
    }

    static func appCSS() -> String {
        """
        html, body { margin: 0; width: 100%; height: 100%; background: var(--bg); }
        .wrap { padding: 20; display: flex; flex-direction: column; gap: 12; }
        h1 { color: #e8eaf0; font-size: 20; margin: 0; }
        .sub { color: #6b7386; font-size: 12; margin: 4 0 0 0; }
        .card { background: var(--card); border: 1 solid #2b3444; border-radius: 12;
                padding: 14; display: flex; flex-direction: column; gap: 8; }
        .k { color: #9aa3b5; font-size: 12; }
        .v { color: #e8eaf0; font-size: 13; font-weight: 600; }
        .row { display: flex; gap: 8; }
        .row input { flex: 1; }
        .btn { background: var(--accent); color: #fff; padding: 8 14; border-radius: 8;
               text-align: center; font-weight: 600; font-size: 12; transition: 200ms; }
        .btn:hover { background: #6b8dff; }
        .foot { color: #4c5468; font-size: 10; margin-top: 4; }
        """
    }

    static func readmeMD(name: String) -> String {
        """
        # \(name)

        html-native 应用 —— HTML 描述界面, 原生渲染, 无浏览器。

        ## 运行

            hn dev app.html --css app.css    # 开发模式: 保存即热更新
            hn open app app.html --css app.css

        ## 文件

        - `app.html` — 界面(`<include src="head.html">` 演示文件级组件)
        - `app.css` — 样式(CSS 变量主题)
        - `head.html` — 被引用的片段

        ## 能力速查

        - `hx-get="sys://info|cpu|memory|disk|battery|uptime|host"` 系统数据
        - `sys://store/get?key=k` / `hx-post="sys://store/set?key=k"` 本地持久化
        - `sys://clipboard/get|set`、`sys://notify`、`sys://open`
        - `hn open msg.html --ttl 8` 消息卡(到期热销毁)、`hn-drag`、`hn-dismiss`
        - `@media (max-width: 600px)` 响应式; `transition` 过渡动画
        """
    }
}
