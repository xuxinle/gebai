#include "st/raster/gltf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <numeric>

#include "st/core/base64.hpp"
#include "st/core/fs.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/color_pipeline.hpp"
#include "st/math/quaternion.hpp"
#include "st/raster/mesh_ops.hpp"

namespace st::raster {

namespace {

// glTF 2.0 的常量（来自规范，不是本地约定）
constexpr std::uint32_t kMagicGlb = 0x46546C67U;      // "glTF" little-endian
constexpr std::uint32_t kChunkJson = 0x4E4F534AU;     // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942U;      // "BIN\0"
constexpr std::uint32_t kGlbVersion = 2U;


/// 从字节串读一个小端 u32。
///
/// ## 为什么用手写的位拼装而不是 `bit_cast`
///
/// 需要的是**小端**语义，而 `bit_cast` 得到的是宿主字节序（x86/ARM 恰好都是小端，
/// 但"恰好"不是契约）。这里用移位拼装，字节序由代码写死，跑在大端机器上也对。
[[nodiscard]] auto read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
    -> std::uint32_t {
  if (offset + 4U > bytes.size()) return 0;
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

/// 把一个 u32 写成小端 4 字节追加到 `out`。
void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

/// 把一段字节当 UTF-8 文本交给 JSON 解析器（复制一次：解析器要 `string_view`，
/// 而 GLB 的 JSON chunk 一般只有几 KB，复制成本可忽略）。
[[nodiscard]] auto bytes_as_text(std::span<const std::uint8_t> bytes, std::size_t offset,
                                std::size_t length) -> std::string {
  if (offset + length > bytes.size()) return {};
  std::string text;
  text.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    text.push_back(static_cast<char>(bytes[offset + i]));
  }
  return text;
}

/// glTF 组件类型（规范表 3）。
enum class ComponentType : std::uint32_t {
  Byte = 5120,
  UnsignedByte = 5121,
  Short = 5122,
  UnsignedShort = 5123,
  UnsignedInt = 5125,
  Float = 5126,
};

[[nodiscard]] auto component_size(std::uint32_t type) noexcept -> std::size_t {
  switch (static_cast<ComponentType>(type)) {
    case ComponentType::Byte:
    case ComponentType::UnsignedByte: return 1;
    case ComponentType::Short:
    case ComponentType::UnsignedShort: return 2;
    case ComponentType::UnsignedInt:
    case ComponentType::Float: return 4;
  }
  return 0;
}

/// accessor 的元素分量数（`SCALAR`=1、`VEC2`=2、`VEC3`=3、`VEC4`=4、`MAT4`=16）。
[[nodiscard]] auto type_components(std::string_view type) noexcept -> std::size_t {
  if (type == "SCALAR") return 1;
  if (type == "VEC2") return 2;
  if (type == "VEC3") return 3;
  if (type == "VEC4") return 4;
  if (type == "MAT2") return 4;
  if (type == "MAT3") return 9;
  if (type == "MAT4") return 16;
  return 0;
}

/// 从二进制里读一个标量（按组件类型）并转成 float。
[[nodiscard]] auto read_component(const std::uint8_t* data, std::uint32_t type) noexcept -> float {
  switch (static_cast<ComponentType>(type)) {
    case ComponentType::Float: {
      float value = 0.0f;
      std::memcpy(&value, data, sizeof(float));
      return value;
    }
    case ComponentType::UnsignedByte: return static_cast<float>(*data);
    case ComponentType::Byte: return static_cast<float>(static_cast<std::int8_t>(*data));
    case ComponentType::UnsignedShort: {
      std::uint16_t value = 0;
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(value);
    }
    case ComponentType::Short: {
      std::int16_t value = 0;
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(value);
    }
    case ComponentType::UnsignedInt: {
      std::uint32_t value = 0;
      std::memcpy(&value, data, sizeof(value));
      return static_cast<float>(value);
    }
  }
  return 0.0f;
}

/// 读一个整数（索引专用；浮点型索引不合法，按 0 处理）。
[[nodiscard]] auto read_index(const std::uint8_t* data, std::uint32_t type) noexcept
    -> std::uint32_t {
  switch (static_cast<ComponentType>(type)) {
    case ComponentType::UnsignedByte: return *data;
    case ComponentType::UnsignedShort: {
      std::uint16_t value = 0;
      std::memcpy(&value, data, sizeof(value));
      return value;
    }
    case ComponentType::UnsignedInt: {
      std::uint32_t value = 0;
      std::memcpy(&value, data, sizeof(value));
      return value;
    }
    default: return 0;
  }
}

/// 解析一个 `data:` URI（base64）。
[[nodiscard]] auto decode_data_uri(std::string_view uri) -> Result<std::vector<std::uint8_t>> {
  const std::size_t comma = uri.find(',');
  if (comma == std::string_view::npos) {
    return st::unexpected(ErrorCode::Parse, "data URI 缺少逗号（格式应为 data:...;base64,<数据>）");
  }
  const std::string_view payload = uri.substr(comma + 1U);
  return st::base64_decode(payload);
}

/// 取一个 accessor 指向的**原始字节窗口**（含 offset / stride 处理前的线性视图）。
struct AccessorView {
  const std::uint8_t* data{nullptr};
  std::size_t count{0};
  std::size_t components{0};
  std::size_t component_bytes{0};
  std::size_t stride{0};   ///< 元素间字节跨度（交错缓冲 > 组件总宽）
  std::uint32_t type{0};

