# 内嵌 Python（bpy 三维资产构建）

> 状态：**地基完成，三维抽取待接**。本文档记录能力边界与已知坑。
> 相关：`docs/three3d.md`（three.js 那条路）、`design/02b-builtin-agents.md`。

## 一、为什么做这件事

本框架的三维表达层已有两条：

| 表达层 | 语料丰富度 | 适合 |
|---|---|---|
| `scene3d_script`（JSON） | 差（只有本框架自己的模型见过） | 简单声明式场景 |
| three.js | 极好 | 一般三维场景 |
| **bpy（本文）** | **极好，且在"精确建模"上更厚** | 机械件、程序化几何、资产级建模 |

Blender 是"写代码建三维"的默认工具，模型对 `bmesh` / `bpy.ops.primitive_*` /
`mathutils` 的熟悉度高于 three.js 的建模部分。所以第三条路值得做。

## 二、怎么实现的

**MicroPython 内嵌 + Python 实现的 bpy 兼容层。**

```
src/python/
  mp_bridge.c         C 胶水：初始化解释器、exec 源码、取回 result
  mphal_port.cpp      移植层：时钟/输出/堆分配
  runtime.cpp         C++ 侧：线程内单例、Result 包装
  bpy_bridge.c        bpy 兼容层装载器（按依赖顺序 exec）
  bpy_embed.cpp       b::embed 取 .py 内容（编译期嵌入）
  prelude.py          mathutils
  bpy_data.py         bpy.data（meshes/objects/materials/cameras/lights）
  bpy_ops.py          基本体生成器
  bpy_ops_ns.py       bpy.ops 命名空间 + bmesh
  bpy_module.py       组装三个模块并注册到 sys.modules
third_party/micropython/
  py/                 MicroPython 上游（204 文件，2.35 MB，一字未改）
  mpconfigport.h      移植配置（只留必要能力）
  mphalport.h         移植层声明
  genhdr/             构建期生成（10 KB，已入库）
```

### 为什么 bpy 用 Python 而不是 C 扩展

走过 C 类型那条路：`MP_DEFINE_CONST_OBJ_TYPE` 产出的类型一旦通过
`MP_ROM_PTR` 进入模块导出表，进程就在 `mp_init()` 阶段**访问违例
（0xC0000005）**，无任何输出。

已排除：所有槽位（`make_new`/`print`/`attr`/`binary_op`/`unary_op`/
`subscr`/`locals_dict`）单独加都正常；四种注册方式（运行期三种 + 编译期
`MP_REGISTER_MODULE`）全试过；拿掉那个导出就一切正常。判断是 MinGW 下
`const` 数据段跨单元重定位的问题。

Python 实现的取舍：

| 项 | C 类型 | Python |
|---|---|---|
| 正确性风险 | 高 | 无 |
| 性能 | 快 | 慢 3~8 倍 |
| 加一个方法 | 要重编 | 改文本 |

**先要正确，再要快。** mathutils 的调用密度远低于渲染热路径——它在
"建几何时"被调（每顶点一次），典型场景几万个顶点，慢几倍也只是几十毫秒，
相对渲染（百毫秒级）可接受。将来若成瓶颈，只把热点改成 C，接口不变。

## 三、当前支持什么

### mathutils

| 有 | 没有（明确 AttributeError） |
|---|---|
| `Vector((x,y,z))`、`.x/.y/.z/.w` 读写、`length` | `rotation_difference` 等高阶几何 |
| `+ - * /`、`dot`、`cross`、`normalize`/`normalized`、`copy` | — |
| `Matrix.Identity/Translation/Scale/Rotation`、`matmul` | `LocRotScale` |
| `Euler(x,y,z,order)`、`to_matrix` | 12 种欧拉序（只做 XYZ/ZYX） |
| `Quaternion`、`to_matrix` | slerp 等插值 |

### bpy

| 有 | 没有 |
|---|---|
| `bpy.data.meshes/objects/materials/cameras/lights`（`.new()`、按名/下标索引） | 修改器栈、约束、动画曲线 |
| `bpy.context.active_object/selected_objects/scene` | 场景切换、视图层 |
| `bpy.ops.mesh.primitive_{cube,cylinder,cone,uv_sphere,plane,torus,grid}_add` | 其余 primitive |
| `bpy.ops.object.{select_all,delete,origin_set}` | 其余 object 操作 |
| `Object.location/rotation_euler/scale/matrix_world` | 父子关系、约束 |

### bmesh

| 有 | 没有 |
|---|---|
| `new()`、`verts.new/ensure_lookup_table/index_update` | 边（`bm.edges`） |
| `faces.new`、`to_mesh`/`from_mesh`、`free` | 斜切/挤出/细分等编辑算子 |

## 四、三条必须记住的边界

### 4.1 `@` 运算符不可用

MicroPython 1.24 的**编译器不支持 PEP 465**：`py/lexer.c` 认 `@` 这个
token，但 `py/compile.c` 里**没有任何 `MP_TOKEN_OP_AT` 的语法处理分支**。

于是 `rot @ v` 报 `TypeError: unsupported type for operator`——那条报错
看起来像"类型不对"，实际是"语法不支持"。

本框架的做法：**不静默改写源码**（改写要写 Python 词法器、要区分装饰器/
字符串/注释，风险大于收益），而是

- 兼容层内部一律用 `Matrix.matmul()` / `Vector.dot()`
- `__matmul__` 方法仍保留，可显式调用 `a.__matmul__(b)`
- **文档里写明这条**——比偷偷改写诚实

