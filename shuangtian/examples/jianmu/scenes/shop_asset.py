"""资产库 · bpy 版：小卖部的零件

这是「bpy 做资产、three.js 组装」工作流的资产端（同 `gears_asset.py`）。
本文件**不组成场景**，只产出可复用的零件几何，由
`tools/bpy_export_asset` 导出成 JSON，再由 `scene_shop.js` 用 three.js 组装。

## 尺度（米）

这是**真实尺度**的小卖部，不是夸张的展示模型：

| 项 | 值 |
|---|---|
| 店内宽 | 3.60 |
| 店内深 | 2.40 |
| 层高 | 2.50 |
| 墙厚 | 0.14 |
| 货架高 | 1.70（4 层） |
| 柜台高 | 0.92 |

## 「娃娃屋」视角

左墙、右墙、后墙齐全，但**前面完全开放**（不建前墙）。这样相机从
斜前方看进去，既能看到门面（招牌、遮阳棚、门槛），也能看到店内
（货架、商品、柜台、冰柜）。

真实的封闭店铺从外面看只有一个门洞，信息量太低；而这个示例要展示
的是「bpy 能产什么 + js 能组装出什么」。

## 产出的零件

| 名称 | 说明 |
|---|---|
| `wall_back` / `wall_side` | 后墙 / 侧墙（含窗洞的是 `wall_window`） |
| `floor_slab` | 地台（略大于店内，做出踏面） |
| `roof_slab` | 屋顶（带薄檐） |
| `awning` | 遮阳棚（斜面 + 边条） |
| `sign_board` / `sign_glyph` | 招牌底板 / 招牌上的字块 |
| `shelf` | 货架（4 层 + 2 立柱 + 背板） |
| `counter` | 柜台（台面 + 前挡 + 侧板） |
| `register` | 收银机（机身 + 屏） |
| `fridge` | 冰柜（柜体 + 玻璃门框） |
| `bottle` / `can` / `box_item` / `bag_item` | 商品 |
| `crate` | 周转箱 |
| `step` | 门槛台阶 |
| `lamp_post` / `lamp_head` | 路灯 |
| `door_frame` | 门框（立在前面开口处） |

## 兼容层限制（第一版踩到的）

| API | 状态 | 绕法 |
|---|---|---|
| `bm.normal_update()` | 未实现 | `bm.to_mesh()` 内部会算 |
| `poly.normal` | 未实现 | 无法按法线挑选平滑面 → 全平滑或全平面 |
| `obj.scale`（bpy 侧） | 不作用于 `matrix_world` | **在 bpy 里按实际尺寸建几何**，组装端再用 three 的 `mesh.scale`（那条可用） |
| `primitive_uv_sphere_add` | 几何异常（顶点超半径） | 用圆柱/锥台代替球 |
"""

import math

import bmesh
import bpy
from mathutils import Euler, Vector

TAU = 6.283185307179586


# ── 材质 ────────────────────────────────────────────────────────────────
def material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


# 颜色是**线性**值（与 three 的 `new Color(r,g,b)` 同口径）
MAT_WALL = material('wall', (0.34, 0.32, 0.29), 0.0, 0.85)         # 水泥墙
MAT_WALL_WARM = material('wall_warm', (0.28, 0.21, 0.17), 0.0, 0.80)  # 砖色
MAT_FLOOR = material('floor', (0.16, 0.15, 0.14), 0.0, 0.75)       # 水泥地
MAT_ROOF = material('roof', (0.18, 0.21, 0.23), 0.65, 0.42)        # 铁皮
MAT_WOOD = material('wood', (0.13, 0.075, 0.040), 0.0, 0.60)       # 木
MAT_WOOD_LIGHT = material('wood_light', (0.28, 0.17, 0.09), 0.0, 0.65)
MAT_METAL = material('metal', (0.30, 0.32, 0.35), 0.88, 0.30)      # 金属
MAT_GLASS = material('glass', (0.42, 0.52, 0.58), 0.15, 0.08)      # 玻璃
MAT_SIGN_RED = material('sign_red', (0.48, 0.055, 0.045), 0.0, 0.55)
MAT_SIGN_CREAM = material('sign_cream', (0.82, 0.74, 0.56), 0.0, 0.60)
MAT_WHITE = material('white', (0.72, 0.73, 0.72), 0.0, 0.55)
MAT_BOTTLE_G = material('bottle_green', (0.055, 0.22, 0.10), 0.0, 0.20)
MAT_BOTTLE_B = material('bottle_blue', (0.045, 0.14, 0.30), 0.0, 0.20)
MAT_CAN_RED = material('can_red', (0.42, 0.045, 0.035), 0.75, 0.28)
MAT_CAN_Y = material('can_yellow', (0.62, 0.42, 0.045), 0.75, 0.28)
MAT_BOX = material('box', (0.32, 0.18, 0.05), 0.0, 0.75)
MAT_BAG = material('bag', (0.52, 0.10, 0.06), 0.0, 0.55)
MAT_CRATE = material('crate', (0.09, 0.20, 0.28), 0.0, 0.70)