  [[nodiscard]] auto element_bytes() const noexcept -> std::size_t {
    return components * component_bytes;
  }
};

struct DocContext {
  const Json* json{nullptr};
  std::vector<std::vector<std::uint8_t>> buffers{};
  std::string base_dir{};
};

/// 解析 `buffers[]`（外部 .bin / data URI / GLB 的 BIN 块）。
auto load_buffers(DocContext& ctx) -> Status {
  const Json* buffers = json_find(*ctx.json, "buffers");
  if (buffers == nullptr || !buffers->is_array()) return st::ok();
  std::size_t index = 0;
  for (const Json& buffer : *buffers) {
    const std::string path = "buffers[" + std::to_string(index) + "]";
    ++index;
    const std::string uri = json_get_string(buffer, "uri", "");
    if (uri.empty()) {
      // 没有 uri 的 buffer 只允许出现在 GLB 里（由调用方预先塞好）
      if (ctx.buffers.size() <= index - 1U) ctx.buffers.resize(index);
      continue;
    }
    if (uri.rfind("data:", 0) == 0) {
      auto decoded = decode_data_uri(uri);
      if (!decoded) return st::forward_error(decoded.error());
      ctx.buffers.resize(index);
      ctx.buffers[index - 1U] = std::move(*decoded);
      continue;
    }
    // 外部文件：相对 .gltf 所在目录
    const std::string resolved = st::fs::join(ctx.base_dir, uri);
    auto bytes = st::fs::read_bytes(resolved);
    if (!bytes.has_value()) {
      return st::unexpected(bytes.error().code,
                            std::format("{}.uri：读不到外部缓冲 \"{}\"（{}）", path, resolved,
                                        bytes.error().message));
    }
    ctx.buffers.resize(index);
    ctx.buffers[index - 1U] = std::move(*bytes);
  }
  return st::ok();
}

/// 解析一个 accessor 到 `AccessorView`。
[[nodiscard]] auto accessor_view(const DocContext& ctx, int accessor_index, std::string_view where)
    -> Result<AccessorView> {
  const Json* accessors = json_find(*ctx.json, "accessors");
  if (accessors == nullptr || !accessors->is_array()) {
    return st::unexpected(ErrorCode::Parse, std::format("{}：文件没有 accessors 数组", where));
  }
  if (accessor_index < 0 || static_cast<std::size_t>(accessor_index) >= accessors->size()) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：accessor 索引 {} 越界（共 {} 个）", where, accessor_index,
                                      accessors->size()));
  }
  const Json& accessor = (*accessors)[static_cast<std::size_t>(accessor_index)];
  const std::string base = std::format("accessors[{}]", accessor_index);

  if (json_find(accessor, "sparse") != nullptr) {
    return st::unexpected(ErrorCode::Unsupported,
                          std::format("{}：不支持 sparse accessor（{}.sparse）", where, base));
  }
  const int buffer_view_index = static_cast<int>(json_get_i64(accessor, "bufferView", -1));
  if (buffer_view_index < 0) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：{}.bufferView 缺失（无 bufferView 的 accessor 需要 sparse）",
                                      where, base));
  }
  const Json* views = json_find(*ctx.json, "bufferViews");
  if (views == nullptr || !views->is_array() ||
      static_cast<std::size_t>(buffer_view_index) >= views->size()) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：{}.bufferView = {} 越界", where, base, buffer_view_index));
  }
  const Json& view = (*views)[static_cast<std::size_t>(buffer_view_index)];
  const int buffer_index = static_cast<int>(json_get_i64(view, "buffer", 0));
  if (buffer_index < 0 || static_cast<std::size_t>(buffer_index) >= ctx.buffers.size()) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：bufferView 指向 buffer {}，但只加载到 {} 个", where,
                                      buffer_index, ctx.buffers.size()));
  }
  const std::vector<std::uint8_t>& buffer = ctx.buffers[static_cast<std::size_t>(buffer_index)];
  const std::size_t view_offset = static_cast<std::size_t>(json_get_i64(view, "byteOffset", 0));
  const std::size_t view_length =
      static_cast<std::size_t>(json_get_i64(view, "byteLength", 0));
  if (view_offset + view_length > buffer.size()) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：bufferView 越界（{} + {} > 缓冲 {} 字节）", where,
                                      view_offset, view_length, buffer.size()));
  }

  AccessorView out{};
  out.type = static_cast<std::uint32_t>(json_get_i64(accessor, "componentType", 5126));
  out.components = type_components(json_get_string(accessor, "type", "SCALAR"));
  out.component_bytes = component_size(out.type);
  out.count = static_cast<std::size_t>(json_get_i64(accessor, "count", 0));
  const std::size_t accessor_offset =
      static_cast<std::size_t>(json_get_i64(accessor, "byteOffset", 0));
  const std::size_t view_stride = static_cast<std::size_t>(json_get_i64(view, "byteStride", 0));
  out.stride = view_stride != 0 ? view_stride : out.components * out.component_bytes;
  if (out.components == 0 || out.component_bytes == 0) {
    return st::unexpected(ErrorCode::Unsupported,
                          std::format("{}：{}.type/componentType 非法（type={}, componentType={}）", where,
                                      base, json_get_string(accessor, "type", "?"), out.type));
  }
  const std::size_t total = out.count == 0 ? 0 : (out.count - 1U) * out.stride + out.element_bytes();
  if (accessor_offset + total > view_length) {
    return st::unexpected(
        ErrorCode::Parse,
        std::format("{}：accessor 需要 {} 字节，但 bufferView 只有 {} 字节（count={}, stride={}）",
                    where, accessor_offset + total, view_length, out.count, out.stride));
  }
  out.data = buffer.data() + view_offset + accessor_offset;
  return out;
}

/// 读一个 VEC3 accessor 成 Vec3 列表。
[[nodiscard]] auto read_vec3(const AccessorView& view, std::string_view where)
    -> Result<std::vector<math::Vec3>> {
  if (view.components < 3U) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：需要 VEC3，实际是 {} 个分量的类型", where, view.components));
  }
  std::vector<math::Vec3> out;
  out.reserve(view.count);
  for (std::size_t i = 0; i < view.count; ++i) {
    const std::uint8_t* element = view.data + i * view.stride;
    out.push_back(math::Vec3{read_component(element + 0U * view.component_bytes, view.type),
                             read_component(element + 1U * view.component_bytes, view.type),
                             read_component(element + 2U * view.component_bytes, view.type)});
  }
  return out;
}

/// 读一个 VEC2 accessor。
[[nodiscard]] auto read_vec2(const AccessorView& view, std::string_view where)
    -> Result<std::vector<std::pair<float, float>>> {
  if (view.components < 2U) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("{}：需要 VEC2，实际是 {} 个分量", where, view.components));
  }
  std::vector<std::pair<float, float>> out;
  out.reserve(view.count);
  for (std::size_t i = 0; i < view.count; ++i) {
    const std::uint8_t* element = view.data + i * view.stride;
    out.emplace_back(read_component(element, view.type),
                     read_component(element + view.component_bytes, view.type));
  }
  return out;
}

/// 读 indices（标量整数）。
[[nodiscard]] auto read_indices(const AccessorView& view) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  out.reserve(view.count);
  for (std::size_t i = 0; i < view.count; ++i) {
    out.push_back(read_index(view.data + i * view.stride, view.type));
  }
  return out;
}

/// 节点变换（matrix 优先；否则 T·R·S）。
[[nodiscard]] auto node_matrix(const Json& node) -> math::Mat4 {
  const Json* matrix = json_find(node, "matrix");
  if (matrix != nullptr && matrix->is_array() && matrix->size() >= 16U) {
    math::Mat4 out = math::Mat4::identity();
    // glTF 的 matrix 是**列主序**（与霜天 Mat4 相同），按列读即可。
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        const Json& value = (*matrix)[static_cast<std::size_t>(column * 4 + row)];
        out.set(row, column,
                value.is_number() ? static_cast<float>(value.get<double>()) : 0.0f);
      }
    }
    return out;
  }
  math::Vec3 translation{0.0f, 0.0f, 0.0f};
  math::Vec3 scale{1.0f, 1.0f, 1.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // x, y, z, w
  if (const Json* t = json_find(node, "translation"); t != nullptr && t->is_array() && t->size() >= 3U) {
    translation = math::Vec3{static_cast<float>((*t)[0].get<double>()),
                             static_cast<float>((*t)[1].get<double>()),
                             static_cast<float>((*t)[2].get<double>())};
  }
  if (const Json* s = json_find(node, "scale"); s != nullptr && s->is_array() && s->size() >= 3U) {
    scale = math::Vec3{static_cast<float>((*s)[0].get<double>()),
                       static_cast<float>((*s)[1].get<double>()),
                       static_cast<float>((*s)[2].get<double>())};
  }
  if (const Json* r = json_find(node, "rotation"); r != nullptr && r->is_array() && r->size() >= 4U) {
    for (std::size_t i = 0; i < 4U; ++i) rotation[i] = static_cast<float>((*r)[i].get<double>());
  }
  // 四元数 → 旋转矩阵（glTF 存 x,y,z,w；右手系，与霜天一致）
  const float x = rotation[0];
  const float y = rotation[1];
  const float z = rotation[2];
  const float w = rotation[3];
  math::Mat4 rotation_m = math::Mat4::identity();
  rotation_m.set(0, 0, 1.0f - 2.0f * (y * y + z * z));
  rotation_m.set(0, 1, 2.0f * (x * y - z * w));
  rotation_m.set(0, 2, 2.0f * (x * z + y * w));
  rotation_m.set(1, 0, 2.0f * (x * y + z * w));
  rotation_m.set(1, 1, 1.0f - 2.0f * (x * x + z * z));
  rotation_m.set(1, 2, 2.0f * (y * z - x * w));
  rotation_m.set(2, 0, 2.0f * (x * z - y * w));
  rotation_m.set(2, 1, 2.0f * (y * z + x * w));
  rotation_m.set(2, 2, 1.0f - 2.0f * (x * x + y * y));
  return math::Mat4::translation(translation) * rotation_m * math::Mat4::scaling(scale);
}

