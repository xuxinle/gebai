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

    __slots__ = ('vertices', 'material_index', 'use_smooth')

    def __init__(self, indices, material_index=0, use_smooth=False):
        self.vertices = list(indices)
        self.material_index = material_index
        # ## `use_smooth`（2026-11-07 补）
        #
        # 真 bpy 里它是**逐面**的开关：立方体默认 False（棱角锋利）、
        # 球默认 True（平滑）。两个真渲染器都尊重它
        #（Blender 的 shade_flat/smooth、three 的 flatShading）。
        #
        # 抽取层（`extract.py`）早就读它了，但**之前没有任何地方能设它**——
        # `bpy.ops.object.shade_smooth()` 在兼容层根本不存在。
        # 后果：脚本写 `shade_smooth()` 会在解析阶段就报
        # `AttributeError: no such attribute`（不说哪个属性），
        # 而即使过了，球也会渲成多面体（块状高光）——对标里直接是硬伤。
        self.use_smooth = use_smooth

    def __repr__(self):
        return 'MeshPolygon(%r, mat=%d, smooth=%r)' % (
            self.vertices, self.material_index, self.use_smooth)


class Mesh:
    """网格数据（几何 + 面）。不含变换——那是 Object 的事。

    ## 为何要有一份 `materials`（2026-11-06 加）

    真 bpy 里挂材质的标准写法是：

        obj.data.materials.append(mat)      # ← 冒、真 bpy 都认

    而本兼容层原本只有 `obj.material_slots.append(mat)`。于是“两边都能跑”
    的脚本必须写两套（实测撞过：`AttributeError: bpy_prop_collection:
    attribute "append" not found`）。

    解法：给 Mesh 一个双向代理列表，append 时**同时**写进 obj.material_slots。
    这样两种写法都能用，而且只存一份数据（不会两份漂移）。
    """

    def __init__(self, name='Mesh'):
        self.name = name
        self.vertices = []
        self.polygons = []
        # 由 Object 建立关联时回填（见 Object.__init__）
        self.materials = None

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


class _MaterialSlotProxy:
    """`obj.data.materials` 的代理：把操作转给 `obj.material_slots`。

    ## 为何需要一层代理而不是直接给个 list

    真 bpy 里这两个是不同的东西：

        obj.data.materials          # 网格上的材质槽（列表，append 直接加槽）
        obj.material_slots          # 对象的槽（带 link/offset 等额外属性）

    本兼容层只实现了后者。如果给 `data.materials` 一个**独立的** list，
    两份数据就会漂移（向一个 append、从另一个读不到）。那正是本仓反复
    批判的“同一个概念有两份真相”。

    所以做成代理：**只有一份数据**（`material_slots`），两种写法都指向它。
    """

    __slots__ = ('_owner',)

    def __init__(self, owner):
        self._owner = owner

    def append(self, mat):
        self._owner.material_slots.append(mat)

    def clear(self):
        del self._owner.material_slots[:]

    def __len__(self):
        return len(self._owner.material_slots)

    def __iter__(self):
        return iter(self._owner.material_slots)

    def __getitem__(self, index):
        return self._owner.material_slots[index]

    def __repr__(self):
        return 'bpy_prop_collection(%d materials)' % len(self._owner.material_slots)


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

    @angle_y.setter
    def angle_y(self, radians):
        """反推 `lens`（2026-11-07 补的 setter）。

        ## 为何必须有 setter

        真 bpy 里设定垂直视角的标准写法就是
        `cam_data.angle_y = math.radians(40)`。兼容层原先只有 getter，
        于是这种写法报 `AttributeError: no such attribute`
        （且不说是哪个属性，极难定位）。

        ## 反推公式与 getter **严格互逆**

        getter 是 `angle_y = 2·atan(sensor_width/2 / lens)`，
        所以 `lens = (sensor_width/2) / tan(angle_y/2)`。

        ⚠ getter 那段长注释说的“本框架把 lens 当**水平** fov 用”
        这个口径**不能改**——那是对比实测定标过的。
        所以这里只做互逆反推，不引入任何宽高比修正。
        """
        import math
        half = max(float(radians), 1.0e-6) * 0.5
        self.lens = (self.sensor_width * 0.5) / math.tan(half)

    @property
    def angle(self):
        """水平视野角（弧度）。与 `angle_y` 同值（见上：口径未分离）。"""
        import math
        return 2.0 * math.atan(self.sensor_width * 0.5 / self.lens)


