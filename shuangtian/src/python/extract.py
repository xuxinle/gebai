"""霜天 · bpy 场景抽取器（Python 侧）

> 本模块由 `bpy_module.py` 在装配时用 `bind()` 注入宿主（`bpy.data` /
> `bpy.context` / 各个类型），因此它不依赖任何全局变量，也能独立测试。

## 职责

把 `bpy.data` 里的场景抽成一份**扁平的、可序列化的**结构，交给 C++ 侧
搬进 `ThreeScene`。

    bpy.data.objects ──→ extract() ──→ dict ──→ JSON ──→ ThreeScene

## 为什么抽取写在 Python 里

C++ 侧读 bpy 对象要逐层调 MicroPython API：

    mp_obj_t obj = mp_obj_dict_get(...);
    mp_obj_t loc = mp_load_attr(obj, MP_QSTR_location);   // 每个属性一次查表
    mp_obj_t x   = mp_load_attr(loc, MP_QSTR_x);          // 还要再查一层
    double vx = mp_obj_get_float(x);

三层深度的对象（`obj.data.vertices[0].co.x`）就是四五次查表 + 类型检查，
而错误处理在每个调用点都要写。代码量极大、极易错。

Python 侧写就是自然表达：

    for v in obj.data.vertices:
        out.append(v.co.x)

所以分工是：**Python 负责理解 bpy，C++ 负责搬数据**。这个边界让两边都短。

## 三角化：扇形

本框架渲染只吃三角形，而 bpy 的面可以是任意多边形（`primitive_cube_add`
给的是四边形，bmesh 建的可能更多）。

用**扇形**（`(v0, vi, vi+1)`）而不是"随便切"：
- 凸多边形：扇形完全正确
- 凹多边形：扇形会得到跨出边界的三角形（**已知边界**，写在 docs/python.md）

不做耳切（ear clipping）：那要在 Python 里写一套几何判定，而 bpy 脚本
建的面绝大多数是凸的（基本体端盖、bmesh 的规则网格）。等真遇到凹多边形
再加，那时会有具体的失败例子可对照。

## 矩阵展开成 16 个数

`ThreeObject::world` 要 `math::Mat4`。Python 侧把 4×4 展开成 16 个 float
（**列主序**，与本框架 `Mat4` 布局一致），C++ 侧直接 memcpy 式填。
"""


# 宿主（由 `bpy_module.py` 的 `bind()` 注入）
_host = {}


def bind(data, context, mesh_type, camera_type, light_type):
    """注入宿主。

    ## 为什么用注入而不是直接 import bpy

    `extract` 是**内部工具**（模块名 `_st_extract`），它不该让用户脚本
    感知到。而如果它 `import bpy`，那就得保证 import 顺序（bpy 要先注册）。
    注入把依赖显式化，也让它能在测试里对着假宿主跑。

    参数里传 `camera_type` / `light_type` 而不是让调用方自己去比：
    那两个类型定义在 `bpy_data.py`，抽取器不该关心它们叫什么名字。
    """
    global Camera, Light
    _host['data'] = data
    _host['context'] = context
    _host['mesh_type'] = mesh_type
    Camera = camera_type
    Light = light_type


def _flatten_matrix(m):
    """4×4 矩阵 → 16 个数（列主序）。

    ## 为什么要转置

    bpy 的 `Matrix[i][j]` 是**行主序**（i 是行），而本框架的 `Mat4`
    是**列主序**（与 OpenGL/three.js 一致）。两者互为转置。

    漏了这一步的症状：物体"位置对了但旋转方向反了"，或者缩放被错位
    应用到别的轴上——比完全不出现还难查。
    """
    out = []
    for col in range(4):
        for row in range(4):
            out.append(float(m[row][col]))
    return out