auto read_number(const Json& node, const char* key, float fallback) -> float {
  const Json* value = json_find(node, key);
  if (value == nullptr || !value->is_number()) return fallback;
  return static_cast<float>(value->get<double>());
}

auto read_vec3_or(const Json& node, const char* key, math::Vec3 fallback) -> math::Vec3 {
  const Json* value = json_find(node, key);
  if (value == nullptr || !value->is_array() || value->size() < 3U) return fallback;
  return math::Vec3{static_cast<float>((*value)[0].get<double>()),
                    static_cast<float>((*value)[1].get<double>()),
                    static_cast<float>((*value)[2].get<double>())};
}

/// `[r,g,b,a]`（线性，0..1）→ `math::Color`（sRGB 字节）。
[[nodiscard]] auto linear_rgba_to_color(math::Vec3 linear, float alpha) -> math::Color {
  const math::Vec3 srgb = linear_to_srgb(linear);
  const auto byte = [](float value) {
    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
  };
  const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
  return math::Color{byte(srgb.x), byte(srgb.y), byte(srgb.z),
                     static_cast<std::uint8_t>(std::lround(a * 255.0f))};
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// 读
// ────────────────────────────────────────────────────────────────────────────

auto gltf_read_impl(std::span<const std::uint8_t> bytes, std::string_view base_dir)
    -> Result<GltfScene> {
  if (bytes.size() < 12U) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("glTF 文件太短（{} 字节，至少要 12 字节头）", bytes.size()));
  }
  const std::uint32_t magic = read_u32_le(bytes, 0);

  Json document;
  std::vector<std::uint8_t> glb_binary;
  if (magic == kMagicGlb) {
    // GLB 头：magic(4) version(4) length(4)，随后是 chunk：length(4) type(4) data
    const std::uint32_t version = read_u32_le(bytes, 4);
    const std::uint32_t total = read_u32_le(bytes, 8);
    if (version != kGlbVersion) {
      return st::unexpected(ErrorCode::Unsupported,
                            std::format("GLB 版本 {} 不支持（只支持 2）", version));
    }
    if (total > bytes.size()) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("GLB 头声明 {} 字节，实际只有 {} 字节", total, bytes.size()));
    }
    std::size_t offset = 12U;
    bool got_json = false;
    while (offset + 8U <= total) {
      const std::uint32_t chunk_length = read_u32_le(bytes, offset);
      const std::uint32_t chunk_type = read_u32_le(bytes, offset + 4U);
      offset += 8U;
      if (offset + chunk_length > total) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("GLB chunk 越界（偏移 {} + {} > {}）", offset, chunk_length,
                                          total));
      }
      if (chunk_type == kChunkJson) {
        const std::string text = bytes_as_text(bytes, offset, chunk_length);
        auto parsed = st::json_parse(text);
        if (!parsed.has_value()) return st::forward_error(parsed.error());
        document = std::move(*parsed);
        got_json = true;
      } else if (chunk_type == kChunkBin) {
        glb_binary.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + chunk_length));
      }
      offset += chunk_length;
    }
    if (!got_json) return st::unexpected(ErrorCode::Parse, "GLB 里没有 JSON chunk");
  } else {
    auto parsed = st::json_parse(bytes_as_text(bytes, 0U, bytes.size()));
    if (!parsed.has_value()) {
      return st::unexpected(ErrorCode::Parse,
                            std::format("既不是 GLB（魔数不对），也不是合法 JSON：{}",
                                        parsed.error().message));
    }
    document = std::move(*parsed);
  }

  if (!document.is_object()) {
    return st::unexpected(ErrorCode::Parse, "glTF 顶层需要对象");
  }
  const Json* asset = json_find(document, "asset");
  if (asset == nullptr) {
    return st::unexpected(ErrorCode::Parse, "asset 缺失（glTF 2.0 要求 asset.version）");
  }
  const std::string version = json_get_string(*asset, "version", "");
  if (!version.empty() && version.rfind("2.", 0) != 0) {
    return st::unexpected(ErrorCode::Unsupported,
                          std::format("asset.version = \"{}\"，只支持 2.x", version));
  }

  DocContext ctx{};
  ctx.json = &document;
  ctx.base_dir = std::string(base_dir);
  // GLB 的 BIN 块是 buffers[0]
  const Json* buffers = json_find(document, "buffers");
  const std::size_t buffer_count = (buffers != nullptr && buffers->is_array()) ? buffers->size() : 0;
  if (buffer_count > 0U) ctx.buffers.resize(buffer_count);
  if (!glb_binary.empty()) {
    if (ctx.buffers.empty()) ctx.buffers.resize(1);
    ctx.buffers[0] = std::move(glb_binary);
  }

  {
    // 外部 `.bin` / data URI 必须在解析几何**之前**读完：accessor 解析依赖缓冲内容。
    // （第一版把这一步放在末尾，结果"外部 .bin 的模型全是空网格"——文件本身合法，
    //   只是解在加载之前；这类顺序依赖只能靠端到端往返测试拦住。）
    const auto loaded = load_buffers(ctx);
    if (!loaded) return st::forward_error(loaded.error());
  }

  GltfScene scene{};
  // 材质
  if (const Json* materials = json_find(document, "materials");
      materials != nullptr && materials->is_array()) {
    std::size_t index = 0;
    for (const Json& material : *materials) {
      GltfScene::MaterialSlot slot{};
      slot.name = json_get_string(material, "name", std::format("material{}", index));
      const Json* pbr = json_find(material, "pbrMetallicRoughness");
      math::Vec3 base{1.0f, 1.0f, 1.0f};
      float alpha = 1.0f;
      if (pbr != nullptr && pbr->is_object()) {
        const Json* factor = json_find(*pbr, "baseColorFactor");
        if (factor != nullptr && factor->is_array() && factor->size() >= 3U) {
          base = math::Vec3{static_cast<float>((*factor)[0].get<double>()),
                            static_cast<float>((*factor)[1].get<double>()),
                            static_cast<float>((*factor)[2].get<double>())};
          if (factor->size() >= 4U) alpha = static_cast<float>((*factor)[3].get<double>());
        }
        slot.metallic = read_number(*pbr, "metallicFactor", 1.0f);
        slot.roughness = read_number(*pbr, "roughnessFactor", 1.0f);
      }
      slot.base_color = linear_rgba_to_color(base, alpha);
      slot.opacity = alpha;
      const math::Vec3 emissive = read_vec3_or(material, "emissiveFactor", math::Vec3{});
      slot.emissive_color = emissive;
      slot.emissive = std::max({emissive.x, emissive.y, emissive.z});
      scene.materials.push_back(std::move(slot));
      ++index;
    }
  }

  // 网格：每个 glTF mesh 的 primitives 合成一个 `Mesh`（当前渲染器一网格一材质）
  if (const Json* meshes = json_find(document, "meshes");
      meshes != nullptr && meshes->is_array()) {
    std::size_t mesh_index = 0;
    for (const Json& mesh : *meshes) {
      const std::string mesh_path = "meshes[" + std::to_string(mesh_index) + "]";
      ++mesh_index;
      const Json* primitives = json_find(mesh, "primitives");
      if (primitives == nullptr || !primitives->is_array()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("{}：缺少 primitives 数组", mesh_path));
      }
      std::vector<Mesh> parts;
      std::size_t primitive_index = 0;
      for (const Json& primitive : *primitives) {
        const std::string where =
            std::format("{}.primitives[{}]", mesh_path, primitive_index);
        ++primitive_index;
        const int mode = static_cast<int>(json_get_i64(primitive, "mode", 4));
        if (mode != 4) {
          // 点/线/条带不支持——但**报错说清楚**，不静默跳过整份文件
          return st::unexpected(ErrorCode::Unsupported,
                                std::format("{}：只支持三角形图元（mode=4），收到 mode={}", where, mode));
        }
        const Json* attributes = json_find(primitive, "attributes");
        if (attributes == nullptr || !attributes->is_object()) {
          return st::unexpected(ErrorCode::Parse, std::format("{}：缺少 attributes", where));
        }
        const int position_index = static_cast<int>(json_get_i64(*attributes, "POSITION", -1));
        if (position_index < 0) {
          return st::unexpected(ErrorCode::Parse,
                                std::format("{}：attributes.POSITION 缺失（三角形必须有位置）", where));
        }
        auto positions = [&]() -> Result<std::vector<math::Vec3>> {
          auto view = accessor_view(ctx, position_index, where + ".attributes.POSITION");
          if (!view) return st::forward_error(view.error());
          return read_vec3(*view, where + ".attributes.POSITION");
        }();
        if (!positions) return st::forward_error(positions.error());

        std::vector<math::Vec3> normals;
        if (const Json* normal_node = json_find(*attributes, "NORMAL"); normal_node != nullptr &&
            normal_node->is_number()) {
          auto view = accessor_view(ctx, static_cast<int>(normal_node->get<std::int64_t>()),
                                    where + ".attributes.NORMAL");
          if (view) {
            auto values = read_vec3(*view, where + ".attributes.NORMAL");
            if (values) normals = std::move(*values);
          }
        }
        std::vector<std::pair<float, float>> uvs;
        if (const Json* uv_node = json_find(*attributes, "TEXCOORD_0");
            uv_node != nullptr && uv_node->is_number()) {
          auto view = accessor_view(ctx, static_cast<int>(uv_node->get<std::int64_t>()),
                                    where + ".attributes.TEXCOORD_0");
          if (view) {
            auto values = read_vec2(*view, where + ".attributes.TEXCOORD_0");
            if (values) uvs = std::move(*values);
          }
        }

        Mesh mesh_out{};
        const math::Vec3 color{1.0f, 1.0f, 1.0f};
        if (normals.size() != positions->size()) {
          // 没有法线：先写位置，随后统一算法线（否则整块是黑的——法线为 0 时点积恒为 0）
          for (const math::Vec3& position : *positions) {
            push_vertex(mesh_out.vertices, position, math::Vec3{0.0f, 0.0f, 0.0f}, color);
          }
        } else {
          for (std::size_t i = 0; i < positions->size(); ++i) {
            push_vertex(mesh_out.vertices, (*positions)[i], normals[i].normalized(), color);
          }
        }
        if (uvs.size() == positions->size()) {
          for (const auto& [u, v] : uvs) {
            mesh_out.uvs.push_back(u);
            mesh_out.uvs.push_back(v);
          }
        }

        const int indices_index = static_cast<int>(json_get_i64(primitive, "indices", -1));
        if (indices_index >= 0) {
          auto view = accessor_view(ctx, indices_index, where + ".indices");
          if (!view) return st::forward_error(view.error());
          mesh_out.indices = read_indices(*view);
          const auto vertex_total = static_cast<std::uint32_t>(mesh_out.vertex_count());
          for (std::uint32_t& index : mesh_out.indices) {
            if (index >= vertex_total) index = 0;   // 越界索引按 0（不静默丢掉整个三角形）
          }
        } else {
          // 无索引：顶点按顺序三个一组
          for (std::uint32_t i = 0; i + 2U < mesh_out.vertex_count(); i += 3U) {
            mesh_out.indices.insert(mesh_out.indices.end(), {i, i + 1U, i + 2U});
          }
        }
        if (normals.size() != mesh_out.vertex_count()) {
          // 缺法线 → 平滑重算（按位置平均，避免接缝出现硬线；
          // 也保证 `CullMode::Back` 下的绕序与法线一致——glTF 规定逆时针为正面，同本框架）
          mesh_out = recompute_normals(mesh_out, NormalShading::Smooth);
        }
        if (!mesh_out.is_empty()) parts.push_back(std::move(mesh_out));
      }
      if (parts.empty()) {
        scene.meshes.push_back(Mesh{});
      } else if (parts.size() == 1U) {
        scene.meshes.push_back(std::move(parts.front()));
      } else {
        scene.meshes.push_back(merge(std::span<const Mesh>(parts.data(), parts.size())));
      }
    }
  }

  // 节点（含层级展开：父变换逐级乘下来）
  if (const Json* nodes = json_find(document, "nodes"); nodes != nullptr && nodes->is_array()) {
    // 先建"索引 → 子节点列表"，再用栈做深度遍历（避免递归爆栈 + 便于累乘父变换）
    std::vector<std::vector<int>> children(nodes->size());
    std::vector<int> parent(nodes->size(), -1);
    std::size_t node_index = 0;
    for (const Json& node : *nodes) {
      if (const Json* list = json_find(node, "children"); list != nullptr && list->is_array()) {
        for (const Json& child : *list) {
          const int child_index = static_cast<int>(child.get<std::int64_t>());
          if (child_index >= 0 && static_cast<std::size_t>(child_index) < nodes->size()) {
            children[node_index].push_back(child_index);
            parent[static_cast<std::size_t>(child_index)] = static_cast<int>(node_index);
          }
        }
      }
      ++node_index;
    }
    std::vector<math::Mat4> world(nodes->size(), math::Mat4::identity());
    std::vector<bool> computed(nodes->size(), false);
    const auto world_of = [&](int index, const auto& self) -> math::Mat4 {
      if (computed[static_cast<std::size_t>(index)]) return world[static_cast<std::size_t>(index)];
      const math::Mat4 local = node_matrix((*nodes)[static_cast<std::size_t>(index)]);
      const int up = parent[static_cast<std::size_t>(index)];
      const math::Mat4 out = up < 0 ? local : self(up, self) * local;
      world[static_cast<std::size_t>(index)] = out;
      computed[static_cast<std::size_t>(index)] = true;
      return out;
    };
    node_index = 0;
    for (const Json& node : *nodes) {
      GltfNode entry{};
      entry.name = json_get_string(node, "name", std::format("node{}", node_index));
      entry.material = static_cast<int>(json_get_i64(node, "mesh", -1)) >= 0
                           ? static_cast<int>(json_get_i64(node, "material", -1))
                           : -1;
      // 材质在 primitive 上（规范），节点上没有 `material`；这里取第一个 primitive 的材质
      if (const int mesh_index = static_cast<int>(json_get_i64(node, "mesh", -1)); mesh_index >= 0) {
        if (const Json* meshes_node = json_find(document, "meshes"); meshes_node != nullptr &&
            meshes_node->is_array() &&
            static_cast<std::size_t>(mesh_index) < meshes_node->size()) {
          const Json& mesh_json = (*meshes_node)[static_cast<std::size_t>(mesh_index)];
          if (const Json* primitives = json_find(mesh_json, "primitives");
              primitives != nullptr && primitives->is_array() && !primitives->empty()) {
            entry.material =
                static_cast<int>(json_get_i64((*primitives)[0], "material", -1));
          }
        }
        entry.meshes.push_back(static_cast<std::size_t>(mesh_index));
      }
      entry.transform = world_of(static_cast<int>(node_index), world_of);
      scene.nodes.push_back(std::move(entry));
      ++node_index;
    }
  }

  // ── 动画（读）──
  //
  // ## 三个必须说清的取舍（都写在 `GltfScene::animations` 的注释里）
  //
  // 1. `rotation` 的采样是**四元数**，而本框架的 `Keyframe::value` 是欧拉角
  //    → 逐个采样转欧拉角。万向锁附近有损，这是已知近似。
  // 2. `CUBICSPLINE` 的 accessor 每个关键帧有 3 个值（入切线、值、出切线），
  //    这里**取中间那个**（值是准的，曲率丢掉了）。
  // 3. 时间轴：`input` accessor 是秒（与本框架同一单位），直接搬。
  if (const Json* animations = json_find(document, "animations");
      animations != nullptr && animations->is_array()) {
    std::size_t anim_index = 0;
    for (const Json& animation : *animations) {
      const std::string anim_path = std::format("animations[{}]", anim_index);
      ++anim_index;
      Clip clip{};
      clip.name = json_get_string(animation, "name", std::format("animation{}", anim_index - 1U));

      // sampler 索引 → (输入时间 accessor, 输出值 accessor, 插值方式)
      struct SamplerRef {
        int input{-1};
        int output{-1};
        std::string interpolation{"LINEAR"};
      };
      std::vector<SamplerRef> samplers;
      if (const Json* list = json_find(animation, "samplers");
          list != nullptr && list->is_array()) {
        for (const Json& sampler : *list) {
          SamplerRef ref{};
          ref.input = static_cast<int>(json_get_i64(sampler, "input", -1));
          ref.output = static_cast<int>(json_get_i64(sampler, "output", -1));
          ref.interpolation = json_get_string(sampler, "interpolation", "LINEAR");
          samplers.push_back(ref);
        }
      }

      if (const Json* channels = json_find(animation, "channels");
          channels != nullptr && channels->is_array()) {
        std::size_t channel_index = 0;
        for (const Json& channel : *channels) {
          const std::string channel_path =
              std::format("{}.channels[{}]", anim_path, channel_index);
          ++channel_index;
          const Json* target = json_find(channel, "target");
          if (target == nullptr || !target->is_object()) {
            return st::unexpected(ErrorCode::Parse, channel_path + ".target：缺少");
          }
          const int node_index = static_cast<int>(json_get_i64(*target, "node", -1));
          const std::string path_name = json_get_string(*target, "path", "");
          if (node_index < 0) {
            return st::unexpected(ErrorCode::Parse,
                                  std::format("{}.target.node：缺失或越界", channel_path));
          }
          const auto kind = [&]() -> std::optional<TrackKind> {
            if (path_name == "translation") return TrackKind::Translation;
            if (path_name == "rotation") return TrackKind::Rotation;
            if (path_name == "scale") return TrackKind::Scale;
            return std::nullopt;
          }();
          if (!kind.has_value()) {
            // `weights`（morph target）不支持——跳过这一条通道，不让整份文件失败
            continue;
          }
          const int sampler_index = static_cast<int>(json_get_i64(channel, "sampler", -1));
          if (sampler_index < 0 ||
              static_cast<std::size_t>(sampler_index) >= samplers.size()) {
            return st::unexpected(
                ErrorCode::Parse,
                std::format("{}.sampler = {} 越界（共 {} 个）", channel_path, sampler_index,
                            samplers.size()));
          }
          const SamplerRef& sampler = samplers[static_cast<std::size_t>(sampler_index)];
          auto time_view = accessor_view(ctx, sampler.input, channel_path + ".sampler.input");
          if (!time_view) return st::forward_error(time_view.error());
          // 时间 accessor 是 SCALAR float
          std::vector<float> times;
          times.reserve(time_view->count);
          for (std::size_t i = 0; i < time_view->count; ++i) {
            times.push_back(read_component(time_view->data + i * time_view->stride,
                                           time_view->type));
          }
          auto value_view = accessor_view(ctx, sampler.output, channel_path + ".sampler.output");
          if (!value_view) return st::forward_error(value_view.error());

          Track track{};
          // 节点名：与 `nodes` 的读取保持同一规则（无名则 `nodeN`）
          const Json* nodes_json = json_find(document, "nodes");
          if (nodes_json != nullptr && nodes_json->is_array() &&
              static_cast<std::size_t>(node_index) < nodes_json->size()) {
            track.node = json_get_string((*nodes_json)[static_cast<std::size_t>(node_index)],
                                         "name", std::format("node{}", node_index));
          } else {
            track.node = std::format("node{}", node_index);
          }
          track.kind = *kind;
          // CUBICSPLINE 的每个关键帧占 3 个元素（入切线 / 值 / 出切线）
          const std::size_t stride_elements = sampler.interpolation == "CUBICSPLINE" ? 3U : 1U;
          for (std::size_t i = 0; i < times.size(); ++i) {
            const std::size_t element = i * stride_elements +
                                        (stride_elements == 3U ? 1U : 0U);   // 取"值"那个
            const std::uint8_t* base = value_view->data + element * value_view->stride;
            Keyframe key{};
            key.time = times[i];
            key.easing = sampler.interpolation == "STEP" ? Easing::Constant : Easing::Linear;
            const auto component = [&](std::size_t index) {
              return read_component(base + index * value_view->component_bytes, value_view->type);
            };
            if (track.kind == TrackKind::Rotation) {
              // 四元数（x,y,z,w）→ 欧拉角
              if (value_view->components < 4U) {
                return st::unexpected(
                    ErrorCode::Parse,
                    std::format("{}：rotation 轨道需要 VEC4（四元数），收到 {} 个分量",
                                channel_path, value_view->components));
              }
              const math::Quat quat{component(0), component(1), component(2), component(3)};
              key.value = quat.to_euler();
            } else {
              key.value = math::Vec3{component(0), component(1), component(2)};
            }
            track.keys.push_back(key);
          }
          if (!track.keys.empty()) clip.tracks.push_back(std::move(track));
        }
      }
      if (!clip.tracks.empty()) scene.animations.push_back(std::move(clip));
    }
  }

  // 相机
  if (const Json* cameras = json_find(document, "cameras"); cameras != nullptr &&
      cameras->is_array() && !cameras->empty()) {
    const Json& camera = (*cameras)[0];
    if (const Json* perspective = json_find(camera, "perspective"); perspective != nullptr) {
      scene.has_camera = true;
      const float yfov = read_number(*perspective, "yfov", 0.735f);   // 弧度
      scene.camera.fov_y_degrees = yfov * (180.0f / 3.14159265f);
      scene.camera.near_z = read_number(*perspective, "znear", 0.1f);
      scene.camera.far_z = read_number(*perspective, "zfar", 100.0f);
    } else if (const Json* ortho = json_find(camera, "orthographic"); ortho != nullptr) {
      scene.has_camera = true;
      scene.camera.projection = ProjectionKind::Orthographic;
      scene.camera.ortho_scale = read_number(*ortho, "ymag", 1.0f) * 2.0f;
      scene.camera.near_z = read_number(*ortho, "znear", 0.1f);
      scene.camera.far_z = read_number(*ortho, "zfar", 100.0f);
    }
  }


  scene.generator = json_get_string(*asset, "generator", "");
  return scene;
}

