#include "st/raster/scene_constraints.hpp"

#include "st/math/quaternion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace st::raster {

// ─────────────────────────────────────────────────────────────────────────────
// 锚点
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr float kPi = 3.14159265358979323846f;

constexpr std::pair<std::string_view, AnchorKind::Value> kAnchorNames[] = {
    {"bottom", AnchorKind::Bottom}, {"center", AnchorKind::Center},
    {"top", AnchorKind::Top},       {"left", AnchorKind::Left},
    {"right", AnchorKind::Right},   {"front", AnchorKind::Front},
    {"back", AnchorKind::Back},
};

auto iequals(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return false;
  }
  return true;
}

/// 按逗号切数字（容忍空格；恰好三个）。
auto parse_offset(std::string_view text, std::string_view where) -> Result<math::Vec3> {
  std::vector<float> parts;
  std::string current;
  const auto flush = [&] {
    if (current.empty()) return;
    try {
      std::size_t used = 0;
      const float value = std::stof(current, &used);
      if (used == current.size() && std::isfinite(value)) parts.push_back(value);
    } catch (const std::exception&) {
      // 解析失败留给下面的数量检查报错（消息带原文更有用）
    }
    current.clear();
  };
  for (const char ch : text) {
    if (ch == ',') {
      flush();
    } else if (ch != ' ' && ch != '\t') {
      current.push_back(ch);
    }
  }
  flush();
  if (parts.size() != 3U) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：偏移要写三个数（+dx,dy,dz），收到 \"{}\"", where, text));
  }
  return math::Vec3{parts[0], parts[1], parts[2]};
}

// degrees 在 rotation_to_point_y 内以 lambda 使用；此不单独保留定义。

}  // namespace

auto AnchorKind::parse(std::string_view name_text) -> std::optional<AnchorKind> {
  for (const auto& entry : kAnchorNames) {
    if (iequals(name_text, entry.first)) return AnchorKind{entry.second};
  }
  return std::nullopt;
}

auto AnchorKind::name() const -> std::string_view {
  for (const auto& entry : kAnchorNames) {
    if (entry.second == value) return entry.first;
  }
  return "bottom";
}

auto anchor_of_bounds(const Bounds& box, AnchorKind anchor) -> math::Vec3 {
  const math::Vec3 lo = box.min;
  const math::Vec3 hi = box.max;
  switch (anchor.value) {
    case AnchorKind::Bottom: return {box.center().x, lo.y, box.center().z};
    case AnchorKind::Top: return {box.center().x, hi.y, box.center().z};
    case AnchorKind::Left: return {lo.x, box.center().y, box.center().z};
    case AnchorKind::Right: return {hi.x, box.center().y, box.center().z};
    case AnchorKind::Front: return {box.center().x, box.center().y, hi.z};
    case AnchorKind::Back: return {box.center().x, box.center().y, lo.z};
    case AnchorKind::Center: return box.center();
  }
  return box.center();
}

auto parse_anchor_ref(std::string_view text) -> Result<AnchorRef> {
  // 形态：[anchor[_of]:]name[+dx,dy,dz]
  AnchorRef ref{};
  std::string_view rest = text;

  // 偏移（最后一个 '+' 之后——名字里不含 '+'，偏移一定带逗号）
  if (const auto plus = rest.rfind('+'); plus != std::string_view::npos) {
    const auto offset = parse_offset(rest.substr(plus + 1), "约束锚点");
    if (!offset) return st::forward_error(offset.error());
    ref.offset = *offset;
    rest = rest.substr(0, plus);
  }

  if (const auto colon = rest.find(':'); colon != std::string_view::npos) {
    // 有锚点段：`top_of:x` / `top:x`（_of 剥离大小写不敏感——TOP_OF: 同样收）。
    std::string_view anchor_word = rest.substr(0, colon);
    if (anchor_word.size() > 3) {
      const std::string_view tail = anchor_word.substr(anchor_word.size() - 3);
      if (iequals(tail, "_of")) {
        anchor_word = anchor_word.substr(0, anchor_word.size() - 3);
      }
    }
    const auto anchor = AnchorKind::parse(anchor_word);
    if (!anchor) {
      std::string allowed;
      for (const auto& entry : kAnchorNames) {
        if (!allowed.empty()) allowed += "/";
        allowed += entry.first;
      }
      return st::unexpected(ErrorCode::Parse,
                            std::format("约束锚点 \"{}\"：只接受 {}（或省略用默认）", anchor_word,
                                        allowed));
    }
    ref.anchor = *anchor;
    rest = rest.substr(colon + 1);
  }
  if (rest.empty()) {
    return st::unexpected(ErrorCode::Parse, "约束锚点：冒号后需要目标名字");
  }
  ref.target = std::string(rest);
  return ref;
}

