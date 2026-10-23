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
#include "st/core/log.hpp"

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

/// 校验 `tracks` 里的属性名（轨道只能驱动 x/y/w/h/scale/alpha/rotate）。
///
/// 为何需要单独一条：`tracks` 是对象时，它的**子键就是属性名**，而属性名
/// 不是图层字段（不在 `SchemaScope::Layer` 里），所以顶层字段名校验会
/// 把整个 `tracks` 当成已知键放过去，而内部的错写一个都抓不到。
/// 实测意义：`"tracks": {"scae": [...]}`（scale 少写一个 l）完全静默不生效。
/// 把一个数值字段“直取 + 夹取”并**报告实际发生的夹取**。
///
/// ## 为何不能只直夹取
///
/// 夹取是失败安全（脚本写错不该让整片失败），但**静默夹取会误导写脚本的人**：
/// 写 `"model_segments": 200` 得到 96 段，画面上看不出来，会以为是“200 也不更圆”。
/// 所以这里把“写了多少 / 生效多少”一并记进诊断，既保住宽容，又不隐短。
///
/// ## 为何是“后置校验”而不是逐字段包一层
///
/// 逐字段包一层要在解析里写四十多道 `report_clamp(...)`，既碟又容易漏——
/// 而漏掉的那个字段会变成“夹取不报告”，比全不报告更难发现（因为其余都正常）。
/// 这里改成：解析照旧（一行不改），然后**拿 schema 遍历节点**比“写了什么”与
/// “实际生效什么”。新增字段只需登记进表，校验自动生效。
auto collect_range_diagnostics(const st::Json& node, SchemaScope scope, const std::string& path,
                              std::vector<Diagnostic>& diagnostics) -> void {
  for (const auto& [key, value] : node.items()) {
    const FieldSpec* spec = find_field_schema(scope, key);
    if (spec == nullptr || !spec->has_range || !spec->clamped) continue;
    if (!value.is_number()) continue;
    const double written = value.get<double>();
    const double applied = std::clamp(written, spec->min_value, spec->max_value);
    if (written < spec->min_value - 1e-9 || written > spec->max_value + 1e-9) {
      diagnostics.push_back(out_of_range_diagnostic(*spec, written, applied, path));
    }
  }
}

auto collect_track_diagnostics(const st::Json& node, const std::string& path,
                               std::vector<Diagnostic>& diagnostics) -> void {
  const auto supported = [](std::string_view property) {
    return property == "x" || property == "y" || property == "w" || property == "h" ||
           property == "scale" || property == "alpha" || property == "rotate";
  };
  const st::Json* tracks = st::json_find(node, "tracks");
  if (tracks == nullptr) return;
  if (tracks->is_object()) {
    for (const auto& [property, keys] : tracks->items()) {
      (void)keys;
      if (supported(property)) continue;
      Diagnostic diag{};
      diag.path = path + ".tracks";
      diag.key = property;
      // 属性名集合很小，直接给出正确清单比给单个建议更有用。
      diag.message = std::format("tracks 不支持属性 '{}'（已忽略）——可用："
                                 "x/y/w/h/scale/alpha/rotate",
                                 property);
      diagnostics.push_back(std::move(diag));
    }
  } else if (tracks->is_array()) {
    for (const st::Json& entry : *tracks) {
      if (!entry.is_object()) continue;
      const std::string property = st::json_get_string(entry, "property");
      if (!property.empty() && !supported(property)) {
        Diagnostic diag{};
        diag.path = path + ".tracks";
        diag.key = property;
        diag.message = std::format("tracks 不支持属性 '{}'（已忽略）——可用："
                                   "x/y/w/h/scale/alpha/rotate",
                                   property);
        diagnostics.push_back(std::move(diag));
      }
    }
  }
}

