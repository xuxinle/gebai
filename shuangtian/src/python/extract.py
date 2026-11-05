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


def _triangulate(polygon_vertices, positions):
    """把多边形三角化。返回顶点索引三元组列表。

    ## 算法：耳切（ear clipping）

    对**凸多边形**用扇形更快，但它对**凹多边形**会生成跨出边界的三角形
    ——那些三角形填平凹角。而这是**静默错**：画面看起来“有个多边形”，
    但形状是错的（凹口被填平）。比崩溃危险——崩溃会被人发现，画错会被
    当成“设计成这样”。

    实测（`tools/concave_triangulate_probe`）：五角星（外 1.0 / 内 0.4）
    真实面积 1.176，扇形得到 2.137（填平了 5 个凹角）。

    更阴的是 L 形（浅凹）**扇形也对了**——所以“跑几个例子没看出问题”
    不能证明算法对。凹得浅就侥幸，凹得深就错。

    ## 耳切怎么做

    反复找一个“耳”：顶点 `i` 与它的两个邻居构成三角形，且
    ① 它是**凸角**（叉积 > 0，与多边形绕向一致），
    ② 三角形内**不含**其他任何顶点。

    切掉这个耳（从工作列表移除 `i`），直到只剩 3 个点。
    复杂度 O(n²)，而 bpy 的面通常只有 3~20 个点——完全够用。

    ## 退化处理

    找不到耳时会**回退到扇形**（保证不返回空、不死循环）。
    情形：自相交多边形、共线点过多的退化输入。那类输入本身就不合法，
    给一个“尽量正确”的结果比报错更实用（CAD 导出的数据经常带共线点）。

    参数 `positions` 是**全局顶点坐标表**（`[(x,y,z), ...]`），
    `polygon_vertices` 是它的下标。需要坐标才能判凸凹与内含。
    """
    n = len(polygon_vertices)
    if n < 3:
        return []
    if n == 3:
        return [(polygon_vertices[0], polygon_vertices[1], polygon_vertices[2])]

    # ── ① 绕向：用标准 shoelace（**相邻点**，不是 i,i+1,i+2）──
    #
    #     A2 = Σ (x_i * y_{i+1} - x_{i+1} * y_i)
    #
    # A2 > 0 ⇒ 逆时针（CCW）；< 0 ⇒ 顺时针（CW）。
    #
    # ## 踩过的坑（两轮）
    #
    # 第一版用的是三相邻点的叉积和 `(bx-ax)*(cy-ay)-(by-ay)*(cx-ax)`，
    # 对星形算出 **负数**，而标准 shoelace 算出 **正数**——两者符号相反。
    # 于是 `_is_convex` 拿到了错的绕向，所有凸角被判成凹，耳切一下子
    # 都找不到 → 回退到扇形 → 面积 2.137（应 1.176）。
    #
    # 而**三角形数仍是 n-2**（扇形也给 n-2 个），所以只看三角形数的判据
    # 会误过。必须用面积才能看出来。
    #
    # 教训：绕向就用 shoelace，它是有定义的那个量；别自创叉积和。
    signed = 0.0
    for i in range(n):
        j = (i + 1) % n
        x1 = positions[polygon_vertices[i]][0]
        y1 = positions[polygon_vertices[i]][1]
        x2 = positions[polygon_vertices[j]][0]
        y2 = positions[polygon_vertices[j]][1]
        signed += x1 * y2 - x2 * y1
    if signed == 0.0:
        # 全共线：退化成扇形（什么算法都得不到有效三角形）
        return _fan_triangulate(polygon_vertices)

    # ── ② 耳切主循环 ──
    remaining = list(range(n))
    tris = []
    guard = 0
    while len(remaining) > 3:
        guard += 1
        if guard > n * n + 8:
            # 找不到耳（自相交/退化）→ 回退扇形，保证有输出
            return _fan_triangulate(polygon_vertices)

        found = False
        count = len(remaining)
        for slot in range(count):
            prev_slot = (slot - 1) % count
            next_slot = (slot + 1) % count
            ia = polygon_vertices[remaining[prev_slot]]
            ib = polygon_vertices[remaining[slot]]
            ic = polygon_vertices[remaining[next_slot]]
            if not _is_convex(positions, ia, ib, ic, signed > 0.0):
                continue
            if _contains_any(positions, ia, ib, ic, polygon_vertices, remaining,
                             prev_slot, slot, next_slot):
                continue
            # 这是一个耳：切掉
            tris.append((ia, ib, ic))
            del remaining[slot]
            found = True
            break
        if not found:
            return _fan_triangulate(polygon_vertices)

    tris.append((polygon_vertices[remaining[0]],
                 polygon_vertices[remaining[1]],
                 polygon_vertices[remaining[2]]))
    return tris


