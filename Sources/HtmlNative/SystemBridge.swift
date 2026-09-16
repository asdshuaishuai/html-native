import Foundation
import IOKit
// 电池走 pmset 输出解析(IOKit.ps 模块名随 SDK 漂移, 进程方式最稳)

/// 系统集成桥: hn 编码里的 `sys://` 路径由本地直接应答(零网络),
/// 返回 HTML 片段 —— 引擎的"小程序系统 API"。
///
///   hx-get="sys://info"      完整系统信息卡
///   hx-get="sys://cpu"       CPU 占用(两次采样)
///   hx-get="sys://memory"    物理内存
///   hx-get="sys://disk"      磁盘容量
///   hx-get="sys://battery"   电池状态
///   hx-get="sys://uptime"    开机时长
///   hx-get="sys://host"      主机名 / 系统版本 / 架构
public enum SystemBridge {
    public static func handles(_ url: String) -> Bool {
        url.hasPrefix("sys://") || url.hasPrefix("hn://sys/")
    }

    /// 路由路径归一化: "sys://info" / "hn://sys/info" → "info"
    public static func route(of url: String) -> String {
        var s = url
        if s.hasPrefix("sys://") { s.removeFirst("sys://".count) }
        else if s.hasPrefix("hn://sys/") { s.removeFirst("hn://sys/".count) }
        if let q = s.firstIndex(of: "?") { s = String(s[..<q]) }
        while s.hasPrefix("/") { s.removeFirst() }
        return s
    }

    /// 生成片段(可在任意队列调用; CPU 采样会阻塞 ~120ms)
    public static func fragment(for url: String) -> String {
        switch route(of: url) {
        case "cpu": return cpuFragment()
        case "memory", "mem": return memFragment()
        case "disk": return diskFragment()
        case "battery", "power": return batteryFragment()
        case "uptime": return uptimeFragment()
        case "host": return hostFragment()
        default: return infoFragment()
        }
    }

    // MARK: - 片段模板(自带样式, 换入任何文档即可用)

    static let css = """
    <style>
    .sysrow { display: flex; padding: 4 0; }
    .sysk { color: #8b93a7; font-size: 11; width: 92; }
    .sysv { color: #e8eaf0; font-size: 12; font-weight: 600; }
    .sysbar { height: 5; border-radius: 2.5; background: #232733; margin-top: 3; }
    .sysfill { height: 100%; border-radius: 2.5; background: #4f7cff; }
    .syshead { color: #eef0f5; font-size: 12; font-weight: 700; margin: 0 0 6 0; }
    </style>
    """

    /// 阈值色阶: 占用越高越暖(蓝 → 琥珀 → 红), 数据语义一眼可读
    static func levelColor(_ pct: Double) -> String {
        if pct >= 85 { return "#ff5f57" }
        if pct >= 60 { return "#f5b73d" }
        return "#4f7cff"
    }

    static func row(_ k: String, _ v: String, pct: Double? = nil) -> String {
        var bar = ""
        if let p = pct {
            let c = levelColor(p)
            bar = "<div class=\"sysbar\"><div class=\"sysfill\" style=\"width:\(Int(p))%;background:\(c)\"></div></div>"
        }
        return "<div class=\"sysrow\"><div class=\"sysk\">\(k)</div><div class=\"sysv\">\(v)</div></div>\(bar)"
    }

    static func infoFragment() -> String {
        css
        + "<div class=\"syshead\">系统信息</div>"
        + hostFragmentBody()
        + cpuFragmentBody()
        + memFragmentBody()
        + diskFragmentBody()
        + batteryFragmentBody()
        + uptimeFragmentBody()
    }
    static func cpuFragment() -> String { css + "<div class=\"syshead\">CPU</div>" + cpuFragmentBody() }
    static func memFragment() -> String { css + "<div class=\"syshead\">内存</div>" + memFragmentBody() }
    static func diskFragment() -> String { css + "<div class=\"syshead\">磁盘</div>" + diskFragmentBody() }
    static func batteryFragment() -> String { css + "<div class=\"syshead\">电池</div>" + batteryFragmentBody() }
    static func uptimeFragment() -> String { css + "<div class=\"syshead\">运行时间</div>" + uptimeFragmentBody() }
    static func hostFragment() -> String { css + "<div class=\"syshead\">主机</div>" + hostFragmentBody() }

    // MARK: - 系统数据

    static func hostFragmentBody() -> String {
        let pi = ProcessInfo.processInfo
        let host = Host.current().localizedName ?? "Mac"
        let v = pi.operatingSystemVersion
        let ver = v.patchVersion == 0 ? "\(v.majorVersion).\(v.minorVersion)"
                                      : "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)"
        return row("主机", host)
            + row("系统", "macOS \(ver)")
            + row("架构", cpuArch())
    }