// ─────────────────────────────────────────────────────────────────────────────
// 解算
// ─────────────────────────────────────────────────────────────────────────────

auto rotation_to_point_y(math::Vec3 direction) -> math::Vec3 {
  // 目标：把局部 +Y 旋到 direction。引擎应用序是 R = Rz·Ry·Rx（X 最先作用
  //于向量，与 Blender XYZ 欧拉一致）。两步角：
  // 1) 绕 Z 把 +Y 转到水平投影方向（XY 平面内）；
  // 2) 绕 X 抬仰角。
  // 但「先 X 后 Z」的应用序下，绕 X 的抬升会作用于已被 Z 转过的轴——直接拼
  // 两角会差一个耦合项。稳妥做法：构造四元数（任意两单位向量的最短旋转），
  // 再 to_euler（XYZ 序）——与引擎 `from_euler` 严格互逆，不靠手推三角。
  const math::Vec3 d = direction.normalized();
  const math::Vec3 up{0.0f, 1.0f, 0.0f};
  const float dot = std::clamp(up.dot(d), -1.0f, 1.0f);
  // 反向（180°）时任意水平轴都行；取 X 轴（与 Blender 的退化约定同风格）。
  if (dot < -0.999995f) {
    return math::Vec3{180.0f, 0.0f, 0.0f};
  }
  const math::Vec3 axis = up.cross(d);
  const float axis_length = axis.length();
  if (axis_length < 1e-6f) {
    return math::Vec3{};  // 已对准（同向）
  }
  const math::Vec3 unit_axis = axis * (1.0f / axis_length);
  const float angle = std::acos(dot);
  const math::Quat rotation =
      math::Quat::from_axis_angle(unit_axis, angle);
  const math::Vec3 euler = rotation.to_euler();  // 弧度 XYZ
  const auto deg = [](float radians) {
    return radians * 180.0f / kPi;
  };
  return math::Vec3{deg(euler.x), deg(euler.y), deg(euler.z)};
}

auto solve_attach(const AnchorRef& mine, const AnchorRef& target,
                  const ConstraintContext& context) -> Result<math::Vec3> {
  // `mine`（我的锚点）由调用方换算成差量后应用；解算层只负责目标侧世界点
  // （见头文件「写回」契约）。这里返回「我的锚点应当对准的世界点」。
  (void)mine;
  const auto target_point = context.world_anchor_of(target.target, target.anchor);
  if (!target_point) {
    return st::unexpected(
        ErrorCode::NotFound,
        std::format("attach：目标 \"{}\" 不存在（或尚未解算——约束只能引用先出现的名字）",
                    target.target));
  }
  return *target_point + target.offset;
}

auto solve_connect(const AnchorRef& from, const AnchorRef& to,
                   const ConstraintContext& context) -> Result<ConnectSolution> {
  const auto a = context.world_anchor_of(from.target, from.anchor);
  if (!a) {
    return st::unexpected(ErrorCode::NotFound,
                          std::format("connect：起点 \"{}\" 不存在（或尚未解算）", from.target));
  }
  const auto b = context.world_anchor_of(to.target, to.anchor);
  if (!b) {
    return st::unexpected(ErrorCode::NotFound,
                          std::format("connect：终点 \"{}\" 不存在（或尚未解算）", to.target));
  }
  const math::Vec3 pa = *a + from.offset;
  const math::Vec3 pb = *b + to.offset;
  const math::Vec3 delta = pb - pa;
  const float length = delta.length();
  if (length < 1e-4f) {
    return st::unexpected(ErrorCode::Invalid,
                          std::format("connect：\"{}\" 与 \"{}\" 的锚点重合（长度 0，无法定向）",
                                      from.target, to.target));
  }
  ConnectSolution solution;
  solution.length = length;
  solution.direction = delta * (1.0f / length);
  solution.midpoint = (pa + pb) * 0.5f;
  solution.rotation_deg = rotation_to_point_y(solution.direction);
  return solution;
}

auto solve_align(const AnchorRef& target, float my_current_y,
                 const ConstraintContext& context) -> Result<math::Vec3> {
  const auto box = context.world_bounds_of(target.target);
  if (!box) {
    return st::unexpected(ErrorCode::NotFound,
                          std::format("align：目标 \"{}\" 不存在（或尚未解算）", target.target));
  }
  const math::Vec3 center = box->center() + target.offset;
  return math::Vec3{center.x, my_current_y, center.z};
}

}  // namespace st::raster