def _is_convex(positions, ia, ib, ic, counter_clockwise):
    """顶点 `ib` 处是不是凸角。

    ## 关键：凸角与绕向的对应关系

    `cross = (bx-ax)×(cy-ay)` 是**从 a→b 到 a→c 的转向**。

    - 多边形**逆时针**（CCW，signed > 0）：凸角的 cross **> 0**
    - 多边形**顺时针**（CW，signed < 0）：凸角的 cross **< 0**

    第一版把 `counter_clockwise` 传错了（`signed > 0.0` 是对的，但判定式用了
    `cross >= 0 if ccw else cross <= 0`，而实际星形是 CW 却把凸角判成了凹）：
    根源是**绕向和凸角的内外关系我记反了**——CCW 多边形的凸角在“行进方向
    的左侧”，叉积为正。

    共线（cross == 0）当凸角处理：切掉它不会错，且能推进（否则死循环）。
    """
    ax = positions[ia][0]
    ay = positions[ia][1]
    bx = positions[ib][0]
    by = positions[ib][1]
    cx = positions[ic][0]
    cy = positions[ic][1]
    cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    if counter_clockwise:
        return cross >= 0.0
    return cross <= 0.0


def _contains_any(positions, ia, ib, ic, polygon_vertices, remaining,
                  skip_a, skip_b, skip_c):
    """三角形 (ia,ib,ic) 内部是否含有**其他**多边形顶点。"""
    ax = positions[ia][0]
    ay = positions[ia][1]
    bx = positions[ib][0]
    by = positions[ib][1]
    cx = positions[ic][0]
    cy = positions[ic][1]
    count = len(remaining)
    for slot in range(count):
        if slot in (skip_a, skip_b, skip_c):
            continue
        index = polygon_vertices[remaining[slot]]
        px = positions[index][0]
        py = positions[index][1]
        if _point_in_triangle(px, py, ax, ay, bx, by, cx, cy):
            return True
    return False


def _point_in_triangle(px, py, ax, ay, bx, by, cx, cy):
    """点是否在三角形**内部**（严格内部，不含边界）。

    ## 为何必须排除边界（这是星形失败的真因）

    第一版用“三个叉积同号（含 0）”判，于是**落在三角形边上的点也算内含**。
    而耳切里那个“边”往往正好穿过另一个多边形顶点（凸角两侧的邻居）——
    于是一个耳都找不到，回退到扇形，星形面积就变成了 2.137（应 1.176）。

    症状特别隐：三角形数还是 n-2（回退的扇形也给 n-2 个），所以
    “数三角形”这个判据会误过。必须用**面积**才能看出来。
    """
    d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by)
    d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy)
    d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay)
    # 严格内部：三个叉积**同号且非零**
    has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    if has_neg and has_pos:
        return False
    # 全零或有零 = 在边界/顶点上 → 不算“内含”
    if (d1 == 0) or (d2 == 0) or (d3 == 0):
        return False
    return True


