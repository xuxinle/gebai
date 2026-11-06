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
class _World:
    """世界背景（只实现环境反射需要的最小面）。

    ## 为何同时提供 `background_color` 与 `node_tree`（2026-11-06 补）

    真 bpy 的颜色**只在节点树里**：

        bg = world.node_tree.nodes['Background']
        bg.inputs['Color'].default_value = (0.05, 0.05, 0.05, 1.0)

    而本兼容层只想提供一个直接字段（实现节点树的成本远高于收益）。
    代价是：「两边都能跑」的脚本必须写两套。实测撞过：

        AttributeError: 'World' object has no attribute 'background_color'

    ## 解法：一个极小的节点树影子

    只实现 `world.node_tree.nodes['Background'].inputs['Color'|'Strength']`
    这一个路径——它写进去直接落到 `background_color` / `background_strength`。
    于是两种写法看到的是**同一份数据**，不会漂移。
    """

    def __init__(self, name='World'):
        self.name = name
        # 与 Blender 默认世界灰一致（0.05 中性灰）
        self.background_color = (0.05, 0.05, 0.05)
        self.background_strength = 1.0
        self.use_nodes = False
        self.node_tree = _WorldNodeTree(self)

    def __repr__(self):
        return 'World(%r)' % (self.name,)


class _WorldSocket:
    """`Background` 节点上的一个输入口（只有 `default_value`）。"""

    __slots__ = ('_world', '_kind')

    def __init__(self, world, kind):
        self._world = world
        self._kind = kind

    @property
    def default_value(self):
        if self._kind == 'color':
            c = self._world.background_color
            return (c[0], c[1], c[2], 1.0)
        return self._world.background_strength

    @default_value.setter
    def default_value(self, value):
        if self._kind == 'color':
            self._world.background_color = (float(value[0]), float(value[1]),
                                            float(value[2]))
        else:
            self._world.background_strength = float(value)


class _BackgroundNode:
    """`Background` 节点：只给 `inputs`。"""

    __slots__ = ('inputs',)

    def __init__(self, world):
        self.inputs = {
            'Color': _WorldSocket(world, 'color'),
            'Strength': _WorldSocket(world, 'strength'),
        }


class _WorldNodes:
    """`world.node_tree.nodes`：只支持 `nodes['Background']` 与 `.get(...)`。"""

    __slots__ = ('_background',)

    def __init__(self, world):
        self._background = _BackgroundNode(world)

    def __getitem__(self, key):
        if key == 'Background':
            return self._background
        raise KeyError('找不到节点：%r（本兼容层只实现 Background）' % (key,))

    def get(self, key, default=None):
        return self._background if key == 'Background' else default


class _WorldNodeTree:
    """`world.node_tree`：只给 `nodes`。"""

    __slots__ = ('nodes',)

    def __init__(self, world):
        self.nodes = _WorldNodes(world)




# ---- bpy.data ----
class _Data:
    """`bpy.data`：数据块集合的宿主。"""

    def __init__(self):
        self.meshes = _Collection(Mesh)
        self.objects = _Collection(Object)
        self.materials = _Collection(Material)
        self.cameras = _Collection(Camera)
        self.lights = _Collection(Light)
        # `bpy.data.worlds`：Blender 里 `bpy.data.worlds.new('World')` 是标准写法
        # （参照脚本 `tools/blender_reference.py:336` 就是这么建世界的）。
        # 缺它会让“参数与参照同构”这件事做不到——实测报
        # `AttributeError: no such attribute`，于是对标里混进了一条**参数差**
        # 而不是渲染差（背景占画面 80%，一差就是十几级）。
        self.worlds = _Collection(_World)

    def reset(self):
        """清空场景（新脚本开始前调）。

        ## 为什么需要

        `bpy.data` 是**模块级单例**，跨 `run` 存活（刻意的：否则
        “跑完脚本场景就没了”）。代价是连续跑两个脚本时第二个会看到
        第一个的残留，报 `ValueError: 已存在同名数据块`。

        宿主侧 `BpySceneExtractor::extract` 每次跑脚本前会调它。

        ## 为何连 scene 上的自定义属性也要清（2026-11-06 修）

        `self.cameras.clear()` 这些只清了**数据块**，而 `bpy.context.scene`
        上由脚本挂的**自定义属性**（如 `__st_compat__`）不在任何 collection 里，
        于是会跨 run 残留。

        实测后果（很严重）：先跑一份声明了 `__st_compat__='cycles'` 的脚本，
        再跑一份**没声明**的，后者会意外继承对标口径
        （`tone_map` 从 AgX 变成 None、`ambient` 从 0.35 变成 0）。
        现象是“后一份场景无故变暗”，而报错为零。

        所以把 scene 上的可写属性也清掉（除了 `name` 与 `world`——
        后者由模块初始化负责）。
        """
        self.meshes.clear()
        self.objects.clear()
        self.materials.clear()
        self.cameras.clear()
        self.lights.clear()
        _reset_scene_attributes()


