// 霜天 · MicroPython 移植配置（嵌入宿主进程用）
//
// ## 设计立场
//
// 这份配置只做一件事：**让 Python 能在宿主进程里跑起来，且不碰任何系统能力**。
// 与 QuickJS 那边同一口径——脚本层不给文件/网络/线程访问（本框架的三维脚本
// 只需要建模与数学）。
//
// 取 `MICROPY_CONFIG_ROM_LEVEL_MINIMUM`：关掉一切可选特性（socket/ssl/thread/
// vfs/asyncio…），只留编译器 + GC + 基本类型。实测这档编出来的 .text 约
// 400KB，而 standard 档是数倍。
//
// ## 堆
//
// 不用 `MICROPY_ENABLE_GC` 的静态堆，而是宿主启动后显式调
// `mp_init()` 后由 `gc_init()` 指定一块我们自己管理的区间——这样同一个进程里
// 多个解释器实例（多线程渲染）互不干扰。
//
// ## 没有 REPL / 没有 sys.stdin
//
// 嵌入场景不需要交互。`MICROPY_HELPER_REPL` 关掉，省掉 readline 与
// 一堆终端相关的条件编译。
#ifndef ST_MPCONFIGPORT_H
#define ST_MPCONFIGPORT_H

#include <stdint.h>

// 最小 ROM 档：只留编译器 + GC + 基础对象
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)

// 必须开：要能编译执行 Python 源码（bpy 脚本）
#define MICROPY_ENABLE_COMPILER (1)
// 必须开：脚本里会建大量临时对象（Vector 运算、bmesh 顶点）
#define MICROPY_ENABLE_GC (1)

// 关掉 REPL：嵌入场景无交互
#define MICROPY_HELPER_REPL (0)

// ## 用功能开关而不是改 ROM 档
//
// `MICROPY_CONFIG_ROM_LEVEL_*` 这些宏是在 `py/mpconfig.h` **里**定义的，
// 而 mpconfigport.h 比它先被包含——在这里引用会报 undeclared。
//
// 所以保持 MINIMUM 档，需要什么就单独打开。bpy 脚本实际用到的是：
//   · `%` 字符串格式化（`'z%d' % teeth`）
//   · `math` 模块（三角函数、模数计算）
//   · 类定义（`class Gear:`）
//
// 实测（`tools/python_embed_probe`）：不打开时 `class` 报
// `TypeError: unsupported type for operator`、`import math` 报 `ImportError`
// ——那两条看起来像“我写错了”，实际是**开关没开**。
#define MICROPY_PY_MATH (1)
#define MICROPY_PY_BUILTINS_STR_OP_MODULO (1)

// ## `sys` 必须打开
//
// 默认值是 `MICROPY_CONFIG_ROM_LEVEL_AT_LEAST_CORE_FEATURES`，而我们是
// MINIMUM 档 → 它为 0 → 整个 `py/modsys.c` 被条件编译掉。
//
// 症状很隐蔽：`import sys` 报 `ImportError: module not found`，
// 而 `py/modsys.c` 明明在编译（它在文件列表里）。实际是**那块代码
// 被 `#if MICROPY_PY_SYS` 包着，直接没生成 ELF**。
//
// 为什么必须要：bpy 兼容层靠 `sys.modules` 注册三个模块（见
// `src/python/bpy_module.py`），没有 `sys` 就什么都 import 不进来。
#define MICROPY_PY_SYS (1)
#define MICROPY_PY_SYS_MODULES (1)

// ## `max` / `min` 必须打开
//
// 默认值是 `AT_LEAST_CORE_FEATURES`，MINIMUM 档下为 0。
//
// 这是本移植里**最险的一个开关**：`max`/`min` 是 Python 最常用的内建之一，
// 而缺它时的报错是 `NameError: name not defined`（没有名字）。
//
// 实测经过（值得记）：一个 400 行的齿轮脚本在 `make_gear` 里写了
// `max(bore * hub_scale, root_radius * 0.26)`，于是整个场景建不出来，
// 而报错只有“name not defined”——花了很久才通过“逐名 eval 探测”
// 定位到是 `max`（参见 `tools/st_py` 那个调试入口的用法）。
//
// 教训：**MINIMUM 档剪掉的内建远多于直觉**。凡是 bpy 语料里常见的
// 内建（`max`/`min`/`abs`/`sum`/`sorted`/`enumerate`…）都要验一遍。
// 判据在 `tools/python_features_probe`。
#define MICROPY_PY_BUILTINS_MIN_MAX (1)

