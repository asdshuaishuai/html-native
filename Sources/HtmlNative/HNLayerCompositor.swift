import AppKit
import CoreGraphics
import Foundation
import CHtmlNative

/// 图层合成器: 处理需要**图像重采样**的绘制指令。
///
/// 这里实现的是两类动画技术共用的底层能力:
///
/// - **网格变形(MESH)**: 把源图按顶点网格做分段仿射变形。
///   Live2D 的核心原理就是"用网格驱动一张立绘做形变"(呼吸/摇摆/口型),
///   网格与 UV 的数据格式是公开的; 这里实现同一原理的渲染管线。
/// - Lottie 的矢量层走 HN_CMD_POLYGON(无需重采样), 不在此文件。
///
/// 实现方式: 每个网格单元(四边形)分成两个三角形, 对每个三角形
/// 用仿射矩阵把源图纹理映射到目标位置 —— 即经典的三角形纹理映射。
/// 逐单元裁剪 + 绘制的做法避免了写完整的软件光栅器,
/// 又能得到正确的形变效果。
public enum HNLayerCompositor {

    /// 网格变形绘制: 逐单元计算仿射变换并绘制源图对应区域
    static func drawMesh(_ cmd: hn_cmd, into cg: CGContext) {
        guard let verts = cmd.mesh_verts, let uv = cmd.mesh_uv,
              cmd.mesh_cols > 0, cmd.mesh_rows > 0,
              let tp = cmd.text, cmd.text_len > 0 else { return }
        let src = String(decoding: UnsafeBufferPointer(
            start: UnsafeRawPointer(tp).assumingMemoryBound(to: UInt8.self),
            count: Int(cmd.text_len)), as: UTF8.self)
        guard let image = ImageStore.shared.image(src) else { return }

        let cols = Int(cmd.mesh_cols), rows = Int(cmd.mesh_rows)
        let stride = cols + 1
        let iw = CGFloat(image.size.width), ih = CGFloat(image.size.height)
        // 整体不透明度由 fill 的 alpha 承载(引擎侧把节点 alpha 编码在这里)
        let alpha = CGFloat(cmd.fill & 0xFF) / 255.0
        if alpha <= 0.004 { return }

        cg.saveGState()
        cg.interpolationQuality = .high
        cg.setAlpha(alpha)

        for r in 0..<rows {
            for c in 0..<cols {
                // 单元四角的顶点索引(行优先)
                let i00 = r * stride + c
                let i10 = r * stride + c + 1
                let i01 = (r + 1) * stride + c
                let i11 = (r + 1) * stride + c + 1

                // 目标位置(屏幕坐标, 已是绝对坐标)
                let p00 = CGPoint(x: CGFloat(verts[i00 * 2]), y: CGFloat(verts[i00 * 2 + 1]))
                let p10 = CGPoint(x: CGFloat(verts[i10 * 2]), y: CGFloat(verts[i10 * 2 + 1]))
                let p01 = CGPoint(x: CGFloat(verts[i01 * 2]), y: CGFloat(verts[i01 * 2 + 1]))
                let p11 = CGPoint(x: CGFloat(verts[i11 * 2]), y: CGFloat(verts[i11 * 2 + 1]))

                // 源图 UV → 像素矩形(注意 UV 的 y 向下, CGImage 裁剪 y 向上)
                let u00 = CGFloat(uv[i00 * 2]), v00 = CGFloat(uv[i00 * 2 + 1])
                let u10 = CGFloat(uv[i10 * 2]), v10 = CGFloat(uv[i10 * 2 + 1])
                let u01 = CGFloat(uv[i01 * 2]), v01 = CGFloat(uv[i01 * 2 + 1])
                let u11 = CGFloat(uv[i11 * 2]), v11 = CGFloat(uv[i11 * 2 + 1])

                // 单元四边形(源)与单元四边形(目标) → 两条三角形分别映射
                drawTri(cg, image,
                        srcA: (u00 * iw, v00 * ih), srcB: (u10 * iw, v10 * ih), srcC: (u01 * iw, v01 * ih),
                        dstA: p00, dstB: p10, dstC: p01, iw: iw, ih: ih)
                drawTri(cg, image,
                        srcA: (u10 * iw, v10 * ih), srcB: (u11 * iw, v11 * ih), srcC: (u01 * iw, v01 * ih),
                        dstA: p10, dstB: p11, dstC: p01, iw: iw, ih: ih)
            }
        }
        cg.restoreGState()
    }