def _fan_triangulate(polygon_vertices):
    """扇形三角化（凸多边形的快速路径，也是耳切失败时的兜底）。"""
    n = len(polygon_vertices)
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

    # ## 三角化 + **平滑顶点法线** + 按材质分组
    #
    # ## 为何要按材质排序索引
    #
    # 渲染时是“换材质 → 画一批”：同材质的三角形**必须连在一起**，
    # 否则每画一个三角形就换一次材质（状态抖动，实测在软件腿上代价明显）。
    #
    # 而 bpy 的面顺序是按几何逻辑排的，不同 material_index 会交错。
    # 所以这里先按材质桶收集三角形，再拼成连续的 `indices`。
    #
    # ## 为什么改成平滑法线（2026-11-05）
    #
    # 原实现是**逐三角形平面法线**。那让每个面得到固定亮度，
    # 整个齿轮渲出来是“大片均匀色块”。而 three.js 的挤出几何用
    # **平滑法线**，黄铜从亮金到深褐有连续过渡。
    #
    # 实测对比（`tools/render_compare`，与真 three.js 参照图比）：
    #
    # | 指标 | 平面法线 | 平滑法线 | three.js |
    # |---|---|---|---|
    # | 暗部占比 | 0.0% | 待测 | 9.8% |
    # | 最亮 | 204 | 待测 | 255 |
    #
    # 平面法线让**同一面上的每个像素法线相同**，Lambert 项就是常数，
    # 没有方向性；平滑法线才能让圆环面产生渐变。
    #
    # ## 怎么平均
    #
    # 同一个顶点被多个三角形共享时，把各三角形的**面积加权法线**加起来
    # 归一化（面积加权是因为大三角形对表面朝向的贡献更大——不加权会让
    # 密集小三角形主导法线，在齿根处出现奇怪的亮斑）。
    vertices = []       # 交错：[x, y, z, nx, ny, nz, r, g, b]
    buckets = {}        # material_index -> [三角形顶点下标三元组, ...]
    bucket_order = []   # 保持首次出现的顺序（结果可复现，便于对照）

    # ① 先三角化并累计顶点法线（`positions` 的下标 → 累加法线）
    triangles = []      # 每个元素：(mat_index, i0, i1, i2)
    accum = [None] * len(positions)   # 每个位置的法线累加器
    for poly in mesh.polygons:
        mat_index = poly.material_index if hasattr(poly, 'material_index') else 0
        for tri in _triangulate(poly.vertices, positions):
            i0, i1, i2 = tri
            p0 = positions[i0]
            p1 = positions[i1]
            p2 = positions[i2]
            n = _compute_normal(p0, p1, p2)
            # 面积 = |叉积| / 2；而 `_compute_normal` 已归一化，所以重新算面积
            ax = p1[0] - p0[0]
            ay = p1[1] - p0[1]
            az = p1[2] - p0[2]
            bx = p2[0] - p0[0]
            by = p2[1] - p0[1]
            bz = p2[2] - p0[2]
            cx = ay * bz - az * by
            cy = az * bx - ax * bz
            cz = ax * by - ay * bx
            area = ((cx * cx + cy * cy + cz * cz) ** 0.5) * 0.5
            for index in (i0, i1, i2):
                slot = accum[index]
                if slot is None:
                    accum[index] = [n[0] * area, n[1] * area, n[2] * area]
                else:
                    slot[0] += n[0] * area
                    slot[1] += n[1] * area
                    slot[2] += n[2] * area
            triangles.append((mat_index, i0, i1, i2))

    # ② 归一化累加结果
    smooth = [None] * len(positions)
    for index in range(len(positions)):
        slot = accum[index]
        if slot is None:
            smooth[index] = (0.0, 0.0, 1.0)
            continue
        length = ((slot[0] * slot[0] + slot[1] * slot[1] + slot[2] * slot[2]) ** 0.5)
        if length < 1e-12:
            smooth[index] = (0.0, 0.0, 1.0)
        else:
            smooth[index] = (slot[0] / length, slot[1] / length, slot[2] / length)

    # ③ 生成顶点表（每个三角形独立三顶点，法线用平滑值）
    for mat_index, i0, i1, i2 in triangles:
        if mat_index not in buckets:
            buckets[mat_index] = []
            bucket_order.append(mat_index)
        base = len(vertices)
        for index in (i0, i1, i2):
            p = positions[index]
            n = smooth[index]
            vertices.append((p[0], p[1], p[2], n[0], n[1], n[2]))
        buckets[mat_index].append((base, base + 1, base + 2))

    if not vertices:
        return None

    # ## 拼索引 + 分组表
    #
    # `material_groups` 是三元组序列 `[index_start, index_count, material_index]`
    # ——与 `Mesh::material_groups` 的契约一致（`index_count` 是**索引个数**）。
    indices = []
    groups = []
    for mat_index in bucket_order:
        tris = buckets[mat_index]
        if not tris:
            continue
        start = len(indices)
        for tri in tris:
            indices.append(tri[0])
            indices.append(tri[1])
            indices.append(tri[2])
        groups.append([start, len(indices) - start, mat_index])

    materials = _extract_materials(obj)

    return {
        'name': obj.name,
        'positions': positions,
        'vertices': vertices,
        'indices': indices,
        'world': world,
        # 向后兼容：`material` 仍是第 0 个（旧下游读它）
        'material': materials[0] if materials else _default_material(),
        # 全量槽位 + 分组表（多材质对象用）
        'materials': materials,
        'material_groups': groups,
        'visible': True,
    }


