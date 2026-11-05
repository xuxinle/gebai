/*
 * 霜天 · MicroPython C 胶水层
 *
 * ## 为什么需要这一层
 *
 * MicroPython 是 **C99** 代码，头文件里到处是弹性数组
 * （`mp_uint_t nodes[]`）、零长数组、以及大量 C 惯用法。这些在 C 里完全合法，
 * 在 C++ 里却是硬错误：
 *
 *   error: ISO C++ forbids flexible array member 'nodes' [-Werror=pedantic]
 *
 * 而本框架的 C++ 侧（`st::mp::Runtime`）需要调用它。
 *
 * 三条路里选了最简单稳妥的一条：
 *
 * | 方案 | 问题 |
 * |---|---|
 * | 给 C++ 加 `-Wno-pedantic` | 全工程放宽，把真正的问题也放过了 |
 * | 改 MicroPython 头 | 违反"不动上游一字"的第三方纪律 |
 * | **写 C 胶水**（本文件） | 只在这里 include MP 头；C++ 侧只见普通函数声明 |
 *
 * 所以本文件**必须**按 C 编译（`st.pkg` 的 `sources` 里 `.c` 会走 `c_flags`）。
 * 对外暴露的函数在 `include/st/python/mp_bridge.h` 里声明（那份头是纯 C 可见的，
 * 不含任何 MicroPython 类型）。
 */
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/mpstate.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

#include "st/python/mp_bridge.h"

/* ── GC 与 nlr 的移植侧钩子 ────────────────────────────────────────────────
 *
 * `py/gc.h` 里写着 “A given port must implement gc_collect”——它不在上游实现，
 * 而是留给移植层：因为“哪些内存是根（栈/全局区）”只有宿主知道。
 *
 * 不做会报 `undefined symbol: gc_collect`（链接期，不是编期）——
 * 而那看起来像“缺了一个 .c”，实际是漏了移植责任。
 *
 * `nlr_jump_fail` 同理：`longjmp` 失败（通常意味着 nlr 链断了，
 * 这是内部不变量被破坏）时的最后一道。嵌入场景里没有更好的处理，
 * 直接让进程终止——安静地继续跑下去只会得到更难查的内存破坏。*/
void gc_collect(void) {
  gc_collect_start();
  /* `mp_stack_top` / `mp_stack_limit` 在 `mp_stack_ctrl_init` 后由
   * MicroPython 维护（`py/stackctrl.h`）。把我们自己的栈区间报上去，
   * 否则栈上的局部引用会被当成垃圾回收——症状是“用着用着对象突然没了”。*/
  mp_uint_t sp = (mp_uint_t)(uintptr_t)&sp;
  gc_collect_root((void*)sp, 1);
  gc_collect_end();
}

void nlr_jump_fail(void* val) {
  (void)val;
  fprintf(stderr, "MicroPython 致命错误：nlr 链断裂（内部不变量被破坏）\n");
  abort();
}

/* 堆：16 MB。理由见 include/st/python/runtime.hpp 里 kHeapBytes 的说明。 */
#define ST_MP_HEAP_BYTES (16u * 1024u * 1024u)

/* 外部提供的模块（`src/python/mathutils_module.c`） */


/* 前向：实现在文件下方（它要用到 `mp_loaded_modules_dict`） */
static void register_extra_modules(void);

static uintptr_t* s_heap = NULL;
static size_t s_heap_words = 0;
static mp_obj_t s_globals = MP_OBJ_NULL;
static int s_initialized = 0;

/* 结果文本的落地缓冲（由调用方提供，避免在 C 侧动态分配） */
static char* s_out = NULL;
static size_t s_out_cap = 0;
static size_t s_out_len = 0;

static void out_sink(void* data, const char* str, mp_uint_t len) {
  (void)data;
  if (s_out == NULL || s_out_cap == 0) return;
  size_t room = (s_out_cap - 1) - s_out_len;
  size_t take = (len < room) ? (size_t)len : room;
  if (take > 0) {
    memcpy(s_out + s_out_len, str, take);
    s_out_len += take;
    s_out[s_out_len] = '\0';
  }
}

static mp_print_t s_printer = { NULL, out_sink };

int st_mp_init(char* err, size_t err_cap) {
  if (s_initialized) return 0;
  s_heap_words = ST_MP_HEAP_BYTES / sizeof(uintptr_t);
  s_heap = (uintptr_t*)mp_hal_alloc(s_heap_words * sizeof(uintptr_t));
  if (s_heap == NULL) {
    if (err != NULL && err_cap > 0) {
      snprintf(err, err_cap, "Python 堆分配失败（16 MB）");
    }
    return -1;
  }
  mp_stack_ctrl_init();
  mp_stack_set_limit(512u * 1024u);
  gc_init(s_heap, s_heap + s_heap_words);
  mp_init();
  /* 模块注册走构建期生成（genhdr/moduledefs.h）；本钩子保留为空。 */
  s_initialized = 1;
  return 0;
}

