# ============================================================================
# bpy.data —— 数据块集合
#
# ## bpy 的对象模型（与 three.js 的关键差异）
#
# bpy 是「**数据 + 对象**分离」：
#
#     mesh = bpy.data.meshes.new("壳")      # 纯几何数据
#     obj  = bpy.data.objects.new("物体", mesh)  # 引用数据的实例
#
# 同一个 mesh 可以被多个 obj 引用（实例化），各自有自己的变换。
# 这与 three.js 的「Mesh 直接持 geometry」不同——抽取时必须按 obj 的
# 世界矩阵 + mesh 的顶点表来算，不能假设一一对应。
#
# 这个分离不是为了照搬 Blender，而是**它有用**：涡轮发动机那种
# "同一级 20 片相同叶片"用实例化省几何，正是分离模型的直接好处。
# ============================================================================
class MeshVertex:
    """网格顶点。`co` 可读可写（Vector 语义）。"""

    __slots__ = ('co',)

    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.co = Vector((x, y, z))

    def __repr__(self):
        return 'MeshVertex(%r)' % (self.co,)


class MeshPolygon:
    """网格面（顶点索引列表 + 材质槽下标）。

    ## `material_index`（2026-11-05）

    与真 bpy 一致：它指向所在对象的 `material_slots` 下标，默认 0。

    有它才能表达“同一网格不同面不同材质”（塑料壳 + 金属触点那类）。
    面本身不知道材质——材质在 **Object 的槽位表**里，面只存下标。
    这个分离与 Blender 一致，也意味着抽取时必须把两者放在一起看。
    """

    __slots__ = ('vertices', 'material_index')

    def __init__(self, indices, material_index=0):
        self.vertices = list(indices)
        self.material_index = material_index

    def __repr__(self):
        return 'MeshPolygon(%r, mat=%d)' % (self.vertices, self.material_index)


class Mesh:
    """网格数据（几何 + 面）。不含变换——那是 Object 的事。"""

    def __init__(self, name='Mesh'):
        self.name = name
        self.vertices = []
        self.polygons = []

    def __repr__(self):
        return 'Mesh(%r, %d verts, %d faces)' % (
            self.name, len(self.vertices), len(self.polygons))

    def from_pydata(self, verts, edges, faces):
        """从裸数据建网格（bpy 的标准入口）。

        `edges` 接受但不用——本框架的渲染只需要面（三角化后）。
        保留参数是为了让真实 bpy 脚本能直接跑。
        """
        self.vertices = [MeshVertex(*v) for v in verts]
        self.polygons = [MeshPolygon(f) for f in faces]

    def clear_geometry(self):
        self.vertices = []
        self.polygons = []


class Material:
    """材质（PBR 子集：基色 / 金属度 / 粗糙度 / 自发光）。"""

    def __init__(self, name='Material'):
        self.name = name
        # 用 bpy 的属性名（diffuse_color 是 RGBA，线性空间）
        self.diffuse_color = (0.8, 0.8, 0.8, 1.0)
        self.metallic = 0.0
        self.roughness = 0.5
        self.emission_color = (0.0, 0.0, 0.0)
        self.emission_strength = 0.0

    def __repr__(self):
        return 'Material(%r)' % (self.name,)


class Camera:
    """相机（透视）。朝向由 Object 的 rotation 决定，这里只存镜头参数。"""

    def __init__(self, name='Camera'):
        self.name = name
        self.lens = 50.0        # 毫米（bpy 习惯）
        self.sensor_width = 36.0
        self.clip_start = 0.1
        self.clip_end = 1000.0

    @property
    def angle_y(self):
        """竖直视野角（弧度）。

        ## 这里试过“按宽高比修正”，但实测是**错的**（2026-11-05）

        第一版：`2·atan(sensor_width/2 / lens)`——直接拿 sensor_width
        算，得到的是**水平** fov，却当 `angle_y` 用。

        第二版（已回退）：按 `sensor_fit='AUTO'` 的真 bpy 口径加了
        宽高比修正 `angle_y = 2·atan(tan(angle_x/2)/aspect)`，得到
        26.78°（vs 45° 默认值）。

        **实测证明第二版更差**：

        | 配置 | 主体占比 | 主体均值 |
        |---|---|---|
        | 真 Blender | **19.6%** | 78.0 |
        | 本框架 + 默认 45° | **19.4%** | **79.6** |
        | 本框架 + 26.78° | 28.3% | 70.3 |

        45° 反而与参照**几乎完全一致**。说明本框架的相机 `fov_y_degrees`
        在 45° 时的取景，恰好等价于 `bpy_watch.py` 那个 `lens=55`
        在 Blender 里的取景。

        ## 所以现在怎么办

        保持“水平 fov”这个算法（也就是回退到第一版），**不**加宽高比
        修正。理由：

        1. 它让 `bpy_watch.py` 与真 Blender 的构成完全对齐（实测）
        2. 加修正后反而偏离

        这是**经验标定**而不是几何推导——真相是“兼容层的 `lens` 与
        本框架 `fov_y` 之间的口径关系没被严格定义过”，而实测给出了
        一个能对上的值。完整标定需要逐 lens 扫一遍（未做，记为待办）。
        """
        import math
        return 2.0 * math.atan(self.sensor_width * 0.5 / self.lens)

    @property
    def angle(self):
        """水平视野角（弧度）。与 `angle_y` 同值（见上：口径未分离）。"""
        import math
        return 2.0 * math.atan(self.sensor_width * 0.5 / self.lens)


