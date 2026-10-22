#include "st/text/weight_curve.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

#include "st/core/fs.hpp"

namespace st::text {

// 注意：这里的辅助函数必须在 **`WeightCurve` 定义完整可见之后**才能引用 `WeightCurve::Sample`。
// 头文件已 `#include` 进来，所以直接展开（不用匿名命名空间包——包了会变成
// “不在外层命名空间内”故拿不到成员类型，实测报 -fpermissive 一串错误）。
namespace wc_detail {

/// 夹取到合法区间。γ 的下界 0.3 与 `sanitize_coverage_gamma` 同口径
/// （再小会把低覆盖率区直接推成全黑，形状走样）；对比度上界 3 与 Skia 的量纲一致。
constexpr float kMinGamma = 0.3f;
constexpr float kMaxGamma = 4.0f;
constexpr float kMaxContrast = 3.0f;

[[nodiscard]] inline auto clamp_sample(WeightCurve::Sample s) noexcept -> WeightCurve::Sample {
  if (!std::isfinite(s.pixel_size) || s.pixel_size <= 0.0f) {
    return WeightCurve::Sample{0.0f, 1.0f, 0.0f};
  }
  if (!std::isfinite(s.gamma)) s.gamma = 1.0f;
  if (!std::isfinite(s.contrast)) s.contrast = 0.0f;
  s.gamma = std::clamp(s.gamma, kMinGamma, kMaxGamma);
  s.contrast = std::clamp(s.contrast, 0.0f, kMaxContrast);
  return s;
}

[[nodiscard]] inline auto parse_float(std::string_view token) noexcept -> float {
  // `from_chars` 对浮点在某些 libstdc++ 版本支持不全，用 `strtof` 更稳。
  std::string buf(token);
  char* end = nullptr;
  const float value = std::strtof(buf.c_str(), &end);
  if (end == buf.c_str()) return std::numeric_limits<float>::quiet_NaN();
  return value;
}

}  // namespace wc_detail

WeightCurve::WeightCurve(std::vector<Sample> samples) { set_samples(std::move(samples)); }
void WeightCurve::set_samples(std::vector<Sample> samples) {
  std::vector<Sample> cleaned;
  cleaned.reserve(samples.size());
  for (const Sample& s : samples) {
    const Sample c = wc_detail::clamp_sample(s);
    if (c.pixel_size > 0.0f) cleaned.push_back(c);
  }
  // 升序；同字号后者覆盖前者（`stable_sort` 保住插入顺序，便于"后来者胜"）
  std::stable_sort(cleaned.begin(), cleaned.end(),
                   [](const Sample& a, const Sample& b) { return a.pixel_size < b.pixel_size; });
  std::vector<Sample> deduped;
  deduped.reserve(cleaned.size());
  for (const Sample& s : cleaned) {
    if (!deduped.empty() && std::abs(deduped.back().pixel_size - s.pixel_size) < 1e-4f) {
      deduped.back() = s;   // 同字号：后来者胜
    } else {
      deduped.push_back(s);
    }
  }
  samples_ = std::move(deduped);
}

auto WeightCurve::load_from_text(std::string_view text) -> std::size_t {
  std::vector<Sample> parsed;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    // 去注释
    if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
      line = line.substr(0, hash);
    }
    // 拆分空白
    std::array<std::string_view, 8> tokens{};
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < line.size() && count < tokens.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
      const std::size_t begin = i;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
      if (i > begin) tokens[count++] = line.substr(begin, i - begin);
    }
    if (count == 0) continue;              // 空行
    if (count < 3) continue;               // 字段不够：**静默跳过**（允许只有备注的行）
    Sample s;
    s.pixel_size = wc_detail::parse_float(tokens[0]);
    s.gamma = wc_detail::parse_float(tokens[1]);
    s.contrast = wc_detail::parse_float(tokens[2]);
    // 解析失败（NaN）由 clamp_sample 兜底成默认值；字号无效则整点丢弃
    if (!std::isfinite(s.pixel_size) || s.pixel_size <= 0.0f) continue;
    parsed.push_back(s);
  }
  set_samples(std::move(parsed));
  return samples_.size();
}

auto WeightCurve::load_from_file(std::string_view path) -> std::size_t {
  std::ifstream file{std::string(path)};
  if (!file) return 0;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return load_from_text(buffer.str());
}

auto WeightCurve::evaluate(float pixel_size, float global_gamma,
                           float global_contrast) const noexcept -> Value {
  if (samples_.empty()) return Value{global_gamma, global_contrast, false};
  if (!std::isfinite(pixel_size) || pixel_size <= 0.0f) {
    return Value{global_gamma, global_contrast, false};
  }
  // 超出范围：**端点值**（不外推斜率——极字号上外推会得到荒谬的 γ）
  //
  // ⚠ contrast 是**乘法修正**（曲线的对比度 × 全局强度），不是覆盖：
  // 全局 `coverage_contrast_strength` 是“基底强度”（亮/暗各自标定，见
  // `DarkClassGammas` 旁边的说明），曲线里的 contrast 表达的是“该字号相对基底
  // 要打几折”。旧实现直接返回曲线值，使得一旦加载曲线，全局 contrast 就**完全失效**
  // （实测：`--cstrength` 从 0 到 3.0 输出逐位相同）。
  const auto scale_contrast = [global_contrast](float curve_contrast) noexcept -> float {
    return curve_contrast * global_contrast;
  };
  if (pixel_size <= samples_.front().pixel_size) {
    return Value{samples_.front().gamma, scale_contrast(samples_.front().contrast), true};
  }
  if (pixel_size >= samples_.back().pixel_size) {
    return Value{samples_.back().gamma, scale_contrast(samples_.back().contrast), true};
  }
  // 二分找右端点
  std::size_t lo = 0;
  std::size_t hi = samples_.size() - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (samples_[mid].pixel_size <= pixel_size) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const Sample& a = samples_[lo];
  const Sample& b = samples_[hi];
  const float span = b.pixel_size - a.pixel_size;
  const float t = span > 1e-6f ? (pixel_size - a.pixel_size) / span : 0.0f;
  return Value{a.gamma + (b.gamma - a.gamma) * t,
               scale_contrast(a.contrast + (b.contrast - a.contrast) * t), true};
}

auto WeightCurve::describe() const -> std::string {
  if (samples_.empty()) return "（空：未启用字号补偿）";
  std::string out;
  for (const Sample& s : samples_) {
    if (!out.empty()) out += ", ";
    out += std::to_string(static_cast<int>(std::lround(s.pixel_size)));
    out += "→γ";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f/c%.2f", static_cast<double>(s.gamma),
                  static_cast<double>(s.contrast));
    out += buf;
  }
  return out;
}

}  // namespace st::text