# ── 通用几何助手 ────────────────────────────────────────────────────────
def _add_box(bm, sx, sy, sz, cx=0.0, cy=0.0, cz=0.0):
    """往 bm 里加一个盒子（中心在 cx,cy,cz）。返回顶点列表。"""
    hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
    v = []
    for dz in (-hz, hz):
        for dy, dx in ((-hy, -hx), (-hy, hx), (hy, hx), (hy, -hx)):
            v.append(bm.verts.new((cx + dx, cy + dy, cz + dz)))
    bm.verts.index_update()
    # 底面 / 顶面 / 四个侧面（逆时针朝外）
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5),
             (2, 3, 7, 6), (3, 0, 4, 7)]
    for f in faces:
        try:
            bm.faces.new((v[f[0]], v[f[1]], v[f[2]], v[f[3]]))
        except ValueError:
            pass
    return v


def _add_ring(bm, radius, z, segments, cx=0.0, cy=0.0, radius_y=None, phase=0.0):
    """在高度 z 处加一圈顶点（可做椭圆）。返回顶点列表。"""
    ry = radius if radius_y is None else radius_y
    out = []
    for i in range(segments):
        a = phase + TAU * i / segments
        out.append(bm.verts.new((cx + math.cos(a) * radius, cy + math.sin(a) * ry, z)))
    bm.verts.index_update()
    return out


def _bridge(bm, lower, upper):
    """把两圈顶点用四边形连起来（侧面）。"""
    n = len(lower)
    for i in range(n):
        j = (i + 1) % n
        try:
            bm.faces.new((lower[i], lower[j], upper[j], upper[i]))
        except ValueError:
            pass


def _cap(bm, ring, center, flip=False):
    """把一圈顶点用扇形封口。"""
    n = len(ring)
    for i in range(n):
        j = (i + 1) % n
        try:
            if flip:
                bm.faces.new((center, ring[j], ring[i]))
            else:
                bm.faces.new((center, ring[i], ring[j]))
        except ValueError:
            pass


def _finish(name, bm, mat, smooth=True):
    """把 bm 落成 Object 并挂材质。"""
    bm.verts.index_update()
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for poly in mesh.polygons:
        poly.use_smooth = smooth
    obj = bpy.data.objects.new(name, mesh)
    obj.material_slots.append(mat)
    return obj


def make_box(name, sx, sy, sz, mat, smooth=False):
    bm = bmesh.new()
    _add_box(bm, sx, sy, sz)
    return _finish(name, bm, mat, smooth)


def make_cylinder(name, radius, height, mat, segments=20, radius_top=None,
                  smooth=True, cap=True):
    """圆柱 / 圆台。轴沿 **Z**（bpy 约定）。"""
    rt = radius if radius_top is None else radius_top
    bm = bmesh.new()
    hz = height * 0.5
    bottom = _add_ring(bm, radius, -hz, segments)
    top = _add_ring(bm, rt, hz, segments)
    _bridge(bm, bottom, top)
    if cap:
        cb = bm.verts.new((0.0, 0.0, -hz))
        ct = bm.verts.new((0.0, 0.0, hz))
        bm.verts.index_update()
        _cap(bm, bottom, cb, flip=True)
        _cap(bm, top, ct, flip=False)
    return _finish(name, bm, mat, smooth)


# ── 建筑主体 ────────────────────────────────────────────────────────────
INNER_W = 3.60
INNER_D = 2.40
WALL_H = 2.50
WALL_T = 0.14

# 地台：比店内略大，做出踏面
floor_slab = make_box('floor_slab', INNER_W + 0.36, INNER_D + 0.36, 0.12, MAT_FLOOR)