def _triangulate(polygon_vertices):
    """把多边形三角化（扇形）。返回顶点索引三元组列表。

    `polygon_vertices` 是一个索引列表（长度 >= 3）。
    """
    n = len(polygon_vertices)
    if n < 3:
        return []
    if n == 3:
        return [(polygon_vertices[0], polygon_vertices[1], polygon_vertices[2])]
    tris = []
    first = polygon_vertices[0]
    for i in range(1, n - 1):
        tris.append((first, polygon_vertices[i], polygon_vertices[i + 1]))
    return tris


def _compute_normal(p0, p1, p2):
    """三角形法线（右手规则）。退化三角形返回 (0,0,1)。"""
    ax = p1[0] - p0[0]
    ay = p1[1] - p0[1]
    az = p1[2] - p0[2]
    bx = p2[0] - p0[0]
    by = p2[1] - p0[1]
    bz = p2[2] - p0[2]
    nx = ay * bz - az * by
    ny = az * bx - ax * bz
    nz = ax * by - ay * bx
    length = (nx * nx + ny * ny + nz * nz) ** 0.5
    if length < 1e-12:
        return (0.0, 0.0, 1.0)
    return (nx / length, ny / length, nz / length)


def _extract_mesh_object(obj):
    """一个带 mesh 的 bpy Object → 扁平 dict（顶点表 + 索引 + 材质 + 世界矩阵）。

    返回 None 表示"不可导出"（不是 mesh 对象、没有顶点、不可见）。

    ## 为何要显式判“是不是 Mesh”而不是只看 `.data`

    `bpy.data.objects` 里**什么都能放**：mesh 对象、相机、灯光、空物体。
    而后三者没有 `.vertices`。只判 `data is None` 不够——相机有 data
    （`Camera` 对象），但读 `.vertices` 不是 `None` 而是 `AttributeError`。

    症状：相机被当 mes​h 抽取时抛异常，被外层 try 捕获成一条 warn
    （“对象 'cam' 抽取失败：no such attribute”）——功能上没错（相机本来就
    不该当几何），但**日志里多一条假警告**，而假警告会淹没真警告。
    """
    mesh = obj.data
    if not isinstance(mesh, _host['mesh_type']):
        return None
    if len(mesh.vertices) == 0:
        return None
    if not obj.visible:
        return None

    world = _flatten_matrix(obj.matrix_world)
    m = obj.matrix_world

    # ## 顶点：位置直接取，法线由三角形算
    #
    # 为什么不在 `mesh.vertices` 上存法线：真实 bpy 的顶点法线是
    # `mesh.vertex_normals`（需要 `calc_normals()`），而我们的兼容层
    # 没实现它。**逐三角形算法线**足够：本框架的着色是逐顶点插值，
    # 而同一个顶点被多个三角形共享时会得到平均法线——这正是平滑着色
    # 想要的结果，且不需要额外数据结构。
    positions = []
    for v in mesh.vertices:
        co = v.co
        positions.append((float(co.x), float(co.y), float(co.z)))

    # 世界变换用 Python 侧算好（C++ 只搬数）
    #
    # ⚠ 这里**不**把顶点变换到世界空间：`ThreeObject::world` 是
    # 变换矩阵，渲染器会用它。把顶点也变换一次就是变换两遍。
    # 见 docs/python.md 的"为什么顶点留局部空间"。

    # ## 三角化 + 逐三角形法线
    vertices = []       # 交错：[x, y, z, nx, ny, nz, r, g, b]
    indices = []
    for poly in mesh.polygons:
        for tri in _triangulate(poly.vertices):
            i0, i1, i2 = tri
            p0 = positions[i0]
            p1 = positions[i1]
            p2 = positions[i2]
            n = _compute_normal(p0, p1, p2)
            for idx, p in ((i0, p0), (i1, p1), (i2, p2)):
                indices.append(len(vertices))
                vertices.append((p[0], p[1], p[2], n[0], n[1], n[2]))

    if not indices:
        return None

    material = _extract_material(obj)

    return {
        'name': obj.name,
        'positions': positions,
        'vertices': vertices,
        'indices': indices,
        'world': world,
        'material': material,
        'visible': True,
    }


