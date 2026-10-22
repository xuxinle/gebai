#pragma once

/// 裁云（Nimbus）· **脚本编排层**：用 JS 描述"每帧的图层状态"。
///
/// ## 为什么需要它（而 JSON 字段做不到）
///
/// JSON 描述动画的表达力天花板很低：循环、条件、数学都得变成新字段。
/// 实测（2026-10-22）补能力时反复撞同一堵墙——`path_*` 8 个字段、
/// `bounce_*` 4 个、`model_*` 二十多个，台账里 28 项能力大半是字段膨胀。
/// 而"20 个图标错峰入场"用 JS 是一个 `for` 循环，用 JSON 要写 20 遍。
///
/// ## 设计取向：**强能力 + 强护栏**
///
/// 用户要求"能力强大"与"AI 使用稳定"——这两者互相拉扯。本层的取舍是：
///
/// | 面 | 处置 |
/// |---|---|
/// | **能力** | 一次 `layout(t)` 调用返回整帧所有图层；可循环/条件/数学；可调宿主函数 |
/// | **稳定 ①：加载期校验** | `check_syntax` 在**加载时**就报语法错，不是渲到一半才崩 |
/// | **稳定 ②：确定性** | 同一脚本 + 同一时间 ⇒ 同一结果。脚本**不能读时钟/随机**（未注册），保证批量导出可复现 |
/// | **稳定 ③：配额** | 继承框架四重配额（内存/栈/超时/转换深度）；死循环被打断而不是挂死 342 帧导出 |
/// | **稳定 ④：失败可见** | 脚本抛错 ⇒ 该帧**画出醒目占位**并记一次告警，而不是静默空白或整片失败 |
/// | **稳定 ⑤：缺省不变** | 没写 `layout` 时行为与纯 JSON **一字不差**（老脚本零改动） |
///
/// ## 能力边界（故意画死的地方）
///
/// 脚本**只能算数值**：位置、尺寸、缩放、透明度、旋转、颜色。
/// 它**不能**：加载素材、创建图层类型、改渲染管线、读写文件。
///
/// 为什么这样切：一旦脚本能"造图层"，渲染就不再是 `(Scene, t) -> 像素`
/// 的纯函数，并行导出与逐像素可复现都会崩。而"算数值"能覆盖
/// 95% 的真实需求（所有动画本质都是数值随时间变化）。
///
/// 另外——**脚本不能新增图层**，只能改 JSON 里已声明的那些。
/// 这条限制让"这个图层是谁画的"始终可追：图层集合来自静态 JSON。

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "layer.hpp"
#include "st/core/error.hpp"
#include "st/ext/json_fwd.hpp"
#include "st/ext/script.hpp"

namespace caiyun {

/// 脚本编排层的配置（来自 JSON 的 `layout` 字段）。
struct LayoutScript {
  /// 脚本源（内联或从文件读入后的内容）。
  std::string source{};
  /// 源文件名（报错定位用；内联时写 `<layout>`）。
  std::string filename{"<layout>"};
  /// 单次求值时限（毫秒）。默认 400：整帧所有图层一次算完，这个量级很宽裕。
  int timeout_ms{400};
  [[nodiscard]] auto empty() const noexcept -> bool { return source.empty(); }
};

/// 一帧的求值结果：每个图层的状态增量。
///
/// 用"增量"而不是"绝对值"：JSON 里写的 `x/y/w/h` 仍是基准，
/// 脚本只表达"相对基准变化多少"。这让两种写法天然可以混用，
/// 也让脚本出错时退化到基准值（而不是变成 0 跑到画面外）。
struct LayerDelta {
  float dx{0.0f};
  float dy{0.0f};
  float dw{0.0f};
  float dh{0.0f};
  float scale{1.0f};
  float rotate{0.0f};
  float alpha{1.0f};
  /// 颜色覆盖（`a=0` = 不覆盖）。
  st::math::Color tint{0, 0, 0, 0};
};

/// 脚本编排引擎（**每线程一个实例**：QuickJS 运行时非线程安全）。
///
/// ## 为什么持有而非每次新建
///
/// 建一个 QuickJS 运行时要分配堆、编译上下文（毫秒级）。而导出是
/// **逐帧**调用——342 帧 × 每帧新建 = 几秒纯开销。持有实例后，
/// 脚本只编译一次（`load` 时），每帧只是调用已编译的函数。
///
/// ## 线程模型
///
/// 导出开 16 路并行，每线程各持一个实例（`thread_local` 池，见 `layout_pool()`）。
/// 不要跨线程共享同一个 `LayoutEngine`。
class LayoutEngine {
 public:
  LayoutEngine();
  ~LayoutEngine();
  LayoutEngine(const LayoutEngine&) = delete;
  auto operator=(const LayoutEngine&) -> LayoutEngine& = delete;
  LayoutEngine(LayoutEngine&&) noexcept;
  auto operator=(LayoutEngine&&) noexcept -> LayoutEngine&;