# 后墙（沿 X 方向展开）
wall_back = make_box('wall_back', INNER_W + WALL_T * 2.0, WALL_T, WALL_H, MAT_WALL_WARM)

# 侧墙（沿 Y 方向展开）
wall_side = make_box('wall_side', WALL_T, INNER_D, WALL_H, MAT_WALL)

# 带窗洞的侧墙：用四块拼成一个"口"字（窗洞 0.9 × 0.8）
def make_window_wall(name):
    """侧墙 + 窗洞：用四块板拼成「口」字（比布尔运算可靠）。

    ## 坐标约定（这一版算错过，记下来）

    板要铺在 **y ∈ [-INNER_D/2, +INNER_D/2]**、**z ∈ [0, WALL_H]** 的矩形里，
    中间挖掉一块窗洞。四块板：

        ┌──────────┬──────────┬──────────┐
        │          │  窗上块  │          │   z: win_z1 .. WALL_H
        │  左块    ├──────────┤   右块   │
        │          │  窗洞    │          │   z: win_z0 .. win_z1
        │          ├──────────┤          │
        │          │  窗下块  │          │   z: 0 .. win_z0
        └──────────┴──────────┴──────────┘
          y: -D/2  win_y0   win_y1  +D/2

    每块的中心 = (起点 + 终点) / 2，尺寸 = 终点 - 起点。

    ## 上一版的错

    左/右块的 y 中心算成了 `win_y0 * 0.5 - D/2`（少加了起点偏移），
    结果整面墙的 y 范围跑到 [-1.20, **1.80**]（应当 [-1.20, 1.20]），
    多出 0.6 米——在画面上表现为**一侧墙伸出去一截**。

    判据：拼接出来的几何，**总包围盒必须等于设计尺寸**。这次的教训是
    拼完没有验证包围盒。
    """
    bm = bmesh.new()
    half_d = INNER_D * 0.5

    win_w = 0.90                     # 窗宽（沿 Y）
    win_h = 0.80                     # 窗高（沿 Z）
    win_z0 = 1.15                    # 窗台高
    win_z1 = win_z0 + win_h          # 窗顶高
    win_y0 = -win_w * 0.5            # 窗口在 Y 方向居中
    win_y1 = win_w * 0.5

    def slab(y_from, y_to, z_from, z_to):
        """按区间放一块板（避免手算中心点出错的封装）。"""
        cy = (y_from + y_to) * 0.5
        cz = (z_from + z_to) * 0.5
        sy = y_to - y_from
        sz = z_to - z_from
        if sy <= 0.0 or sz <= 0.0:
            return
        _add_box(bm, WALL_T, sy, sz, 0.0, cy, cz)

    # 左块：y ∈ [-D/2, win_y0]，整高
    slab(-half_d, win_y0, 0.0, WALL_H)
    # 右块：y ∈ [win_y1, +D/2]，整高
    slab(win_y1, half_d, 0.0, WALL_H)
    # 窗下块：y ∈ [win_y0, win_y1]，z ∈ [0, win_z0]
    slab(win_y0, win_y1, 0.0, win_z0)
    # 窗上块：y ∈ [win_y0, win_y1]，z ∈ [win_z1, WALL_H]
    slab(win_y0, win_y1, win_z1, WALL_H)

    return _finish(name, bm, MAT_WALL, smooth=False)


wall_window = make_window_wall('wall_window')

# 屋顶：略大于墙体，做出薄檐
roof_slab = make_box('roof_slab', INNER_W + 0.56, INNER_D + 0.56, 0.10, MAT_ROOF)

# 遮阳棚：斜面（用薄板 + 旋转在组装端做；这里做成平薄板）
awning = make_box('awning', INNER_W + 0.20, 1.10, 0.055, MAT_SIGN_RED)

# 门槛台阶
step = make_box('step', INNER_W - 0.20, 0.42, 0.16, MAT_WALL)

# 门框（立在前面开口的左侧，做出"有门"的暗示）
door_frame = make_box('door_frame', 0.10, 0.10, 2.10, MAT_WOOD)

