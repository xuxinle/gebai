#pragma once

/// 格斗示例 · 神经网络策略（推理侧）。
///
/// ## 为什么推理要住在 C++
///
/// 训练在 Python（torch），但**打游戏的是 C++ 进程**。如果为了"用训练好的机器人"
/// 而把游戏改成"每帧问一次 Python"，延迟、进程边界、部署都会变成新问题，
/// 而它们与"机器人强不强"完全无关。
///
/// 所以策略是一份**纯数据**（JSON 权重）+ 一个**极小**的定长 MLP：
/// 训练脚本导出权重，游戏与评测工具各自加载。两条路径共用本文件的 forward，
/// 因此"评测里 80% 胜率"与"游戏里看到的行为"不会分岔。
///
/// ## 网络结构（刻意极小）
///
/// `obs(20) → Linear(64) → tanh → Linear(64) → tanh → Linear(8)` → softmax 取 argmax。
/// 参数量约 6k：一秒能给几万帧出决策（软件光栅一帧才 ~12ms），
/// 而格斗的操作面本来就小——更大的网络只会让"确定性"和"可复现"变难。

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rl_env.hpp"

namespace gedou {

/// 一层全连接（行主序：`weight[out * in + in_index]`）。
struct LinearLayer {
  std::size_t in{0};
  std::size_t out{0};
  std::vector<float> weight{};  ///< out × in
  std::vector<float> bias{};    ///< out
  /// 激活：`tanh` 或 `none`（最后一层）。
  bool tanh{true};
};

class PolicyNet {
 public:
  PolicyNet() = default;

  /// 从 JSON 文本装载（格式见 `PolicyNet::save`）。
  /// 失败返回原因（**不静默回退到随机策略**：那会让"加载错了"表现为"机器人很弱"）。
  [[nodiscard]] auto load_json(std::string_view text) -> std::optional<std::string>;
  /// 从文件装载。
  [[nodiscard]] auto load_file(std::string_view path) -> std::optional<std::string>;
  /// 序列化为 JSON（训练脚本用同一格式写；这里保留以便单测往返）。
  [[nodiscard]] auto to_json(bool pretty = false) const -> std::string;

  /// **前向自检**：从 JSON 请求（含 `layers` 与 `inputs` 矩阵）算 logits/argmax。
  ///
  /// 为何需要它：训练在 Python（torch）、推理在 C++。两边只要有一处差别
  /// （矩阵行/列、激活、层顺序），训练就会“收敛但学不会”——而图上完全看不出来
  /// （损失正常、熵正常，只有胜率不动）。这个接口让训练脚本能把**同一批输入**
  /// 同时跑两边并逐值对比，把这种偏差在开工时就钉住。実測：这一条把本项目的
  /// “17 万步仍学不会”直接定位到实现侧。
  [[nodiscard]] static auto forward_json(const std::string& request) -> std::string;

  /// 直接用层构造（单测与"手写基线"用）。
  static auto from_layers(std::vector<LinearLayer> layers) -> PolicyNet;

  [[nodiscard]] auto valid() const noexcept -> bool { return !layers_.empty(); }
  [[nodiscard]] auto input_dim() const noexcept -> std::size_t {
    return layers_.empty() ? 0 : layers_.front().in;
  }
  [[nodiscard]] auto output_dim() const noexcept -> std::size_t {
    return layers_.empty() ? 0 : layers_.back().out;
  }
  [[nodiscard]] auto parameter_count() const noexcept -> std::size_t;

  /// 前向：返回 logits（未 softmax）。
  [[nodiscard]] auto forward(const Observation& observation) const -> std::vector<float>;
  /// 贪心动作（评测/游戏内使用：**确定性**，同输入必然同输出）。
  [[nodiscard]] auto act(const Observation& observation) const -> RlAction;
  /// 采样动作（训练侧探索用；`random01` 由调用方给，保持可复现）。
  [[nodiscard]] auto sample(const Observation& observation, float random01) const -> RlAction;

 private:
  std::vector<LinearLayer> layers_{};
};

}  // namespace gedou
