// 霜天 · MicroPython C 胶水层的对外声明
//
// ## 为什么单独一份"纯 C 可见"的头
//
// MicroPython 的头文件是 C99 的（弹性数组、零长数组），**不能被 C++ 直接 include**
// ——那会触发 `-Werror=pedantic` 的硬错误。
//
// 所以边界划在这里：这份头只用 `<stddef.h>` 的基础类型，C 与 C++ 都能包含；
// 真正的 MicroPython 头只在 `src/python/mp_bridge.c` 里出现。
//
// 好处是 C++ 侧（`st::mp::Runtime`）完全不知道 MicroPython 的存在——
// 换运行时（比如将来换 CPython 或 Lua）只改胶水层。
#ifndef ST_PYTHON_MP_BRIDGE_H
#define ST_PYTHON_MP_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 初始化解释器。成功返回 0，失败返回 -1 并在 `err` 里写原因。
///
/// 可重复调用（已初始化时直接返回 0）——但**不可跨线程**：
/// MicroPython 的解释器状态是进程全局的。
int st_mp_init(char* err, size_t err_cap);

/// 释放解释器。不调用会泄漏 16 MB（进程退出时由 OS 回收，但库不该依赖它）。
void st_mp_deinit(void);

/// 执行一段 Python 源码。
///
/// - 成功返回 0，脚本里 `result = ...` 的值的 `str()` 写进 `out`
/// - 异常返回 1，可读原因写进 `err`
/// - 未初始化返回 -2
///
/// `out` / `err` 都按 C 字符串语义处理（保证 NUL 结尾），超长会截断。
int st_mp_run(const char* source, size_t source_len,
              const char* filename, size_t filename_len,
              char* out, size_t out_cap,
              char* err, size_t err_cap);

/// MicroPython 版本串（诊断用）。
const char* st_mp_version(void);

#ifdef __cplusplus
}
#endif

#endif  // ST_PYTHON_MP_BRIDGE_H