    static func cpuArch() -> String {
        #if arch(arm64)
        return "Apple Silicon (arm64)"
        #elseif arch(x86_64)
        return "Intel (x86_64)"
        #else
        return "unknown"
        #endif
    }

    static func cpuFragmentBody() -> String {
        let cores = ProcessInfo.processInfo.activeProcessorCount
        let load = cpuUsage()
        return row("占用", String(format: "%.0f%%", load * 100), pct: load * 100)
            + row("核心", "\(cores) 核")
    }

    /// 两次 host_statistics 采样求差(阻塞 ~120ms)
    static func cpuUsage() -> Double {
        let s0 = cpuTicks()
        Thread.sleep(forTimeInterval: 0.12)
        let s1 = cpuTicks()
        guard let a = s0, let b = s1 else { return 0 }
        let dUser = Double(b.0 - a.0), dSys = Double(b.1 - a.1), dIdle = Double(b.2 - a.2)
        let total = dUser + dSys + dIdle
        guard total > 0 else { return 0 }
        return (dUser + dSys) / total
    }

    static func cpuTicks() -> (UInt64, UInt64, UInt64)? {
        var size = mach_msg_type_number_t(MemoryLayout<host_cpu_load_info_data_t>.size / MemoryLayout<integer_t>.size)
        var info = host_cpu_load_info_data_t()
        let kr = withUnsafeMutablePointer(to: &info) { ptr in
            ptr.withMemoryRebound(to: integer_t.self, capacity: Int(size)) {
                host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, $0, &size)
            }
        }
        guard kr == KERN_SUCCESS else { return nil }
        return (UInt64(info.cpu_ticks.0), UInt64(info.cpu_ticks.1), UInt64(info.cpu_ticks.2))
    }

    static func memFragmentBody() -> String {
        let total = ProcessInfo.processInfo.physicalMemory
        var size = mach_msg_type_number_t(MemoryLayout<vm_statistics64_data_t>.size / MemoryLayout<integer_t>.size)
        var vm = vm_statistics64_data_t()
        let kr = withUnsafeMutablePointer(to: &vm) { ptr in
            ptr.withMemoryRebound(to: integer_t.self, capacity: Int(size)) {
                host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &size)
            }
        }
        var free: UInt64 = 0
        if kr == KERN_SUCCESS {
            free = UInt64(vm.free_count) * UInt64(vm_page_size)
        }
        let used = total > free ? total - free : 0
        let pct = total > 0 ? Double(used) / Double(total) * 100 : 0
        return row("物理内存", gb(total), pct: pct)
            + row("已用", gb(used))
            + row("可用", gb(free))
    }

    static func diskFragmentBody() -> String {
        guard let vals = try? URL(fileURLWithPath: "/").resourceValues(forKeys: [
            .volumeTotalCapacityKey, .volumeAvailableCapacityForImportantUsageKey,
        ]) else { return row("磁盘", "不可用") }
        let total = UInt64(vals.volumeTotalCapacity ?? 0)
        let free = vals.volumeAvailableCapacityForImportantUsage ?? 0
        let used = total > free ? total - UInt64(free) : 0
        let pct = total > 0 ? Double(used) / Double(total) * 100 : 0
        return row("总容量", gb(total), pct: pct)
            + row("可用", gb(UInt64(free)))
    }

    static func batteryFragmentBody() -> String {
        // pmset -g batt 输出形如: "Now drawing from 'AC Power'" / "-InternalBattery-0 (id=...)	100%; charged;"
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/pmset")
        p.arguments = ["-g", "batt"]
        p.standardError = FileHandle.nullDevice
        let pipe = Pipe()
        p.standardOutput = pipe
        guard (try? p.run()) != nil else { return row("电池", "不可用") }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let out = String(data: data, encoding: .utf8) ?? ""
        let pct = out.range(of: #"(\d+)%"#, options: .regularExpression)
            .flatMap { Int(out[$0].dropLast()) }
        guard let pct else { return row("类型", "台式机") }
        let charging = out.contains("discharging") ? false : true
        let ac = out.contains("AC Power")
        return row("电量", "\(pct)%\(charging && !ac ? " · 充电中" : "")", pct: Double(pct))
            + row("来源", ac ? "电源适配器" : "电池")
    }

    static func uptimeFragmentBody() -> String {
        var boot = timeval()
        var len = MemoryLayout<timeval>.size
        sysctlbyname("kern.boottime", &boot, &len, nil, 0)
        let up = Date().timeIntervalSince1970 - TimeInterval(boot.tv_sec)
        let h = Int(up) / 3600, m = (Int(up) % 3600) / 60
        let d = h / 24
        let text = d > 0 ? "\(d) 天 \(h % 24) 小时" : "\(h) 小时 \(m) 分"
        return row("开机时长", text)
    }

    static func gb(_ bytes: UInt64) -> String {
        String(format: "%.1f GB", Double(bytes) / 1_073_741_824.0)
    }
}
