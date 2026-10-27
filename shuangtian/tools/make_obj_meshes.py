"""生成一组**参数化复杂网格**，绕开「只有 5 种内置图元」的限制。

## 为什么需要它

霜天/裁云的三维几何只有 sphere/box/cylinder/torus/plane 五种内置工厂。
任何超出这五种的形状（齿轮、管道、产品外壳、螺杆、锥体、棱柱……）都必须
先变成网格文件。`model_shape: "obj:<路径>"` 已实测可用（能正确加载、光照、着色），
所以缺的只是**网格生成器**，不是渲染能力。

## 设计约束

- 只写纯 OBJ（`v` / `f` / 可选 `vn`）：不写 mtl、不依赖贴图，颜色由
  `model_color` 在脚本里给。这样一份网格能被任意配色复用。
- 三角形**逆时针**朝外（面法线由叉积定，错了会让背面剔除吃掉表面）。
- 顶点**不重复**（索引化的好处是省内存，但更省事的是先按索引化写，
  由 `weld` 保证接缝处不裂）。
- 确定性：不引入随机，同一参数永远同一网格。

## 用法

    python tools/make_obj_meshes.py <输出目录>

生成 gear.obj / tube.obj / capsule.obj / hexprism.obj / torusknot.obj。
"""

from __future__ import annotations

import io
import math
import os
import sys

Vec = tuple[float, float, float]


def _sub(a: Vec, b: Vec) -> Vec:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a: Vec, b: Vec) -> Vec:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _dot(a: Vec, b: Vec) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


class MeshBuilder:
    """收集顶点与三角形，最后写出 OBJ。

    三角形朝向在 `add_tri` 时按调用方给的顺序保留；`write` 不翻转。
    """

    def __init__(self) -> None:
        self.verts: list[Vec] = []
        self.faces: list[tuple[int, int, int]] = []

    def add_vert(self, v: Vec) -> int:
        self.verts.append(v)
        return len(self.verts) - 1

    def add_tri(self, a: int, b: int, c: int) -> None:
        if a == b or b == c or a == c:
            return  # 退化三角形（极点、接缝）直接丢掉，否则法线是零向量
        self.faces.append((a, b, c))

    def add_quad(self, a: int, b: int, c: int, d: int) -> None:
        self.add_tri(a, b, c)
        self.add_tri(a, c, d)

    def orient_outward(self) -> None:
        """把整体朝向修正为「法线背离原点」。

        参数化曲面几乎总是绕轴生成的，若某段角度方向写反，整片会朝内、
        被背面剔除吃掉（表现为「模型缺了一块」，很容易误判成生成器坏）。
        这里按**面积加权**投票决定是否整体翻转——比逐面判断稳，
        因为逐面判断会在非凸形状（齿轮、环面结）上胡乱翻。
        """
        score = 0.0
        for i0, i1, i2 in self.faces:
            a, b, c = self.verts[i0], self.verts[i1], self.verts[i2]
            n = _cross(_sub(b, a), _sub(c, a))
            centroid = (
                (a[0] + b[0] + c[0]) / 3.0,
                (a[1] + b[1] + c[1]) / 3.0,
                (a[2] + b[2] + c[2]) / 3.0,
            )
            score += _dot(n, centroid)
        if score < 0.0:
            self.faces = [(a, c, b) for a, b, c in self.faces]

    def smooth_normals(self) -> list[Vec]:
        """按面积加权累加相邻面的法线，得到**平滑顶点法线**。

        ## 为什么必须写 vn

        不写 `vn` 时 `Mesh::load_obj` 会回退到**面法线**（每个三角形一个常量）。
        于是相邻三角形着色不连续，管身出现一圈圈横向亮带——实测环面结在
        1280x720 下有明显带状伪影，**加密网格也治不了**（只让带变细）。

        按面积加权而不是简单平均：叉积的模长本身就是两倍面积，
        直接累加叉积即得面积加权；简单平均会让密集小面把法线拉向自己，
        在大面与小面交界处折出假棱。
        """
        accum: list[list[float]] = [[0.0, 0.0, 0.0] for _ in self.verts]
        for i0, i1, i2 in self.faces:
            a, b, c = self.verts[i0], self.verts[i1], self.verts[i2]
            n = _cross(_sub(b, a), _sub(c, a))
            for idx in (i0, i1, i2):
                accum[idx][0] += n[0]
                accum[idx][1] += n[1]
                accum[idx][2] += n[2]
        out: list[Vec] = []
        for v in accum:
            ln = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
            if ln < 1e-12:
                out.append((0.0, 1.0, 0.0))  # 孤立顶点：兜底向上
                continue
            out.append((v[0] / ln, v[1] / ln, v[2] / ln))
        return out

    def write(self, path: str, name: str, smooth: bool = True) -> None:
        """写出 OBJ。

        `smooth` 决定法线策略，**必须按几何类型选**：

        - `True`（默认）：面积加权平滑法线。适合曲面（球/管/环面结）。
        - `False`：不写 `vn`，让加载器回退到**面法线**。适合有棱角的机械件
          （齿轮的齿、棱柱的平面）——实测用平滑法线会把齿的硬边抹平，
          齿轮看起来像“软塌的面团”，失去辨识度。
        """
        self.orient_outward()
        lines = [f"# {name}", f"o {name}"]
        for x, y, z in self.verts:
            lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
        if smooth:
            normals = self.smooth_normals()
            for x, y, z in normals:
                lines.append(f"vn {x:.6f} {y:.6f} {z:.6f}")
            # 顶点与法线下标一一对应，面引用写 `v//vn` 即可（不带纹理坐标）。
            for a, b, c in self.faces:
                lines.append(f"f {a + 1}//{a + 1} {b + 1}//{b + 1} {c + 1}//{c + 1}")
        else:
            for a, b, c in self.faces:
                lines.append(f"f {a + 1} {b + 1} {c + 1}")
        with io.open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines))
            fh.write("\n")


