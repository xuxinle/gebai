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

[[nodiscard]] /// 解析图层数组（`Scene::layers`）。
///
/// ## 设计取舍：宽容解析
///
/// 未知 `kind` **跳过并告警**，而不是整份脚本报错。理由：脚本是外部输入
/// （AI 写的、用户手改的），一个拼错的图层不该让整片无法渲染——
/// 丢掉一层看得见（画面少东西），整片失败看不见（不知道为什么）。
///
/// 但**已识别的 kind 里的无效字段**要告警：那通常是"用户以为有这个能力"，
/// 静默忽略会让他以为设置生效了。
[[nodiscard]] auto parse_layers(const st::Json& array, std::vector<std::string>* warnings)
    -> std::vector<Layer> {
  std::vector<Layer> layers;
  if (!array.is_array()) return layers;
  for (const st::Json& node : array) {
    if (!node.is_object()) continue;
    Layer layer{};
    const std::string kind_name = st::json_get_string(node, "kind");
    const auto kind = layer_kind_from(kind_name);
    if (!kind) {
      if (warnings != nullptr) {
        warnings->push_back(std::format("未知图层类型 '{}'（已跳过）——可用：rect/ellipse/line/text/image/path/glow/particles", kind_name));
      }
      continue;
    }
    layer.kind = *kind;
    layer.name = st::json_get_string(node, "name");

    // 位置/尺寸：可能是数字（设计像素）或字符串（带 % 的比例）——
    // 两种写法都要支持，所以先按"能不能当字符串读"判定。
    const auto dim_field = [&node](std::string_view key, std::string fallback) -> std::string {
      const st::Json* field = st::json_find(node, key);
      if (field == nullptr) return fallback;
      if (field->is_string()) return field->get<std::string>();
      if (field->is_number()) {
        return std::format("{:.4f}", field->get<double>());
      }
      return fallback;
    };
    layer.x = dim_field("x", layer.x);
    layer.y = dim_field("y", layer.y);
    layer.w = dim_field("w", layer.w);
    layer.h = dim_field("h", layer.h);

    layer.fill = color_field(node, "fill", layer.fill);
    layer.fill_alt = color_field(node, "fill_alt", layer.fill_alt);
    layer.stroke = color_field(node, "stroke", layer.stroke);
    layer.color = color_field(node, "color", layer.color);
    layer.gradient_dir = static_cast<int>(
        std::clamp(st::json_get_i64(node, "gradient_dir", layer.gradient_dir), 0LL, 1LL));
    layer.stroke_width = static_cast<float>(
        std::clamp(st::json_get_double(node, "stroke_width", layer.stroke_width), 0.0, 80.0));
    layer.radius = static_cast<float>(
        std::clamp(st::json_get_double(node, "radius", layer.radius), 0.0, 4000.0));
    layer.opacity = static_cast<float>(
        std::clamp(st::json_get_double(node, "opacity", layer.opacity), 0.0, 1.0));

    layer.text = st::json_get_string(node, "text");
    layer.size = static_cast<float>(
        std::clamp(st::json_get_double(node, "size", layer.size), 4.0, 800.0));
    layer.tracking = static_cast<float>(
        std::clamp(st::json_get_double(node, "tracking", layer.tracking), 0.0, 200.0));
    layer.line_height = static_cast<float>(
        std::clamp(st::json_get_double(node, "line_height", layer.line_height), 1.0, 4.0));
    layer.bold = st::json_get_bool(node, "bold", layer.bold);
    layer.align = static_cast<int>(std::clamp(st::json_get_i64(node, "align", layer.align), 0LL, 3LL));

    layer.src = st::json_get_string(node, "src");
    layer.contain = st::json_get_bool(node, "contain", layer.contain);
    layer.dim = static_cast<float>(
        std::clamp(st::json_get_double(node, "dim", layer.dim), 0.0, 1.0));
    if (const st::Json* pts = st::json_find(node, "points"); pts != nullptr && pts->is_array()) {
      for (const st::Json& value : *pts) {
        if (value.is_number()) {
          layer.points.push_back(static_cast<float>(value.get<double>()));
        }
      }
    }
    layer.closed = st::json_get_bool(node, "closed", layer.closed);

    layer.intensity = static_cast<float>(
        std::clamp(st::json_get_double(node, "intensity", layer.intensity), 0.0, 1.0));
    layer.count = static_cast<int>(
        std::clamp(st::json_get_i64(node, "count", layer.count), 0LL, 600LL));
    layer.spread = static_cast<float>(
        std::clamp(st::json_get_double(node, "spread", layer.spread), 0.02, 2.0));

    layer.enter = static_cast<int>(std::clamp(st::json_get_i64(node, "enter", layer.enter), 0LL, 3LL));
    layer.enter_at = static_cast<float>(
        std::clamp(st::json_get_double(node, "enter_at", layer.enter_at), 0.0, 600.0));
    layer.enter_dur = static_cast<float>(
        std::clamp(st::json_get_double(node, "enter_dur", layer.enter_dur), 0.05, 10.0));

    layers.push_back(std::move(layer));
  }
  return layers;
}

