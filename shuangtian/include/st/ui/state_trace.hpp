/// State 写入追踪（诊断）：`ST_TRACE_STATE=1` 打开，崩溃时随栈输出。
///
/// 专治"数据进了、界面不动"——本会话实测三次（标脏缺失 / Tabs 属性面不通知 /
/// Input 属性面不通知），共同点是"写入发生了但没人知道要重画"。
/// 排查这类问题时"写入到底有没有发生、从哪来"是第一问；有了追踪直接看日志，
/// 没有时只能到处插 print（每次重来一遍）。
///
/// 用法：
/// ```
/// ST_TRACE_STATE=1 ./build/dev/bin/gbcode --headless ...
/// ```
/// 崩溃时 stderr 会自动附带最近 64 次写入；也可主动调 `st::ui::trace::snapshot()`。
///
/// 开销：默认关闭（环境变量开关）。开启时每次写入记一条（固定 64 条环形缓冲，
/// 不增长）。`site` 传编译期字符串（不持有所有权，避免追踪本身分配内存）。

#pragma once

#include <string>
#include <vector>

namespace st::ui::trace {

/// 追踪是否开启（读一次 `ST_TRACE_STATE` 并缓存）。
[[nodiscard]] auto is_enabled() -> bool;

/// 记一次 State 写入。
///
/// `site` 应为**静态存储期**的字符串（字面量）——追踪不能自己制造分配/生命周期问题。
/// `during_build` 标记构建期写入（与 `ReconcileStats::build_time_state_writes` 互补：
/// 那个是"每帧去重的清单"，这里是"每一次的流水"）。
void record_write(const void* state, const char* site, bool during_build);

/// 取最近写入的可读描述（最旧在前；未开启时为空）。
[[nodiscard]] auto snapshot() -> std::vector<std::string>;

/// 是否已有记录（崩溃处理器用它决定要不要打这一段）。
[[nodiscard]] auto has_records() -> bool;

/// 渲染成可直接打印的多行文本（崩溃处理器用；信号安全上尽力而为——
/// 内部会分配，但这是"崩溃诊断"的既有折中，与栈符号化同一取舍）。
/// 未开启或无记录时返回空串。
[[nodiscard]] auto format_for_crash() -> std::string;

/// 把追踪挂到崩溃处理器上（应用启动时调一次；幂等）。
///
/// 走 `st::set_crash_extra_provider` 的**函数指针**而不是让 core 直接调用 ui：
/// 依赖方向是 core ← ui（`st` 构建器只链 core，直接依赖会让它链接失败）。
void install_crash_hook();

}  // namespace st::ui::trace