def _extract_material(obj):
    """材质 → dict（PBR 子集：基色 / 金属度 / 粗糙度 / 自发光）。

    ## 只取第 0 个槽

    bpy 支持一个对象多个材质槽（按面分配）。本框架的 `Mesh` 是单一材质
    （见 `docs/three3d.md` 的说明），所以只取第 0 个。

    这是**已知边界**：多材质对象会被统一成第一个材质。写在
    `docs/python.md` 里，不静默。
    """
    mat = obj.active_material
    if mat is None:
        return {
            'base_color': [0.8, 0.8, 0.8],
            'metallic': 0.0,
            'roughness': 0.5,
            'emission': 0.0,
            'opacity': 1.0,
        }
    color = mat.diffuse_color
    return {
        # diffuse_color 是 RGBA，本框架要 RGB
        'base_color': [float(color[0]), float(color[1]), float(color[2])],
        'metallic': float(mat.metallic),
        'roughness': float(mat.roughness),
        'emission': float(mat.emission_strength),
        'opacity': float(color[3]),
    }


def _extract_camera(context):
    """找场景里的相机 → dict（位置 / 目标 / fov / near / far）。

    ## 找法：优先 active_object，其次 data.cameras 第 0 个

    bpy 的"哪个相机生效"由 `scene.camera` 决定，而我们的兼容层没做
    场景相机绑定（`bpy.data.scenes` 没实现）。所以按"能找到就用"：
    先看 active_object 是不是相机，再从 `bpy.data.cameras` 取第一个。

    都没有就返回 None——C++ 侧会用记录里的默认机位，并给一条 warning。
    """
    cam_obj = None
    active = context.active_object
    if active is not None and isinstance(active.data, Camera):
        cam_obj = active
    if cam_obj is None:
        for obj in _host['data'].objects:
            if isinstance(obj.data, Camera):
                cam_obj = obj
                break
    if cam_obj is None:
        return None

    world = cam_obj.matrix_world
    eye = (float(world[0][3]), float(world[1][3]), float(world[2][3]))

    # ## 朝向：从旋转矩阵取 -Z 轴
    #
    # bpy（与所有右手系一样）相机看 **-Z**，上方向是 +Y。
    # 所以视线方向是旋转矩阵第三列的**取负**。
    #
    # ⚠ 曾经踩过：读 `world` 的平移列当方向，得到"目标就是自己"——
    # 表现为 `look_at` 失败、画面全黑。方向是**列**不是**行**。
    dx = -float(world[0][2])
    dy = -float(world[1][2])
    dz = -float(world[2][2])
    target = (eye[0] + dx, eye[1] + dy, eye[2] + dz)

    cam = cam_obj.data
    return {
        'eye': list(eye),
        'target': list(target),
        'up': [0.0, 1.0, 0.0],
        'fov_y': float(cam.angle_y),
        'near': float(cam.clip_start),
        'far': float(cam.clip_end),
    }


def _extract_lights():
    """灯光 → list（类型 / 位置 / 朝向 / 颜色 / 强度）。"""
    out = []
    for obj in _host['data'].objects:
        light = obj.data
        if not isinstance(light, Light):
            continue
        world = obj.matrix_world
        pos = (float(world[0][3]), float(world[1][3]), float(world[2][3]))
        direction = (-float(world[0][2]), -float(world[1][2]), -float(world[2][2]))
        out.append({
            'type': light.type,
            'position': list(pos),
            'direction': list(direction),
            'color': [float(light.color[0]), float(light.color[1]), float(light.color[2])],
            'energy': float(light.energy),
        })
    return out


