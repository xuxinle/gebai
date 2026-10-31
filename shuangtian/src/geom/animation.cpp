#include "st/raster/animation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace st::raster {

namespace {

using math::Quat;
using math::Vec3;

/// 采样一条轨道在 `time` 时刻的值。
///
/// ## 端点保持（不外推、不循环）
///
/// `time` 落在首帧之前 → 取首帧值；落在末帧之后 → 取末帧值。
/// 这是"给我 t 时刻的值"这个约定的自然结果；**循环播放是播放器的策略**
/// （`t = fmod(t, duration)`），不该由采样器偷偷做——否则"只想取某一时刻"
/// 的调用方会拿到被绕回的值。
[[nodiscard]] auto sample_track(const Track& track, float time) -> Keyframe {
  if (track.keys.empty()) return Keyframe{};
  // 解析时已排序，但采样是热路径，不再假设——万一有人手搓 Track 也不会崩。
  // 关键帧通常只有几个到几十个，线性扫描比排序便宜。
  const Keyframe* before = &track.keys.front();
  const Keyframe* after = &track.keys.front();
  for (const Keyframe& key : track.keys) {
    if (key.time <= time) {
      before = &key;
    } else {
      after = &key;
      break;
    }
  }
  if (time <= track.keys.front().time) return track.keys.front();
  if (time >= track.keys.back().time) return track.keys.back();
  if (before == after) return *before;

  const float span = after->time - before->time;
  if (span <= 1.0e-9f) return *after;   // 同一时刻的两个关键帧：后写的赢
  const float raw = (time - before->time) / span;
  const float t = apply_easing(before->easing, std::clamp(raw, 0.0f, 1.0f));

  Keyframe out{};
  out.time = time;
  if (track.kind == TrackKind::Visible) {
    // 阶跃语义：按 `before` 的值（`Constant` 是唯一有意义的选择）
    out.value = before->value;
    return out;
  }
  if (track.kind == TrackKind::Rotation) {
    // ## 旋转必须走四元数 slerp（不能用欧拉角线性插值）
    //
    // 欧拉角插值会绕远路、并在一轴对齐时跳变（万向锁）。
    // 这里把两个关键帧的欧拉角转成四元数、slerp、再转回欧拉角输出——
    // 因为 `SampledTransform::local.rotation` 是欧拉角字段（与场景脚本同口径）。
    const Quat qa = Quat::from_euler(before->value);
    const Quat qb = Quat::from_euler(after->value);
    out.value = slerp(qa, qb, t).to_euler();
    return out;
  }
  // 平移 / 缩放：逐分量线性
  out.value = before->value + (after->value - before->value) * t;
  return out;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// 缓动
// ────────────────────────────────────────────────────────────────────────────

auto easing_name(Easing easing) noexcept -> std::string_view {
  switch (easing) {
    case Easing::Linear: return "linear";
    case Easing::Constant: return "constant";
    case Easing::EaseIn: return "ease-in";
    case Easing::EaseOut: return "ease-out";
    case Easing::EaseInOut: return "ease-in-out";
  }
  return "linear";
}

auto parse_easing(std::string_view name) -> std::optional<Easing> {
  std::string key;
  key.reserve(name.size());
  for (const char ch : name) {
    if (ch == '_' || ch == ' ') {
      key.push_back('-');
    } else {
      key.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
  }
  if (key == "linear" || key == "none" || key == "step-linear") return Easing::Linear;
  if (key == "constant" || key == "step" || key == "hold" || key == "discrete") {
    return Easing::Constant;
  }
  if (key == "ease-in" || key == "easein" || key == "begin") return Easing::EaseIn;
  if (key == "ease-out" || key == "easeout" || key == "end") return Easing::EaseOut;
  if (key == "ease-in-out" || key == "easeinout" || key == "smooth" || key == "bezier") {
    return Easing::EaseInOut;
  }
  return std::nullopt;
}

auto easing_names() -> std::string { return "linear, constant, ease-in, ease-out, ease-in-out"; }

auto apply_easing(Easing easing, float t) noexcept -> float {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  switch (easing) {
    case Easing::Linear: return clamped;
    // 阶跃：过半即取后值？不——`Constant` 的语义是"保持前值直到下一关键帧"，
    // 所以整段都返回 0（前值权重 1.0），在关键帧处自然跳到新值。
    case Easing::Constant: return 0.0f;
    // smoothstep 的变体：三次曲线在两端导数为 0（观感"起步/收尾不生硬"）
    case Easing::EaseIn: return clamped * clamped;
    case Easing::EaseOut: {
      const float inv = 1.0f - clamped;
      return 1.0f - inv * inv;
    }
    case Easing::EaseInOut: return clamped * clamped * (3.0f - 2.0f * clamped);
  }
  return clamped;
}

// ────────────────────────────────────────────────────────────────────────────
// 轨道种类
// ────────────────────────────────────────────────────────────────────────────

auto track_kind_name(TrackKind kind) noexcept -> std::string_view {
  switch (kind) {
    case TrackKind::Translation: return "translate";
    case TrackKind::Rotation: return "rotate";
    case TrackKind::Scale: return "scale";
    case TrackKind::Visible: return "visible";
  }
  return "translate";
}

auto parse_track_kind(std::string_view name) -> std::optional<TrackKind> {
  std::string key;
  key.reserve(name.size());
  for (const char ch : name) {
    if (ch == '-' || ch == '_' || ch == ' ') {
      key.push_back('-');
    } else {
      key.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
  }
  if (key == "translate" || key == "translation" || key == "position" || key == "location" ||
      key == "move") {
    return TrackKind::Translation;
  }
  if (key == "rotate" || key == "rotation" || key == "spin") return TrackKind::Rotation;
  if (key == "scale" || key == "size") return TrackKind::Scale;
  if (key == "visible" || key == "visibility" || key == "show") return TrackKind::Visible;
  return std::nullopt;
}

auto track_kind_names() -> std::string { return "translate, rotate, scale, visible"; }

// ────────────────────────────────────────────────────────────────────────────
// 时长
// ────────────────────────────────────────────────────────────────────────────

auto Track::duration() const noexcept -> float {
  float latest = 0.0f;
  for (const Keyframe& key : keys) latest = std::max(latest, key.time);
  return latest;
}

auto Clip::duration() const noexcept -> float {
  float latest = 0.0f;
  for (const Track& track : tracks) latest = std::max(latest, track.duration());
  return latest;
}

// ────────────────────────────────────────────────────────────────────────────
// 采样
// ────────────────────────────────────────────────────────────────────────────

auto sample_clip(const Clip& clip, float time) -> std::vector<SampledTransform> {
  // 按节点聚合成一条 `SampledTransform`（三条轨道都写同一个节点的不同字段）
  std::map<std::string, SampledTransform> by_node;
  std::vector<std::string> order;   // 保持首次出现的顺序（确定性输出）
  for (const Track& track : clip.tracks) {
    if (track.keys.empty()) continue;
    if (!by_node.contains(track.node)) {
      order.push_back(track.node);
      SampledTransform entry{};
      entry.node = track.node;
      entry.animated = true;
      by_node.emplace(track.node, entry);
    }
    SampledTransform& entry = by_node[track.node];
    const Keyframe value = sample_track(track, time);
    switch (track.kind) {
      case TrackKind::Translation:
        entry.local.translation = value.value;
        entry.has_translation = true;
        break;
      case TrackKind::Rotation:
        entry.local.rotation = value.value;
        entry.has_rotation = true;
        break;
      case TrackKind::Scale:
        entry.local.scale = value.value;
        entry.has_scale = true;
        break;
      case TrackKind::Visible:
        entry.visible = value.value.x > 0.5f;
        entry.has_visible = true;
        break;
    }
  }
  std::vector<SampledTransform> out;
  out.reserve(order.size());
  for (const std::string& name : order) out.push_back(by_node[name]);
  return out;
}

auto apply_sample(const std::vector<SceneNode>& roots,
                  const std::vector<SampledTransform>& sampled) -> std::vector<SceneNode> {
  // 名字 → 采样值（同名节点在 `resolve` 阶段已被拒绝，所以这里无歧义）
  std::map<std::string, const SampledTransform*> by_name;
  for (const SampledTransform& entry : sampled) by_name.emplace(entry.node, &entry);

  std::vector<SceneNode> out = roots;
  const auto walk = [&](auto&& self, SceneNode& node) -> void {
    if (const auto found = by_name.find(node.name); found != by_name.end()) {
      const SampledTransform& entry = *found->second;
      // ## 按通道覆盖（不是整体覆盖）
      //
      // 只写 rotate 轨道的物体，其 `translation` 必须保持场景里写死的值；
      // 否则它会被搬到原点。实测：给 7 个物体中的 5 个写 rotate 轨道，
      // 那 5 个全部消失（挤在原点互相遮挡），画面像是"物体都没了"。
      if (entry.has_translation) node.local.translation = entry.local.translation;
      if (entry.has_rotation) node.local.rotation = entry.local.rotation;
      if (entry.has_scale) node.local.scale = entry.local.scale;
      if (entry.has_visible) node.visible = entry.visible;
    }
    for (SceneNode& child : node.children) self(self, child);
  };
  for (SceneNode& root : out) walk(walk, root);
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 稠密化
// ────────────────────────────────────────────────────────────────────────────

auto densify(const Clip& clip, float step) -> Clip {
  if (!(step > 0.0f)) return clip;
  Clip out{};
  out.name = clip.name;
  out.tracks.reserve(clip.tracks.size());
  for (const Track& track : clip.tracks) {
    if (track.keys.empty()) continue;
    // 是否有非线性插值：全是 Linear 时原样返回（不白白膨胀关键帧）
    const bool needs_densify =
        std::any_of(track.keys.begin(), track.keys.end(), [](const Keyframe& key) {
          return key.easing != Easing::Linear;
        });
    if (!needs_densify) {
      out.tracks.push_back(track);
      continue;
    }
    Track dense{};
    dense.node = track.node;
    dense.kind = track.kind;
    const float last = track.duration();
    // 步长比整段还大时至少保留首尾两点（否则会得到 1 个关键帧、动画等于静止）
    const int steps = std::max(static_cast<int>(std::ceil(last / step)), 1);
    for (int i = 0; i <= steps; ++i) {
      const float time = std::min(static_cast<float>(i) * step, last);
      Keyframe sampled = sample_track(track, time);
      sampled.time = time;
      sampled.easing = Easing::Linear;   // 稠密化之后每段都是线性
      dense.keys.push_back(sampled);
      if (time >= last) break;
    }
    // 末点必须精确落在 `last`（浮点累加可能差一个 step）
    if (!dense.keys.empty() && dense.keys.back().time < last) {
      Keyframe tail = sample_track(track, last);
      tail.time = last;
      tail.easing = Easing::Linear;
      dense.keys.push_back(tail);
    }
    out.tracks.push_back(std::move(dense));
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 校验
// ────────────────────────────────────────────────────────────────────────────

auto validate_clip(const Clip& clip, const std::vector<SceneNode>& roots) -> Status {
  // 收集树上的全部节点名（含嵌套）
  std::set<std::string> known;
  const auto collect = [&](auto&& self, const SceneNode& node) -> void {
    if (!node.name.empty()) known.insert(node.name);
    for (const SceneNode& child : node.children) self(self, child);
  };
  for (const SceneNode& root : roots) collect(collect, root);

  std::set<std::string> seen_track;   // "node|kind"
  for (std::size_t i = 0; i < clip.tracks.size(); ++i) {
    const Track& track = clip.tracks[i];
    const std::string path = std::format("tracks[{}]", i);
    if (track.node.empty()) {
      return st::unexpected(ErrorCode::Invalid, path + ".node：不能为空（要指向一个节点名）");
    }
    if (!known.contains(track.node)) {
      return st::unexpected(ErrorCode::Invalid,
                            std::format("{}.node：场景里没有节点 \"{}\"", path, track.node));
    }
    if (track.keys.empty()) {
      return st::unexpected(ErrorCode::Invalid, path + ".keys：至少要有一个关键帧");
    }
    const std::string unique_key = track.node + "|" + std::string(track_kind_name(track.kind));
    if (!seen_track.insert(unique_key).second) {
      return st::unexpected(
          ErrorCode::Invalid,
          std::format("{}：节点 \"{}\" 的 {} 轨道重复（同一属性只能有一条轨道——"
                      "两条会互相覆盖，而谁赢取决于顺序，结果不可复现）",
                      path, track.node, track_kind_name(track.kind)));
    }
  }
  return st::ok();
}

}  // namespace st::raster