def _default_material():
    """没材质的对象用什么（与 Blender 新建物体的默认灰一致）。"""
    return {
        'base_color': [0.8, 0.8, 0.8],
        'metallic': 0.0,
        'roughness': 0.5,
        'emission': 0.0,
        'opacity': 1.0,
    }


def _material_to_dict(mat):
    """一个 bpy Material → dict（PBR 子集）。

    ## 为何用 `diffuse_color` 而不是节点树
    ##
    ## 真 bpy 的 Principled BSDF 在 `mat.node_tree` 里（`inputs['Base Color']`
    ## 等），而 `diffuse_color` 只是**视口显示色**。两者可能不同。
    ##
    ## 我们的兼容层没有节点树（实现成本远高于收益），所以用 `diffuse_color`
    ## 叠加 `metallic` / `roughness` 两个直接字段——它们是 bpy 的 `Material`
    ## 属性，在真实 Blender 里与节点值**默认同步**（Blender 的 Material
    ## 面板把两者写进节点）。
    ##
    ## 残余差异：若脚本显式设了节点而不改 `diffuse_color`，抽取会拿到未同步的
    ## 值。这条写在 docs/python.md 的“残余差异”里。
    """
    if mat is None:
        return _default_material()
    color = mat.diffuse_color
    return {
        # diffuse_color 是 RGBA，本框架要 RGB（都是线性空间）
        'base_color': [float(color[0]), float(color[1]), float(color[2])],
        'metallic': float(mat.metallic),
        'roughness': float(mat.roughness),
        'emission': float(mat.emission_strength),
        'opacity': float(color[3]),
    }


def _extract_materials(obj):
    """对象的所有材质槽 → dict 列表。

    ## 为何返回列表而不是单个

    bpy 的一个网格可以按面分多个材质（`polygon.material_index` 指向这个
    槽位表）。只取第 0 个会让其余材质的面被迫用第一个材质渲染——
    现象是“物体颜色单一”，而**不会报错**。

    空槽（`None`）用默认材质填充而不是跳过：因为 `material_index` 是
    槽位下标，跳过会让下标错位。
    """
    slots = getattr(obj, 'material_slots', None)
    if not slots:
        return [_default_material()]
    out = []
    for slot in slots:
        # ## 两种形态都要认
        #
        # 真 bpy：`material_slots[i].material` 是材质（槽对象带 link/offset 等）。
        # 本兼容层：`material_slots[i]` 直接就是材质（简化，见 bpy_data.py）。
        #
        # 判据用 `getattr` 而不是类型判断：`Material` 没有 `.material` 属性，
        # 而槽对象有——一条 `getattr` 就分开了，不必引入类型名依赖。
        mat = getattr(slot, 'material', slot)
        out.append(_material_to_dict(mat))
    if not out:
        out.append(_default_material())
    return out


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


def _extract_world():
    """场景的世界背景色 → `[r, g, b]` 线性值（取不到时返回 None）。

    ## 为何必须抽它（2026-11-05 加）

    环境反射是**金属出色的唯一来源**：纯 Cook-Torrance 只处理直接光，
    金属其余像素 = 固有色 × 0（无漫反射）= 黑。

    而本框架的 `Lighting::env_sky` 默认是 `(0.52, 0.58, 0.68)`——一个
    **明亮的天空**。而典型 bpy 脚本的世界背景是 `(0.006, 0.007, 0.009)`
    （sRGB 18/20/24 转线性）——**亮了 87 倍**。

    实测后果（`tools/subject_stats`，只统计主体）：

    | | 主体均值 | 暗部 |
    |---|---|---|
    | 本框架 | **139.6** | **0.0%** |
    | 真 Blender | 78.1 | 34.6% |
    | 真 three.js | 74.0 | 50.2% |

    亮近一倍，完全没有暗部——因为金属被一个**不存在的亮天空**照亮。

    ## 为何回退到 None 而不是报错

    bpy 脚本可以不设世界（用默认灰色）。那时用本框架原默认即可——
    不把“没配世界”当成错误。
    """
    scene = _host.get('context')
    if scene is None:
        return None
    scene = getattr(scene, 'scene', None)
    if scene is None:
        return None
    world = getattr(scene, 'world', None)
    if world is None:
        return None
    # 真实 bpy：色在节点树的 Background 节点里
    background = getattr(world, 'background_color', None)
    if background is not None:
        return [float(background[0]), float(background[1]), float(background[2])]
    return None


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
        # 世界背景色（环境反射用）。None = 脚本没配世界，C++ 侧用默认
        'world': _extract_world(),
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