def extract():
    """抽取整个场景。返回 dict（可直接 JSON 序列化）。

    ## 返回结构

        {
          'objects': [ {name, vertices, indices, world, material, visible}, ... ],
          'camera': {eye, target, up, fov_y, near, far} | None,
          'lights': [ {type, position, direction, color, energy}, ... ],
          'warnings': [str, ...],
        }

    `vertices` 每顶点 9 个 float（位置 3 + 法线 3 + 颜色 3）——
    与 `Mesh::kVertexFloats` 一致，C++ 侧可以直接填。
    """
    warnings = []
    objects = []

    for obj in _host['data'].objects:
        try:
            item = _extract_mesh_object(obj)
        except Exception as exc:
            # ## 单个对象出错不中止整个抽取
            #
            # 一个畸形对象（顶点数不够、材质属性缺失）不该让整个场景
            # 抽不出来——用户会看到"什么都没渲染"而不知道原因。
            # 记进 warnings，C++ 侧会汇总报出。
            warnings.append('对象 %r 抽取失败：%s' % (obj.name, exc))
            continue
        if item is not None:
            objects.append(item)

    camera = _extract_camera(_host['context'])
    lights = _extract_lights()

    if not objects:
        warnings.append('场景里没有可导出的 mesh 对象（检查脚本是否建了几何）')
    if camera is None:
        warnings.append('场景里没有相机（用 bpy.ops.object.camera_add 或 data.cameras.new）')
    if not lights:
        warnings.append('场景里没有灯光（渲染会得到全黑或仅有环境光）')

    return {
        'objects': objects,
        'camera': camera,
        'lights': lights,
        'warnings': warnings,
    }


def extract_json():
    """同 `extract()`，但返回 JSON 字符串（C++ 侧解析它）。

    ## 为什么走 JSON 而不是让 C++ 逐层读 dict

    在 C++ 里读嵌套结构（dict → list → dict → list）每一步都要
    `mp_obj_dict_get` / `mp_obj_get_array` + 类型检查 + 错误处理，
    而 JSON 只需一次解析。代价是一次序列化，相对渲染可忽略。
    """
    # ## 为何不用 `json` 模块
    #
    # `MICROPY_PY_JSON` 的默认值是 `AT_LEAST_EXTRA_FEATURES`，而本移植用
    # MINIMUM 档 → 值为 0 → `import json` 报 `ImportError: module not found`。
    #
    # 而这里要序列化的结构是**自己构造的**（只有 dict / list / str /
    # int / float / bool / None），所以自己写一个够用的序列化器：
    # 短、无依赖、出错时好查。将来若需要读 JSON（不只是写），再打开
    # `MICROPY_PY_JSON`。
    return _to_json(extract())


def _to_json(value):
    """极简 JSON 序列化（只覆盖本模块产出的类型）。"""
    if value is None:
        return 'null'
    if value is True:
        return 'true'
    if value is False:
        return 'false'
    if isinstance(value, int):
        return '%d' % value
    if isinstance(value, float):
        # ## 为什么要处理 inf / nan
        #
        # 脚本算出的退化值（除零、归一化零向量）可能变成 inf/nan，
        # 而 JSON 规范不允许它们。写成 `null` 让 C++ 侧当 0 处理，
        # 比生成非法 JSON（解析器直接失败）好。
        if value != value or value in (float('inf'), float('-inf')):
            return 'null'
        # %.9g 保证 float32 往返精度（7 位有效数字 + 余量）
        return '%.9g' % value
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, (list, tuple)):
        return '[' + ','.join(_to_json(item) for item in value) + ']'
    if isinstance(value, dict):
        parts = []
        for key in value.keys():
            parts.append(_quote(str(key)) + ':' + _to_json(value[key]))
        return '{' + ','.join(parts) + '}'
    # 其他类型（不应出现）：转成字符串而不是崩
    return _quote(str(value))


def _quote(text):
    """把一个字符串写成 JSON 字符串字面量（转义引号/反斜杠/控制字符）。"""
    out = ['"']
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20:
            out.append('\\u%04x' % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)
