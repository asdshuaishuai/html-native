import Foundation

/// 内置系统信息卡: sys:// 本地传输 + 轮询的参考实现,
/// `hn syscard` 即以此打开一个常驻弹窗。
public enum SysCard {
    public static let html = """
    <html><head>
    <meta charset="utf-8">
    <meta name="hn-surface" content="popup">
    <meta name="hn-theme" content="dark">
    <meta name="hn-window" content="300x430">
    <meta name="hn-title" content="系统信息">
    <style>
    body { margin: 0; padding: 14 16; background: #14171f; border: 1 solid #2a3040; border-radius: 12; }
    .foot { color: #5a6273; font-size: 10; margin-top: 10; }
    </style></head>
    <body hn-drag>
    <div id="p" hx-get="sys://info" hx-trigger="every 2s">
      <div style="color: #6b7386; font-size: 12; padding: 8">正在读取系统数据</div>
    </div>
    <div class="foot">sys:// 本地应答 · 每 2s 刷新 · hn close syscard 销毁</div>
    </body></html>
    """
}