auto gltf_read(std::span<const std::uint8_t> bytes) -> Result<GltfScene> {
  std::string base_dir = st::fs::current_dir().value_or(std::string{});
  return gltf_read_impl(bytes, base_dir);
}

// ────────────────────────────────────────────────────────────────────────────

auto gltf_read_file(std::string_view path) -> Result<GltfScene> {
  auto bytes = st::fs::read_bytes(path);
  if (!bytes.has_value()) {
    return st::unexpected(bytes.error().code,
                          std::format("读 glTF 文件 \"{}\" 失败：{}", path, bytes.error().message));
  }
  // 基目录 = 文件所在目录：`.gltf` 的外部 `.bin` 用**相对路径**引用，
  // 按当前工作目录去解会得到"缓冲不存在"——而文件本身完全合法。
  return gltf_read_impl(*bytes, st::fs::parent(path));
}

// ────────────────────────────────────────────────────────────────────────────
// 写出
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 二进制缓冲累加器（4 字节对齐——glTF 要求 bufferView 的 byteOffset 对齐到组件大小，
/// 而 4 是最严的一档，统一 4 对齐不会有任何浪费）。
struct BufferWriter {
  std::vector<std::uint8_t> bytes{};

  [[nodiscard]] auto add(const void* data, std::size_t length) -> std::size_t {
    while (bytes.size() % 4U != 0U) bytes.push_back(0U);
    const std::size_t offset = bytes.size();
    const auto* begin = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), begin, begin + length);
    return offset;
  }
  [[nodiscard]] auto add_floats(const std::vector<float>& values) -> std::size_t {
    return add(values.data(), values.size() * sizeof(float));
  }
  [[nodiscard]] auto add_uint32(const std::vector<std::uint32_t>& values) -> std::size_t {
    return add(values.data(), values.size() * sizeof(std::uint32_t));
  }
};

