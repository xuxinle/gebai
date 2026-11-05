// 霜天 · bpy 兼容层装载器的对外声明
//
// ## 为什么需要这一层
//
// 两件事在语言上互斥：
//
// | 需要 | 只能用 |
// |---|---|
// | `b::embed<"path">()` 编译期嵌入 Python 源码 | **C++**（它是 C++ 模板） |
// | `mp_obj_t` / `exec` / `nlr` 调用 MicroPython | **C**（MicroPython 头是 C99） |
//
// 所以拆成两层：
//
//     bpy_embed.cpp（C++）  取 embed 出来的源码，注册成"可查询的表"
//            ↓
//     bpy_bridge.c  （C）   按依赖顺序 exec 进解释器
//
// 边界就是这份头：只含 C 可见的类型（`const char*` / `size_t`）。
#ifndef ST_PYTHON_BPY_EMBED_H
#define ST_PYTHON_BPY_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 一份可嵌入的源码单元。
typedef struct st_bpy_unit_t {
  const char* name;    ///< 文件名（报错时显示）
  const char* source;  ///< UTF-8 源码（编译期嵌入，非 NUL 结尾也安全）
  size_t length;       ///< 字节数
} st_bpy_unit_t;

/// 按**依赖顺序**返回全部源码单元；`*count` 写数量。
///
/// 依赖顺序是硬约束：`prelude.py`（mathutils）必须最先，
/// `bpy_module.py`（组装）最后——中间的文件会直接引用前者的定义。
const st_bpy_unit_t* st_bpy_units(size_t* count);

#ifdef __cplusplus
}
#endif

#endif  // ST_PYTHON_BPY_EMBED_H
