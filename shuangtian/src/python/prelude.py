"""霜天 · bpy 兼容层（Python 实现，冻进固件）

## 这是什么

一份用 Python 写的 bpy 子集，让模型能用它最熟悉的 Blender 脚本写三维资产：

    import bpy
    from mathutils import Vector

    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=1.0, depth=2.0)
    obj = bpy.context.active_object
    obj.location = Vector((0, 0, 1))

    mesh = obj.data
    for v in mesh.vertices:
        v.co.z *= 1.5

    result = obj

## 为什么用 Python 而不是 C 扩展

C 类型那条路走过：`MP_DEFINE_CONST_OBJ_TYPE` 产出的类型一旦通过
`MP_ROM_PTR` 进入模块导出表，进程就在 `mp_init()` 阶段访问违例
（0xC0000005），没有任何输出可查。所有槽位单独验都正常、四种注册方式
都试过、拿掉那个导出就一切正常——判断是 MinGW 下 const 数据段跨单元
重定位的问题。

Python 实现的取舍：

| 项 | C 类型 | Python |
|---|---|---|
| 正确性风险 | 高（上面那个坑） | 无 |
| 性能 | 快 | 慢 3~8 倍 |
| 可扩展性 | 加方法要重编 | 改文本即可 |

**先要正确，再要快。** mathutils 的调用密度远低于渲染热路径——它在
"建几何时"被调（每顶点一次），典型场景几万个顶点，慢几倍也只是几十
毫秒，相对渲染（百毫秒级）可接受。将来若成为瓶颈，只把热点
（`Vector.__init__`、`Matrix @ Vector`）改成 C，接口不变。

## 子集边界（明确的，不静默）

有的：

- `mathutils`：`Vector` / `Matrix` / `Euler` / `Quaternion`
- `bpy.data`：`meshes` / `objects` / `materials` / `cameras` / `lights`
- `bpy.context`：`active_object` / `selected_objects` / `scene`
- `bpy.ops.mesh.primitive_*`：cube / cylinder / cone / uv_sphere / plane / torus
- `bpy.ops.object.*`：select_all / delete / transform 类
- `bmesh`：`new()` / `verts` / `faces` / `to_mesh()` / `from_mesh()`

没有的（调用会得到 `AttributeError`，不静默返回 None）：修改器栈、
约束、动画曲线、UV 展开、骨骼蒙皮。那些是 Blender 的"编辑器能力"，
而这里只需要"产出可渲染的几何"。
"""


# ============================================================================
# mathutils
# ============================================================================
class Vector:
    """2~4 维向量（bpy 的 mathutils.Vector 子集）。

    ## 为什么用列表存分量而不是属性

    bpy 的 `Vector` 支持任意维度（2/3/4），用列表存 + `dim` 隐式长度，
    一套代码管三种维度。属性访问（`.x`）走 property，语义与 bpy 一致。

    ## 原地语义必须遵循 bpy

    `normalize()` 是**原地**的（返回 None），`normalized()` 返回新对象。
    若把 `normalize` 做成返回新对象，那些写 `v.normalize()` 而不接返回值
    的脚本会**静默不生效**——然后整段几何都偏了。
    """

    __slots__ = ('_v',)

    def __init__(self, items=None):
        if items is None:
            self._v = [0.0, 0.0, 0.0]
        else:
            self._v = [float(x) for x in items]
            if len(self._v) < 2 or len(self._v) > 4:
                raise ValueError('Vector 需要 2~4 个分量')

    # ---- 表现 ----
    def __repr__(self):
        return 'Vector((' + ', '.join('%.6g' % x for x in self._v) + '))'

    def __len__(self):
        return len(self._v)

    def __iter__(self):
        return iter(self._v)

    def __getitem__(self, i):
        return self._v[i]

    def __setitem__(self, i, value):
        self._v[i] = float(value)

    def __eq__(self, other):
        return isinstance(other, Vector) and self._v == other._v

    # ---- 分量（可读可写）----
    def _get(self, i):
        if i >= len(self._v):
            raise AttributeError('该维度没有这个分量')
        return self._v[i]

    def _set(self, i, value):
        if i >= len(self._v):
            raise AttributeError('该维度没有这个分量')
        self._v[i] = float(value)

    @property
    def x(self):
        return self._get(0)

    @x.setter
    def x(self, v):
        self._set(0, v)

    @property
    def y(self):
        return self._get(1)

    @y.setter
    def y(self, v):
        self._set(1, v)

    @property
    def z(self):
        return self._get(2)

    @z.setter
    def z(self, v):
        self._set(2, v)

    @property
    def w(self):
        return self._get(3)

    @w.setter
    def w(self, v):
        self._set(3, v)

    @property
    def length(self):
        return sum(c * c for c in self._v) ** 0.5

    @property
    def length_squared(self):
        return sum(c * c for c in self._v)

    def copy(self):
        return Vector(self._v)

    # ---- 运算 ----
    def __add__(self, other):
        if isinstance(other, Vector):
            return Vector([a + b for a, b in zip(self._v, other._v)])
        return Vector([a + other for a in self._v])

    def __sub__(self, other):
        if isinstance(other, Vector):
            return Vector([a - b for a, b in zip(self._v, other._v)])
        return Vector([a - other for a in self._v])

    def __mul__(self, other):
        if isinstance(other, Vector):
            return Vector([a * b for a, b in zip(self._v, other._v)])
        return Vector([a * other for a in self._v])

    def __rmul__(self, other):
        return self.__mul__(other)

    def __truediv__(self, other):
        return Vector([a / other for a in self._v])

    def __neg__(self):
        return Vector([-a for a in self._v])

    def __matmul__(self, other):
        # bpy 里 `a @ b` 对两个 Vector 是点乘
        return self.dot(other)

    def dot(self, other):
        if not isinstance(other, Vector):
            raise TypeError('dot 需要 Vector')
        return sum(a * b for a, b in zip(self._v, other._v))

    def cross(self, other):
        if len(self._v) != 3 or len(other._v) != 3:
            raise ValueError('cross 只对 3 维向量有效')
        a, b = self._v, other._v
        return Vector([a[1] * b[2] - a[2] * b[1],
                       a[2] * b[0] - a[0] * b[2],
                       a[0] * b[1] - a[1] * b[0]])

    def normalize(self):
        n = self.length
        if n <= 0:
            raise ValueError('零向量无法归一化')
        inv = 1.0 / n
        self._v = [c * inv for c in self._v]

    def normalized(self):
        out = self.copy()
        out.normalize()
        return out