/// 把网格拆成 glTF 需要的三组数组（位置/法线/uv）+ 索引。
struct GeomData {
  std::vector<float> positions{};
  std::vector<float> normals{};
  std::vector<float> uvs{};
  std::vector<std::uint32_t> indices{};
  float min[3]{};
  float max[3]{};
  bool has_uv{false};
  bool has_normal{false};
};

[[nodiscard]] auto geom_of(const Mesh& mesh) -> GeomData {
  GeomData out{};
  const std::size_t count = mesh.vertex_count();
  out.positions.reserve(count * 3U);
  out.normals.reserve(count * 3U);
  float lo[3] = {1.0e30f, 1.0e30f, 1.0e30f};
  float hi[3] = {-1.0e30f, -1.0e30f, -1.0e30f};
  out.has_normal = count > 0U;
  out.has_uv = mesh.has_uvs();
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t at = i * Mesh::kVertexFloats;
    if (at + Mesh::kVertexFloats > mesh.vertices.size()) break;
    for (int axis = 0; axis < 3; ++axis) {
      const float value = mesh.vertices[at + static_cast<std::size_t>(axis)];
      out.positions.push_back(value);
      lo[axis] = std::min(lo[axis], value);
      hi[axis] = std::max(hi[axis], value);
    }
    const float nx = mesh.vertices[at + 3U];
    const float ny = mesh.vertices[at + 4U];
    const float nz = mesh.vertices[at + 5U];
    if (nx == 0.0f && ny == 0.0f && nz == 0.0f) out.has_normal = false;
    out.normals.insert(out.normals.end(), {nx, ny, nz});
    if (out.has_uv) {
      const auto uv = mesh.uv_at(i);
      out.uvs.push_back(uv.first);
      out.uvs.push_back(uv.second);
    }
  }
  if (count == 0U) {
    for (int axis = 0; axis < 3; ++axis) {
      lo[axis] = 0.0f;
      hi[axis] = 0.0f;
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    out.min[axis] = lo[axis];
    out.max[axis] = hi[axis];
  }
  out.indices = mesh.indices;
  if (!out.has_uv) out.uvs.clear();
  return out;
}