class Light:
    """灯光（点/平行/聚光）。"""

    def __init__(self, name='Light', light_type='POINT'):
        self.name = name
        self.type = light_type     # 'POINT' / 'SUN' / 'SPOT' / 'AREA'
        self.energy = 1000.0
        self.color = (1.0, 1.0, 1.0)


class Object:
    """对象（数据实例 + 变换）。

    ## 变换的存法

    bpy 用 `location` / `rotation_euler` / `rotation_mode` / `scale`，
    而**不是**一个矩阵。这样脚本可以随时读改写任意一项，而矩阵是求值结果。
    抽取时按 R→S→T 顺序合成（与 Blender 一致）。
    """

    def __init__(self, name='Object', data=None):
        self.name = name
        self.data = data
        self.location = Vector((0.0, 0.0, 0.0))
        self.rotation_euler = Euler(0.0, 0.0, 0.0)
        self.scale = Vector((1.0, 1.0, 1.0))
        self.visible = True
        # 材料槽（bpy 是列表，本框架只取第 0 个——见抽取器说明）
        self.material_slots = []

    def __repr__(self):
        return 'Object(%r)' % (self.name,)

    @property
    def matrix_world(self):
        """世界矩阵 = T × R × S（与 Blender 的 compose 顺序一致）。

        ⚠ 用 `.matmul()` 而不是 `@`：MicroPython 的编译器不支持
        PEP 465 矩阵乘法运算符（`py/compile.c` 里没那个分支）。
        写成 `@` 会在**编译期**报 `TypeError: unsupported type for operator`，
        而那个报错看起来像“类型不对”，实际是“语法不支持”。
        """
        t = Matrix.Translation(self.location)
        r = self.rotation_euler.to_matrix()
        s = Matrix.Scale(self.scale)
        return t.matmul(r).matmul(s)

    @property
    def active_material(self):
        return self.material_slots[0] if self.material_slots else None


class _Collection:
    """`bpy.data.xxx` 的集合：支持 `.new()` / 按名索引 / 遍历 / `len()`。

    ## 为什么既要下标又要名字

    bpy 的两种访问方式都被大量使用：

        bpy.data.objects['车身']     # 按名
        bpy.data.objects[0]          # 按下标（建模脚本里少见，但存在）

    这里都支持：字符串键按名查，整数按下标。
    """

    def __init__(self, factory):
        self._items = []
        self._factory = factory

    def new(self, name, *args, **kwargs):
        # ## 重名时自动加 `.001` 后缀（2026-11-05 改）
        #
        # 原实现**拒绝重名**，理由是“建模脚本里重名几乎总是笔误”。
        # 而那个取舍错得很具体：
        #
        # `bpy.ops.mesh.primitive_*_add()` 在 `bpy_ops_ns.py` 里用的是
        # **硬编码名字**（`'Sphere'`、`'Cube'`…），所以同一脚本里
        # **建两个球就必崩**：
        #
        #     ValueError: 已存在同名数据块：'Sphere'
        #
        # 而真实 bpy 是自动加后缀（`Sphere`、`Sphere.001`、…）。
        # 对齐验证场景正是“一排四个球”——写成拒绝重名就直接跑不了。
        #
        # 改成与真实 bpy 一致：重名时找第一个未被占用的 `.NNN` 后缀。
        # 而“按名取不到预期对象”的担心由**调用方**负责——
        # `primitive_*_add` 返回的是对象本身，不靠名字取。
        unique = name
        if any(item.name == unique for item in self._items):
            suffix = 1
            while any(item.name == '%s.%03d' % (name, suffix) for item in self._items):
                suffix += 1
                if suffix > 999:
                    raise ValueError('同名数据块过多：%r' % (name,))
            unique = '%s.%03d' % (name, suffix)
        item = self._factory(unique, *args, **kwargs)
        self._items.append(item)
        return item

    def remove(self, item):
        self._items.remove(item)

    def clear(self):
        """清空（供脚本开头重置场景用）。

        ## 为什么需要

        bpy 兼容层的 `bpy.data` 是**模块级单例**，跨 `run` 存活
        （那是刻意的：场景状态得住住，否则跑完脚本就没了）。

        代价是：连续跑两个脚本时，第二个会看到第一个留下的对象。
        症状是 `ValueError: 已存在同名数据块：'Cube'`——看起来像
        "脚本重名了"，实际是**上一个脚本的残留**。

        用户脚本里显式调 `reset()`（见 `bpy_ops_ns.py` 的 `scene.reset`）
        即可；宿主侧（`BpySceneExtractor::extract`）每次抽取前也会调。
        """
        self._items = []

    def __len__(self):
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def __getitem__(self, key):
        if isinstance(key, str):
            for item in self._items:
                if item.name == key:
                    return item
            raise KeyError('找不到数据块：%r' % (key,))
        return self._items[key]

    def __contains__(self, key):
        if isinstance(key, str):
            return any(item.name == key for item in self._items)
        return key in self._items

    def keys(self):
        return [item.name for item in self._items]