class Matrix:
    """4×4 矩阵（行主序，与 bpy 的 `Matrix[i][j]` 一致）。"""

    __slots__ = ('_m',)

    def __init__(self, rows=None):
        if rows is None:
            self._m = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
        else:
            self._m = [[float(x) for x in row] for row in rows]

    @staticmethod
    def Identity(size=4):
        return Matrix()

    @staticmethod
    def Translation(vec):
        m = Matrix()
        m._m[0][3] = vec[0]
        m._m[1][3] = vec[1]
        m._m[2][3] = vec[2]
        return m

    @staticmethod
    def Scale(factor, size=4):
        m = Matrix()
        if isinstance(factor, (int, float)):
            s = [factor, factor, factor]
        else:
            s = list(factor)
        for i in range(3):
            m._m[i][i] = s[i]
        return m

    @staticmethod
    def Rotation(angle, size=4, axis='Z'):
        c, s = _cos(angle), _sin(angle)
        m = Matrix()
        if axis == 'X':
            m._m[1][1] = c; m._m[1][2] = -s
            m._m[2][1] = s; m._m[2][2] = c
        elif axis == 'Y':
            m._m[0][0] = c; m._m[0][2] = s
            m._m[2][0] = -s; m._m[2][2] = c
        else:
            m._m[0][0] = c; m._m[0][1] = -s
            m._m[1][0] = s; m._m[1][1] = c
        return m

    def __repr__(self):
        return 'Matrix(%r)' % (self._m,)

    def __getitem__(self, i):
        return self._m[i]

    def matmul(self, other):
        return self.__matmul__(other)

    def __matmul__(self, other):
        if isinstance(other, Vector):
            v = [other[0], other[1], other[2], 1.0]
            out = [sum(self._m[i][j] * v[j] for j in range(4)) for i in range(3)]
            return Vector(out)
        if isinstance(other, Matrix):
            out = [[0.0] * 4 for _ in range(4)]
            for i in range(4):
                for j in range(4):
                    out[i][j] = sum(self._m[i][k] * other._m[k][j] for k in range(4))
            return Matrix(out)
        raise TypeError('Matrix @ 只接受 Vector / Matrix')


class Euler:
    """欧拉角（弧度）。order 只支持 XYZ / ZYX。"""

    __slots__ = ('x', 'y', 'z', 'order')

    def __init__(self, x=0.0, y=0.0, z=0.0, order='XYZ'):
        self.x = float(x)
        self.y = float(y)
        self.z = float(z)
        if order not in ('XYZ', 'ZYX'):
            raise ValueError("Euler 只支持 order='XYZ' / 'ZYX'")
        self.order = order

    def __repr__(self):
        return 'Euler((%.6g, %.6g, %.6g), %r)' % (self.x, self.y, self.z, self.order)

    def to_matrix(self):
        # ⚠ 不用 @：MicroPython 编译器不支持 PEP 465（见 mp_bridge.c 的说明）
        m = Matrix.Rotation(self.x, 4, 'X').matmul(Matrix.Rotation(self.y, 4, 'Y'))
        return m.matmul(Matrix.Rotation(self.z, 4, 'Z'))


class Quaternion:
    """四元数 (w, x, y, z)——与 bpy 的分量顺序一致。"""

    __slots__ = ('w', 'x', 'y', 'z')

    def __init__(self, values=None):
        if values is None:
            self.w, self.x, self.y, self.z = 1.0, 0.0, 0.0, 0.0
        else:
            self.w, self.x, self.y, self.z = [float(v) for v in values]

    def __repr__(self):
        return 'Quaternion((%.6g, %.6g, %.6g, %.6g))' % (self.w, self.x, self.y, self.z)

    def to_matrix(self):
        w, x, y, z = self.w, self.x, self.y, self.z
        m = Matrix()
        m._m[0][0] = 1 - 2 * (y * y + z * z)
        m._m[0][1] = 2 * (x * y - z * w)
        m._m[0][2] = 2 * (x * z + y * w)
        m._m[1][0] = 2 * (x * y + z * w)
        m._m[1][1] = 1 - 2 * (x * x + z * z)
        m._m[1][2] = 2 * (y * z - x * w)
        m._m[2][0] = 2 * (x * z - y * w)
        m._m[2][1] = 2 * (y * z + x * w)
        m._m[2][2] = 1 - 2 * (x * x + y * y)
        return m


def _sin(a):
    """正弦（泰勒展开到 11 阶，|a| ≤ π 时误差 < 1e-8）。

    不用 `math.sin`：少一个 import 就少一个失败点，而这里精度足够。
    """
    x = a
    term = x
    total = x
    for k in range(1, 6):
        term = -term * x * x / ((2 * k) * (2 * k + 1))
        total += term
    return total


def _cos(a):
    x = a
    term = 1.0
    total = 1.0
    for k in range(1, 6):
        term = -term * x * x / ((2 * k - 1) * (2 * k))
        total += term
    return total