def make_gear(teeth: int = 14, outer: float = 1.0, root: float = 0.78,
              thickness: float = 0.34, bore: float = 0.30,
              steps_per_tooth: int = 6) -> MeshBuilder:
    """齿轮：渐开线齿形的简化版（梯形齿），带中心孔。

    为什么用梯形齿而不是真渐开线：观感上齿轮的辨识度来自「齿的疏密与轮廓」，
    真渐开线的差异在 1080p 里一两个像素，代价却是一个完整的几何推导。
    """
    mb = MeshBuilder()
    hz = thickness * 0.5
    n = teeth * steps_per_tooth

    def profile_radius(k: int) -> float:
        # 一个齿周期内：根→顶→根，用平滑阶跃让齿顶有平台
        phase = (k % steps_per_tooth) / float(steps_per_tooth)
        if phase < 0.28:
            t = phase / 0.28
            return root + (outer - root) * t
        if phase < 0.56:
            return outer
        t = (phase - 0.56) / 0.44
        return outer + (root - outer) * t

    ring_top: list[int] = []
    ring_bot: list[int] = []
    ring_in_top: list[int] = []
    ring_in_bot: list[int] = []
    for k in range(n):
        ang = 2.0 * math.pi * k / n
        r = profile_radius(k)
        ca, sa = math.cos(ang), math.sin(ang)
        ring_top.append(mb.add_vert((r * ca, hz, r * sa)))
        ring_bot.append(mb.add_vert((r * ca, -hz, r * sa)))
        ring_in_top.append(mb.add_vert((bore * ca, hz, bore * sa)))
        ring_in_bot.append(mb.add_vert((bore * ca, -hz, bore * sa)))

    for k in range(n):
        k2 = (k + 1) % n
        # 外壁
        mb.add_quad(ring_top[k], ring_top[k2], ring_bot[k2], ring_bot[k])
        # 内壁（朝向轴心，故顺序相反）
        mb.add_quad(ring_in_top[k2], ring_in_top[k], ring_in_bot[k], ring_in_bot[k2])
        # 顶面环带
        mb.add_quad(ring_in_top[k], ring_in_top[k2], ring_top[k2], ring_top[k])
        # 底面环带
        mb.add_quad(ring_top[k], ring_top[k2], ring_in_top[k2], ring_in_top[k])
    return mb