  /// 装载脚本。
  ///
  /// **加载期就做三件事**，这是"AI 用着稳"的主要来源：
  /// ① 语法校验（`check_syntax`）——语法错立刻报，带行号；
  /// ② 执行一遍顶层代码——捕获"调用了不存在的函数"这类运行期错误；
  /// ③ 确认 `layout` 函数真的存在——否则每帧都会失败。
  ///
  /// 失败返回错误（消息含文件名:行:列）。**失败后引擎保持"未装载"状态**，
  /// 调用方可以继续用纯 JSON 渲染（降级而不是崩）。
  auto load(const LayoutScript& script) -> st::Status;

  /// 求一帧：把 `layers` 的基准状态交给脚本，取回增量。
  ///
  /// `out` 与 `layers` 等长；脚本返回的数组长度不符时**按较短者填充**，
  /// 缺失项用"零增量"（即保持基准值）——不让一处疏漏毁掉整帧。
  ///
  /// 失败（超时/异常）返回错误，且 `out` 被填成零增量（**降级到纯 JSON 行为**）。
  auto evaluate(float time, float duration, float canvas_w, float canvas_h,
                const std::vector<LayerState>& layers, std::vector<LayerDelta>& out)
      -> st::Status;

  /// 取脚本本帧提交的**临时图元**（每帧重建；不调 `evaluate` 时为空）。
  ///
  /// 见 `ScriptShape` 的说明：这是做"结构变化"（点分裂、珠子沿曲线跑、
  /// 批量错峰）的唯一通道。
  [[nodiscard]] auto shapes() const -> const std::vector<ScriptShape>&;

  /// 是否真的可用了。
  [[nodiscard]] auto loaded() const noexcept -> bool;

  /// 累计的求值失败次数（供导出结束时汇总报告）。
  [[nodiscard]] auto failure_count() const noexcept -> std::uint64_t;

  /// 最近一次失败的原因（用于"该帧画占位"时把原因写进日志）。
  [[nodiscard]] auto last_error() const -> const std::string&;

  /// 注册一个宿主函数供脚本调用（白名单见 `register_host_functions`）。
  ///
  /// **不允许通配**：每个函数都要显式写名字，这样"脚本能调什么"
  /// 是代码里可枚举、可审阅的。这是开放能力时的底线。
  auto register_function(std::string name, st::ext::ScriptHostFunction function) -> st::Status;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// 把脚本返回的增量叠到图层状态上（定义在 `layout.cpp`）。
///
/// 放在编排层的头文件里而不是绘制文件里：它是**编排层的语义**，
/// 而且测试要能直接验证“相对而非绝对”。
void apply_delta(LayerState& state, const LayerDelta& delta);

/// **脚本可以调用的 C++ 宿主函数**（能力开放面）。
///
/// ## 为何要有这一层（它比“只算数值”危险得多）
///
/// 只算数值解决不了这些问题：
/// · “旋转角度用缓动函数算”——想要 `easeOutBack` 这类标准缓动；
/// · “把时间对齐到帧”——想要 `snap(t, 12)`；
/// · “测量一段文字的宽度再让它居中”——想要 `measureText(...)`。
/// 这些都是**现成的 C++ 能力**，让脚本重写一遍是浪费且容易写错。
///
/// ## 安全姿态（这是本模块最需要读的一段）
///
/// 开放 C++ 函数 = 把内部能力暴露给脚本，必须逐个显式注册、不允许通配。
/// 本模块只开放**纯函数**（无副作用、无 IO、无状态）：
///
/// | 白名单函数 | 能力 | 为何安全 |
/// |---|---|---|
/// | `ease(name, t)` | 标准缓动曲线 | 纯数学 |
/// | `lerp(a, b, t)` | 线性插值 | 纯数学 |
/// | `clamp(v, lo, hi)` | 限幅 | 纯数学 |
/// | `snap(t, fps)` | 吸附到帧 | 纯数学 |
/// | `noise(seed)` | 确定性伪随机 | 同种子同结果，**不破坏确定性** |
/// | `measureText(text, size)` | 文字宽度 | 读字体缓存，无可写 |
///
/// **故意不开放的**：文件读写、网络、时钟（会破坏确定性）、
/// 创建图层/素材（会破坏 `(Scene,t)->像素` 的纯函数性）。
///
/// 这就是“能力大且稳”的具体切法：**开放计算，不开放副作用**。
void register_host_functions(class LayoutEngine& engine);

/// 取本线程的编排引擎（`thread_local` 池）。
///
/// 导出是 16 路并行：每线程一个实例，各自编译一次脚本。
/// 换来的是**无锁**——QuickJS 运行时非线程安全，共享就得全程加锁，
/// 而每帧每图层都要过一遍锁，那比多编译几次贵得多。
[[nodiscard]] auto layout_engine() -> LayoutEngine&;

/// 从 JSON 节点解析 `layout` 配置。
///
/// 支持三种写法：
/// - `"layout": "path/to/layout.js"` —— 从文件读（相对脚本目录）
/// - `"layout": { "source": "...", "timeout_ms": 400 }` —— 内联
/// - 不写 —— 纯 JSON 行为
[[nodiscard]] auto parse_layout_script(const st::Json& node, std::string_view asset_basis)
    -> st::Result<LayoutScript>;

}  // namespace caiyun