_data = _Data()
_context = _Context()
_context.scene = _Module('Scene')
_context.scene.name = 'Scene'


# 场景上的自定义属性（脚本挂的，不在任何 collection 里）。
#
# ## 为何要单独记一份
#
# `_Module` 是普通对象，脚本可以随时 `scene.任意名 = 值`。`_Data.reset()`
# 只能清数据块，清不了这些。而它们会跨 run 残留——实测后果見
# `_Data.reset()` 的说明（后一份场景意外继承对标口径）。
#
# 用一份白名单不行（无法预知脚本会挂什么），所以重置时直接重建一个
# 干净的 `_Module`，只保留 `name` 与 `world`。
_SCENE_RESERVED = ('name', 'world')


def _reset_scene_attributes():
    """把 `bpy.context.scene` 上的自定义属性清掉（保留 `name` / `world`）。

    用「重建」而不是「逐项删除」：属性名不可枚举（`_Module` 没有
    `__dict__` 遍历的保证），而重建一定干净。
    """
    fresh = _Module('Scene')
    fresh.name = 'Scene'
    for key in _SCENE_RESERVED:
        if hasattr(_context.scene, key):
            setattr(fresh, key, getattr(_context.scene, key))
    _context.scene = fresh


# ---- 世界背景（环境反射的来源）----
#
# ## 为何需要它（2026-11-05）
#
# 环境反射是**金属出色的唯一来源**。而本框架 `env_sky` 默认是
# `(0.52, 0.58, 0.68)`（一个明亮的天空），而典型 bpy 脚本的世界背景
# 只相当于线性 `0.006`——差 87 倍。
#
# 后果：金属被一个**不存在的亮天空**照亮，主体均值 139.6 vs 真渲染器 78。
#
# ## 为何用 `background_color` 而不是节点树
#
# 真实 bpy 的颜色在 `world.node_tree.nodes['Background'].inputs['Color']`。
# 我们的兼容层没有节点树（与材质同样的取舍，见 `docs/python.md`）。
# 所以提供一个**直接字段**：脚本能用、抽取器能读、与“节点值默认同步”的
# 真实行为意图一致。




_context.scene.world = _World()


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


# ---- 抽取器（内部模块）----
#
# ## 为何是独立模块而不是直接给 bpy 加方法
#
# 抽取是**宿主侧的活**，不是 bpy 的功能。真实 Blender 里没有
# `bpy.export_to_three()` 这种东西，把它挂在 `bpy` 上会让模型以为
# 那是 bpy 的 API（然后去用，然后在真 Blender 里失败）。
#
# 用下划线开头的内部模块名，表达"这是实现细节"。
_st_extract = _Module('_st_extract')
# 注入宿主（`bind` 是 `extract.py` 里的全局函数，不是模块方法——
# 写成 `_st_extract.bind(...)` 会报 `AttributeError: no such attribute`，
# 而 `_Module` 是个普通对象，那个报错不会告诉你是哪个属性）
bind(_data, _context, Mesh, Camera, Light)
_st_extract.extract = extract
_st_extract.extract_json = extract_json

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
_sys.modules['_st_extract'] = _st_extract