/// 构造 PBR 材质节点（省缺值不写——glTF 的默认值是 metallic=1/roughness=1，
/// 而霜天的默认是 metallic=0/roughness=1，所以 metallic 必须显式写 0）。
[[nodiscard]] auto material_json(const Material& material, std::string_view name) -> Json {
  Json out = Json::object();
  out["name"] = std::string(name);
  Json pbr = Json::object();
  // 固有色：`base_color_set` 时用它，否则白（颜色来自顶点色 → 顶点色在 glTF 里是 COLOR_0，本实现不写）
  const math::Vec3 linear = material.base_color_set
                                ? material.base_color
                                : math::Vec3{1.0f, 1.0f, 1.0f};
  Json factor = Json::array();
  factor.push_back(linear.x);
  factor.push_back(linear.y);
  factor.push_back(linear.z);
  factor.push_back(material.opacity);
  pbr["baseColorFactor"] = factor;
  pbr["metallicFactor"] = material.metallic;
  pbr["roughnessFactor"] = material.roughness;
  out["pbrMetallicRoughness"] = pbr;
  if (material.emission > 0.0f) {
    const math::Vec3 emissive = material.emissive_color_set
                                    ? material.emissive_color
                                    : math::Vec3{material.emission, material.emission,
                                                 material.emission};
    Json emissive_node = Json::array();
    emissive_node.push_back(emissive.x);
    emissive_node.push_back(emissive.y);
    emissive_node.push_back(emissive.z);
    out["emissiveFactor"] = emissive_node;
  }
  if (material.opacity < 1.0f) {
    out["alphaMode"] = "BLEND";
  }
  return out;
}

/// glTF 的 matrix 是列主序 16 个数。
[[nodiscard]] auto matrix_json(const math::Mat4& matrix) -> Json {
  Json out = Json::array();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) out.push_back(matrix.value(row, column));
  }
  return out;
}

}  // namespace