真实 bpy 脚本若用了 `@`，需要把它改成 `.matmul()` / `.dot()`。

### 4.2 `sys` / `property` 必须显式打开

`MICROPY_PY_SYS` 与 `MICROPY_PY_BUILTINS_PROPERTY` 的默认值都是
`AT_LEAST_CORE_FEATURES`，而本移植用 MINIMUM 档 → 值为 0 → 整块代码
**被条件编译掉**。

症状极具误导性：

| 缺什么 | 报什么 | 看起来像 |
|---|---|---|
| `sys` | `import sys` → `ImportError: module not found` | "没编 modsys.c"（实际它在编译，只是代码被剪了） |
| `property` | `@property` → `NameError: name not defined` | "我写错了"（没说哪个名字，因为 MINIMUM 档连异常参数格式化都剪了） |

判据：`tools/python_features_probe` 逐条探测 20 项语言特性。

### 4.3 genhdr 是构建期生成的，已入库

`third_party/micropython/genhdr/` 四个头文件由
`tools/micropython_genhdr.py` 生成，**已提交入库**（10 KB）——
这样构建不依赖 Python（与 SQLite amalgamation 同一思路）。

改了 `mpconfigport.h` 或新增自己的 `MP_QSTR_*` / `MP_REGISTER_MODULE` 后
**必须重新生成**：

```sh
export ST_EXTRA_QSTR_DIRS="..\..\src\python"   # 让扫描带上我们自己的 .c
py tools/micropython_genhdr.py third_party/micropython build/mpgen
cp build/mpgen/genhdr/* third_party/micropython/genhdr/
```

生成器的七个坑（都写在 `tools/micropython_genhdr.py` 的注释里）：

1. qstr 扫描必须走**预处理器**（`gcc -E`），否则漏掉所有 `#if` 内的 qstr
2. `QCFG(...)` 不是 `#define`，且 `py/qstrdefs.h` 自带值是**宏名**的占位行
3. 扫描时必须 `-DNO_QSTR`，否则 `MP_REGISTER_MODULE` 展开成空、模块表恒 0
4. split 输出文件名是 `<path>.c.module`——**后缀**不是前缀
5. 相邻两条宏（`MP_REGISTER_MODULE` 紧跟 `..._DELEGATION`）会被合并，
   要自己按括号配平取第一条
6. `mpversion.h` 必须在**预处理之前**就存在（`modsys.c` 无条件 include）
7. root pointer 要扫**所有** `py/*.c`（不只 `mpstate.h`）

## 五、其他实测踩到的坑

| 坑 | 症状 | 原因 |
|---|---|---|
| `mp_hal_stdout_tx_strn_cooked` 返回 void 而 `_strn` 返回 `mp_uint_t` | `conflicting types`（只在 3 个文件报） | 上游两者签名不同，别"统一" |
| `mp_hal_pin_obj_t` 是**宏**检查 | 找不到 `extmod/virtpin.h` | 写 `typedef` 不管用，必须 `#define` |
| `mp_obj_dict_get` 键不存在**抛异常** | `KeyError: result` 而脚本没问题 | 不是返回 NULL，要用 `mp_map_lookup` |
| Windows 默认 GBK | `UnicodeDecodeError: 'gbk'` | 上游生成脚本读文件不指定编码，需 `PYTHONUTF8=1` |
| 运行期往模块表塞会崩 | `mp_init()` 访问违例 | 模块表是编译期常量，走生成器 |
| 读 `camera.target` 当方向 | 判断"角度减半"（假缺陷） | 那是**点**不是方向；`target = eye + dir` |

## 六、怎么用

```python
import bpy
import bmesh
from mathutils import Vector

# 基本体
bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=1.0, depth=2.0)
ring = bpy.context.active_object
ring.location = Vector((0, 0, 1.0))

# 逐顶点（齿轮/叶片这类靠它）
mesh = bpy.data.meshes.new('blade')
bm = bmesh.new()
root = bm.verts.new((0, 0, 0))
tip = bm.verts.new((0, 0, 2))
# …建面…
bm.faces.new((root, tip, bm.verts.new((1, 0, 0))))
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new('blade', mesh)
obj.material_slots.append(bpy.data.materials.new('steel'))

# 结果：把要交给渲染的东西放进 result
result = obj
```

**约定**：脚本结尾把"要交给渲染的东西"赋给 `result`。
（不用"最后一个表达式"——Python 没那语义，要复刻得手写 AST 遍历。）

## 七、验证

```sh
st build python_embed_probe && ./build/dev/bin/python_embed_probe
st build python_features_probe && ./build/dev/bin/python_features_probe
st build bpy_load_probe        && ./build/dev/bin/bpy_load_probe
```

| 探针 | 判的东西 |
|---|---|
| `python_embed_probe` | 18 条端到端判据（语言基础 + mathutils + bpy + bmesh） |
| `python_features_probe` | 20 项 Python 特性逐条探测（定位"缺哪个开关"） |
| `bpy_load_probe` | `sys.modules` 状态（诊断"装载成功但 import 失败"） |

## 八、下一步

1. **bpy 场景 → `ThreeScene` 抽取器**：把 `bpy.data.objects` 里带 mesh 的
   对象（顶点 + 面 + 材质 + 世界矩阵）转成 `ThreeObject` 列表
2. **接进 `scene3d_script`**：加 `bpy_script` 字段，与 `three_script` / JSON
   三路并存
3. 面三角化（bpy 支持多边形，本框架渲染只吃三角形）
4. 端到端：用 bpy 写一个真实资产（齿轮箱/涡轮盘），与 three.js 版本对照
