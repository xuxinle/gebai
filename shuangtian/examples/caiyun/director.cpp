/// 导演脚本的模型实现（见 `director.hpp`）。
///
/// 解析一律走 `st/ext/json.hpp` 的安全读取面（`json_find` / `json_get_*`）：
/// 缺字段取默认值、类型不符取默认值——**脚本是外部输入**（AI 生成的），一个写错的
/// 字段不该让整个工程打不开。真正"无法继续"的情形只有：顶层不是对象 / `scenes` 为空 /
/// 某个场景的 `kind` 不认识——这三种都如实报错（否则会得到一部空片，而空片看起来
/// 像"渲染器坏了"）。

#include "director.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/ext/json.hpp"

namespace caiyun {

namespace {

using st::math::Color;

/// 取颜色字段：支持 `#rrggbb` / `#rrggbbaa` 字符串，也支持 `0xRRGGBBAA` 数字。
[[nodiscard]] auto color_field(const st::Json& object, std::string_view key, Color fallback)
    -> Color {
  const st::Json* node = st::json_find(object, key);
  if (node == nullptr) return fallback;
  if (node->is_string()) return parse_color(node->get<std::string>(), fallback);
  if (node->is_number()) {
    return Color::from_rgba_hex(static_cast<std::uint32_t>(node->get<double>()));
  }
  return fallback;
}

/// 数字数组（`size` / `values`）。
[[nodiscard]] auto number_array(const st::Json& object, std::string_view key)
    -> std::vector<double> {
  std::vector<double> out;
  const st::Json* node = st::json_find(object, key);
  if (node == nullptr || !node->is_array()) return out;
  for (const st::Json& item : *node) {
    if (item.is_number()) out.push_back(item.get<double>());
  }
  return out;
}

[[nodiscard]] auto scene_from(const st::Json& node, std::size_t index) -> Result<Scene> {
  const std::string kind_name = st::json_get_string(node, "kind");
  const auto kind = scene_kind_from(kind_name);
  if (!kind) {
    return st::unexpected(st::ErrorCode::Parse,
                          std::format("scenes[{}].kind 不认识：「{}」", index, kind_name));
  }
  Scene scene{};
  scene.kind = *kind;
  scene.duration = std::clamp(st::json_get_double(node, "duration", 2.0), 0.2, 60.0);
  scene.title = st::json_get_string(node, "title");
  scene.subtitle = st::json_get_string(node, "subtitle");
  scene.lines = st::json_get_string_array(node, "lines");
  if (const st::Json* accent = st::json_find(node, "accent"); accent != nullptr) {
    scene.accent = color_field(node, "accent", scene.accent);
  }
  const st::Json* bg = st::json_find(node, "background");
  if (bg != nullptr && bg->is_array() && bg->size() >= 2) {
    const st::Json& top = *bg->begin();
    const st::Json& bottom = *(bg->begin() + 1);
    if (top.is_string()) scene.bg_top = parse_color(top.get<std::string>(), scene.bg_top);
    if (bottom.is_string()) {
      scene.bg_bottom = parse_color(bottom.get<std::string>(), scene.bg_bottom);
    }
  }
  scene.values = number_array(node, "values");
  // 素材：三种写法都存原文，由渲染侧的 `AssetLibrary` 按基准目录解析
  //（脚本不该知道自己被拷到哪，路径语义归素材库）。
  scene.image = st::json_get_string(node, "image");
  scene.image_card = st::json_get_string(node, "image_card");
  scene.image_dim = static_cast<float>(
      std::clamp(st::json_get_double(node, "image_dim", scene.image_dim), 0.0, 1.0));
  scene.image_ken_burns = static_cast<float>(
      std::clamp(st::json_get_double(node, "image_ken_burns", scene.image_ken_burns), 0.0, 0.25));
  // 影像特效（默认 0 = 关闭且零开销）
  scene.dof = static_cast<float>(std::clamp(st::json_get_double(node, "dof", scene.dof), 0.0, 1.0));
  scene.particles = static_cast<int>(
      std::clamp(st::json_get_i64(node, "particles", scene.particles), 0LL, 600LL));
  scene.particle_alpha = static_cast<float>(
      std::clamp(st::json_get_double(node, "particle_alpha", scene.particle_alpha), 0.0, 1.0));
  scene.glow = static_cast<float>(std::clamp(st::json_get_double(node, "glow", scene.glow), 0.0, 1.0));
  scene.glow_cx = static_cast<float>(
      std::clamp(st::json_get_double(node, "glow_cx", scene.glow_cx), 0.0, 1.0));
  scene.glow_cy = static_cast<float>(
      std::clamp(st::json_get_double(node, "glow_cy", scene.glow_cy), 0.0, 1.0));
  scene.glow_radius = static_cast<float>(
      std::clamp(st::json_get_double(node, "glow_radius", scene.glow_radius), 0.05, 1.5));
  scene.god_ray = static_cast<float>(
      std::clamp(st::json_get_double(node, "god_ray", scene.god_ray), 0.0, 1.0));
  scene.god_ray_angle = static_cast<float>(
      st::json_get_double(node, "god_ray_angle", scene.god_ray_angle));
  scene.light_sweep = static_cast<float>(
      std::clamp(st::json_get_double(node, "light_sweep", scene.light_sweep), 0.0, 1.0));
  // 运镜
  scene.cam_zoom_from = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_zoom_from", scene.cam_zoom_from), 0.5, 2.0));
  scene.cam_zoom_to = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_zoom_to", scene.cam_zoom_to), 0.5, 2.0));
  scene.cam_rotate = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_rotate", scene.cam_rotate), -8.0, 8.0));
  scene.cam_pan_x = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_pan_x", scene.cam_pan_x), -0.5, 0.5));
  scene.cam_pan_y = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_pan_y", scene.cam_pan_y), -0.5, 0.5));
  scene.cam_shake = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_shake", scene.cam_shake), 0.0, 0.08));
  return scene;
}

}  // namespace

auto scene_kind_name(SceneKind kind) -> std::string_view {
  switch (kind) {
    case SceneKind::Title: return "title";
    case SceneKind::Terminal: return "terminal";
    case SceneKind::Triptych: return "triptych";
    case SceneKind::Metrics: return "metrics";
    case SceneKind::Outro: return "outro";
  }
  return "title";
}

auto scene_kind_from(std::string_view name) -> std::optional<SceneKind> {
  for (const SceneKind kind : kSceneKinds) {
    if (scene_kind_name(kind) == name) return kind;
  }
  return std::nullopt;
}

auto Script::total_duration() const noexcept -> double {
  double total = 0.0;
  for (const Scene& scene : scenes) total += scene.duration;
  return total;
}

auto parse_color(std::string_view text, Color fallback) -> Color {
  if (text.empty() || text.front() != '#') return fallback;
  std::string digits(text.substr(1));
  if (digits.size() != 6 && digits.size() != 8) return fallback;
  std::uint32_t value = 0;
  for (const char ch : digits) {
    std::uint32_t nibble = 0;
    if (ch >= '0' && ch <= '9') {
      nibble = static_cast<std::uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = static_cast<std::uint32_t>(ch - 'a') + 10U;
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = static_cast<std::uint32_t>(ch - 'A') + 10U;
    } else {
      return fallback;
    }
    value = value * 16U + nibble;
  }
  if (digits.size() == 6) value = value * 256U + 255U;
  return Color::from_rgba_hex(value);
}

auto color_to_hex(Color color) -> std::string {
  return std::format("#{:02x}{:02x}{:02x}{:02x}", color.r, color.g, color.b, color.a);
}

auto parse_script(std::string_view json) -> Result<Script> {
  const auto parsed = st::json_parse(json);
  if (!parsed) return st::forward_error(parsed.error());
  const st::Json& root = parsed.value();
  if (!root.is_object()) {
    return st::unexpected(st::ErrorCode::Parse, "脚本顶层必须是对象");
  }
  Script script{};
  script.name = st::json_get_string(root, "name", script.name);
  script.fps = std::clamp(st::json_get_double(root, "fps", script.fps), 1.0, 120.0);
  const std::vector<double> size = number_array(root, "size");
  if (size.size() >= 2) {
    script.width = static_cast<int>(std::clamp(size[0], 64.0, 7680.0));
    script.height = static_cast<int>(std::clamp(size[1], 64.0, 4320.0));
  }
  script.transition = std::clamp(st::json_get_double(root, "transition", script.transition), 0.0, 2.0);
  script.fade_in = std::clamp(st::json_get_double(root, "fade_in", script.fade_in), 0.0, 5.0);
  script.fade_out = std::clamp(st::json_get_double(root, "fade_out", script.fade_out), 0.0, 5.0);

  if (const st::Json* audio = st::json_find(root, "audio"); audio != nullptr && audio->is_object()) {
    script.audio.enabled = st::json_get_bool(*audio, "enabled", script.audio.enabled);
    script.audio.style = st::json_get_string(*audio, "style", script.audio.style);
    script.audio.bpm = static_cast<int>(
        std::clamp(st::json_get_i64(*audio, "bpm", script.audio.bpm), 40LL, 240LL));
    script.audio.gain = static_cast<float>(
        std::clamp(st::json_get_double(*audio, "gain", script.audio.gain), 0.0, 1.0));
    if (const st::Json* voice = st::json_find(*audio, "voice");
        voice != nullptr && voice->is_array()) {
      for (const st::Json& item : *voice) {
        if (!item.is_object()) continue;
        VoiceTrack track{};
        track.path = st::json_get_string(item, "path");
        if (track.path.empty()) continue;
        track.start = st::json_get_double(item, "start", 0.0);
        track.gain =
            static_cast<float>(std::clamp(st::json_get_double(item, "gain", 1.0), 0.0, 4.0));
        script.audio.voice.push_back(std::move(track));
      }
    }
  }

  const st::Json* scenes = st::json_find(root, "scenes");
  if (scenes == nullptr || !scenes->is_array() || scenes->empty()) {
    return st::unexpected(st::ErrorCode::Parse, "脚本缺少 scenes（至少一个分镜）");
  }
  std::size_t index = 0;
  for (const st::Json& node : *scenes) {
    if (!node.is_object()) {
      return st::unexpected(st::ErrorCode::Parse, std::format("scenes[{}] 不是对象", index));
    }
    const auto scene = scene_from(node, index);
    if (!scene) return st::forward_error(scene.error());
    script.scenes.push_back(scene.value());
    ++index;
  }
  return script;
}

auto script_to_json(const Script& script) -> std::string {
  st::Json root = st::Json::object();
  root["name"] = script.name;
  root["fps"] = script.fps;
  root["size"] = st::Json::array({script.width, script.height});
  root["transition"] = script.transition;
  root["fade_in"] = script.fade_in;
  root["fade_out"] = script.fade_out;
  st::Json audio = st::Json::object();
  audio["enabled"] = script.audio.enabled;
  audio["style"] = script.audio.style;
  audio["bpm"] = script.audio.bpm;
  audio["gain"] = static_cast<double>(script.audio.gain);
  if (!script.audio.voice.empty()) {
    st::Json voice = st::Json::array();
    for (const VoiceTrack& track : script.audio.voice) {
      st::Json item = st::Json::object();
      item["path"] = track.path;
      item["start"] = track.start;
      item["gain"] = static_cast<double>(track.gain);
      voice.push_back(std::move(item));
    }
    audio["voice"] = std::move(voice);
  }
  root["audio"] = std::move(audio);
  st::Json scenes = st::Json::array();
  for (const Scene& scene : script.scenes) {
    st::Json node = st::Json::object();
    node["kind"] = std::string(scene_kind_name(scene.kind));
    node["duration"] = scene.duration;
    if (!scene.title.empty()) node["title"] = scene.title;
    if (!scene.subtitle.empty()) node["subtitle"] = scene.subtitle;
    if (!scene.lines.empty()) node["lines"] = scene.lines;
    node["accent"] = color_to_hex(scene.accent);
    node["background"] = st::Json::array({color_to_hex(scene.bg_top), color_to_hex(scene.bg_bottom)});
    if (!scene.values.empty()) node["values"] = scene.values;
    // 素材字段只在非空时写回——空字符串会让保存出来的脚本充满噪声。
    if (!scene.image.empty()) node["image"] = scene.image;
    if (!scene.image_card.empty()) node["image_card"] = scene.image_card;
    if (!scene.image.empty()) {
      node["image_dim"] = static_cast<double>(scene.image_dim);
      node["image_ken_burns"] = static_cast<double>(scene.image_ken_burns);
    }
    // 特效：只在开了时写（保持生成出来的脚本干净、可读）
    if (scene.dof > 0.0001f) node["dof"] = static_cast<double>(scene.dof);
    if (scene.particles > 0) {
      node["particles"] = static_cast<std::int64_t>(scene.particles);
      node["particle_alpha"] = static_cast<double>(scene.particle_alpha);
    }
    if (scene.glow > 0.0001f) {
      node["glow"] = static_cast<double>(scene.glow);
      node["glow_cx"] = static_cast<double>(scene.glow_cx);
      node["glow_cy"] = static_cast<double>(scene.glow_cy);
      node["glow_radius"] = static_cast<double>(scene.glow_radius);
    }
    if (scene.god_ray > 0.0001f) {
      node["god_ray"] = static_cast<double>(scene.god_ray);
      node["god_ray_angle"] = static_cast<double>(scene.god_ray_angle);
    }
    if (scene.light_sweep > 0.0001f) node["light_sweep"] = static_cast<double>(scene.light_sweep);
    // 运镜：只在真会动时写（from==to 且无旋转平移 = 静止）
    const bool camera_moves = scene.cam_zoom_from != scene.cam_zoom_to ||
                              scene.cam_rotate != 0.0f || scene.cam_pan_x != 0.0f ||
                              scene.cam_pan_y != 0.0f || scene.cam_shake > 0.0001f;
    if (camera_moves) {
      node["cam_zoom_from"] = static_cast<double>(scene.cam_zoom_from);
      node["cam_zoom_to"] = static_cast<double>(scene.cam_zoom_to);
      if (scene.cam_rotate != 0.0f) node["cam_rotate"] = static_cast<double>(scene.cam_rotate);
      if (scene.cam_pan_x != 0.0f) node["cam_pan_x"] = static_cast<double>(scene.cam_pan_x);
      if (scene.cam_pan_y != 0.0f) node["cam_pan_y"] = static_cast<double>(scene.cam_pan_y);
      if (scene.cam_shake > 0.0001f) node["cam_shake"] = static_cast<double>(scene.cam_shake);
    }
    scenes.push_back(std::move(node));
  }
  root["scenes"] = std::move(scenes);
  return st::json_dump(root, true, 2);
}

auto builtin_gebai_promo() -> Script {
  Script script{};
  script.name = "歌白宣传片 · 10 秒";
  script.fps = 30.0;
  script.width = 1920;
  script.height = 1080;
  script.transition = 0.30;
  script.fade_in = 0.20;
  script.fade_out = 0.45;
  script.audio = AudioSpec{.enabled = true, .style = "uplift", .bpm = 112, .gain = 0.60f};

  Scene title{};
  title.kind = SceneKind::Title;
  title.duration = 2.2;
  title.title = "歌白";
  title.subtitle = "GEBAI AGENT";
  title.lines = {"融合旧世界 IT 的所有技术", "打造新世界智能的躯体"};
  title.accent = Color{120, 176, 255, 255};
  title.bg_top = Color{6, 10, 22, 255};
  title.bg_bottom = Color{20, 32, 62, 255};
  script.scenes.push_back(title);

  Scene terminal{};
  terminal.kind = SceneKind::Terminal;
  terminal.duration = 2.3;
  terminal.title = "一个 TS 文件 = 一个子 Agent";
  terminal.subtitle = "声明的工具即刻并入当前会话 · 全程审批留痕";
  terminal.lines = {"export default {", "  name: 'facts',", "  tools: { read, grep },",
                    "};  // agent_load('facts')"};
  terminal.accent = Color{124, 226, 178, 255};
  terminal.bg_top = Color{8, 14, 20, 255};
  terminal.bg_bottom = Color{14, 30, 34, 255};
  script.scenes.push_back(terminal);

  Scene triptych{};
  triptych.kind = SceneKind::Triptych;
  triptych.duration = 2.2;
  triptych.title = "能力外置，权力内守";
  triptych.subtitle = "任意语言、任意进程，统一裁决";
  triptych.lines = {"子 Agent 扩展", "工具统一收编", "审批与留痕"};
  triptych.accent = Color{255, 196, 120, 255};
  triptych.bg_top = Color{18, 12, 24, 255};
  triptych.bg_bottom = Color{40, 24, 40, 255};
  script.scenes.push_back(triptych);

  Scene metrics{};
  metrics.kind = SceneKind::Metrics;
  metrics.duration = 1.9;
  metrics.title = "一个二进制，三端合一";
  metrics.subtitle = "桌面 · 浏览器 · 服务端";
  metrics.lines = {"子 Agent", "工具", "测试"};
  metrics.values = {26, 120, 2650};
  metrics.accent = Color{150, 200, 255, 255};
  metrics.bg_top = Color{4, 10, 26, 255};
  metrics.bg_bottom = Color{10, 22, 46, 255};
  script.scenes.push_back(metrics);

  Scene outro{};
  outro.kind = SceneKind::Outro;
  outro.duration = 1.4;
  outro.title = "歌白";
  outro.subtitle = "GEBAI AGENT · 新世界智能的躯体";
  outro.accent = Color{120, 176, 255, 255};
  outro.bg_top = Color{6, 10, 22, 255};
  outro.bg_bottom = Color{14, 22, 44, 255};
  script.scenes.push_back(outro);
  return script;
}

}  // namespace caiyun
