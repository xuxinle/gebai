/*
 * 霜天 · bpy 兼容层装载器（C 侧）
 *
 * ## 职责
 *
 * 把 `bpy_embed.cpp` 登记的源码单元按依赖顺序 exec 进解释器。
 * 这里**不含任何 bpy 语义**——那些全在 `src/python/*.py` 里。
 *
 * ## 为什么共用同一个 globals dict
 *
 * 那几份 .py 是**一份代码被拆开**（为了可读性与 lint），它们之间要能
 * 直接互相引用（`bpy_ops.py` 里调 `_mesh_object`）。所以顺序 exec 进
 * 同一个命名空间——等价于一个文件。
 *
 * 必须包 nlr：任何一份源码有语法错或运行时错，都要能抛出可读的
 * Python 异常，而不是让进程崩在无 nlr 的上下文里（那样只会看到
 * "nlr 链断裂"，而那是初始化阶段最没用的报错）。
 */
#include <stdio.h>
#include <string.h>

#include "py/builtin.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "st/python/bpy_embed.h"

/* 把 MicroPython 的打印输出接到 stderr。
 *
 * ⚠ 必须是**具名函数**而不是 C++ lambda：本文件是 C 文件
 * （`.c`，走 C99 编译），lambda 是 C++ 才有的。
 * 一开始写成 lambda 会报 "expected expression before ']' token"——
 * 而那个错误位置在初始化器里，很难看出根因是"这个文件得按 C 编"。 */
static void stderr_print(void* data, const char* str, mp_uint_t len) {
  (void)data;
  fwrite(str, 1, len, stderr);
}

static const mp_print_t kStderrPrinter = { NULL, stderr_print };
/* 把一段源码 exec 进 `globals`。成功返回 0。 */
static int exec_unit(mp_obj_t globals, const st_bpy_unit_t* unit) {
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_obj_t args[2];
    args[0] = mp_obj_new_str(unit->source, unit->length);
    args[1] = globals;
    (void)mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_builtin_exec_obj), 2, 0, args);
    nlr_pop();
    return 0;
  }
  /* 异常路径：把原因打到 stderr（初始化阶段只有它可靠） */
  fprintf(stderr, "bpy 兼容层加载失败（%s）：", unit->name);
  mp_obj_print_helper(&kStderrPrinter, (mp_obj_t)nlr.ret_val, PRINT_EXC);
  fprintf(stderr, "\n");
  return -1;
}

int st_bpy_bootstrap(mp_obj_t globals) {
  size_t count = 0;
  const st_bpy_unit_t* units = st_bpy_units(&count);
  if (units == NULL || count == 0) {
    fprintf(stderr, "bpy 兼容层：没有可加载的源码单元\n");
    return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    if (exec_unit(globals, &units[i]) != 0) return -1;
  }
  return 0;
}