def make_tube(segments: int = 64, bend_segments: int = 56,
              radius: float = 0.26, bend_radius: float = 1.0,
              arc: float = math.pi * 1.35) -> MeshBuilder:
    """弯管：沿圆弧扫掠圆截面（圆周 × 扫掠 的规则网格）。

    用途：产品片里的「管线/线缆/流道」几乎都是这个形状，
    而内置图元一个都做不出来（cylinder 是直的）。
    """
    mb = MeshBuilder()
    rings: list[list[int]] = []
    for i in range(bend_segments + 1):
        t = i / float(bend_segments)
        a = arc * t
        # 弯管中心线在 XY 平面内绕 Y 轴弯
        cxy = (bend_radius * math.cos(a), 0.0, bend_radius * math.sin(a))
        # 该处的切向与法向
        tangent = (-math.sin(a), 0.0, math.cos(a))
        normal = (math.cos(a), 0.0, math.sin(a))
        binormal = (0.0, 1.0, 0.0)
        ring: list[int] = []
        for j in range(segments):
            th = 2.0 * math.pi * j / segments
            ct, st = math.cos(th), math.sin(th)
            p = (
                cxy[0] + radius * (ct * normal[0] + st * binormal[0]),
                cxy[1] + radius * (ct * normal[1] + st * binormal[1]),
                cxy[2] + radius * (ct * normal[2] + st * binormal[2]),
            )
            ring.append(mb.add_vert(p))
        rings.append(ring)
    for i in range(bend_segments):
        r0, r1 = rings[i], rings[i + 1]
        for j in range(segments):
            j2 = (j + 1) % segments
            mb.add_quad(r0[j], r0[j2], r1[j2], r1[j])
    # 两端封口（扇形）
    for ring, flip in ((rings[0], True), (rings[-1], False)):
        center = (
            sum(mb.verts[i][0] for i in ring) / segments,
            sum(mb.verts[i][1] for i in ring) / segments,
            sum(mb.verts[i][2] for i in ring) / segments,
        )
        ci = mb.add_vert(center)
        for j in range(segments):
            j2 = (j + 1) % segments
            if flip:
                mb.add_tri(ci, ring[j2], ring[j])
            else:
                mb.add_tri(ci, ring[j], ring[j2])
    return mb


def make_capsule(segments: int = 56, rings: int = 34,
                 radius: float = 0.55, height: float = 1.2) -> MeshBuilder:
    """胶囊体：圆柱 + 两端半球。"""
    mb = MeshBuilder()
    half = height * 0.5
    loops: list[list[int]] = []
    # 上半球 → 圆柱 → 下半球，用统一环列表
    for i in range(rings + 1):
        t = i / float(rings)
        if t < 0.5:
            # 上半球：phi 从 0 到 pi/2
            phi = (t / 0.5) * (math.pi * 0.5)
            y = half + radius * math.cos(phi)
            r = radius * math.sin(phi)
        else:
            phi = ((t - 0.5) / 0.5) * (math.pi * 0.5)
            y = -half - radius * math.sin(phi)
            r = radius * math.cos(phi)
        ring: list[int] = []
        for j in range(segments):
            a = 2.0 * math.pi * j / segments
            ring.append(mb.add_vert((r * math.cos(a), y, r * math.sin(a))))
        loops.append(ring)
    for i in range(rings):
        r0, r1 = loops[i], loops[i + 1]
        for j in range(segments):
            j2 = (j + 1) % segments
            mb.add_quad(r0[j], r0[j2], r1[j2], r1[j])
    return mb


def make_hexprism(sides: int = 6, radius: float = 0.8,
                  height: float = 1.4, bevel: float = 0.10) -> MeshBuilder:
    """棱柱（默认六棱柱）：带倒角。

    倒角的意义：没有倒角的硬边在着色器里是一条「亮度突变线」，
    在 3D 里读起来像低模；倒一个 45° 小面就能让高光顺着它走，
    这是产品渲染里最便宜的一处「高级感」。
    """
    mb = MeshBuilder()
    hz = height * 0.5
    outer_top, outer_bot, ch_top, ch_bot = [], [], [], []
    for j in range(sides):
        a = 2.0 * math.pi * j / sides + math.pi / sides
        ca, sa = math.cos(a), math.sin(a)
        rin = radius - bevel
        outer_top.append(mb.add_vert((radius * ca, hz - bevel, radius * sa)))
        outer_bot.append(mb.add_vert((radius * ca, -hz + bevel, radius * sa)))
        ch_top.append(mb.add_vert((rin * ca, hz, rin * sa)))
        ch_bot.append(mb.add_vert((rin * ca, -hz, rin * sa)))
    for j in range(sides):
        j2 = (j + 1) % sides
        mb.add_quad(outer_top[j], outer_top[j2], outer_bot[j2], outer_bot[j])
        mb.add_quad(ch_top[j], ch_top[j2], outer_top[j2], outer_top[j])
        mb.add_quad(outer_bot[j], outer_bot[j2], ch_bot[j2], ch_bot[j])
    # 顶/底盖（扇形）
    for ring, flip in ((ch_top, False), (ch_bot, True)):
        cx = sum(mb.verts[i][0] for i in ring) / sides
        cy = sum(mb.verts[i][1] for i in ring) / sides
        cz = sum(mb.verts[i][2] for i in ring) / sides
        ci = mb.add_vert((cx, cy, cz))
        for j in range(sides):
            j2 = (j + 1) % sides
            if flip:
                mb.add_tri(ci, ring[j2], ring[j])
            else:
                mb.add_tri(ci, ring[j], ring[j2])
    return mb


