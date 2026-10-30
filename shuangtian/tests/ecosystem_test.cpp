/// 生态兼容层的回归测试：图片解码、glTF 贴图、扩展三档、二进制通道。
///
/// ## 判据为什么必须"内容对不对"而不是"没崩"
///
/// 这一层的错误几乎全是**静默降级**：
/// - JPEG 色度上采样错 → 偏色，但图还在；
/// - 贴图没读 → 模型变纯色，解析完全成功；
/// - Draco 不支持 → 空几何，"加载成功"但画面空白；
/// - 二进制通道传错类型 → 隐式转成慢路径，功能对但性能莫名差。
///
/// 所以每一条都钉具体像素值、具体错误码、具体类型校验。
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "st/codec/image.hpp"
#include "st/codec/jpeg.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/ext/script.hpp"
#include "st/raster/gltf.hpp"
#include "st/test/test.hpp"

namespace {

using st::codec::DecodedImage;

/// 生成一张最小 PNG（纯色），用于构造 glTF 内的贴图。
///
/// 为什么自造而不带资源文件：测试要能**独立运行**（不依赖外部素材路径），
/// 而自造的图内容已知，判据可以钉死。
auto make_solid_png(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b)
    -> std::vector<std::uint8_t> {
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  for (std::size_t i = 0; i + 3U < image.rgba.size(); i += 4U) {
    image.rgba[i + 0U] = r;
    image.rgba[i + 1U] = g;
    image.rgba[i + 2U] = b;
  }
  const auto encoded = st::codec::png_encode(image);
  return encoded.has_value() ? *encoded : std::vector<std::uint8_t>{};
}

/// 极简 base64（避免测试依赖 st::base64 的接口细节）。
auto to_base64(std::span<const std::uint8_t> data) -> std::string {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2U) / 3U * 4U);
  std::size_t i = 0;
  while (i + 2U < data.size()) {
    const auto chunk = (static_cast<std::uint32_t>(data[i]) << 16U) |
                       (static_cast<std::uint32_t>(data[i + 1U]) << 8U) |
                       static_cast<std::uint32_t>(data[i + 2U]);
    out.push_back(kTable[(chunk >> 18U) & 0x3F]);
    out.push_back(kTable[(chunk >> 12U) & 0x3F]);
    out.push_back(kTable[(chunk >> 6U) & 0x3F]);
    out.push_back(kTable[chunk & 0x3F]);
    i += 3U;
  }
  if (i + 1U == data.size()) {
    const auto chunk = static_cast<std::uint32_t>(data[i]) << 16U;
    out.push_back(kTable[(chunk >> 18U) & 0x3F]);
    out.push_back(kTable[(chunk >> 12U) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2U == data.size()) {
    const auto chunk = (static_cast<std::uint32_t>(data[i]) << 16U) |
                       (static_cast<std::uint32_t>(data[i + 1U]) << 8U);
    out.push_back(kTable[(chunk >> 18U) & 0x3F]);
    out.push_back(kTable[(chunk >> 12U) & 0x3F]);
    out.push_back(kTable[(chunk >> 6U) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

/// 造一份最小 glTF（单三角形 + 可选贴图）。
auto make_gltf_json(const std::string& image_uri, const std::string& extra_extensions)
    -> std::string {
  // 顶点缓冲：3 个位置（36 字节）+ 3 个 UV（24 字节）
  std::vector<std::uint8_t> buffer(60U, 0);
  const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  std::memcpy(buffer.data(), positions, sizeof(positions));
  const float uvs[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  std::memcpy(buffer.data() + 36, uvs, sizeof(uvs));

  return std::string(R"j({
  "asset": {"version": "2.0", "generator": "test"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Tri", "mesh": 0}],
  "meshes": [{"name": "Tri", "primitives": [{
    "attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "material": 0, "mode": 4}]}],
  "materials": [{"name": "Mat", "pbrMetallicRoughness": {
    "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.8}}],
  "textures": [{"source": 0}],
  "images": [{"uri": ")j") + image_uri + R"j("}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 24, "target": 34962}],
  "buffers": [{"uri": "data:application/octet-stream;base64,)j" +
      to_base64(buffer) + R"j(", "byteLength": 60}] )j" + extra_extensions + R"j(
})j";
}

}  // namespace

ST_TEST(image_sniff_detects_format_by_magic) {
  // ## 按魔数识别，不看扩展名
  //
  // 素材包里"扩展名与实际内容不符"真实存在（下载时名字被截断、工具导出写错后缀）。
  // 按扩展名分发会得到"明明有图却解不出来"，而错误只说"不是 PNG"。
  const auto png = make_solid_png(2, 2, 10, 20, 30);
  ST_REQUIRE(!png.empty());
  ST_CHECK_EQ(st::codec::sniff_image_format(png), std::string("png"));

  // JPEG 魔数（只需前 3 字节 FF D8 FF）
  const std::vector<std::uint8_t> jpeg_head{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
  ST_CHECK_EQ(st::codec::sniff_image_format(jpeg_head), std::string("jpeg"));

  // 非图片：识别为空串（而不是瞎猜一个）
  const std::vector<std::uint8_t> text{'h', 'e', 'l', 'l', 'o'};
  ST_CHECK_EQ(st::codec::sniff_image_format(text), std::string(""));
  ST_CHECK_EQ(st::codec::sniff_image_format({}), std::string(""));
}

ST_TEST(image_decode_rejects_unknown_with_evidence) {
  // 识别不出时的错误必须带**证据**（前几字节），而不是一句"不支持"。
  // 否则用户无法判断"这是不是图""是不是下载错了"。
  const std::vector<std::uint8_t> html{'<', '!', 'D', 'O', 'C', 'T', 'Y', 'P', 'E'};
  const auto decoded = st::codec::decode_image(html);
  ST_REQUIRE(!decoded.has_value());
  ST_CHECK(decoded.error().code == st::ErrorCode::Unsupported);
  const std::string message = decoded.error().message;
  ST_CHECK(message.find("PNG") != std::string::npos);          // 说清支持什么
  ST_CHECK(message.find("JPEG") != std::string::npos);
  ST_CHECK(message.find("3C 21 44 4F") != std::string::npos);  // 前 4 字节 = "<!DO"
}

ST_TEST(jpeg_decoder_reports_unsupported_variants) {
  // 不支持的 JPEG 变体必须**明确报错并说明是哪一项**，
  // 而不是解出一张乱码图（那会让用户以为素材坏了）。
  //
  // 用"手工构造的 SOF 标记 + 最小段"来触发各分支——不依赖外部样本文件。
  const auto make_sof = [](std::uint8_t sof_marker) {
    std::vector<std::uint8_t> bytes{0xFF, 0xD8};          // SOI
    bytes.insert(bytes.end(), {0xFF, sof_marker});       // SOFn
    bytes.insert(bytes.end(), {0x00, 0x11});             // 段长 17
    bytes.push_back(8);                                  // 精度 8
    bytes.insert(bytes.end(), {0x00, 0x10});             // 高 16
    bytes.insert(bytes.end(), {0x00, 0x10});             // 宽 16
    bytes.push_back(3);                                  // 分量数
    for (int i = 0; i < 3; ++i) {
      bytes.push_back(static_cast<std::uint8_t>(i + 1));
      bytes.push_back(0x11);                             // 1×1 采样
      bytes.push_back(0);                                // 量化表
    }
    return bytes;
  };

  // SOF3 = 无损 → 报"无损模式"
  const auto lossless = st::codec::jpeg_decode(make_sof(0xC3));
  ST_REQUIRE(!lossless.has_value());
  ST_CHECK(lossless.error().code == st::ErrorCode::Unsupported);
  ST_CHECK(lossless.error().message.find("无损") != std::string::npos);

  // SOF9 = 算术编码 → 报"算术编码"
  const auto arithmetic = st::codec::jpeg_decode(make_sof(0xC9));
  ST_REQUIRE(!arithmetic.has_value());
  ST_CHECK(arithmetic.error().message.find("算术编码") != std::string::npos);

  // 缺 SOI → 报签名问题
  const std::vector<std::uint8_t> no_soi{0x00, 0x00, 0x00, 0x00};
  const auto bad = st::codec::jpeg_decode(no_soi);
  ST_REQUIRE(!bad.has_value());
  ST_CHECK(bad.error().message.find("SOI") != std::string::npos);
}

ST_TEST(gltf_loads_texture_from_data_uri) {
  // ## 生态素材最常见形态之一：单文件 .gltf，贴图内嵌为 data URI
  //
  // 这条钉"贴图真的被解码出来了"（不是只存了原始字节）：
  // 解码后的宽高与像素必须与生成时一致。
  const auto png = make_solid_png(4, 3, 200, 100, 50);
  ST_REQUIRE(!png.empty());
  const std::string uri = "data:image/png;base64," + to_base64(png);
  const std::string json = make_gltf_json(uri, "");

  const auto scene = st::raster::gltf_read(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                    json.size()));
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(scene->textures.size() == 1U);
  const auto& texture = scene->textures.front();
  ST_CHECK(texture.is_valid());
  ST_CHECK_EQ(texture.width, 4);
  ST_CHECK_EQ(texture.height, 3);
  // 像素内容必须真的解出来了（纯色 200,100,50）
  ST_REQUIRE(texture.rgba.size() >= 4U);
  ST_CHECK_EQ(static_cast<int>(texture.rgba[0]), 200);
  ST_CHECK_EQ(static_cast<int>(texture.rgba[1]), 100);
  ST_CHECK_EQ(static_cast<int>(texture.rgba[2]), 50);

  // 材质必须真的引用到它（否则前面解了也白解）
  ST_REQUIRE(scene->materials.size() == 1U);
  ST_CHECK_EQ(scene->materials.front().base_color_texture, 0);
}

ST_TEST(gltf_texture_failure_is_reported_not_swallowed) {
  // 贴图坏了：**保留占位 + 记下错误**，不中断整份文件（模型可能完好）。
  // 但绝不能静默丢掉——"这个模型怎么是纯色"查起来极慢。
  const std::string json = make_gltf_json("data:image/png;base64,bm90YXBuZw==", "");
  const auto scene = st::raster::gltf_read(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                    json.size()));
  // 文件本身合法（几何没问题），所以必须读成功
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(scene->textures.size() == 1U);
  const auto& texture = scene->textures.front();
  ST_CHECK(!texture.is_valid());        // 明确标为失败
  ST_CHECK(!texture.error.empty());     // 且带原因
  // 几何仍然可用（这才是"不中断"的意义）
  ST_CHECK(scene->meshes.size() == 1U);
  ST_CHECK(!scene->meshes.front().indices.empty());
}

ST_TEST(gltf_missing_texture_file_is_reported) {
  // 外部文件不存在 → 记错误，不崩
  const std::string json = make_gltf_json("C:/definitely/not/here/nope.png", "");
  const auto scene = st::raster::gltf_read(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                    json.size()));
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(scene->textures.size() == 1U);
  ST_CHECK(!scene->textures.front().is_valid());
  ST_CHECK(scene->textures.front().error.find("失败") != std::string::npos);
}

ST_TEST(gltf_extension_three_tier_disposition) {
  // ## 三档处置：支持 / 忽略 / 阻断
  //
  // 这条是本轮最重要的行为契约：**必需扩展不支持时必须失败**，
  // 而不是静默读出一个空模型（那是最糟的失败形态）。
  const auto png = make_solid_png(2, 2, 10, 10, 10);
  const std::string uri = "data:image/png;base64," + to_base64(png);

  // ① 无扩展 → 正常
  {
    const std::string json = make_gltf_json(uri, "");
    const auto scene = st::raster::gltf_read(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                      json.size()));
    ST_REQUIRE(scene.has_value());
    ST_CHECK(!scene->extensions.has_blocking());
  }
  // ② 可忽略的扩展（只影响材质细节）→ 读成功，但列进报告
  {
    const std::string json = make_gltf_json(
        uri, R"j(, "extensionsUsed": ["KHR_materials_clearcoat", "KHR_texture_transform"])j");
    const auto scene = st::raster::gltf_read(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                      json.size()));
    if (!scene.has_value()) {
      std::printf("[dbg] read failed: %s\n", scene.error().message.c_str());
    }
    ST_REQUIRE(scene.has_value());
    ST_CHECK(!scene->extensions.has_blocking());
    ST_CHECK_EQ(scene->extensions.ignored.size(), 1U);   // clearcoat 被忽略
    ST_CHECK_EQ(scene->extensions.ignored[0], std::string("KHR_materials_clearcoat"));
    ST_CHECK_EQ(scene->extensions.used.size(), 2U);
  }
  // ③ 必需的 Draco → **必须失败**，且错误里给出解决动作
  {
    const std::string json = make_gltf_json(
        uri,
        R"j(, "extensionsUsed": ["KHR_draco_mesh_compression"],
              "extensionsRequired": ["KHR_draco_mesh_compression"])j");
    const auto scene = st::raster::gltf_read(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                      json.size()));
    ST_REQUIRE(!scene.has_value());
    ST_CHECK(scene.error().code == st::ErrorCode::Unsupported);
    const std::string message = scene.error().message;
    ST_CHECK(message.find("KHR_draco_mesh_compression") != std::string::npos);
    ST_CHECK(message.find("Blender") != std::string::npos);   // 给解决动作
    ST_CHECK(message.find("压缩") != std::string::npos);
  }
  // ④ 必需的 KTX2 → 也失败，且建议换成 PNG/JPEG
  {
    const std::string json = make_gltf_json(
        uri,
        R"j(, "extensionsUsed": ["KHR_texture_basisu"],
              "extensionsRequired": ["KHR_texture_basisu"])j");
    const auto scene = st::raster::gltf_read(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(json.data()),
                                      json.size()));
    ST_REQUIRE(!scene.has_value());
    ST_CHECK(scene.error().message.find("KTX2") != std::string::npos);
  }
}

ST_TEST(script_binary_channel_transfers_float_arrays) {
  // ## 二进制通道：Float32Array 直传，内容必须逐元素一致
  st::ext::ScriptEngine engine{};
  std::vector<float> received;
  const auto add = [&received](const std::vector<std::span<const float>>& arrays)
      -> st::Result<st::Json> {
    if (arrays.empty()) {
      return st::unexpected(st::ErrorCode::Invalid, "需要至少一个数组");
    }
    received.insert(received.end(), arrays[0].begin(), arrays[0].end());
    return st::Json{static_cast<std::int64_t>(received.size())};
  };
  ST_CHECK(engine.register_binary_function("addData", add).has_value());

  const auto result = engine.eval(R"js(
const data = new Float32Array(6);
for (let i = 0; i < 6; i++) data[i] = i * 0.5;
addData(data);
)js",
                                  "binary.js");
  ST_REQUIRE(result.has_value());
  ST_REQUIRE(received.size() == 6U);
  for (int i = 0; i < 6; ++i) {
    ST_CHECK_NEAR(received[static_cast<std::size_t>(i)], static_cast<float>(i) * 0.5f, 1e-6f);
  }
}

ST_TEST(script_binary_channel_rejects_wrong_type) {
  // ## 传错类型必须**立即报错**，不做隐式转换
  //
  // 隐式转换（接受普通数组再转 float）会悄悄退回慢路径，于是
  // "我明明用了二进制通道怎么还是慢"变成查不出的问题。
  st::ext::ScriptEngine engine{};
  const auto add = [](const std::vector<std::span<const float>>& arrays)
      -> st::Result<st::Json> {
    return st::Json{static_cast<std::int64_t>(arrays.size())};
  };
  ST_CHECK(engine.register_binary_function("addData", add).has_value());

  // 传普通数组 → 报错，且消息里指出"第几个参数"与"期望 Float32Array"
  const auto result = engine.eval("addData([1, 2, 3]);", "bad.js");
  ST_REQUIRE(!result.has_value());
  const std::string message = result.error().message;
  st::log::out("[test] binary type error: {}\n", message);
  ST_CHECK(message.find("Float32Array") != std::string::npos);
}

ST_TEST(script_binary_and_json_channels_coexist) {
  // 两种通道可以同时注册、互不干扰（现有的 Json 宿主函数不受影响）
  st::ext::ScriptEngine engine{};
  int json_calls = 0;
  int binary_calls = 0;
  const auto json_fn = [&json_calls](const std::vector<st::Json>& args) -> st::Result<st::Json> {
    ++json_calls;
    return st::Json{static_cast<std::int64_t>(args.size())};
  };
  const auto binary_fn = [&binary_calls](const std::vector<std::span<const float>>&)
      -> st::Result<st::Json> {
    ++binary_calls;
    return st::Json{0};
  };
  ST_CHECK(engine.register_function("jsonFn", json_fn).has_value());
  ST_CHECK(engine.register_binary_function("binFn", binary_fn).has_value());
  ST_REQUIRE(engine.eval("jsonFn(1, 2); binFn(new Float32Array(4)); 0;", "both.js").has_value());
  ST_CHECK_EQ(json_calls, 1);
  ST_CHECK_EQ(binary_calls, 1);

  // 同名冲突要被拒绝（静默覆盖会让"谁改了这个函数"无从追查）
  const auto duplicate = engine.register_binary_function("jsonFn", binary_fn);
  ST_REQUIRE(!duplicate.has_value());
  ST_CHECK(duplicate.error().code == st::ErrorCode::Busy);
}