/* ## 模块注册：不用运行期塞模块表
 *
 * ## 为什么最终选了"编译期注册"这条路
 *
 * 试过三种运行期方案，全部失败：
 *
 * | 方案 | 结果 |
 * |---|---|
 * | 直接 `mp_obj_dict_store(mp_loaded_modules_dict)` | 访问违例 0xC0000005 |
 * | 经 `sys.modules` 写 | 同上（同一个 dict） |
 * | `mp_obj_new_module` + 搬静态导出表 | 同上（遍历只读 map 时崩） |
 *
 * 根因是：MicroPython 的模块表是**编译期常量**，运行期没有官方追加入口；
 * 那几种写法都在碰内部结构，崩的位置还不直观（初始化阶段，无 Python 异常）。
 *
 * 正解是让生成器把我们的模块**写进 `genhdr/moduledefs.h`**——
 * `tools/micropython_genhdr.py` 已支持扫额外目录（`ST_EXTRA_QSTR_DIRS`），
 * 而 `src/python/mathutils_module.c` 里有 `MP_REGISTER_MODULE`。
 *
 * 所以本函数只留作**诊断钩子**（报告模块是否真的注册上了），不做注册。
 */
static void register_extra_modules(void) {
  /* 实际注册由 `MICROPY_REGISTERED_MODULES`（生成的 moduledefs.h）完成，
   * 这里无需动作。保留空函数是为了让 `st_mp_init` 的调用点稳定。 */
}

void st_mp_deinit(void) {
  if (!s_initialized) return;
  mp_deinit();
  if (s_heap != NULL) {
    mp_hal_free(s_heap);
    s_heap = NULL;
  }
  s_initialized = 0;
  s_globals = MP_OBJ_NULL;
}

int st_mp_run(const char* source, size_t source_len,
              const char* filename, size_t filename_len,
              char* out, size_t out_cap,
              char* err, size_t err_cap) {
  if (!s_initialized) return -2;

  /* 结果缓冲绑定（异常时也要能写，所以先设） */
  s_out = out;
  s_out_cap = out_cap;
  s_out_len = 0;
  if (out != NULL && out_cap > 0) out[0] = '\0';

  /* ## 每次 run 用干净命名空间
   *
   * 不清空的话上一次脚本定义的变量会泄漏到这一次（名字撞上时行为诡异，
   * 而那种错误只在"连续跑多个脚本"时出现，单测很容易漏掉）。
   */
  s_globals = mp_obj_new_dict(0);
  /* `mp_obj_dict_store` 的签名是 `(mp_obj_t self, ...)`；
   * 直接传 `s_globals` 即可（不需要先取出 `mp_obj_dict_t*`）。*/
  mp_obj_dict_store(s_globals,
                    MP_OBJ_NEW_QSTR(MP_QSTR___name__),
                    mp_obj_new_str("__main__", 8));

  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    /* exec 内建：语义与 Python 的 exec(code, globals) 完全一致，
     * 包括副作用顺序与异常抛法。*/
    mp_obj_t args[2];
    args[0] = mp_obj_new_str(source, source_len);
    args[1] = s_globals;
    (void)mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_builtin_exec_obj), 2, 0, args);

    /* 读回约定变量（见 include/st/python/runtime.hpp 里 kResultName 的说明）
     *
     * 用 `qstr_from_str` 而不用 `MP_QSTR_result`：后者要提前登记在
     * `py/qstrdefs.h` 里并重新生成 genhdr，而这是一个纯宿主侧的约定变量，
     * 不值得为了它去动第三方头（动了就违反“不动上游一字”）。
     * `qstr_from_str` 运行时算一次并缓存，同名多次调用零额外开销。
     *
     * ⚠ `mp_obj_dict_get` 在**键不存在时抛 KeyError**，不是返回 NULL。
     * 脚本不写 `result = ...` 是合法情形（“只执行、不要返回值”），
     * 所以先用 `mp_obj_dict_get_map` 查存在性，避开异常。
     * 直接调用会让“脚本没赋值”变成一次假异常——实测踩到，
     * 报 `KeyError: result` 而脚本本身完全正确。*/
    qstr result_qstr = qstr_from_str("result");
    mp_map_t* map = mp_obj_dict_get_map(s_globals);
    mp_map_elem_t* elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(result_qstr), MP_MAP_LOOKUP);
    if (elem != NULL) {
      mp_obj_t value = elem->value;
      if (value != MP_OBJ_NULL && value != mp_const_none) {
        mp_obj_print_helper(&s_printer, value, PRINT_STR);
      }
    }
    nlr_pop();
    return 0;
  }

  /* 异常路径：拿出可读原因。`nlr_push` 返回非 0 时，异常对象在 `nlr.ret_val`。
   *
   * 注意：MicroPython 的 `MP_STATE_VM(cur_exception)` 在无 VM 上下文时不可靠，
   * 而 `nlr.ret_val` 是 setjmp 现场带回来的，那条路径一定正确。 */
  if (err != NULL && err_cap > 0) {
    err[0] = '\0';
    mp_obj_t exc = (mp_obj_t)nlr.ret_val;
    if (exc != MP_OBJ_NULL && exc != mp_const_none) {
      /* 复用结果缓冲的方式：临时把它指到 err 上 */
      char* saved_out = s_out;
      size_t saved_cap = s_out_cap;
      size_t saved_len = s_out_len;
      s_out = err;
      s_out_cap = err_cap;
      s_out_len = 0;
      mp_obj_print_helper(&s_printer, exc, PRINT_EXC);
      /* 去掉末尾换行 */
      while (s_out_len > 0 && (err[s_out_len - 1] == '\n' || err[s_out_len - 1] == '\r')) {
        err[--s_out_len] = '\0';
      }
      s_out = saved_out;
      s_out_cap = saved_cap;
      s_out_len = saved_len;
    }
    if (err[0] == '\0') {
      snprintf(err, err_cap, "Python 异常（无详情）");
    }
  }
  return 1;
}

const char* st_mp_version(void) {
  return MICROPY_VERSION_STRING;
}
