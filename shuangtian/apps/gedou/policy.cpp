#include "policy.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"

namespace gedou {

namespace {

[[nodiscard]] auto softmax(const std::vector<float>& logits) -> std::vector<float> {
  std::vector<float> out(logits.size());
  if (logits.empty()) return out;
  const float peak = *std::max_element(logits.begin(), logits.end());
  float total = 0.0f;
  for (std::size_t index = 0; index < logits.size(); ++index) {
    out[index] = std::exp(logits[index] - peak);
    total += out[index];
  }
  if (total <= 0.0f) return out;
  for (float& value : out) value /= total;
  return out;
}

}  // namespace

auto PolicyNet::from_layers(std::vector<LinearLayer> layers) -> PolicyNet {
  PolicyNet net{};
  net.layers_ = std::move(layers);
  return net;
}

auto PolicyNet::parameter_count() const noexcept -> std::size_t {
  std::size_t total = 0;
  for (const LinearLayer& layer : layers_) total += layer.weight.size() + layer.bias.size();
  return total;
}

auto PolicyNet::forward(const Observation& observation) const -> std::vector<float> {
  if (layers_.empty()) return {};
  if (observation.size() != input_dim()) return {};
  std::vector<float> current = observation;
  for (std::size_t depth = 0; depth < layers_.size(); ++depth) {
    const LinearLayer& layer = layers_[depth];
    if (current.size() != layer.in || layer.weight.size() != layer.in * layer.out) return {};
    std::vector<float> next(layer.out, 0.0f);
    for (std::size_t row = 0; row < layer.out; ++row) {
      const float* weights = layer.weight.data() + row * layer.in;
      float sum = layer.bias[row];
      for (std::size_t column = 0; column < layer.in; ++column) {
        sum += weights[column] * current[column];
      }
      if (layer.tanh) sum = std::tanh(sum);
      next[row] = sum;
    }    current = std::move(next);
  }
  return current;
}

auto PolicyNet::act(const Observation& observation) const -> RlAction {
  const std::vector<float> logits = forward(observation);
  if (logits.empty()) return RlAction::Idle;
  const auto best = std::max_element(logits.begin(), logits.end());
  return rl_action_from_index(static_cast<int>(std::distance(logits.begin(), best)));
}

auto PolicyNet::sample(const Observation& observation, float random01) const -> RlAction {
  const std::vector<float> logits = forward(observation);
  if (logits.empty()) return RlAction::Idle;
  const std::vector<float> probabilities = softmax(logits);
  const float threshold = std::clamp(random01, 0.0f, 0.999999f);
  float cumulative = 0.0f;
  for (std::size_t index = 0; index < probabilities.size(); ++index) {
    cumulative += probabilities[index];
    if (threshold < cumulative) return rl_action_from_index(static_cast<int>(index));
  }
  return rl_action_from_index(static_cast<int>(probabilities.size()) - 1);
}

auto PolicyNet::load_json(std::string_view text) -> std::optional<std::string> {
  using st::Json;
  auto parsed = st::json_parse(text);
  if (!parsed) return std::format("权重 JSON 解析失败：{}", parsed.error().message);
  const Json& root = *parsed;
  if (!root.is_object()) return std::string("权重必须是 JSON 对象");
  const Json* layers_node = st::json_find(root, "layers");
  if (layers_node == nullptr || !layers_node->is_array()) {
    return std::string("权重缺少 layers 数组");
  }
  // 规格校验：推理实现只支持 tanh，训练脚本若换了别的激活，
  // 在这里就报错（而不是在游戏里表现为"行为怪异"）。
  const std::string activation = st::json_get_string(root, "activation", "tanh");
  if (activation != "tanh") {
    return std::format("只支持 tanh 激活，权重声明的是「{}」", activation);
  }
  std::vector<LinearLayer> layers;
  layers.reserve(layers_node->size());
  for (std::size_t depth = 0; depth < layers_node->size(); ++depth) {
    const Json& node = (*layers_node)[depth];
    if (!node.is_object()) return std::format("layers[{0}] 不是对象", depth);
    const std::size_t in = static_cast<std::size_t>(st::json_get_i64(node, "in", 0));
    const std::size_t out = static_cast<std::size_t>(st::json_get_i64(node, "out", 0));
    if (in == 0 || out == 0) return std::format("layers[{0}] 的 in/out 非正", depth);
    const Json* weight_node = st::json_find(node, "weight");
    const Json* bias_node = st::json_find(node, "bias");
    if (weight_node == nullptr || !weight_node->is_array() ||
        weight_node->size() != out * in) {
      return std::format("layers[{0}] 的 weight 长度应为 {1}×{2}={3}", depth, out, in, out * in);
    }
    if (bias_node == nullptr || !bias_node->is_array() || bias_node->size() != out) {
      return std::format("layers[{0}] 的 bias 长度应为 {1}", depth, out);
    }
    LinearLayer layer{};
    layer.in = in;
    layer.out = out;
    layer.tanh = st::json_get_bool(node, "tanh", depth + 1 < layers_node->size());
    layer.weight.reserve(out * in);
    for (const Json& value : *weight_node) layer.weight.push_back(value.get<float>());
    layer.bias.reserve(out);
    for (const Json& value : *bias_node) layer.bias.push_back(value.get<float>());
    layers.push_back(std::move(layer));
  }
  if (layers.empty()) return std::string("layers 为空");
  // 相邻层维度必须连续（否则 forward 会在运行时返回空、表现为"机器人不动"）。
  for (std::size_t depth = 1; depth < layers.size(); ++depth) {
    if (layers[depth].in != layers[depth - 1].out) {
      return std::format("layers[{0}].in({1}) != layers[{2}].out({3})", depth, layers[depth].in,
                         depth - 1, layers[depth - 1].out);
    }
  }
  if (layers.front().in != observation_dim()) {
    return std::format("输入维度 {0} 与观测维度 {1} 不符（权重与代码版本不匹配）",
                       layers.front().in, observation_dim());
  }
  if (layers.back().out != kRlActionCount) {
    return std::format("输出维度 {0} 与动作数 {1} 不符", layers.back().out, kRlActionCount);
  }
  layers_ = std::move(layers);
  return std::nullopt;
}

auto PolicyNet::load_file(std::string_view path) -> std::optional<std::string> {
  auto text = st::fs::read_text(path);
  if (!text.has_value()) return std::format("读不到权重文件：{}", path);
  return load_json(*text);
}

auto PolicyNet::to_json(bool pretty) const -> std::string {  using st::Json;
  Json root = Json::object();
  root["version"] = 1;
  root["activation"] = "tanh";
  root["observation_dim"] = input_dim();
  root["action_count"] = output_dim();
  Json layers = Json::array();
  for (const LinearLayer& layer : layers_) {
    Json node = Json::object();
    node["in"] = layer.in;
    node["out"] = layer.out;
    node["tanh"] = layer.tanh;
    Json weight = Json::array();
    for (float value : layer.weight) weight.push_back(value);
    Json bias = Json::array();
    for (float value : layer.bias) bias.push_back(value);
    node["weight"] = std::move(weight);
    node["bias"] = std::move(bias);
    layers.push_back(std::move(node));
  }
  root["layers"] = std::move(layers);
  return st::json_dump(root, pretty);
}

namespace {

/// 从 st::Json 读一个 float 数组（型不对返回 false）。
[[nodiscard]] auto read_float_array(const st::Json& node, std::vector<float>& out) -> bool {
  if (!node.is_array()) return false;
  out.clear();
  out.reserve(node.size());
  for (const st::Json& value : node) {
    if (!value.is_number()) return false;
    out.push_back(value.get<float>());
  }
  return true;
}

}  // namespace

auto PolicyNet::forward_json(const std::string& request) -> std::string {
  using st::Json;
  auto parsed = st::json_parse(request);
  if (!parsed) return std::format("{{\"ok\":false,\"error\":\"请求 JSON 解析失败：{}\"}}",
                                  parsed.error().message);
  const Json& root = *parsed;
  PolicyNet net{};
  if (auto error = net.load_json(st::json_dump(root, false)); error.has_value()) {
    return std::format("{{\"ok\":false,\"error\":\"{0}\"}}", *error);
  }
  const Json* inputs = st::json_find(root, "inputs");
  if (inputs == nullptr || !inputs->is_array()) {
    return std::string("{\"ok\":false,\"error\":\"缺少 inputs 数组\"}");
  }
  Json logits_all = Json::array();
  Json actions = Json::array();
  std::size_t row = 0;
  for (const Json& record : *inputs) {
    std::vector<float> observation;
    if (!read_float_array(record, observation)) {
      return std::format("{{\"ok\":false,\"error\":\"inputs[{0}] 不是数字数组\"}}", row);
    }
    const std::vector<float> logits = net.forward(observation);
    if (logits.empty()) {
      return std::format(
          "{{\"ok\":false,\"error\":\"inputs[{0}] 维度 {1} 与网络输入 {2} 不符\"}}", row,
          observation.size(), net.input_dim());
    }
    Json row_logits = Json::array();
    for (float value : logits) row_logits.push_back(value);
    logits_all.push_back(std::move(row_logits));
    actions.push_back(static_cast<std::int64_t>(rl_action_from_index(
        static_cast<int>(std::distance(logits.begin(),
                                       std::max_element(logits.begin(), logits.end()))))));
    ++row;
  }
  Json response = Json::object();
  response["ok"] = true;
  response["logits"] = std::move(logits_all);
  response["actions"] = std::move(actions);
  response["observation_dim"] = static_cast<std::int64_t>(net.input_dim());
  response["parameter_count"] = static_cast<std::int64_t>(net.parameter_count());
  return st::json_dump(response, false);
}

}  // namespace gedou
