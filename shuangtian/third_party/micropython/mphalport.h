// 霜天 · MicroPython 移植层（HAL）
//
// ## 这份文件是什么
//
// MicroPython 把「与宿主 OS 交互的最小集合」抽成 `mphalport.h`：输出一个字符、
// 读一个字符、取当前时间、可能要 sleep。每个移植（裸机/linux/unix/windows）
// 各写一份。
//
// 我们是**嵌入宿主进程**，所以这里做的事很少：
//
// | 能力 | 本移植的做法 |
// |---|---|
// | 字符输出（`mp_hal_stdout_tx_strn`） | 丢弃。脚本不该往进程 stdout 写东西 |
// | 字符输入 | 不支持（无 REPL） |
// | 时间（`mp_hal_ticks_ms`） | 用 `std::chrono` 的单调时钟 |
// | sleep | 不提供（嵌入场景里 sleep 会卡住渲染线程） |
//
// ## 为什么不给 stdout
//
// 与 QuickJS 那边同一立场：脚本层**不是**一个通用运行时，它的契约是
// 「场景进、场景出」。让脚本能写 stdout 会引出两个问题：
// ① 宿主进程的输出流被污染（尤其是裁云的渲染日志）；
// ② 模型会开始用它做「调试输出」，而真正的诊断应该走返回值。
//
// 需要诊断时用 `raise` 或返回哨兵对象，不是 print。
#ifndef ST_MPHALPORT_H
#define ST_MPHALPORT_H

#include <stdint.h>
#include <stdlib.h>

// `mp_uint_t` 在本框架里就是**指针宽度无符号整数**（见 mpconfigport.h 的 typedef）。
// 这里直接用 `uintptr_t` 而不 include `py/mpconfig.h`：
// mphalport.h 可能在 mpconfig.h **之前**被包含（`py/mphal.h` 的包含顺序），
// 那时 `mp_uint_t` 还没定义，用它会被当成隐式 int，与后来 mphal.h 里的真声明冲突
//（→ "conflicting types for mp_hal_stdout_tx_strn"）。
// 写 uintptr_t 与 typedef 完全等价，且不依赖包含顺序。

// ── 堆分配 ────────────────────────────────────────────────────────────
//
// GC 堆要在**初始化前**就分配好（`gc_init` 需要一块连续区间），
// 所以它不能走 Python 的内存管理。这里包一层，原因：
// ① C 胶水层里不想直接碰平台分配（Windows 的 `_aligned_malloc` 与 POSIX 的
//    `posix_memalign` 签名不同，封装后两边一致）；
// ② 将来要改成宿主统一内存池时只改一处。
void* mp_hal_alloc(size_t bytes);
void mp_hal_free(void* ptr);

// ── 时间 ────────────────────────────────────────────────────────────────
// 由 `src/python/mphal_port.cpp` 提供实现（用 C++ 的 steady_clock）。
uint64_t mp_hal_ticks_ms(void);
uint64_t mp_hal_ticks_us(void);
uint64_t mp_hal_ticks_cpu(void);

// ── 输出 / 输入 ─────────────────────────────────────────────────────────
// 输出：丢弃（理由见文件头）。
// 输入：返回 -1 表示"没有输入"，REPL 已关闭所以不会被调用。
//
// ⚠ 两者的返回类型**不一样**（上游就是这么写的，别“统一”）：
//   `mp_hal_stdout_tx_strn`        → mp_uint_t（本例用 uintptr_t，等价）
//   `mp_hal_stdout_tx_strn_cooked` → void
// 跟着上游走；自作主张统一成同一类型会报 conflicting types，
// 而且只在少数几个 .c 里报（大多数文件不调 cooked 版）。
uintptr_t mp_hal_stdout_tx_strn(const char* str, size_t len);
void mp_hal_stdout_tx_strn_cooked(const char* str, size_t len);
int mp_hal_stdin_rx_chr(void);
// ── 杂项 ────────────────────────────────────────────────────────────────
#define mp_hal_delay_ms(ms) ((void)(ms))
#define mp_hal_delay_us(us) ((void)(us))

// ## 定义 pin 类型，跳过 extmod/virtpin.h
//
// `py/mphal.h` 用 `#ifndef mp_hal_pin_obj_t` 守卫一段引脚抽象。
// 注意它是查**宏**，不是查类型——写 `typedef` 不管用（守卫照样成立，
// 于是去 include 我们不编的 `extmod/virtpin.h`），必须用 `#define`。
//
// 嵌入宿主进程没有 GPIO，这些值不会被真用到。
#define mp_hal_pin_obj_t int
#define mp_hal_get_pin_obj(pin) (pin)
#define mp_hal_pin_read(pin) (0)
#define mp_hal_pin_write(pin, v) ((void)(pin), (void)(v))

// 退出：嵌入场景里脚本不该能退出宿主进程
#define mp_hal_raise_if_async(void)
#define mp_hal_set_interrupt_char(c) ((void)(c))
#define mp_hal_get_interrupt_char() (-1)

#endif  // ST_MPHALPORT_H