[[nodiscard]] /// 解析图层数组（`Scene::layers`）。
///
/// 未知 `kind` **跳过并告警**，而不是整份脚本报错。理由：脚本是外部输入
/// （AI 写的、用户手改的），一个拼错的图层不该让整片无法渲染——
/// 丢掉一层看得见（画面少东西），整片失败看不见（不知道为什么）。
///
/// 但**已识别的 kind 里的无效字段**要告警：那通常是"用户以为有这个能力"，
/// 静默忽略会让他以为设置生效了。
///
/// ## 为何字段名校验要走 schema（而不是在这里手写白名单）
///
/// 图层有七十九个可写键。手写白名单就是第二份真相：加字段时忘了改它就变成
/// "字段能用但不被承认"，而两份都只报"未知字段"，很难定位。
/// `schema.hpp` 那份表才是唯一真相，这里只做一件事：把节点里"表里没有的键"
/// 交给它生成诊断（它知道该建议什么）。
[[nodiscard]] auto parse_layers(const st::Json& array, std::vector<std::string>* warnings,
                                std::vector<Diagnostic>* diagnostics, std::string_view path_prefix)
    -> std::vector<Layer> {
  std::vector<Layer> layers;
  if (!array.is_array()) return layers;
  for (const st::Json& node : array) {
    if (!node.is_object()) continue;
    Layer layer{};
    const std::string kind_name = st::json_get_string(node, "kind");
    // 诊断路径：`scenes[0].layers[2]`——智能体拿到就能直接定位到编辑位置。
    const std::string path = std::format("{}.layers[{}]", path_prefix, layers.size());
    const auto kind = layer_kind_from(kind_name);
    // **非法色值要报出来**（而不是静默回退）。
    //
    // `parse_color` 只接受 `#rrggbb` / `#rrggbbaa`；写 `rgb(30,40,50)`、
    // `#ff` 或 `red` 都会**静默退回默认色**（通常是白）。
    // 实测代价：一份 480 个格子的脚本全用了 `rgb(...)`，
    // 结果整屏是白色方阵，而**没有任何报错**——花了很久才找到是颜色格式问题。
    //
    // 色值字段名很少（fill/fill_alt/stroke/color/background），直接枚举。
    if (diagnostics != nullptr) {
      for (const char* key : {"fill", "fill_alt", "stroke", "color"}) {
        const st::Json* value = st::json_find(node, key);
        if (value == nullptr || !value->is_string()) continue;
        const std::string text = value->get<std::string>();
        if (text.empty() || text.front() != '#') {
          Diagnostic diag{};
          diag.path = path;
          diag.key = key;
          diag.is_error = true;
          diag.message = std::format(
              "色值 '{}' 不可识别（已回退默认色）——只接受 #rrggbb 或 #rrggbbaa 十六进制；"
              "不要写 rgb()/rgba()/颜色名",
              text);
          diagnostics->push_back(std::move(diag));
          continue;
        }
        const std::size_t digits = text.size() - 1;
        if (digits != 6 && digits != 8) {
          Diagnostic diag{};
          diag.path = path;
          diag.key = key;
          diag.is_error = true;
          diag.message = std::format("色值 '{}' 位数不对（需 6 或 8 位十六进制）", text);
          diagnostics->push_back(std::move(diag));
        }
      }
    }
    if (!kind) {
      if (warnings != nullptr) {
        warnings->push_back(std::format("未知图层类型 '{}'（已跳过）——可用：rect/ellipse/line/text/image/path/glow/particles/model3d", kind_name));
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
    // 逐字排版（`kintext`）：风格参数写在 JSON，时间演化交给渲染器。
    layer.kin_stagger = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_stagger", layer.kin_stagger), 0.0, 2.0));
    layer.kin_dur = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_dur", layer.kin_dur), 0.01, 10.0));
    layer.kin_enter = static_cast<int>(
        std::clamp(st::json_get_double(node, "kin_enter", layer.kin_enter), 0.0, 2.0));
    layer.kin_rise = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_rise", layer.kin_rise), 0.0, 400.0));
    layer.kin_float = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_float", layer.kin_float), 0.0, 80.0));
    layer.kin_float_hz = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_float_hz", layer.kin_float_hz), 0.01, 8.0));
    layer.kin_caret = static_cast<int>(
        std::clamp(st::json_get_double(node, "kin_caret", layer.kin_caret), 0.0, 1.0));
    layer.kin_caret_scale = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_caret_scale", layer.kin_caret_scale), 0.2, 2.0));
    layer.kin_type_cps = static_cast<float>(
        std::clamp(st::json_get_double(node, "kin_type_cps", layer.kin_type_cps), 0.0, 200.0));
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

    // —— 三维模型（`kind=model3d`）——
    //
    // 宽容解析：形状名不认识不报错（`mesh_for_layer` 会落盒子并告警），
    // 因为“写错一个形状名”不该让整部片子打不开。
    // 角度不夹取到 0~360：`model_spin` 是角速度，负值就是反向转（合法且常用）；
    // 而相机方位角允许 >360 连续累加（设 380 与 20 等价，但便于表达“多转一圈”）。
    layer.model_shape = st::json_get_string(node, "model_shape");
    if (layer.model_shape.empty()) layer.model_shape = "box";
    layer.model_color = color_field(node, "model_color", layer.model_color);
    layer.model_color_alt = color_field(node, "model_color_alt", layer.model_color_alt);
    layer.model_segments = static_cast<int>(
        std::clamp(st::json_get_i64(node, "model_segments", layer.model_segments), 3LL, 96LL));
    layer.model_scale = static_cast<float>(
        std::clamp(st::json_get_double(node, "model_scale", layer.model_scale), 0.05, 8.0));
    layer.model_spin = static_cast<float>(
        std::clamp(st::json_get_double(node, "model_spin", layer.model_spin), -720.0, 720.0));
    layer.model_yaw = static_cast<float>(
        std::clamp(st::json_get_double(node, "model_yaw", layer.model_yaw), -3600.0, 3600.0));
    layer.model_tilt = static_cast<float>(
        std::clamp(st::json_get_double(node, "model_tilt", layer.model_tilt), -89.0, 89.0));
    layer.model_bob = static_cast<float>(
        std::clamp(st::json_get_double(node, "model_bob", layer.model_bob), 0.0, 0.5));
    layer.model_bob_hz = static_cast<float>(
        std::clamp(st::json_get_double(node, "model_bob_hz", layer.model_bob_hz), 0.01, 5.0));
    layer.model_backdrop = st::json_get_bool(node, "model_backdrop", layer.model_backdrop);
          layer.model_texture = st::json_get_string(node, "model_texture", layer.model_texture);
      layer.model_group = st::json_get_string(node, "model_group", layer.model_group);
      layer.model_shadow = static_cast<float>(
          std::clamp(st::json_get_double(node, "model_shadow", layer.model_shadow), 0.0, 1.0));
      layer.model_bounce_squash = static_cast<float>(
          std::clamp(st::json_get_double(node, "model_bounce_squash", layer.model_bounce_squash),
                     0.0, 1.0));
      layer.model_shadow_color =
          color_field(node, "model_shadow_color", layer.model_shadow_color);
    // 阻尼弹跳（层通用，不限于三维）
    layer.bounce = static_cast<float>(
        std::clamp(st::json_get_double(node, "bounce", layer.bounce), 0.0, 400.0));
    layer.bounce_hz = static_cast<float>(
        std::clamp(st::json_get_double(node, "bounce_hz", layer.bounce_hz), 0.05, 8.0));
    layer.bounce_damping = static_cast<float>(
        std::clamp(st::json_get_double(node, "bounce_damping", layer.bounce_damping), 0.0, 20.0));
          layer.bounce_at = static_cast<float>(st::json_get_double(node, "bounce_at", layer.bounce_at));
      // 路径动画（“从 A 飞到 B，走一条弧线”）
      layer.path_from_x = dim_field("path_from_x", layer.path_from_x);
      layer.path_from_y = dim_field("path_from_y", layer.path_from_y);
      layer.path_to_x = dim_field("path_to_x", layer.path_to_x);
      layer.path_to_y = dim_field("path_to_y", layer.path_to_y);
      layer.path_arc = static_cast<float>(
          st::json_get_double(node, "path_arc", layer.path_arc));
      layer.path_dur = static_cast<float>(
          std::clamp(st::json_get_double(node, "path_dur", layer.path_dur), 0.02, 60.0));
      layer.path_at = static_cast<float>(st::json_get_double(node, "path_at", layer.path_at));
      layer.path_ease = static_cast<int>(
          std::clamp(st::json_get_double(node, "path_ease", layer.path_ease), 0.0, 3.0));
      // 关键帧时间线（`tracks`）：每个轨道一条属性曲线。
      //
      // 两种写法都支持：
      //   "tracks": { "y": [{"t":0,"v":400}, {"t":1,"v":200}] }         // 按属性名分组（推荐）
      //   "tracks": [{"property":"y", "keys":[...]}]                      // 显式数组
      // 前者更短（属性名就是键），后者便于程序生成。
      if (const st::Json* tracks = st::json_find(node, "tracks"); tracks != nullptr) {
        const auto parse_keys = [](const st::Json& list) -> std::vector<KeyFrame> {
          std::vector<KeyFrame> keys;
          if (!list.is_array()) return keys;
          for (const st::Json& entry : list) {
            KeyFrame key{};
            key.time = static_cast<float>(st::json_get_double(entry, "t", 0.0));
            key.value = static_cast<float>(st::json_get_double(entry, "v", 0.0));
            key.ease = static_cast<int>(
                std::clamp(st::json_get_double(entry, "ease", 1.0), 0.0, 3.0));
            keys.push_back(key);
          }
          // 排序：乱序输入也能用（AI 写关键帧时顺序很不可靠）
          std::sort(keys.begin(), keys.end(),
                    [](const KeyFrame& a, const KeyFrame& b) { return a.time < b.time; });
          return keys;
        };
        if (tracks->is_object()) {
          for (const auto& [property, list] : tracks->items()) {
            KeyTrack track{};
            track.property = property;
            track.keys = parse_keys(list);
            if (!track.keys.empty()) layer.tracks.push_back(std::move(track));
          }
        } else if (tracks->is_array()) {
          for (const st::Json& entry : *tracks) {
            KeyTrack track{};
            track.property = st::json_get_string(entry, "property");
            if (const st::Json* keys = st::json_find(entry, "keys"); keys != nullptr) {
              track.keys = parse_keys(*keys);
            }
            if (!track.property.empty() && !track.keys.empty()) {
              layer.tracks.push_back(std::move(track));
            }
          }
        }
      }

    layer.cam_azimuth = static_cast<float>(
        std::clamp(st::json_get_double(node, "cam_azimuth", layer.cam_azimuth), -3600.0, 3600.0));
    layer.cam_elevation = static_cast<float>(std::clamp(
        st::json_get_double(node, "cam_elevation", layer.cam_elevation), -85.0, 85.0));
    layer.cam_distance = static_cast<float>(
        std::clamp(st::json_get_double(node, "cam_distance", layer.cam_distance), 0.6, 30.0));
    layer.cam_fov = static_cast<float>(
        std::clamp(st::json_get_double(node, "cam_fov", layer.cam_fov), 8.0, 110.0));
    layer.cam_dolly_from = static_cast<float>(
        std::clamp(st::json_get_double(node, "cam_dolly_from", layer.cam_dolly_from), 0.3, 4.0));
    layer.cam_dolly_to = static_cast<float>(
        std::clamp(st::json_get_double(node, "cam_dolly_to", layer.cam_dolly_to), 0.3, 4.0));

    layer.light_azimuth = static_cast<float>(std::clamp(
        st::json_get_double(node, "light_azimuth", layer.light_azimuth), -3600.0, 3600.0));
    layer.light_elevation = static_cast<float>(std::clamp(
        st::json_get_double(node, "light_elevation", layer.light_elevation), -89.0, 89.0));
    layer.light_ambient = static_cast<float>(
        std::clamp(st::json_get_double(node, "light_ambient", layer.light_ambient), 0.0, 1.0));
    layer.light_rim = static_cast<float>(
        std::clamp(st::json_get_double(node, "light_rim", layer.light_rim), 0.0, 1.0));

    // —— 字段名校验与范围校验（schema 驱动）——
    //
    // 放在解析**之后**：先让合法字段生效，再报剩下那些没人认领的键与越界值。
    // 这样即使一份脚本同时有五个拼写错误，画面仍然是“能看的部分都对”，
    // 而诊断把五个全部列出来（而不是遇到第一个就停）。
    if (diagnostics != nullptr) {
      collect_range_diagnostics(node, SchemaScope::Layer, path, *diagnostics);
      // `tracks` 的子键（属性名）不是图层字段，它自己在表里（合法），
      // 但内部属性名必须单独校验——所以这条必须在“已知键就跳过”**之前**。
      // 实测踩过：先写 `find_field_schema != nullptr → continue`，
      // 于是 `tracks` 被当成合法键放行，内部错写的属性名一个都抓不到。
      collect_track_diagnostics(node, path, *diagnostics);
      for (const auto& [key, ignored] : node.items()) {
        (void)ignored;
        if (find_field_schema(SchemaScope::Layer, key) != nullptr) continue;
        diagnostics->push_back(unknown_key_diagnostic(SchemaScope::Layer, key, path));
        // 旧的 `warnings` 通道保留：渲染侧与旧调用方还在看它。
        if (warnings != nullptr) {
          warnings->push_back(diagnostics->back().to_string());
        }
      }
    }

    layers.push_back(std::move(layer));
  }
  return layers;
}

/// 图层 → JSON（`parse_layers` 的**逆运算**）。
///
/// 为何必须存在：只有解析没有序列化，等于“脚本读进来能用、存下去就丢”。
/// 早先 `script_to_json` 漏了 `layers`，于是用 `kind=custom` 拼的画面
/// 保存一次就全没（而界面上看不出来：时间轴片段还在，只是重开变空）。
///
/// 序列化策略与场景一致：**只在非默认时写**。否则存出来的脚本充满噪声，
/// 人改不动、diff 也读不了。
[[nodiscard]] auto layer_to_json(const Layer& layer) -> st::Json {
  st::Json node = st::Json::object();
  node["kind"] = std::string(layer_kind_name(layer.kind));
  if (!layer.name.empty()) node["name"] = layer.name;
  node["x"] = layer.x;
  node["y"] = layer.y;
  node["w"] = layer.w;
  node["h"] = layer.h;
  // 颜色：只在与默认不同时写（默认白/无描边/无次色）
  if (layer.fill.r != 255 || layer.fill.g != 255 || layer.fill.b != 255 || layer.fill.a != 255) {
    node["fill"] = color_to_hex(layer.fill);
  }
  if (layer.fill_alt.a != 0) node["fill_alt"] = color_to_hex(layer.fill_alt);
  if (layer.gradient_dir != 0) node["gradient_dir"] = static_cast<std::int64_t>(layer.gradient_dir);
  if (layer.stroke.a != 0) node["stroke"] = color_to_hex(layer.stroke);
  if (layer.stroke_width > 0.0f) node["stroke_width"] = static_cast<double>(layer.stroke_width);
  if (layer.radius > 0.0f) node["radius"] = static_cast<double>(layer.radius);
  if (layer.opacity < 0.999f) node["opacity"] = static_cast<double>(layer.opacity);

  if (!layer.text.empty()) node["text"] = layer.text;
  if (layer.size != 48.0f) node["size"] = static_cast<double>(layer.size);
  if (layer.tracking > 0.0f) node["tracking"] = static_cast<double>(layer.tracking);
  if (layer.line_height != 1.45f) node["line_height"] = static_cast<double>(layer.line_height);
  if (layer.bold) node["bold"] = true;
  if (layer.align != 0) node["align"] = static_cast<std::int64_t>(layer.align);
  if (layer.color.r != 255 || layer.color.g != 255 || layer.color.b != 255) {
    node["color"] = color_to_hex(layer.color);
  }

  // —— 逐字排版 ——
  //
  // ## 为何这几个字段必须写回（而不是“反正是默认值”）
  //
  // 序列化的消费者不只是人：智能体会 `get script` 读回当前脚本、改一处、再
  // `apply_script` 写回。任何“能读不能写”的字段都会在那一次往返里**静默消失**——
  // 实测：早先 `kin_*` 九个字段、`bounce*` 四个、`path_*` 八个、`tracks`、
  // `model_group`/`model_shadow`/`model_bounce_squash` 全都不写回，
  // 于是“让文字逐字入场”这类改动会在下一轮读回时集体丢失。
  //
  // 策略仍是“只在非默认时写”（保持脚本干净），但**默认值必须与字段声明一致**。
  if (layer.kin_stagger != 0.035f) node["kin_stagger"] = static_cast<double>(layer.kin_stagger);
  if (layer.kin_dur != 0.42f) node["kin_dur"] = static_cast<double>(layer.kin_dur);
  if (layer.kin_enter != 0) node["kin_enter"] = static_cast<std::int64_t>(layer.kin_enter);
  if (layer.kin_rise != 22.0f) node["kin_rise"] = static_cast<double>(layer.kin_rise);
  if (layer.kin_float > 0.0f) node["kin_float"] = static_cast<double>(layer.kin_float);
  if (layer.kin_float > 0.0f) node["kin_float_hz"] = static_cast<double>(layer.kin_float_hz);
  if (layer.kin_caret != 0) node["kin_caret"] = static_cast<std::int64_t>(layer.kin_caret);
  if (layer.kin_caret != 0) {
    node["kin_caret_scale"] = static_cast<double>(layer.kin_caret_scale);
  }
  if (layer.kin_type_cps > 0.0f) node["kin_type_cps"] = static_cast<double>(layer.kin_type_cps);

  if (!layer.src.empty()) node["src"] = layer.src;
  if (layer.contain) node["contain"] = true;
  if (layer.dim > 0.0f) node["dim"] = static_cast<double>(layer.dim);
  if (!layer.points.empty()) node["points"] = layer.points;
  if (layer.closed) node["closed"] = true;

  if (layer.intensity != 0.5f) node["intensity"] = static_cast<double>(layer.intensity);
  if (layer.count != 60) node["count"] = static_cast<std::int64_t>(layer.count);
  if (layer.spread != 0.4f) node["spread"] = static_cast<double>(layer.spread);

  // —— 阻尼弹跳（层通用）——
  if (layer.bounce > 0.0f) node["bounce"] = static_cast<double>(layer.bounce);
  if (layer.bounce > 0.0f) {
    node["bounce_hz"] = static_cast<double>(layer.bounce_hz);
    node["bounce_damping"] = static_cast<double>(layer.bounce_damping);
  }
  if (layer.bounce_at > 0.0f) node["bounce_at"] = static_cast<double>(layer.bounce_at);

  // —— 路径动画 ——
  if (!layer.path_from_x.empty()) node["path_from_x"] = layer.path_from_x;
  if (!layer.path_from_y.empty()) node["path_from_y"] = layer.path_from_y;
  if (!layer.path_to_x.empty()) node["path_to_x"] = layer.path_to_x;
  if (!layer.path_to_y.empty()) node["path_to_y"] = layer.path_to_y;
  if (layer.path_arc != 0.0f) node["path_arc"] = static_cast<double>(layer.path_arc);
  if (!layer.path_to_x.empty() || !layer.path_to_y.empty()) {
    node["path_dur"] = static_cast<double>(layer.path_dur);
    node["path_at"] = static_cast<double>(layer.path_at);
    node["path_ease"] = static_cast<std::int64_t>(layer.path_ease);
  }

  // —— 关键帧时间线（统一写成“按属性名分组”的短形式）——
  if (!layer.tracks.empty()) {
    st::Json tracks = st::Json::object();
    for (const KeyTrack& track : layer.tracks) {
      if (track.property.empty()) continue;
      st::Json keys = st::Json::array();
      for (const KeyFrame& key : track.keys) {
        st::Json item = st::Json::object();
        item["t"] = static_cast<double>(key.time);
        item["v"] = static_cast<double>(key.value);
        if (key.ease != 1) item["ease"] = static_cast<std::int64_t>(key.ease);
        keys.push_back(std::move(item));
      }
      if (!keys.empty()) tracks[track.property] = std::move(keys);
    }
    if (!tracks.empty()) node["tracks"] = std::move(tracks);
  }

  // 三维：只在 `model3d` 里写，且只写与默认不同的项
  if (layer.kind == LayerKind::Model3d) {
    node["model_shape"] = layer.model_shape;
    node["model_color"] = color_to_hex(layer.model_color);
    if (layer.model_color_alt.a != 0) node["model_color_alt"] = color_to_hex(layer.model_color_alt);
    if (layer.model_segments != 24) {
      node["model_segments"] = static_cast<std::int64_t>(layer.model_segments);
    }
    if (layer.model_scale != 1.0f) node["model_scale"] = static_cast<double>(layer.model_scale);
    if (layer.model_spin != 0.0f) node["model_spin"] = static_cast<double>(layer.model_spin);
    if (layer.model_yaw != 0.0f) node["model_yaw"] = static_cast<double>(layer.model_yaw);
    if (layer.model_tilt != 12.0f) node["model_tilt"] = static_cast<double>(layer.model_tilt);
    if (layer.model_bob > 0.0f) node["model_bob"] = static_cast<double>(layer.model_bob);
    if (layer.model_bob > 0.0f) {
      node["model_bob_hz"] = static_cast<double>(layer.model_bob_hz);
    }
    if (layer.model_backdrop) node["model_backdrop"] = true;
    if (!layer.model_texture.empty()) node["model_texture"] = layer.model_texture;
    // 世界组与接触阴影：不写回的话，读一次脚本就“物体散到各自独立世界里”了。
    if (!layer.model_group.empty()) node["model_group"] = layer.model_group;
    if (layer.model_shadow > 0.0f) {
      node["model_shadow"] = static_cast<double>(layer.model_shadow);
      node["model_shadow_color"] = color_to_hex(layer.model_shadow_color);
    }
    if (layer.model_bounce_squash > 0.0f) {
      node["model_bounce_squash"] = static_cast<double>(layer.model_bounce_squash);
    }
    if (layer.cam_azimuth != 35.0f) node["cam_azimuth"] = static_cast<double>(layer.cam_azimuth);
    if (layer.cam_elevation != 18.0f) {
      node["cam_elevation"] = static_cast<double>(layer.cam_elevation);
    }
    if (layer.cam_distance != 3.0f) node["cam_distance"] = static_cast<double>(layer.cam_distance);
    if (layer.cam_fov != 38.0f) node["cam_fov"] = static_cast<double>(layer.cam_fov);
    if (layer.cam_dolly_from != 1.0f || layer.cam_dolly_to != 1.0f) {
      node["cam_dolly_from"] = static_cast<double>(layer.cam_dolly_from);
      node["cam_dolly_to"] = static_cast<double>(layer.cam_dolly_to);
    }
    if (layer.light_azimuth != 40.0f) {
      node["light_azimuth"] = static_cast<double>(layer.light_azimuth);
    }
    if (layer.light_elevation != 45.0f) {
      node["light_elevation"] = static_cast<double>(layer.light_elevation);
    }
    if (layer.light_ambient != 0.35f) {
      node["light_ambient"] = static_cast<double>(layer.light_ambient);
    }
    if (layer.light_rim != 0.25f) node["light_rim"] = static_cast<double>(layer.light_rim);
  }

  // 入场动画：默认是 rise/0/0.6，只在不同时写
  if (layer.enter != 2) node["enter"] = static_cast<std::int64_t>(layer.enter);
  if (layer.enter_at > 0.0f) node["enter_at"] = static_cast<double>(layer.enter_at);
  if (layer.enter_dur != 0.6f) node["enter_dur"] = static_cast<double>(layer.enter_dur);
  return node;
}

auto scene_from(const st::Json& node, std::size_t index, std::string_view asset_basis,
                std::vector<Diagnostic>* diagnostics) -> Result<Scene> {
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
    scene.layers = parse_layers(*layers, nullptr, diagnostics,
                                std::format("scenes[{}]", index));
  }
  // 脚本编排层（可选）：`layout` 是脚本源文本。
  //
  // **这里不编译**：QuickJS 运行时非线程安全，而导出开 16 路并行。
  // 编译发生在各自的渲染线程（`layout_engine()` 是 thread_local）。
  // 相对路径按 `asset_basis`（脚本所在目录）解析。
  const auto layout_script = parse_layout_script(node, asset_basis);
  scene.layout = layout_script.value_or(LayoutScript{});
  if (scene.layout.empty()) {
    // 解析失败（如相对路径读不到）——**不能静默**：
    // 静默会让“写了 layout 但完全没效果”变成一个极难查的现象
    // （实测：文件路径相对 JSON 解析失败，值被 value_or 吞掉，
    //  表现为“脚本完全没生效”，而无任何报错）。
    if (const st::Json* field = st::json_find(node, "layout"); field != nullptr) {
      ST_LOG_WARN("layout 字段存在但解析失败：{}（basis='{}'）", layout_script.error().message,
                  asset_basis);
    }
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
  // 背景颗粒：让「静止段」安静而不冻结（实测能把变化强度中位数提 30 倍）
      scene.grain = static_cast<int>(
        std::clamp(st::json_get_i64(node, "grain", scene.grain), 0LL, 12000LL));
  scene.grain_alpha = static_cast<float>(
      std::clamp(st::json_get_double(node, "grain_alpha", scene.grain_alpha), 0.0, 0.6));
  scene.wrap = st::json_get_bool(node, "wrap", scene.wrap);
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
  //
  // 缩放范围上下界差得很大（0.02 ~ 400）是故意的：
  // · 小幅度（0.95~1.08）是**镜头呼吸**，观众意识不到有运镜；
  // · 大幅度（1 → 300）是**钻穿尺度**，参考片 clearing 的整个概念靠它。
  //
  // 早先夹在 0.5~2.0，把尺度承接直接堵死（写 300 静默变成 2，画面只是
  // “稍微放大了一点”，看不出哪里错了）。上下界要能盖住两种用法。
  scene.cam_zoom_from = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_zoom_from", scene.cam_zoom_from), 0.02, 400.0));
  scene.cam_zoom_to = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_zoom_to", scene.cam_zoom_to), 0.02, 400.0));
  scene.cam_anchor_x = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_anchor_x", scene.cam_anchor_x), -1.0, 1.0));
  scene.cam_anchor_y = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_anchor_y", scene.cam_anchor_y), -1.0, 1.0));
  scene.cam_zoom_curve = st::json_get_string(node, "cam_zoom_curve", scene.cam_zoom_curve);
  scene.cam_window_start = static_cast<float>(st::json_get_double(
      node, "cam_window_start", scene.cam_window_start));
  scene.cam_window_end = static_cast<float>(
      st::json_get_double(node, "cam_window_end", scene.cam_window_end));
  scene.cam_vector = st::json_get_bool(node, "cam_vector", scene.cam_vector);
  scene.cam_rotate = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_rotate", scene.cam_rotate), -8.0, 8.0));
  scene.cam_pan_x = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_pan_x", scene.cam_pan_x), -0.5, 0.5));
  scene.cam_pan_y = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_pan_y", scene.cam_pan_y), -0.5, 0.5));
  scene.cam_shake = static_cast<float>(
      std::clamp(st::json_get_double(node, "cam_shake", scene.cam_shake), 0.0, 0.08));

  // —— 跨场承接（`carry_out`）——
  //
  // 宽松解析：读不懂就不承接（而不是报错）——承接是增强，不是必需。
  // 但**名字为空时不算承接**：验收器靠名字认身份，没名字的承接在自动化验收里
  // 等于不存在（而画面上又确实画了）——这种“看上去做了、实际没算”最难查。
  if (const st::Json* carry = st::json_find(node, "carry_out");
      carry != nullptr && carry->is_object()) {
    CarryOut out{};
    out.name = st::json_get_string(*carry, "name");
    out.into = static_cast<int>(
        st::json_get_i64(*carry, "into", static_cast<std::int64_t>(index + 1)));
    out.at = st::json_get_double(*carry, "at", 0.0);
    out.dur = std::clamp(st::json_get_double(*carry, "dur", 0.5), 0.01, 10.0);
    out.curve = st::json_get_string(*carry, "curve", out.curve);
    out.rotate = static_cast<float>(st::json_get_double(*carry, "rotate", 0.0));
    out.fade_out =
        static_cast<float>(std::clamp(st::json_get_double(*carry, "fade_out", 0.0), 0.0, 1.0));
    out.fill = color_field(*carry, "fill", out.fill);
    out.stroke = color_field(*carry, "stroke", out.stroke);
    out.stroke_width = static_cast<float>(
        std::clamp(st::json_get_double(*carry, "stroke_width", 0.0), 0.0, 40.0));
    out.text = st::json_get_string(*carry, "text");
    out.text_color = color_field(*carry, "text_color", out.text_color);
    out.text_size = static_cast<float>(
        std::clamp(st::json_get_double(*carry, "text_size", 48.0), 4.0, 400.0));
    // 矩形：`x/y/w/h` 与图层同一口径（数字 = 设计像素，字符串 = 画面比例）。
    const auto read_rect = [](const st::Json* rect, CarryRect fallback) -> CarryRect {
      if (rect == nullptr || !rect->is_object()) return fallback;
      const auto dim = [rect](std::string_view key, std::string fallback_value) -> std::string {
        const st::Json* field = st::json_find(*rect, key);
        if (field == nullptr) return fallback_value;
        if (field->is_string()) return field->get<std::string>();
        if (field->is_number()) return std::format("{:.4f}", field->get<double>());
        return fallback_value;
      };
      CarryRect box{};
      box.x = dim("x", fallback.x);
      box.y = dim("y", fallback.y);
      box.w = dim("w", fallback.w);
      box.h = dim("h", fallback.h);
      box.radius = static_cast<float>(
          std::clamp(st::json_get_double(*rect, "radius", fallback.radius), 0.0, 4000.0));
      return box;
    };
    out.from = read_rect(st::json_find(*carry, "from"), out.from);
    out.to = read_rect(st::json_find(*carry, "to"), out.to);
    if (!out.name.empty()) scene.carry = std::move(out);
  }

  // 场景层字段名校验与范围校验（与图层同一套机制：表驱动，不在多处手写白名单）。
  if (diagnostics != nullptr) {
    const std::string path = std::format("scenes[{}]", index);
    collect_range_diagnostics(node, SchemaScope::Scene, path, *diagnostics);
    for (const auto& [key, ignored] : node.items()) {
      (void)ignored;
      if (find_field_schema(SchemaScope::Scene, key) != nullptr) continue;
      diagnostics->push_back(unknown_key_diagnostic(SchemaScope::Scene, key, path));
    }
  }
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

auto Script::has_errors() const -> bool {
  for (const Diagnostic& diag : diagnostics) {
    if (diag.is_error) return true;
  }
  return false;
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

auto parse_script(std::string_view json, std::string_view asset_basis) -> Result<Script> {
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
  // 脚本顶层：先在解析前校验字段名（顶层没有“夹取”语义，所以只查未知键）。
  for (const auto& [key, ignored] : root.items()) {
    (void)ignored;
    if (find_field_schema(SchemaScope::Script, key) != nullptr) continue;
    if (key == "audio") continue;
    script.diagnostics.push_back(unknown_key_diagnostic(SchemaScope::Script, key, "（顶层）"));
  }
  if (const st::Json* audio = st::json_find(root, "audio");
      audio != nullptr && audio->is_object()) {
    for (const auto& [key, ignored] : audio->items()) {
      (void)ignored;
      if (find_field_schema(SchemaScope::Audio, key) != nullptr) continue;
      script.diagnostics.push_back(unknown_key_diagnostic(SchemaScope::Audio, key, "audio"));
    }
    collect_range_diagnostics(*audio, SchemaScope::Audio, "audio", script.diagnostics);
  }
  std::size_t index = 0;
  for (const st::Json& node : *scenes) {
    if (!node.is_object()) {
      return st::unexpected(st::ErrorCode::Parse, std::format("scenes[{}] 不是对象", index));
    }
    const auto scene = scene_from(node, index, asset_basis, &script.diagnostics);
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
    // 背景颗粒：让静止段“安静而不冻结”——不写回等于每轮读回都变回纯色背景。
    if (scene.grain > 0) {
      node["grain"] = static_cast<std::int64_t>(scene.grain);
      node["grain_alpha"] = static_cast<double>(scene.grain_alpha);
    }
    // `wrap` 默认 true，只在关闭时写（纯画面镜头会关掉它）。
    if (!scene.wrap) node["wrap"] = false;
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
                            scene.cam_pan_y != 0.0f || scene.cam_shake > 0.0001f ||
                            scene.cam_anchor_x >= 0.0f || scene.cam_anchor_y >= 0.0f;
  if (camera_moves) {
    node["cam_zoom_from"] = static_cast<double>(scene.cam_zoom_from);
    node["cam_zoom_to"] = static_cast<double>(scene.cam_zoom_to);
    if (scene.cam_anchor_x >= 0.0f) node["cam_anchor_x"] = static_cast<double>(scene.cam_anchor_x);
    if (scene.cam_anchor_y >= 0.0f) node["cam_anchor_y"] = static_cast<double>(scene.cam_anchor_y);
    if (scene.cam_zoom_curve != "linear") node["cam_zoom_curve"] = scene.cam_zoom_curve;
    if (scene.cam_window_end > scene.cam_window_start) {
      node["cam_window_start"] = static_cast<double>(scene.cam_window_start);
      node["cam_window_end"] = static_cast<double>(scene.cam_window_end);
    }
    if (scene.cam_vector) node["cam_vector"] = true;
    if (scene.cam_rotate != 0.0f) node["cam_rotate"] = static_cast<double>(scene.cam_rotate);
    if (scene.cam_pan_x != 0.0f) node["cam_pan_x"] = static_cast<double>(scene.cam_pan_x);
    if (scene.cam_pan_y != 0.0f) node["cam_pan_y"] = static_cast<double>(scene.cam_pan_y);
    if (scene.cam_shake > 0.0001f) node["cam_shake"] = static_cast<double>(scene.cam_shake);
  }
    // 图层：**必须写回**。
    //
    // 早先这里漏了 `layers`（只读了不写），于是“保存脚本”会把用 `kind=custom`
    // 拼出来的画面**全部丢掉**，而界面上看不出来（时间轴片段还在，只是重开就空了）。
    // 三维图层也是图层，所以它跟着这条一起修。
    if (!scene.layers.empty()) {
      st::Json layers = st::Json::array();
      for (const Layer& layer : scene.layers) {
        layers.push_back(layer_to_json(layer));
      }
      node["layers"] = std::move(layers);
    }
    // 本镜页脚（空则用脚本级）。
    if (!scene.footer_note.empty()) node["footer_note"] = scene.footer_note;
    // 跨场承接：不写回的话，读一次脚本承接就丢了。
    //
    // 而丢承接的后果与丢图层不同：画面仍能出（只是退化成幻灯片），
    // 所以“看起来正常”——靠肉眼几乎发现不了。
    if (!scene.carry.name.empty()) {
      const CarryOut& carry = scene.carry;
      st::Json out = st::Json::object();
      out["name"] = carry.name;
      out["into"] = static_cast<std::int64_t>(carry.into);
      out["at"] = carry.at;
      out["dur"] = carry.dur;
      const auto write_rect = [](const CarryRect& rect) -> st::Json {
        st::Json box = st::Json::object();
        box["x"] = rect.x;
        box["y"] = rect.y;
        box["w"] = rect.w;
        box["h"] = rect.h;
        if (rect.radius > 0.0f) box["radius"] = static_cast<double>(rect.radius);
        return box;
      };
      out["from"] = write_rect(carry.from);
      out["to"] = write_rect(carry.to);
      if (carry.fill.a != 0) out["fill"] = color_to_hex(carry.fill);
      if (carry.stroke.a != 0) {
        out["stroke"] = color_to_hex(carry.stroke);
        out["stroke_width"] = static_cast<double>(carry.stroke_width);
      }
      if (!carry.text.empty()) {
        out["text"] = carry.text;
        out["text_color"] = color_to_hex(carry.text_color);
        if (carry.text_size != 48.0f) out["text_size"] = static_cast<double>(carry.text_size);
      }
      if (!carry.curve.empty()) out["curve"] = carry.curve;
          if (carry.rotate != 0.0f) out["rotate"] = static_cast<double>(carry.rotate);
    if (carry.fade_out > 0.0f) out["fade_out"] = static_cast<double>(carry.fade_out);
    node["carry_out"] = std::move(out);
    }
    // JS 编排层：写回**原写法**（文件路径字符串，或内联 `{source, filename, timeout_ms}`）。
    //
    // 不写回的话，读一次脚本就会把“每帧自己算的动作”整段丢掉，而静态帧看不出来。
    if (!scene.layout.source.empty()) {
      if (scene.layout.from_file) {
        node["layout"] = scene.layout.raw_field;
      } else {
        st::Json inline_layout = st::Json::object();
        inline_layout["source"] = scene.layout.raw_field;
        if (scene.layout.timeout_ms != 400) inline_layout["timeout_ms"] = scene.layout.timeout_ms;
        node["layout"] = std::move(inline_layout);
      }
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
