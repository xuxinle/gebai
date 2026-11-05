# ============================================================================
# bpy.ops —— 操作集合（建模子集）
#
# ## bpy.ops 的语义特点
#
# `bpy.ops.mesh.primitive_cube_add()` 这样的调用**没有返回值**——它把结果
# 放进 `bpy.context`：新建的物体成为 `active_object`，并被选中。
#
# 这与 three.js 的"构造函数返回对象"完全不同，但正是 bpy 语料的写法：
#
#     bpy.ops.mesh.primitive_cone_add(vertices=32, radius1=1, depth=2)
#     cone = bpy.context.active_object
#
# 所以 `context` 不是可选的便利设施，而是**语义的一部分**——不做它就是
# 让所有 bpy 脚本都跑不起来。
# ============================================================================
class _Context:
    """`bpy.context`：记录"当前活动对象"与选择集。

    ## 为什么必须有

    bpy 的 ops 不返回值，结果只在这里。不做它，`bpy.ops.*` 的调用方
    就永远拿不到新建的对象——而那正是 bpy 脚本的标准写法。
    """

    def __init__(self):
        self.active_object = None
        self.selected_objects = []
        self.scene = None

    def _set_active(self, obj):
        self.active_object = obj
        self.selected_objects = [obj] if obj is not None else []


def _mesh_object(name, verts, faces, material=None):
    """建一个 mesh + object，塞进 bpy.data，设为 active。"""
    mesh = _data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    obj = _data.objects.new(name, mesh)
    if material is not None:
        obj.material_slots.append(material)
    _context._set_active(obj)
    return obj


# ---- 基本体生成器 ----
#
# 每个都返回 (verts, faces)。参数名与 bpy 一致（radius / depth / vertices…），
# 因为模型写的调用就是那些名字。

def _make_cube(size=2.0):
    h = size * 0.5
    verts = [
        (-h, -h, -h), (h, -h, -h), (h, h, -h), (-h, h, -h),
        (-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h),
    ]
    faces = [
        (0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
        (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7),
    ]
    return verts, faces


def _make_cylinder(vertices=32, radius=1.0, depth=2.0, radius_top=None):
    """圆柱/圆台。`radius_top` 给了就是圆台（锥台）。

    ## 绕 Z 轴（bpy 的约定）

    bpy 的 `primitive_cylinder_add` 圆柱轴沿 **Z**，而 three.js 的
    `CylinderGeometry` 沿 Y。这个差异必须照 bpy 来——脚本里写
    `rotation_euler` 时是按 Z 轴的直觉写的。
    """
    rt = radius if radius_top is None else radius_top
    hz = depth * 0.5
    verts = []
    for i in range(vertices):
        ang = 2.0 * 3.141592653589793 * i / vertices
        c, s = _cos(ang), _sin(ang)
        verts.append((c * radius, s * radius, -hz))
    for i in range(vertices):
        ang = 2.0 * 3.141592653589793 * i / vertices
        c, s = _cos(ang), _sin(ang)
        verts.append((c * rt, s * rt, hz))
    faces = []
    for i in range(vertices):
        j = (i + 1) % vertices
        faces.append((i, j, j + vertices, i + vertices))
    # 端盖（扇形三角化交给抽取器，这里给多边形）
    faces.append(tuple(range(vertices - 1, -1, -1)))
    faces.append(tuple(range(vertices, vertices * 2)))
    return verts, faces


def _make_cone(vertices=32, radius1=1.0, radius2=0.0, depth=2.0):
    """圆锥/圆台。bpy 的 `primitive_cone_add` 用 radius1/radius2。"""
    return _make_cylinder(vertices, radius1, depth, radius2)


def _make_uv_sphere(segments=32, ring_count=16, radius=1.0):
    """UV 球。

    ## 参数名与 bpy 一致

    bpy 用 `segments`（经线数）与 `ring_count`（纬线数），
    而 three.js 用 `widthSegments` / `heightSegments`。
    """
    verts = []
    faces = []
    # 两极
    verts.append((0.0, 0.0, radius))
    for r in range(1, ring_count):
        phi = 3.141592653589793 * r / ring_count
        z = radius * _cos(phi)
        rr = radius * _sin(phi)
        for s in range(segments):
            theta = 2.0 * 3.141592653589793 * s / segments
            verts.append((rr * _cos(theta), rr * _sin(theta), z))
    verts.append((0.0, 0.0, -radius))

    top = 0
    bottom = len(verts) - 1

    def ring_base(r):
        return 1 + (r - 1) * segments

    # 顶盖
    for s in range(segments):
        s2 = (s + 1) % segments
        faces.append((top, ring_base(1) + s, ring_base(1) + s2))
    # 中间带
    for r in range(1, ring_count - 1):
        base = ring_base(r)
        nxt = ring_base(r + 1)
        for s in range(segments):
            s2 = (s + 1) % segments
            faces.append((base + s, nxt + s, nxt + s2, base + s2))
    # 底盖
    last = ring_base(ring_count - 1)
    for s in range(segments):
        s2 = (s + 1) % segments
        faces.append((bottom, last + s2, last + s))
    return verts, faces


def _make_plane(size=2.0):
    h = size * 0.5
    verts = [(-h, -h, 0.0), (h, -h, 0.0), (h, h, 0.0), (-h, h, 0.0)]
    faces = [(0, 1, 2, 3)]
    return verts, faces


def _make_torus(major_radius=1.25, minor_radius=0.25,
                major_segments=48, minor_segments=12):
    """圆环。绕 Z 轴（bpy 约定）。"""
    verts = []
    faces = []
    for i in range(major_segments):
        u = 2.0 * 3.141592653589793 * i / major_segments
        cu, su = _cos(u), _sin(u)
        for j in range(minor_segments):
            v = 2.0 * 3.141592653589793 * j / minor_segments
            cv, sv = _cos(v), _sin(v)
            r = major_radius + minor_radius * cv
            verts.append((r * cu, r * su, minor_radius * sv))
    for i in range(major_segments):
        i2 = (i + 1) % major_segments
        for j in range(minor_segments):
            j2 = (j + 1) % minor_segments
            a = i * minor_segments + j
            b = i2 * minor_segments + j
            c = i2 * minor_segments + j2
            d = i * minor_segments + j2
            faces.append((a, b, c, d))
    return verts, faces


def _make_grid(x_subdivisions=10, y_subdivisions=10, size=2.0):
    h = size * 0.5
    verts = []
    for iy in range(y_subdivisions + 1):
        y = -h + size * iy / y_subdivisions
        for ix in range(x_subdivisions + 1):
            x = -h + size * ix / x_subdivisions
            verts.append((x, y, 0.0))
    faces = []
    row = x_subdivisions + 1
    for iy in range(y_subdivisions):
        for ix in range(x_subdivisions):
            a = iy * row + ix
            faces.append((a, a + 1, a + 1 + row, a + row))
    return verts, faces
