// 霜天 · bpy 兼容层源码的编译期嵌入（C++ 侧）
//
// ## 这一层为什么必须是 C++
//
// `b::embed<"path">()` 是 C++ 模板——它在**编译期**把文件内容变成一段
// 只读内存（框架的 `three.cjs` 就是这么进的）。而 MicroPython 那侧要的是
// C 函数。所以这里只做"取内容、登记成表"，不碰任何 MicroPython 类型。
//
// 好处：Python 源码是**真正的 .py 文件**（可读、可 lint、可用真实解释器
// 跑测试），而不是 C 字符串字面量（每改一行都要转义，中文注释还要处理编码）。
#include <cstddef>

#include "battery/embed.hpp"
#include "st/python/bpy_embed.h"

namespace {

// ## 依赖顺序是硬约束
//
// 数组顺序 = 执行顺序。`prelude.py` 定义 Vector 等数学类型，
// `bpy_data.py` 直接用它们；`bpy_module.py` 最后组装并注册模块。
// 顺序错了会得到 NameError（而那是加载期，报错信息只有 stderr）。
const st_bpy_unit_t kUnits[] = {
    {"prelude.py", b::embed<"src/python/prelude.py">().data(),
     b::embed<"src/python/prelude.py">().size()},
    {"bpy_data.py", b::embed<"src/python/bpy_data.py">().data(),
     b::embed<"src/python/bpy_data.py">().size()},
    {"bpy_ops.py", b::embed<"src/python/bpy_ops.py">().data(),
     b::embed<"src/python/bpy_ops.py">().size()},
    {"bpy_ops_ns.py", b::embed<"src/python/bpy_ops_ns.py">().data(),
     b::embed<"src/python/bpy_ops_ns.py">().size()},
    {"bpy_module.py", b::embed<"src/python/bpy_module.py">().data(),
     b::embed<"src/python/bpy_module.py">().size()},
};

}  // namespace

extern "C" {

auto st_bpy_units(size_t* count) -> const st_bpy_unit_t* {
  if (count != nullptr) *count = sizeof(kUnits) / sizeof(kUnits[0]);
  return kUnits;
}

}  // extern "C"
