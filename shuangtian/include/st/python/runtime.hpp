// 霜天 · MicroPython 嵌入宿主
//
// ## 这一层做什么
//
// 把 MicroPython 当成"一个能跑 Python 的 C 库"用起来：初始化、跑一段源码、
// 取回结果、反初始化。bpy 兼容层（后续阶段）以 C 模块的形式挂进去。
//
// ## 为什么每个线程一个实例
//
// MicroPython 的解释器状态是**全局的**（`mp_state_ctx`），同一进程里不能有两个
// 独立解释器同时跑。而本框架的渲染是"每线程一条流水线"（裁云导出按线程分片）。
//
// 所以这里的做法与 `ThreeRuntime` 完全一致：**线程内单例**。跨线程访问会被
// 断言拦住——那种错误表现为随机的堆破坏，比崩溃难查得多。
//
// ## 不做的事
//
// - 不给脚本文件/网络/线程访问（与 QuickJS 同一立场：脚本层是场景描述器，不是通用运行时）
// - 不保留脚本定义的全局（每次 `run` 是干净的命名空间，避免上一次的残留影响下一次）
#ifndef ST_MP_RUNTIME_H
#define ST_MP_RUNTIME_H

#include <string>
#include <string_view>

#include "st/core/error.hpp"

namespace st::mp {

/// 一段 Python 脚本的执行结果。
struct RunResult {
  /// 脚本**最后一个表达式**的字符串表示（`str()` 语义）。
  ///
  /// 用字符串而不是 `mp_obj_t`：调用方（bpy 抽取器）不需要碰 MicroPython 的
  /// 对象系统，返回类型一旦是内部句柄，GC 生命周期就会漏到接口外面来。
  std::string value{};
  /// 脚本执行期间的 stdout 输出（本嵌入下恒为空，保留字段以便将来开放）。
  std::string output{};
};

/// MicroPython 运行时（线程内单例）。
class Runtime {
 public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime&) = delete;
  auto operator=(const Runtime&) -> Runtime& = delete;

  /// MicroPython 是否可用（能初始化且堆够用）。
  ///
  /// ## 为什么能查而不是"失败即崩"
  ///
  /// 应用（尤其是不需要 Python 的）不该因为这条链坏了就起不来。
  /// 调用方据此决定回退或给一句明确说明。
  [[nodiscard]] static auto available() -> bool;

  /// 不可用的**具体原因**（可用时返回空串）。
  ///
  /// 与 `ThreeRuntime::unavailable_reason()` 同一立场：只给 bool 时
  /// "为什么"全被吞掉，排查只能靠猜。
  [[nodiscard]] static auto unavailable_reason() -> std::string;

  /// 执行一段 Python 源码，返回最后一个表达式的值。
  ///
  /// ## 语义
  ///
  /// `source` 里最后一条语句如果是**表达式**，它的值就是结果；否则结果为空串。
  /// 这与 QuickJS 那条的约定一致（"最后一个表达式的值"），
  /// 让 bpy 脚本可以自然地以 `bpy.data.objects['x']` 或 `mesh` 结尾。
  [[nodiscard]] auto run(std::string_view source, std::string_view filename = "<python>")
      -> Result<RunResult>;

  /// 本线程复用的运行时（首次调用会初始化，约 1ms）。
  [[nodiscard]] static auto instance() -> Runtime&;

 private:
  struct Impl;
  Impl* impl_{nullptr};
};

}  // namespace st::mp

#endif  // ST_MP_RUNTIME_H
