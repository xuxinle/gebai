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

### 4.2 `sys` / `property` / 常用内建必须显式打开

本移植用 MINIMUM 档，而**被剪掉的东西比直觉多得多**。

症状统一而致命：全部报 `NameError: name not defined`（**没有名字**）——
因为 MINIMUM 档连异常参数格式化也剪了。

| 缺什么 | 报什么 | 看起来像 |
|---|---|---|
| `sys` | `import sys` → `ImportError: module not found` | “没编 modsys.c”（实际代码被条件编译掉了） |
| `property` | `@property` → `NameError: name not defined` | “我写错了” |
| **`max` / `min`** | `max(a, b)` → `NameError: name not defined` | 同上 |
| `reversed` / `enumerate` / `filter` / `set` / `frozenset` | 同上 | 同上 |

### 实测代价（必读）

一个 400 行的齿轮脚本在 `make_gear` 里写了 `max(bore * hub_scale, root_radius * 0.26)`。
因为 `max` 缺失，**整个场景建不出来**，而报错只有 `NameError: name not defined`。

定位过程（花了很久）：

1. 先怀疑语法/名字拼错 → 用真 CPython 跑（`tools/bpy_script_check.py`）→ **完全通过**
2. 再逐行截断二分 → 卡在“切点落在 docstring 里”而一直不准
3. 最后用**逐名 `eval` 探测**才定位到是 `max`

所以本仓现在有四个诊断入口（都在 `tools/`）：

| 工具 | 用途 |
|---|---|
| `st_py` | 跑一个 .py 文件并打印 result/错误（排查时的 `python` 替代品） |
| `bpy_script_check.py` | 用**真 CPython** 跑脚本（bpy 打桩），语法/名字问题一步到位 |
| `python_features_probe` | 逐条探测 41 项 Python 特性（定位“缺哪个开关”） |
| `bpy_load_probe` | 查 `sys.modules` 状态 |

**教训**：MINIMUM 档下“能力缺失”与“我写错了”的报错长得一模一样。
先用 CPython 排除后者，再用 `python_features_probe` 确认前者。

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
| **GC 根扫描手搬栈** | 大场景报 `Assertion failed: ATB_GET_KIND(area, block) == AT_HEAD, gc.c:914` | `gc_collect_root` 要求**精确的指针数组**，而栈里混着非指针数据 → GC 误标后链表被破坏。正解用上游的 `gc_helper_collect_regs_and_stack()`（同时扫寄存器 + 栈） |
| `mp_hal_stdout_tx_strn_cooked` 返回 void 而 `_strn` 返回 `mp_uint_t` | `conflicting types`（只在 3 个文件报） | 上游两者签名不同，别"统一" |
| `mp_hal_pin_obj_t` 是**宏**检查 | 找不到 `extmod/virtpin.h` | 写 `typedef` 不管用，必须 `#define` |
| `mp_obj_dict_get` 键不存在**抛异常** | `KeyError: result` 而脚本没问题 | 不是返回 NULL，要用 `mp_map_lookup` |
| Windows 默认 GBK | `UnicodeDecodeError: 'gbk'` | 上游生成脚本读文件不指定编码，需 `PYTHONUTF8=1` |
| 运行期往模块表塞会崩 | `mp_init()` 访问违例 | 模块表是编译期常量，走生成器 |
| 读 `camera.target` 当方向 | 判断"角度减半"（假缺陷） | 那是**点**不是方向；`target = eye + dir` |
| 相机对象被当 mesh 抽取 | 日志里假警告 `对象 'cam' 抽取失败` | `obj.data` 是 `Camera` 而非 `Mesh`，读 `.vertices` 抛异常。假警告会淹没真警告 |
| 输出缓冲 8 KB | `JSON 解析失败（第 8192 字节）… unexpected end of input` | 场景 JSON 有数 MB，读回来被截断（看起来像序列化器错了） |
| `bpy.data` 跳 run 存活 | 第二个脚本报 `已存在同名数据块：'Cube'` | 模块级单例是刻意设计（场景状态要住住），需 `bpy.data.reset()` |
| `src/python/*.c` 未入源集合 | `st test` 报 `undefined symbol: st_mp_init` | 框架里 `.c` **从不自动纳入**，必须显式列 |
| 堆 16 MB 不够真实资产 | `Assertion failed: ATB_GET_KIND…`（与 GC 根扫描是两回事） | 8 个齿轮场景撞上堆顶；而且 MicroPython GC 分配失败时**直接 assert 而不是抛 MemoryError**。提到 64 MB |

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

## 八、抽取：bpy 场景 → `ThreeScene`

`BpySceneExtractor::extract(script)` 一次做四件事：

```
bpy.data.reset()          ① 重置场景（避免上个脚本的残留）
      ↓
跑脚本                  ② 建出 bpy 场景
      ↓
_st_extract.extract_json()  ③ Python 侧抽取 → 扁平 JSON
      ↓
parse_extracted_json()  ④ C++ 侧搬进 ThreeScene
```

### 为什么抽取写在 Python 里

C++ 侧读 bpy 对象要逐层调 MicroPython API：

