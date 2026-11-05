"""霜天 · bpy 兼容层组装入口

## 这份文件做什么

把前面几份源码拼成一个可执行的 prelude，运行后提供三个模块：

    import bpy          # 数据 / 上下文 / 操作
    import bmesh        # 低层网格编辑
    from mathutils import Vector, Matrix, Euler, Quaternion

## 为什么要"拼接"而不是真的做成三个模块文件

MicroPython 的模块机制要么走 C（我们已经踩到 const 段重定位的坑），
要么走文件系统（嵌入场景没有 FS）。第三条路是**在初始化时 exec 一段
源码**，把得到的对象挂进 `sys.modules` 里——那条路最稳，也最好调试。

代价是源码得拼成一个字符串。这里按"内聚优先"切三份：

| 文件 | 内容 |
|---|---|
| `prelude.py` | mathutils（纯数学，无依赖） |
| `bpy_data.py` | bpy.data 的数据块类 |
| `bpy_ops.py` | 基本体生成器 |
| `bpy_ops_ns.py` | bpy.ops 命名空间 + bmesh |
| `bpy_module.py` | 本文件：组装 + 生成三个模块对象 |
"""

# 兜底：如果有人只 exec 了部分文件，至少把缺失的名字给出来
try:
    Vector
except NameError:
    raise RuntimeError('bpy 兼容层缺少 mathutils 部分（prelude.py 未加载）')


class _Module:
    """一个极简的模块对象（`sys.modules` 里塞的就是它）。

    ## 为什么不用 `types.ModuleType`

    MicroPython 的 `types` 模块在 MINIMUM 档里没有。而我们需要的只是
    "能 `from x import y` 的容器"——一个带属性的对象就够了。
    """

    def __init__(self, name):
        self.__name__ = name

    def __repr__(self):
        return '<module %r>' % (self.__name__,)


# ---- mathutils 模块 ----
_mathutils = _Module('mathutils')
_mathutils.Vector = Vector
_mathutils.Matrix = Matrix
_mathutils.Euler = Euler
_mathutils.Quaternion = Quaternion


# ---- bpy.data ----
class _Data:
    """`bpy.data`：数据块集合的宿主。"""

    def __init__(self):
        self.meshes = _Collection(Mesh)
        self.objects = _Collection(Object)
        self.materials = _Collection(Material)
        self.cameras = _Collection(Camera)
        self.lights = _Collection(Light)


_data = _Data()
_context = _Context()
_context.scene = _Module('Scene')
_context.scene.name = 'Scene'


# ---- bpy 模块 ----
_bpy = _Module('bpy')
_bpy.data = _data
_bpy.context = _context
_bpy.ops = _Ops()
# bpy 的版本串：脚本里偶尔会做能力判断
_bpy.app = _Module('app')
_bpy.app.version = (3, 6, 0)
_bpy.app.version_string = '3.6.0 (shuangtian bpy subset)'


# ---- bmesh 模块 ----
_bmesh = _Module('bmesh')
_bmesh.new = BMesh
_bmesh.BMesh = BMesh
_bmesh.verts = _BMVertSeq
_bmesh.faces = _BMFaceSeq


# ---- 注册到 sys.modules ----
#
# ## 为什么必须注册
#
# `import bpy` 会先查 `sys.modules`；命中就不再找文件。这是能让
# **真实 bpy 脚本原样跑通**的关键——脚本里的 import 语句不需要改。
import sys as _sys

_sys.modules['mathutils'] = _mathutils
_sys.modules['bpy'] = _bpy
_sys.modules['bmesh'] = _bmesh