# 窗框（薄边框，贴在窗洞上）
def make_window_frame(name):
    bm = bmesh.new()
    w, h, t = 0.94, 0.84, 0.05
    # 四条边
    _add_box(bm, t, w, 0.05, 0.0, 0.0, h * 0.5)    # 上
    _add_box(bm, t, w, 0.05, 0.0, 0.0, -h * 0.5)   # 下
    _add_box(bm, t, 0.05, h, 0.0, w * 0.5, 0.0)    # 前
    _add_box(bm, t, 0.05, h, 0.0, -w * 0.5, 0.0)   # 后
    # 中竖梃
    _add_box(bm, t, 0.04, h, 0.0, 0.0, 0.0)
    return _finish(name, bm, MAT_WHITE, smooth=False)


window_frame = make_window_frame('window_frame')


# ── 招牌 ────────────────────────────────────────────────────────────────
sign_board = make_box('sign_board', 3.30, 0.12, 0.64, MAT_SIGN_RED)

# 招牌"字块"：三个凸起的小方块，抽象表达文字（不做真字形）
def make_sign_glyphs(name):
    bm = bmesh.new()
    for i in range(3):
        x = (i - 1) * 0.62
        _add_box(bm, 0.40, 0.06, 0.30, x, 0.0, 0.0)
    return _finish(name, bm, MAT_SIGN_CREAM, smooth=False)


sign_glyph = make_sign_glyphs('sign_glyph')


# ── 店内家具 ────────────────────────────────────────────────────────────
def make_shelf(name):
    """货架：2 立柱 + 背板 + 4 层板。

    尺寸 1.60（宽） × 0.42（深） × 1.70（高），4 层。
    层板间距 0.36，最下层离地 0.10。
    """
    bm = bmesh.new()
    w, d, h = 1.60, 0.42, 1.70
    shelf_t = 0.030
    post_t = 0.045

    # 立柱（四角）
    for sx in (-1, 1):
        for sy in (-1, 1):
            _add_box(bm, post_t, post_t, h,
                     sx * (w * 0.5 - post_t * 0.5),
                     sy * (d * 0.5 - post_t * 0.5), h * 0.5)

    # 层板
    z = 0.10
    while z <= h + 0.001:
        _add_box(bm, w, d, shelf_t, 0.0, 0.0, z)
        z += 0.36

    # 背板（薄）
    _add_box(bm, w, 0.020, h - 0.10, 0.0, d * 0.5, h * 0.5)
    return _finish(name, bm, MAT_METAL, smooth=False)


shelf = make_shelf('shelf')


def make_counter(name):
    """柜台：台面 + 前挡板 + 两侧板。尺寸 1.50 × 0.62 × 0.92。"""
    bm = bmesh.new()
    w, d, h = 1.50, 0.62, 0.92
    top_t = 0.06
    panel_t = 0.05

    _add_box(bm, w, d, top_t, 0.0, 0.0, h - top_t * 0.5)          # 台面
    _add_box(bm, w, panel_t, h - top_t, 0.0, -d * 0.5 + panel_t * 0.5,
             (h - top_t) * 0.5)                                    # 前挡
    for sx in (-1, 1):
        _add_box(bm, panel_t, d, h - top_t, sx * (w * 0.5 - panel_t * 0.5),
                 0.0, (h - top_t) * 0.5)                          # 侧板
    # 前挡上的一条装饰线（浅色木）
    return _finish(name, bm, MAT_WOOD, smooth=False)


counter = make_counter('counter')

# 柜台台面（浅色木，单独一个资产方便换材质）
counter_top = make_box('counter_top', 1.54, 0.66, 0.05, MAT_WOOD_LIGHT)


def make_register(name):
    """收银机：机身 + 斜屏 + 钱箱。"""
    bm = bmesh.new()
    _add_box(bm, 0.34, 0.30, 0.10, 0.0, 0.0, 0.05)        # 钱箱
    _add_box(bm, 0.28, 0.22, 0.12, 0.0, 0.0, 0.16)        # 机身
    _add_box(bm, 0.22, 0.03, 0.16, 0.0, -0.10, 0.26)      # 屏（立起来）
    return _finish(name, bm, MAT_METAL, smooth=False)


register = make_register('register')


def make_fridge(name):
    """冰柜：柜体 + 顶盖 + 玻璃门框。尺寸 0.92 × 0.62 × 1.35。"""
    bm = bmesh.new()
    w, d, h = 0.92, 0.62, 1.35
    _add_box(bm, w, d, h, 0.0, 0.0, h * 0.5)              # 柜体
    _add_box(bm, w + 0.04, d + 0.04, 0.06, 0.0, 0.0, h)   # 顶盖
    return _finish(name, bm, MAT_WHITE, smooth=False)


