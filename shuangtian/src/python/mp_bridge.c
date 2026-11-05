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
/* GC 根扫描（寄存器 + 栈）：上游 `ports/unix/gccollect.c` 用同一个 helper。
 * 自己手搬栈会因“非指针数据混入”而弄坏 GC 链表，见下文 gc_collect 的说明。*/
#include "shared/runtime/gchelper.h"

#include "st/python/mp_bridge.h"

/* bpy 兼容层装载（实现于 `src/python/bpy_bridge.c`）
 *
 * 参数是一个干净的 globals dict：装载过程中定义的名字不该泄漏到
 * 用户脚本的命名空间里。 */
int st_bpy_bootstrap(mp_obj_t globals);

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

/* 栈顶与寄存器现场：交给上游的 `gc_helper_collect_regs_and_stack()`（见下）。
 *
 * ## 踩过的坑（值得完整读一遍）
 *
 * 最初的实现只登记**一个栈字**：
 *
 *     gc_collect_root((void*)sp, 1);
 *
 * 后来改成扫 `[sp, 栈顶]` 整段。两者都错，而且**小场景看不出来**——
 * 单个圆柱压根不触发 GC；而真实资产（轮系 8 个齿轮）立刻崩：
 *
 *     Assertion failed: ATB_GET_KIND(area, block) == AT_HEAD, gc.c:914
 *
 * 那行在 `gc_free` 里（“要释放的块头不是合法块”），看起来像“GC 实现有 bug”。
 * 根因是 `gc_collect_root` 的契约要求**一个 `void*` 数组**，而栈区间里
 * 混着非指针数据：GC 会把某些整数当指针误标、又把已释放的块当活物，
 * 最终把链表结构弄坏。
 *
 * 结论：**不要手搓栈扫描**。上游 `ports/unix/gccollect.c` 的 `gc_collect`
 * 只有一行实质内容 `gc_helper_collect_regs_and_stack()`，它同时处理
 * 寄存器与栈（`setjmp` 把寄存器现场落到栈上再扫），边界与对齐都是验证过的。*/

void gc_collect(void) {
  gc_collect_start();
  /* ## 用上游的 `gc_helper_collect_regs_and_stack()`（不是手搓栈扫描）
   *
   * 它是 `ports/unix/gccollect.c` 里 `gc_collect` 的全部内容。自己做两步：
   *
   * | 步 | 为何必须 |
   * |---|---|
   * | 扫 **寄存器** | 当前活着的对象可能**只在寄存器里**（刚算出的临时值、即将返回的引用）。只扫栈会漏掉它们，然后当垃圾回收 |
   * | 扫 **栈** | 局部变量、未完成的表达式 |
   *
   * 实现上它用 `setjmp` 把当前寄存器现场落到栈上再扫（`MICROPY_GCREGS_SETJMP`
   * 路径），所以跨平台/跨编译器都可靠。
   *
   * ## 踩过的坑（重要）
   *
   * 本文件原先手写了栈扫描（`gc_collect_root((void**)low, n)`）。
   * 小场景正常（根本没触发 GC），**大场景就直接崩**：
   *
   *     Assertion failed: ATB_GET_KIND(area, block) == AT_HEAD, gc.c:914
   *
   * 那行在 `gc_free` 里，意思是“要释放的块头不是合法块”——而根因是
   * 搬进 `gc_collect_root` 的**不是精确的指针数组**：里面混了非指针数据，
   * 于是 GC 把一些地址误标成对象、又把已释放的东西当活物。
   *
   * `gc_collect_root` 的契约是“一个 `void*` 数组”——它不区分“这个字看起来像指针”
   * 与“它真的是指针”。而我们扫的栈区间里两者都有。上游的 helper 做同样的事，
   * 但**保守扫描的边界与对齐**是它处理过的，比自己拼多一层保证。*/
  gc_helper_collect_regs_and_stack();
  gc_collect_end();
}

void nlr_jump_fail(void* val) {
  (void)val;
  fprintf(stderr, "MicroPython 致命错误：nlr 链断裂（内部不变量被破坏）\n");
  abort();
}

/* 堆：64 MB。
 *
 * ## 为何从 16 MB 涨到 64
 *
 * 16 MB 跑单个圆柱够，但**真实资产不够**：机械腕表轮系（8 个齿轮 ×
 * 数千面 × 每面若干顶点对象）会撞上堆顶，报
 *
 *     Assertion failed: ATB_GET_KIND(area, block) == AT_HEAD, py/gc.c:914
 *
 * 那条断言看起来像“GC 实现有 bug”，实际是**堆用完**——MicroPython 的 GC
 * 在分配失败时直接 assert，而不是抛 MemoryError。
 *
 * 64 MB = “几何量再大 4 倍”的余量。它按需提交（Windows 堆 / POSIX mmap），
 * 所以“给 64 MB”不等于占 64 MB 物理内存。
 *
 * ## 为何不无限大
 *
 * 堆越大，一次 GC 要扫的标记区域越大。而且它真被用尽时报 assert 而不是
 * MemoryError（见上），所以留足余量比“刚好够”安全。 */
#define ST_MP_HEAP_BYTES (64u * 1024u * 1024u)

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
  /* 栈控制：与上游 `ports/unix/main.c` 一致（`gc_collect` 的根扫描由
   * `gc_helper_collect_regs_and_stack()` 做，见上方说明）*/
  mp_stack_ctrl_init();
  mp_stack_set_limit(512u * 1024u);
  gc_init(s_heap, s_heap + s_heap_words);
  mp_init();
  /* 装载 bpy 兼容层（Python 实现，见 src/python/prelude.py） */
  register_extra_modules();
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
  /* ## 装载 bpy 兼容层（Python 实现）
   *
   * 详见 `src/python/bpy_bridge.c` 与 `src/python/prelude.py` 的说明。
   *
   * 给一个**干净**的 globals：装载过程中定义的名字（那些类和函数）
   * 不该泄漏到用户脚本的命名空间里。 */
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_obj_t boot_globals = mp_obj_new_dict(64);
    (void)st_bpy_bootstrap(boot_globals);
    nlr_pop();
  }
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

  /* ## `@` 运算符：MicroPython 编译器的能力边界
   *
   * 实测确认：MicroPython 1.24 的 **编译器不支持 `@` 运算符**（PEP 465）。
   * `py/lexer.c` 认这个 token，但 `py/compile.c` 里**没有任何
   * `MP_TOKEN_OP_AT` 的语法处理分支**，所以 `a @ b` 直接报
   * `TypeError: unsupported type for operator`。
   *
   * 这对 bpy 是实际影响：语料里 `rot @ v`（矩阵×向量）到处都是。
   *
   * 处理方式：在**兼容层的 Python 源码里**不依赖 `@`，改用显式方法
   * （`Vector.dot()` / `Matrix.matmul()`），并提供 `__matmul__` 方法——
   * 它虽然不会被 `@` 语法调到，但可以用 `a.__matmul__(b)` 显式调用。
   *
   * 不做源码改写：那要写一个 Python 词法器（区分装饰器/字符串/注释里的
   * `@`），复杂度和“改坏正确脚本”的风险都比“少一种语法糖”大。
   * 这条边界写进文档，比静默改写诚实。 */

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