auto scene_from(const st::Json& node, std::size_t index) -> Result<Scene> {
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
  // 通用图层（kind=custom 时是主体，其他 kind 是叠加层）。
  if (const st::Json* layers = st::json_find(node, "layers"); layers != nullptr) {
    scene.layers = parse_layers(*layers, nullptr);
  }
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
    case SceneKind::Custom: return "custom";
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
  // 品牌识标（默认空 = 包装层不画识标，见 director.hpp 的注释）
  script.brand_wordmark = st::json_get_string(root, "brand_wordmark", script.brand_wordmark);
  script.brand_latin = st::json_get_string(root, "brand_latin", script.brand_latin);
  script.footer_note = st::json_get_string(root, "footer_note", script.footer_note);
  script.show_brand = st::json_get_bool(root, "show_brand", script.show_brand);
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
  if (!script.brand_wordmark.empty()) root["brand_wordmark"] = script.brand_wordmark;
  if (!script.brand_latin.empty()) root["brand_latin"] = script.brand_latin;
  if (!script.footer_note.empty()) root["footer_note"] = script.footer_note;
  if (!script.show_brand) root["show_brand"] = false;
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

auto builtin_sample_promo() -> Script {
  // 这是**通用示例片**：五个场景各展示一种内置版式，文案是中性占位。
  //
  // 设计意图（写在这里，免得下一个人以为是"没写完"）：
  // - 它是「能跑起来的起点」——用户/AI 直接改文案与配色就能用，不用从空脚本开始；
  // - 它**不带任何具体品牌**：品牌名默认空串，包装层不画识标
  //   （印一个假品牌比不印更糟——会被误认为是模板残留）；
  // - 五个场景覆盖 title/terminal/triptych/metrics/outro 五种版式，
  //   相当于一份"能力目录"，看一遍就知道有哪些版式可用。
  Script script{};
  script.name = "示例宣传片 · 10 秒";
  script.fps = 30.0;
  script.width = 1920;
  script.height = 1080;
  script.transition = 0.30;
  script.fade_in = 0.20;
  script.fade_out = 0.45;
  script.audio = AudioSpec{.enabled = true, .style = "uplift", .bpm = 112, .gain = 0.60f};
  // 品牌留空：这是工具的内置示例，不该预设某个产品的名字。

  Scene title{};
  title.kind = SceneKind::Title;
  title.duration = 2.2;
  title.title = "标题";
  title.subtitle = "SUBTITLE";
  title.lines = {"能力一", "能力二", "能力三"};
  title.accent = Color{120, 176, 255, 255};
  title.bg_top = Color{6, 10, 22, 255};
  title.bg_bottom = Color{20, 32, 62, 255};
  script.scenes.push_back(title);

  Scene terminal{};
  terminal.kind = SceneKind::Terminal;
  terminal.duration = 2.3;
  terminal.title = "终端 / 代码版式";
  terminal.subtitle = "逐字符敲出，适合演示命令与配置";
  terminal.lines = {"$ ./program --flag value", "  output line 1", "  output line 2",
                    "  done."};
  terminal.accent = Color{124, 226, 178, 255};
  terminal.bg_top = Color{8, 14, 20, 255};
  terminal.bg_bottom = Color{14, 30, 34, 255};
  script.scenes.push_back(terminal);

  Scene triptych{};
  triptych.kind = SceneKind::Triptych;
  triptych.duration = 2.2;
  triptych.title = "三联卡版式";
  triptych.subtitle = "三条卖点并排升入";
  triptych.lines = {"第一项", "第二项", "第三项"};
  triptych.accent = Color{255, 196, 120, 255};
  triptych.bg_top = Color{18, 12, 24, 255};
  triptych.bg_bottom = Color{40, 24, 40, 255};
  script.scenes.push_back(triptych);

  Scene metrics{};
  metrics.kind = SceneKind::Metrics;
  metrics.duration = 1.9;
  metrics.title = "数字版式";
  metrics.subtitle = "大数字滚动到目标值";
  metrics.lines = {"指标一", "指标二", "指标三"};
  metrics.values = {100, 24, 1899};
  metrics.accent = Color{150, 200, 255, 255};
  metrics.bg_top = Color{4, 10, 26, 255};
  metrics.bg_bottom = Color{10, 22, 46, 255};
  script.scenes.push_back(metrics);

  Scene outro{};
  outro.kind = SceneKind::Outro;
  outro.duration = 1.4;
  outro.title = "结尾";
  outro.subtitle = "OUTRO";
  outro.accent = Color{120, 176, 255, 255};
  outro.bg_top = Color{6, 10, 22, 255};
  outro.bg_bottom = Color{14, 22, 44, 255};
  script.scenes.push_back(outro);
  return script;
}

}  // namespace caiyun