fridge = make_fridge('fridge')

# 冰柜玻璃门（贴在正面）
fridge_glass = make_box('fridge_glass', 0.78, 0.03, 1.05, MAT_GLASS)


# ── 商品 ────────────────────────────────────────────────────────────────
def make_bottle(name, mat):
    """瓶子：瓶身（圆柱）+ 收窄的瓶颈 + 瓶盖。轴沿 Z。"""
    bm = bmesh.new()
    seg = 14
    h_body = 0.175
    h_neck = 0.055
    z0 = -0.12
    # 瓶身
    b = _add_ring(bm, 0.036, z0, seg)
    m = _add_ring(bm, 0.036, z0 + h_body, seg)
    _bridge(bm, b, m)
    cb = bm.verts.new((0.0, 0.0, z0))
    bm.verts.index_update()
    _cap(bm, b, cb, flip=True)
    # 肩 + 颈
    m2 = _add_ring(bm, 0.016, z0 + h_body + h_neck, seg)
    _bridge(bm, m, m2)
    # 瓶盖
    m3 = _add_ring(bm, 0.018, z0 + h_body + h_neck + 0.018, seg)
    _bridge(bm, m2, m3)
    ct = bm.verts.new((0.0, 0.0, z0 + h_body + h_neck + 0.018))
    bm.verts.index_update()
    _cap(bm, m3, ct, flip=False)
    return _finish(name, bm, mat, smooth=True)


bottle_green = make_bottle('bottle_green', MAT_BOTTLE_G)
bottle_blue = make_bottle('bottle_blue', MAT_BOTTLE_B)


def make_can(name, mat):
    """易拉罐：圆柱，略矮胖。"""
    bm = bmesh.new()
    seg = 14
    r, h = 0.033, 0.115
    b = _add_ring(bm, r, -h * 0.5, seg)
    t = _add_ring(bm, r, h * 0.5, seg)
    _bridge(bm, b, t)
    cb = bm.verts.new((0.0, 0.0, -h * 0.5))
    ct = bm.verts.new((0.0, 0.0, h * 0.5))
    bm.verts.index_update()
    _cap(bm, b, cb, flip=True)
    _cap(bm, t, ct, flip=False)
    return _finish(name, bm, mat, smooth=True)


can_red = make_can('can_red', MAT_CAN_RED)
can_yellow = make_can('can_yellow', MAT_CAN_Y)


def make_box_item(name, mat):
    """商品盒（零食盒）：长方体，略薄。"""
    return make_box(name, 0.16, 0.055, 0.22, mat, smooth=False)


box_item = make_box_item('box_item', MAT_BOX)

# 商品袋（薯片袋）：扁的薄盒 + 顶部收窄感用一个薄盒近似
bag_item = make_box('bag_item', 0.18, 0.075, 0.24, MAT_BAG, smooth=False)

# 周转箱：开口盒（四个侧面 + 底）
def make_crate(name):
    bm = bmesh.new()
    w, d, h, t = 0.42, 0.32, 0.20, 0.022
    _add_box(bm, w, d, t, 0.0, 0.0, t * 0.5)               # 底
    for sy in (-1, 1):
        _add_box(bm, w, t, h, 0.0, sy * (d * 0.5 - t * 0.5), h * 0.5)
    for sx in (-1, 1):
        _add_box(bm, t, d, h, sx * (w * 0.5 - t * 0.5), 0.0, h * 0.5)
    return _finish(name, bm, MAT_CRATE, smooth=False)


crate = make_crate('crate')


# ── 路灯 ────────────────────────────────────────────────────────────────
lamp_post = make_cylinder('lamp_post', 0.045, 3.20, MAT_METAL, segments=12)


def make_lamp_head(name):
    """灯头：锥台 + 底圈。"""
    bm = bmesh.new()
    seg = 12
    b = _add_ring(bm, 0.075, 0.0, seg)
    t = _add_ring(bm, 0.135, 0.16, seg)
    _bridge(bm, b, t)
    cb = bm.verts.new((0.0, 0.0, 0.0))
    ct = bm.verts.new((0.0, 0.0, 0.16))
    bm.verts.index_update()
    _cap(bm, b, cb, flip=True)
    _cap(bm, t, ct, flip=False)
    return _finish(name, bm, MAT_METAL, smooth=True)


lamp_head = make_lamp_head('lamp_head')

result = 'shop_assets'