auto gltf_write_json(const std::vector<GltfObject>& objects, const GltfWriteOptions& options)
    -> Result<GltfWriteResult> {
  BufferWriter buffer{};
  Json document = Json::object();

  Json asset = Json::object();
  asset["version"] = "2.0";
  asset["generator"] =
      options.generator.empty() ? std::string("霜天 Shuangtian") : options.generator;
  document["asset"] = asset;
  if (options.binary) {
    // `.glb` 的字符串里不需要额外声明；这里仍标一下便于人读 JSON chunk
    document["scene"] = 0;
  } else {
    document["scene"] = 0;
  }

  Json accessors = Json::array();
  Json views = Json::array();
  Json meshes = Json::array();
  Json materials = Json::array();
  Json nodes = Json::array();

  const auto add_accessor = [&](std::size_t view_index, std::size_t count, std::string_view type,
                                std::uint32_t component_type, bool with_min_max, const float* lo,
                                const float* hi) {
    Json accessor = Json::object();
    accessor["bufferView"] = static_cast<std::int64_t>(view_index);
    accessor["componentType"] = static_cast<std::int64_t>(component_type);
    accessor["count"] = static_cast<std::int64_t>(count);
    accessor["type"] = std::string(type);
    if (with_min_max) {
      Json min_node = Json::array();
      Json max_node = Json::array();
      const std::size_t components = type == "VEC3" ? 3U : (type == "VEC2" ? 2U : 1U);
      for (std::size_t i = 0; i < components; ++i) {
        min_node.push_back(lo[i]);
        max_node.push_back(hi[i]);
      }
      accessor["min"] = min_node;
      accessor["max"] = max_node;
    }
    accessors.push_back(accessor);
    return accessors.size() - 1U;
  };

  std::size_t object_index = 0;
  for (const GltfObject& object : objects) {
    const GeomData geom = geom_of(object.mesh);
    if (geom.positions.empty()) {
      ++object_index;
      continue;
    }
    // POSITION
    const std::size_t pos_offset = buffer.add_floats(geom.positions);
    Json pos_view = Json::object();
    pos_view["buffer"] = 0;
    pos_view["byteOffset"] = static_cast<std::int64_t>(pos_offset);
    pos_view["byteLength"] =
        static_cast<std::int64_t>(geom.positions.size() * sizeof(float));
    pos_view["target"] = 34962;   // ARRAY_BUFFER
    views.push_back(pos_view);
    const std::size_t pos_view_index = views.size() - 1U;
    const std::size_t pos_accessor = add_accessor(pos_view_index, geom.positions.size() / 3U,
                                                  "VEC3", 5126, true, geom.min, geom.max);

    std::size_t normal_accessor = 0;
    bool has_normal = false;
    if (geom.has_normal) {
      const std::size_t offset = buffer.add_floats(geom.normals);
      Json view = Json::object();
      view["buffer"] = 0;
      view["byteOffset"] = static_cast<std::int64_t>(offset);
      view["byteLength"] = static_cast<std::int64_t>(geom.normals.size() * sizeof(float));
      view["target"] = 34962;
      views.push_back(view);
      normal_accessor = add_accessor(views.size() - 1U, geom.positions.size() / 3U, "VEC3",
                                     5126, false, nullptr, nullptr);
      has_normal = true;
    }
    std::size_t uv_accessor = 0;
    if (geom.has_uv) {
      const std::size_t offset = buffer.add_floats(geom.uvs);
      Json view = Json::object();
      view["buffer"] = 0;
      view["byteOffset"] = static_cast<std::int64_t>(offset);
      view["byteLength"] = static_cast<std::int64_t>(geom.uvs.size() * sizeof(float));
      view["target"] = 34962;
      views.push_back(view);
      uv_accessor = add_accessor(views.size() - 1U, geom.positions.size() / 3U, "VEC2", 5126,
                                 false, nullptr, nullptr);
    }
    std::size_t index_accessor = 0;
    if (!geom.indices.empty()) {
      const std::size_t offset = buffer.add_uint32(geom.indices);
      Json view = Json::object();
      view["buffer"] = 0;
      view["byteOffset"] = static_cast<std::int64_t>(offset);
      view["byteLength"] = static_cast<std::int64_t>(geom.indices.size() * sizeof(std::uint32_t));
      view["target"] = 34963;   // ELEMENT_ARRAY_BUFFER
      views.push_back(view);
      index_accessor = add_accessor(views.size() - 1U, geom.indices.size(), "SCALAR", 5125, false,
                                    nullptr, nullptr);
    }

    // 材质（每个对象一份，重复内容不去重——去重会让"改一个对象材质"意外影响另一个）
    materials.push_back(material_json(object.material, object.name));

    Json primitive = Json::object();
    Json attributes = Json::object();
    attributes["POSITION"] = static_cast<std::int64_t>(pos_accessor);
    if (has_normal) attributes["NORMAL"] = static_cast<std::int64_t>(normal_accessor);
    if (geom.has_uv) attributes["TEXCOORD_0"] = static_cast<std::int64_t>(uv_accessor);
    primitive["attributes"] = attributes;
    if (!geom.indices.empty()) {
      primitive["indices"] = static_cast<std::int64_t>(index_accessor);
    }
    primitive["material"] = static_cast<std::int64_t>(materials.size() - 1U);
    primitive["mode"] = 4;   // TRIANGLES

    Json mesh = Json::object();
    mesh["name"] = object.name.empty() ? std::format("mesh{}", object_index) : object.name;
    Json primitives = Json::array();
    primitives.push_back(primitive);
    mesh["primitives"] = primitives;
    meshes.push_back(mesh);

    Json node = Json::object();
    node["name"] = object.name.empty() ? std::format("node{}", object_index) : object.name;
    node["mesh"] = static_cast<std::int64_t>(meshes.size() - 1U);
    node["matrix"] = matrix_json(object.transform);
    nodes.push_back(node);
    ++object_index;
  }

  if (meshes.empty()) {
    return st::unexpected(ErrorCode::Invalid, "导出 glTF：没有可写的网格（对象全为空）");
  }

  // ── 动画（写）──
  //
  // ## 三条必须对齐的口径
  //
  // 1. **旋转存四元数**：glTF 的 `rotation` 采样必须是 VEC4 四元数（规范如此），
  //    而本框架的 `Keyframe::value` 是欧拉角 → 逐帧转四元数。
  // 2. **时间用秒**：input accessor 是 SCALAR float 秒，与本框架同一单位。
  // 3. **节点按名字找**：`Track::node` 匹配 `GltfObject::name`；找不到时**跳过该轨道**
  //    而不是让整份导出失败（少一条轨道 vs 一个文件都出不来，前者可救）。
  Json animations_json = Json::array();
  if (!options.animations.empty()) {
    // 名字 → 节点下标（写出的 nodes 数组里，对象在前、根在后）
    std::map<std::string, int> node_of;
    for (std::size_t i = 0; i < objects.size(); ++i) {
      const std::string name =
          objects[i].name.empty() ? std::format("node{}", i) : objects[i].name;
      node_of.emplace(name, static_cast<int>(i));
    }
    for (const Clip& clip : options.animations) {
      Json channels = Json::array();
      Json samplers = Json::array();
      for (const Track& track : clip.tracks) {
        if (track.keys.empty()) continue;
        const auto found_node = node_of.find(track.node);
        if (found_node == node_of.end()) continue;   // 跳过而不是整份失败
        const std::string path_name = track.kind == TrackKind::Rotation  ? "rotation"
                                      : track.kind == TrackKind::Scale   ? "scale"
                                                                         : "translation";
        if (track.kind == TrackKind::Visible) continue;   // glTF 没有可见性轨道
        // 时间序列
        std::vector<float> times;
        times.reserve(track.keys.size());
        for (const Keyframe& key : track.keys) times.push_back(key.time);
        // 值序列（旋转转四元数）
        std::vector<float> values;
        const bool is_rotation = track.kind == TrackKind::Rotation;
        values.reserve(track.keys.size() * (is_rotation ? 4U : 3U));
        for (const Keyframe& key : track.keys) {
          if (is_rotation) {
            const math::Quat quat = math::Quat::from_euler(key.value);
            values.insert(values.end(), {quat.x, quat.y, quat.z, quat.w});
          } else {
            values.insert(values.end(), {key.value.x, key.value.y, key.value.z});
          }
        }
        // 时间 accessor（SCALAR float，必须带 min/max——播放器用它算时长）
        const std::size_t time_offset = buffer.add_floats(times);
        Json time_view = Json::object();
        time_view["buffer"] = 0;
        time_view["byteOffset"] = static_cast<std::int64_t>(time_offset);
        time_view["byteLength"] = static_cast<std::int64_t>(times.size() * sizeof(float));
        views.push_back(time_view);
        const float time_min = times.empty() ? 0.0f : times.front();
        const float time_max = times.empty() ? 0.0f : times.back();
        const std::size_t time_accessor =
            add_accessor(views.size() - 1U, times.size(), "SCALAR", 5126, true, &time_min,
                         &time_max);
        // 值 accessor（旋转 VEC4 / 其余 VEC3）
        const std::size_t value_offset = buffer.add_floats(values);
        Json value_view = Json::object();
        value_view["buffer"] = 0;
        value_view["byteOffset"] = static_cast<std::int64_t>(value_offset);
        value_view["byteLength"] = static_cast<std::int64_t>(values.size() * sizeof(float));
        views.push_back(value_view);
        const std::size_t value_accessor =
            add_accessor(views.size() - 1U, track.keys.size(), is_rotation ? "VEC4" : "VEC3",
                         5126, false, nullptr, nullptr);

        Json sampler = Json::object();
        sampler["input"] = static_cast<std::int64_t>(time_accessor);
        sampler["output"] = static_cast<std::int64_t>(value_accessor);
        // 缓动：glTF 只有 LINEAR / STEP / CUBICSPLINE。我们的 ease-* 是**曲线**，
        // 无法用 LINEAR 表达 → 退化成 LINEAR 并**逐帧采样**（调用方可用
        // `sample_clip` 按固定步长生成密集关键帧，那样曲线形状就能保真）。
        // 这里如实写 LINEAR，而不是假装支持。
        const bool step = std::any_of(track.keys.begin(), track.keys.end(),
                                      [](const Keyframe& key) {
                                        return key.easing == Easing::Constant;
                                      });
        sampler["interpolation"] = step ? "STEP" : "LINEAR";
        samplers.push_back(sampler);

        Json target = Json::object();
        target["node"] = static_cast<std::int64_t>(found_node->second);
        target["path"] = path_name;
        Json channel = Json::object();
        channel["sampler"] = static_cast<std::int64_t>(samplers.size() - 1U);
        channel["target"] = target;
        channels.push_back(channel);
      }
      if (channels.empty()) continue;
      Json animation = Json::object();
      animation["name"] = clip.name;
      animation["channels"] = channels;
      animation["samplers"] = samplers;
      animations_json.push_back(animation);
    }
  }

  // 根节点：把 scene 的所有节点挂在它下面（便于在 Blender 里看到单一层级）
  Json root = Json::object();
  root["name"] = options.root_name;
  Json children = Json::array();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    children.push_back(static_cast<std::int64_t>(i));
  }
  root["children"] = children;
  nodes.push_back(root);
  const std::size_t root_index = nodes.size() - 1U;

  if (!animations_json.empty()) document["animations"] = animations_json;
  document["accessors"] = accessors;
  document["meshes"] = meshes;
  if (!materials.empty()) document["materials"] = materials;
  document["nodes"] = nodes;
  Json scenes = Json::array();
  Json scene_node = Json::object();
  Json scene_children = Json::array();
  scene_children.push_back(static_cast<std::int64_t>(root_index));
  scene_node["nodes"] = scene_children;
  scenes.push_back(scene_node);
  document["scenes"] = scenes;

  if (options.write_camera) {
    Json camera = Json::object();
    Json perspective = Json::object();
    perspective["yfov"] = static_cast<double>(options.camera.fov_y_degrees * 3.14159265f / 180.0f);
    perspective["znear"] = static_cast<double>(options.camera.near_z);
    perspective["zfar"] = static_cast<double>(options.camera.far_z);
    camera["perspective"] = perspective;
    camera["type"] = "perspective";
    Json cameras = Json::array();
    cameras.push_back(camera);
    document["cameras"] = cameras;
  }

  Json buffer_json = Json::object();
  buffer_json["byteLength"] = static_cast<std::int64_t>(buffer.bytes.size());
  Json buffers = Json::array();
  if (options.binary) {
    // GLB：buffers[0] 不带 uri，数据在 BIN chunk 里
  } else if (options.embed_binary) {
    buffer_json["uri"] = "data:application/octet-stream;base64," + st::base64_encode(buffer.bytes);
  } else {
    buffer_json["uri"] = options.binary_name.empty() ? "buffer.bin" : options.binary_name;
  }
  buffers.push_back(buffer_json);
  document["buffers"] = buffers;
  document["bufferViews"] = views;

  GltfWriteResult result{};
  if (options.binary) {
    // GLB 容器：头(12) + JSON chunk(8 + n, 4 对齐) + BIN chunk(8 + n, 4 对齐)
    std::string json_text = st::json_dump(document, false);
    while (json_text.size() % 4U != 0U) json_text.push_back(' ');
    std::vector<std::uint8_t> bin = std::move(buffer.bytes);
    while (bin.size() % 4U != 0U) bin.push_back(0U);

    const std::size_t total = 12U + 8U + json_text.size() + 8U + bin.size();
    result.binary.reserve(total);
    append_u32_le(result.binary, kMagicGlb);
    append_u32_le(result.binary, kGlbVersion);
    append_u32_le(result.binary, static_cast<std::uint32_t>(total));
    append_u32_le(result.binary, static_cast<std::uint32_t>(json_text.size()));
    append_u32_le(result.binary, kChunkJson);
    for (const char ch : json_text) result.binary.push_back(static_cast<std::uint8_t>(ch));
    append_u32_le(result.binary, static_cast<std::uint32_t>(bin.size()));
    append_u32_le(result.binary, kChunkBin);
    result.binary.insert(result.binary.end(), bin.begin(), bin.end());
    result.binary_is_glb = true;
  } else {
    result.json = st::json_dump(document, true);
    if (!options.embed_binary) {
      result.binary = std::move(buffer.bytes);
      result.binary_name = options.binary_name.empty() ? "buffer.bin" : options.binary_name;
    }
  }
  return result;
}