    /// 单个三角形的纹理映射: 解出"源图 → 目标"的仿射矩阵, 裁剪后整图绘制。
    ///
    /// **接缝处理**: 相邻三角形共享一条边, 若各自严格裁剪到自己的边界,
    /// CG 对裁剪边做抗锯齿会让共享边两侧各留半透明像素 —— 拼起来就是
    /// 一条可见的暗线(网格越密越明显)。做法是把三角形按质心**外扩**
    /// 一点点, 让相邻片互相重叠半个像素; 后画的覆盖先画的, 接缝即消失。
    private static func drawTri(_ cg: CGContext, _ image: NSImage,
                                srcA: (CGFloat, CGFloat), srcB: (CGFloat, CGFloat),
                                srcC: (CGFloat, CGFloat),
                                dstA: CGPoint, dstB: CGPoint, dstC: CGPoint,
                                iw: CGFloat, ih: CGFloat) {
        // 退化三角形(面积≈0)时跳过: 网格折叠会出现, 直接忽略避免除零
        let denom = (srcB.0 - srcA.0) * (srcC.1 - srcA.1) - (srcC.0 - srcA.0) * (srcB.1 - srcA.1)
        if abs(denom) < 0.0001 { return }

        let a1 = (dstB.x - dstA.x), b1 = (dstC.x - dstA.x)
        let a2 = (dstB.y - dstA.y), b2 = (dstC.y - dstA.y)

        // 目标 = M · 源 + t (2x2 线性部分 + 平移), 由源三角形解出
        let m00 = (a1 * (srcC.1 - srcA.1) - b1 * (srcB.1 - srcA.1)) / denom
        let m01 = (b1 * (srcB.0 - srcA.0) - a1 * (srcC.0 - srcA.0)) / denom
        let m10 = (a2 * (srcC.1 - srcA.1) - b2 * (srcB.1 - srcA.1)) / denom
        let m11 = (b2 * (srcB.0 - srcA.0) - a2 * (srcC.0 - srcA.0)) / denom
        let tx = dstA.x - (m00 * srcA.0 + m01 * srcA.1)
        let ty = dstA.y - (m10 * srcA.0 + m11 * srcA.1)

        // 目标三角形按质心外扩(消除共享边抗锯齿缝)。
        // 0.7px 对 12x12 网格仍留残缝; 用 1.0px —— 重叠量固定, 不会随网格
        // 密度放大误差, 也不会让边缘明显溢出(只影响最外圈约 1px)。
        let cx = (dstA.x + dstB.x + dstC.x) / 3
        let cy = (dstA.y + dstB.y + dstC.y) / 3
        let grow: CGFloat = 1.0
        func out(_ p: CGPoint) -> CGPoint {
            let dx = p.x - cx, dy = p.y - cy
            let len = max(0.0001, sqrt(dx * dx + dy * dy))
            return CGPoint(x: p.x + dx / len * grow, y: p.y + dy / len * grow)
        }

        cg.saveGState()
        cg.beginPath()
        let eA = out(dstA), eB = out(dstB), eC = out(dstC)
        cg.move(to: eA)
        cg.addLine(to: eB)
        cg.addLine(to: eC)
        cg.closePath()
        cg.clip()

        let t = CGAffineTransform(a: m00, b: m10, c: m01, d: m11, tx: tx, ty: ty)
        if let cgImage = image.cgImage(forProposedRect: nil, context: nil, hints: nil) {
            // CGImage 原点在左上, 与 UV 约定一致
            cg.concatenate(t)
            cg.draw(cgImage, in: CGRect(x: 0, y: 0, width: iw, height: ih))
        } else {
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(cgContext: cg, flipped: true)
            image.draw(in: CGRect(x: 0, y: 0, width: iw, height: ih))
            NSGraphicsContext.restoreGraphicsState()
        }
        cg.restoreGState()
    }
}