class Light:
    """灯光（点/平行/聚光）。

    ## 为何默认仍是 'POINT' 但要报告分歧

    真 bpy 的 `bpy.data.lights.new(name)` **必须给 type**（实测：
    `TypeError: BlendDataLights.new(): required parameter "type" not specified`）。
    而本兼容层允许省。宽严不一致的代价是：一份在霜天能跑的脚本，
    拿到真 Blender 里直接挂。

    所以这里只记一个 `type_specified` 标志，由抽取器汇总成 warning——
    **不破现有脚本**（那会让存量全部失效），但把差异说出来。
    """

    def __init__(self, name='Light', light_type='POINT', type_specified=False):
        self.name = name
        self.type = light_type     # 'POINT' / 'SUN' / 'SPOT' / 'AREA'
        self.energy = 1000.0
        self.color = (1.0, 1.0, 1.0)
        # 脚本建灯时是否显式给了 type（真 bpy 要求给）
        self.type_specified = type_specified


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
        self.rotation_euler = Euler((0.0, 0.0, 0.0))
        self.scale = Vector((1.0, 1.0, 1.0))
        self.visible = True
        # 材料槽（bpy 是列表，本框架只取第 0 个——见抽取器说明）
        self.material_slots = []
        # `obj.data.materials.append(mat)` 也给通（真 bpy 的标准写法）。
        #
        # 内建一个极小的列表代理：append/clear/下标/len 都转给 material_slots，
        # 于是两种写法看到的是**同一份数据**（不会两份漂移）。
        if data is not None and hasattr(data, 'materials'):
            data.materials = _MaterialSlotProxy(self)

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

    @matrix_world.setter
    def matrix_world(self, m):
        """把世界矩阵**分解**回 location / rotation_euler / scale（2026-11-07 补）。

        ## 为何必须有 setter

        真 bpy 里“直接给相机/物体定位”的标准写法是
        `obj.matrix_world = Matrix.Translation(eye) @ rot`——
        这是把相机摆到任意朝向上**最不会出错**的做法（不必推欧拉角）。
        兼容层原先只有 getter，于是这种写法报
        `AttributeError: no such attribute`。

        ## 分解假设（写成字面，不假装通用）

        本框架的 `matrix_world` 是 `T × R × S`（见 getter）。所以分解时：
        · 平移 = 第 4 列
        · 缩放 = 前三列各自的长度
        · 旋转 = 前三列分别除以缩放后的矩阵

        ⚠ 不处理**斜切**（shear）：真 Blender 的 `matrix_world` 可以是任意
        4×4，而本框架的物体表示里没有斜切这个自由度。带斜切的矩阵会被
        投影成最接近的无斜切形式——差异只出现在人为构造非仿射旋转的场合，
        正常建模遇不到。
        """
        import math
        if m is None:
            return
        # ⚠ 本框架的 `Matrix` 只支持 `m[row][col]`，**没有** `value(r, c)`。
        # （真 bpy 两者都有；这里用两边都有的下标访问。）
        def at(row, col):
            return m[row][col]
        # 平移：第 4 列
        self.location = Vector((at(0, 3), at(1, 3), at(2, 3)))
        # 缩放：前三列的长度
        sx = math.sqrt(at(0, 0) ** 2 + at(1, 0) ** 2 + at(2, 0) ** 2)
        sy = math.sqrt(at(0, 1) ** 2 + at(1, 1) ** 2 + at(2, 1) ** 2)
        sz = math.sqrt(at(0, 2) ** 2 + at(1, 2) ** 2 + at(2, 2) ** 2)
        self.scale = Vector((sx, sy, sz))
        # 旋转：去掉缩放后的 3×3 → 提取欧拉角（XYZ，与 Blender 默认顺序一致）
        #
        # ## 下半三角要用「对称位置」而不是「同行/同列」
        #
        # XYZ 欧拉（R = Rz·Ry·Rx，行主序）的旋转矩阵是：
        #
        #     R[2][0] = -sin(y)            → y = asin(-R[2][0])
        #     R[2][1] =  cos(y)·sin(x)     → x = atan2(R[2][1], R[2][2])
        #     R[1][0] =  cos(y)·sin(z)     → z = atan2(R[1][0], R[0][0])
        #
        # 旧代码把中间两个写成了 `at(2,1)` 和 `at(1,0)` 以外的对称位
        # （`r21 = at(2,1)` 配 `atan2(r21, r22)` 其实对，但 `r10` 用了
        #  `at(1,0)` 而下面 `atan2(r10, r00)` 期望的是 R[1][0]）——
        # 真正错的是：分解出来的三个角再合成时，结果矩阵被**转置**。
        #
        # 实测（`tmp/rt7.py`，用异常把数值带出来）：
        #
        #   setter 存的 col2 = 0.3909, 0.8687, 0.3041
        #   getter 读回 col2 = 0.7894, 0.5334, 0.3041   ← 行列对调
        #   setter 存的 row2 = -0.7894, -0.5334, 0.3041
        #   getter 读回 row2 = -0.3909, -0.8687, 0.3041  ← 同上
        #
        # 后果：任何用 `matrix_world = ...` 定向的东西（相机、灯光）都会
        # 被旋转错位。阴影因此投到错误方向（实测光方向取到 (0.789,0.533)，
        # 正确应为 (0.391,0.869)——差约 40°）。
        # ## 提取公式必须与 `Euler.to_matrix()` **互逆**
        #
        # `Euler.to_matrix()` 写的是：
        #
        #     m = Rx(x).matmul(Ry(y)).matmul(Rz(z))     ← 即 R = Rx·Ry·Rz
        #
        # 而旧提取公式用的是标准 XYZ 欧拉（R = Rz·Ry·Rx）。两者**不是**
        # 同一个旋转的两种写法，而是两个不同的旋转空间——所以往返必然错位。
        #
        # 实测（`tmp/rt7.py`，用异常把数值带出）：
        #
        #   setter 存的 col2 = 0.3909, 0.8687, 0.3041
        #   getter 读回 col2 = 0.7894, 0.5334, 0.3041   ← 行列对调
        #
        # 后果：用 `matrix_world = ...` 定向的相机/灯光全被旋转错位。
        # 阴影因此投到错误方向（光方向实测 (0.789,0.533)，应为 (0.391,0.869)）。
        #
        # ## R = Rx·Ry·Rz 的显式元素（行主序）
        #
        #     R = |  cy·cz            -cy·sz           sy     |
        #         |  sx·sy·cz+cx·sz    -sx·sy·sz+cx·cz  -sx·cy |
        #         | -cx·sy·cz+sx·sz     cx·sy·sz+sx·cz   cx·cy |
        #
        # 所以提取（cos(y) ≠ 0 时）：
        #
        #     y = asin( R[0][2])
        #     x = atan2(-R[1][2], R[2][2])
        #     z = atan2(-R[0][1], R[0][0])
        r00 = at(0, 0) / sx if sx > 1e-12 else 1.0
        r01 = at(0, 1) / sy if sy > 1e-12 else 0.0
        r02 = at(0, 2) / sz if sz > 1e-12 else 0.0
        r12 = at(1, 2) / sz if sz > 1e-12 else 0.0
        r22 = at(2, 2) / sz if sz > 1e-12 else 1.0
        sy_angle = math.asin(max(-1.0, min(1.0, r02)))
        cos_y = math.cos(sy_angle)
        if abs(cos_y) > 1e-6:
            sx_angle = math.atan2(-r12, r22)
            sz_angle = math.atan2(-r01, r00)
        else:
            # 万向锁：x 与 z 只能确定一个，令 x = 0，全给 z
            sx_angle = 0.0
            sz_angle = math.atan2(at(1, 0), at(1, 1))
        self.rotation_euler = Euler((sx_angle, sy_angle, sz_angle))

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
        # `lights` 集合：把「脚本有没有给 type」记下来。
        #
        # 真 bpy 的 `lights.new(name)` **必须** 给 type（实测 TypeError）。
        # 本兼容层允许省，但要把分歧报出来——否则用户写出一份
        # “在霜天能跑、在 Blender 直接挂”的脚本而不可知。
        if self._factory is Light:
            if args or 'light_type' in kwargs:
                kwargs = dict(kwargs)
                kwargs['type_specified'] = True
        item = self._factory(unique, *args, **kwargs)
        self._items.append(item)
        return item

    def add(self, item):
        """直接加入一个**已存在**的数据块（不新建）。

        ## 为何需要它（2026-11-07）

        真 bpy 里把已建好的对象放进场景是：

            obj = bpy.data.objects.new("cam", cam_data)   # 已进 bpy.data
            scene.collection.objects.link(obj)             # 只需要“可见”

        而本框架把“创建”与“入场景”合并成了 `new`，所以 `link` 需要一个
        “把已存在项加回集合”的入口。幂等：已在集合里就不重复加。
        """
        if item is None:
            return None
        if not any(existing is item for existing in self._items):
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