```cpp
mp_obj_t obj = mp_obj_dict_get(...);
mp_obj_t loc = mp_load_attr(obj, MP_QSTR_location);   // 每个属性一次查表
mp_obj_t x   = mp_load_attr(loc, MP_QSTR_x);          // 还要再查一层
double vx = mp_obj_get_float(x);
```

三层深度的对象（`obj.data.vertices[0].co.x`）就是四五次查表 + 类型检查，
每个调用点都要写错误处理。而 Python 侧写就是自然表达：

```python
for v in obj.data.vertices:
    out.append(v.co.x)
```

所以分工是：**Python 理解 bpy，C++ 搬数据**。

### 抽取里三个容易错的地方（都已钉进探针判据）

| 项 | 错法 | 症状 |
|---|---|---|
| 矩阵布局 | bpy 行主序 vs `Mat4` 列主序 | 位置对但旋转/缩放错位 |
| `fov_y` 单位 | bpy 弧度 vs `Camera.fov_y_degrees` 角度 | 视野极窄、只看到一角 |
| 灯光方向 | `Light.direction` 是**指向光源**（与相机 -Z 视线相反） | 物体被从背面照亮 |

### 已知边界

- **面三角化用扇形**：凸多边形正确，凹多边形会出错（`primitive_*` 与
  bmesh 建的面绝大多数是凸的）
- **多材质只取第 0 个槽**：本框架 `Mesh` 是单一材质
- **父子关系不抽**：`matrix_world` 已是绝对变换，父子只影响那个矩阵
- **顶点留局部空间**：变换由 `ThreeObject::world` 承担——两处都变换就是变换两遍

### 验证

```sh
st build bpy_scene_probe && ./build/dev/bin/bpy_scene_probe
```

10 条判据：立方体顶点/索引数、世界矩阵平移、材质 PBR 参数、bmesh 三角面、
相机 fov 单位转换、错误路径可读性。

## 九、接入 `scene3d_script`（三路并存）

`SceneSpec` 现在有**三条**表达路径，按优先级派发：

| 字段 | 优先级 | 说明 |
|---|---|---|
| `bpy_script` | 1 | bpy 源码（Blender Python API） |
| `three_script` | 2 | three.js 源码 |
| `objects` | 3 | 声明式 JSON（原有路径） |

### 自动识别（文本入口）

`parse_scene_spec_text()` 是裁云 `model_script` / 三维预览 / 自检三条路的
共同入口，它按下面顺序分流：

1. **先试 JSON** —— 合法 JSON 且是对象 ⇒ 声明式（里面可带脚本字段）
2. **看是不是 Python** —— 行首含 `import bpy` / `import bmesh` /
   `from mathutils` ⇒ bpy
3. 否则 ⇒ three.js

为何靠 import 语句而不是语法分析：真要做“Python 还是 JS”得写两个词法器，
而**一个三维脚本不可能不 import 它的库**（bpy 脚本必 `import bpy`，
three 脚本必有 `THREE.`）。两个特征都稳。

### 渲染路径统一

`render_scene` 里两条脚本路**共用同一段**（只在前三行决定取哪份
`ThreeScene`）：

```cpp
auto script_scene = bpy_mode ? BpySceneExtractor::extract(spec.bpy_script)
                            : ThreeRuntime::instance().evaluate(...);
// 以下逐行相同：camera_override / extra_transform / material_override / end_frame
```

好处：裁云的镜头编排（推拉/机位/整体姿态/统一材质）**自动对 bpy 生效**
——否则要写两套，而那正是“改一处漏一处”的源头。

### 同时写两个脚本时

不报错，**bpy 胜**，并记一条 warn。理由：两者同时写一定是误操作，而模型
从两个例子各拿一段是常见情形——报错会让一份“其实能跑”的配置失败，
而给一个确定顺序再加警告更有用。

## 十、验证

```sh
st build python_embed_probe && ./build/dev/bin/python_embed_probe    # 18/18
st build python_features_probe && ./build/dev/bin/python_features_probe
st build bpy_scene_probe && ./build/dev/bin/bpy_scene_probe          # 10/10
st build bpy_script_probe && ./build/dev/bin/bpy_script_probe        # 7/7
```

`bpy_script_probe` 的 7 条判据：

1. 纯 Python 文本自动走 bpy 分支（three_script 未被误填）
2. **出像素**（非背景 18%——漏 `end_frame` 会是 0%）
3. 相机覆盖对 bpy 生效（18% → 52%）
4. 材质覆盖对 bpy 生效（亮红 17.9%）
5. 优先级 bpy > three
6. 错误路径可读

> 为何第 2 条必须用**像素判据**：`draw_mesh` 只是提交到内部缓冲，
> 合成到画布发生在 `end_frame`。漏了它，`draw_calls` 会涨、内部缓冲也有内容，
> 但画布上一个像素都没有（本仓实测过一次：抽取出 3 个方块、位置全对，
> 而像素计数为 0）。

## 十一、下一步

1. **真实资产端到端**：用 bpy 写齿轮箱/涡轮盘，与 three.js 版本对照像素
2. 凹多边形三角化（现在是扇形）
3. 多材质槽（现在是第 0 个）