auto gltf_write_binary(const std::vector<GltfObject>& objects, const GltfWriteOptions& options)
    -> Result<std::vector<std::uint8_t>> {
  GltfWriteOptions binary = options;
  binary.binary = true;
  auto result = gltf_write_json(objects, binary);
  if (!result) return st::forward_error(result.error());
  return std::move(result->binary);
}

auto gltf_write_file(std::string_view path, const std::vector<GltfObject>& objects,
                     const GltfWriteOptions& options) -> Status {
  auto result = gltf_write_json(objects, options);
  if (!result) return st::forward_error(result.error());
  if (options.binary) {
    return st::fs::write_bytes(path, result->binary);
  }
  const auto written = st::fs::write_text(path, result->json);
  if (!written) return written;
  if (!result->binary_name.empty()) {
    // 外部 .bin 与 .gltf 同目录
    const std::string dir = st::fs::parent(path);
    return st::fs::write_bytes(st::fs::join(dir, result->binary_name), result->binary);
  }
  return st::ok();
}

auto gltf_draw(const GltfScene& scene, Scene3D& target) -> Status {
  for (const GltfNode& node : scene.nodes) {
    for (const std::size_t mesh_index : node.meshes) {
      if (mesh_index >= scene.meshes.size()) {
        return st::unexpected(ErrorCode::Parse,
                              std::format("节点 \"{}\" 引用了不存在的 mesh {}（共 {} 个）",
                                          node.name, mesh_index, scene.meshes.size()));
      }
      Material material{};
      if (node.material >= 0 &&
          static_cast<std::size_t>(node.material) < scene.materials.size()) {
        const auto& slot = scene.materials[static_cast<std::size_t>(node.material)];
        // ⚠ 口径：glTF 的 `baseColorFactor` 是**线性**，`Material::base_color` 也是线性
        // → 这里用 `to_linear` 还原（读入时从线性编码成 sRGB 字节，写回要解回去）。
        // 不做这一步会亮一档：实测观感像"过曝的塑料"，而且只在 glTF 场景里出现。
        material.base_color = math::Vec3{srgb_to_linear(static_cast<float>(slot.base_color.r) / 255.0f),
                                         srgb_to_linear(static_cast<float>(slot.base_color.g) / 255.0f),
                                         srgb_to_linear(static_cast<float>(slot.base_color.b) / 255.0f)};
        material.base_color_set = true;
        material.metallic = slot.metallic;
        material.roughness = slot.roughness;
        material.opacity = slot.opacity;
        material.specular = slot.metallic > 0.0f ? 1.0f : 0.0f;
        material.emission = slot.emissive;
        material.emissive_color = slot.emissive_color;
        material.emissive_color_set = slot.emissive > 0.0f;
      }
      target.set_material(material);
      target.draw_mesh(scene.meshes[mesh_index], node.transform);
    }
  }
  return st::ok();
}

}  // namespace st::raster