def make_torusknot(segments: int = 200, sides: int = 28,
                   p: int = 2, q: int = 5,
                   radius: float = 1.0, tube: float = 0.30) -> MeshBuilder:
    """环面结：(p,q) 结 —— 一条绕环面缠绕的管。

    它在演示片里特别有用：结构复杂但完全规则、能缓慢自转、任何角度都有
    自遮挡层次，是「这东西不只是个球」最省事的证据。
    """
    mb = MeshBuilder()
    rings: list[list[int]] = []
    for i in range(segments):
        t = 2.0 * math.pi * i / segments
        # 结的中心线
        r = radius * (2.0 + math.cos(q * t / p)) * 0.5
        cx = r * math.cos(t)
        cz = r * math.sin(t)
        cy = radius * math.sin(q * t / p) * 0.5
        # 数值切向（前后差分，比解析式省事且不易错）
        dt = 1e-3
        t2 = t + dt
        r2 = radius * (2.0 + math.cos(q * t2 / p)) * 0.5
        c2 = (r2 * math.cos(t2), radius * math.sin(q * t2 / p) * 0.5, r2 * math.sin(t2))
        tang = (c2[0] - cx, c2[1] - cy, c2[2] - cz)
        # 用世界 Y 做参考向量构正交基
        ref = (0.0, 1.0, 0.0)
        if abs(tang[1]) > 0.9:
            ref = (1.0, 0.0, 0.0)
        n1 = _cross(tang, ref)
        ln = math.sqrt(_dot(n1, n1)) or 1.0
        n1 = (n1[0] / ln, n1[1] / ln, n1[2] / ln)
        n2 = _cross(tang, n1)
        ln2 = math.sqrt(_dot(n2, n2)) or 1.0
        n2 = (n2[0] / ln2, n2[1] / ln2, n2[2] / ln2)
        ring: list[int] = []
        for j in range(sides):
            a = 2.0 * math.pi * j / sides
            ca, sa = math.cos(a), math.sin(a)
            pnt = (
                cx + tube * (ca * n1[0] + sa * n2[0]),
                cy + tube * (ca * n1[1] + sa * n2[1]),
                cz + tube * (ca * n1[2] + sa * n2[2]),
            )
            ring.append(mb.add_vert(pnt))
        rings.append(ring)
    for i in range(segments):
        r0, r1 = rings[i], rings[(i + 1) % segments]
        for j in range(sides):
            j2 = (j + 1) % sides
            mb.add_quad(r0[j], r0[j2], r1[j2], r1[j])
    return mb


def main(argv: list[str]) -> int:
    out_dir = argv[1] if len(argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)
    specs = [
        # smooth=False：有棱角的机械件。齿/平面之间要保持锐利。
        ("gear", make_gear(), False),
        ("hexprism", make_hexprism(), False),
        # smooth=True：曲面体。不写 vn 会出现一圈圈横向亮带。
        ("tube", make_tube(), True),
        ("capsule", make_capsule(), True),
        ("torusknot", make_torusknot(), True),
    ]
    for name, mb, smooth in specs:
        path = os.path.join(out_dir, name + ".obj")
        mb.write(path, name, smooth)
        size = os.path.getsize(path)
        mode = "平滑" if smooth else "硬边"
        print(f"{name:10s} 顶点 {len(mb.verts):6d}  三角形 {len(mb.faces):6d}  "
              f"{mode}  {size/1024:.1f} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