// ## MINIMUM 档剪掉的其他常用内建
//
// 全是同一个默认值（`AT_LEAST_CORE_FEATURES`）→ MINIMUM 下为 0。
// 逐条在 `tools/python_features_probe` 里有判据。
//
// 为何全部打开而不是“用到了再加”：
// 缺它们的报错都是 `NameError: name not defined`（**没有名字**），
// 而从报错到定位往往要花几十分钟（本仓实测：一个 `max` 找了很久）。
// 它们加起来只有几 KB，全开是划算的。
#define MICROPY_PY_BUILTINS_REVERSED (1)
#define MICROPY_PY_BUILTINS_ENUMERATE (1)
#define MICROPY_PY_BUILTINS_FILTER (1)
#define MICROPY_PY_BUILTINS_SET (1)
#define MICROPY_PY_BUILTINS_FROZENSET (1)
#define MICROPY_PY_BUILTINS_SLICE (1)

// `property` 必须打开
//
// 默认值同样是 `AT_LEAST_CORE_FEATURES`，MINIMUM 档下为 0——
// 于是 `@property` 装饰器本身不存在，类定义时报
// `NameError: name not defined`（**没说是哪个名字**，因为 MINIMUM 档
// 连异常参数格式化都剪了）。
//
// 为什么必须要：bpy 的 `Vector.x/y/z/w`、`Mesh.vertices` 全是 property——
// 而 `v.x` 这种写法在 bpy 语料里到处都是。
#define MICROPY_PY_BUILTINS_PROPERTY (1)

// 关掉"冻结模块"：bpy 兼容层用 C 模块实现（性能与可控性都更好），
// 不走 .mpy 冻结字节码那条路
#define MICROPY_MODULE_FROZEN_MPY (0)
#define MICROPY_MODULE_FROZEN_STR (0)

// 关掉外部导入：脚本不能 import 任意文件（与 QuickJS 不给系统访问同一立场）
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)

// 类型定义（x86_64 / arm64 都是 8 字节指针）
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

// Windows 没有 <alloca.h>，用 <malloc.h> 的 _alloca
#if defined(_MSC_VER) || defined(_WIN32)
#include <malloc.h>
#define MICROPY_ALLOCA _alloca
#else
#include <alloca.h>
#define MICROPY_ALLOCA alloca
#endif

// 堆大小：不给（用 gc_init 显式指定宿主提供的区间）
#define MICROPY_ALLOC_PATH_MAX (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT (16)

// 板级标识（诊断输出里会带）
#define MICROPY_HW_BOARD_NAME "shuangtian-embed"
#define MICROPY_HW_MCU_NAME "host"

// ## 浮点：用 double
//
// bpy 的 mathutils 全是 double（Blender 内部就是 double）。用 float 会在
// 坐标上丢精度——一个按毫米建模的齿轮，float 只有 7 位有效数字，
// 累加几十次变换后误差肉眼可见。
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_DOUBLE)

// ## 关闭长整型以外的扩展整数特性
//
// 保留任意精度整数（bpy 脚本里用来算模数、齿数这些），但关掉
// `int` 的位运算优化路径（最小档默认已是最简）。
#define MICROPY_PY_BUILTINS_FLOAT (1)

// 需要 dict/list 的有序性（bpy.data 的集合语义依赖插入顺序）
#define MICROPY_PY_COLLECTIONS_ORDEREDDICT (0)

// 关掉不需要的大件
#define MICROPY_PY_ASYNC_AWAIT (0)
#define MICROPY_PY_BUILTINS_MEMORYVIEW (0)
#define MICROPY_PY_SYS_EXIT (0)
#define MICROPY_PY_SYS_STDIO_BUFFER (0)
#define MICROPY_PY_IO (0)
#define MICROPY_PY_STRUCT (0)
#define MICROPY_PY_ARRAY (0)
#define MICROPY_PY_JSON (0)   // bpy 脚本不需要；宿主侧用 C++ 处理 JSON

#endif  // ST_MPCONFIGPORT_H
