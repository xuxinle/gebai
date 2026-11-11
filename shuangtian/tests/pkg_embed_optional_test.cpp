/// 编译期嵌入：**必顶 vs 可选**的语义边界（`optional_embed`）。
///
/// ## 为什么这批用例必须存在
///
/// 两种失败的形态**恰好相反**，而两者看起来都是“资源没嵌进去”：
///
/// - **必顶资源缺失** → 必须**硬失败**。这是「路径写错」的典型形态；若静默通过，
///   运行期 `b::embed<"…">()` 会撞 `static_assert("No such file or directory")`，
///   报错位置在生成的样板代码里，几乎无法定位到“清单里那条 glob 写错了”。
/// - **可选资源缺失** → 必须**照常编译**。这是「本地生图产线产出、因体积不入库」的
///   大资源（格斗的 34 张立绘 ~5.1MB，`.gitignore` 有全局 `*.png`）——克隆下来的机器上
///   必然没有。若按前者报错，整个应用连**编译**都过不了，而它本可降级成矢量画风跑起来。
///
/// 两者的唯一区别是清单里写在 `embed` 还是 `optional_embed`，所以判据必须钉在
/// **同一个缺失场景下的两种声明**上，否则“把两种资源都当必顶”或“都当可选”都能过测试。
///
/// 另有一条容易写错的实现细节被这里钉住：可选资源缺失时生成的是**零字节占位**，
/// 它的查找键必须是**声明的字面路径**（`b::embed<"apps/x/a.png">()` 按字符串查表），
/// 所以带通配符的模式在缺失时无从补键——那种情况只记日志、不生成占位。
#include <filesystem>
#include <format>
#include <string>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/pkg/embed.hpp"
#include "st/test/test.hpp"

namespace {

/// 临时工程目录：析构时整个删掉（与 `pkg_library_test` 的 `TempDir` 同一套设施）。
///
/// ## 唯一化用 pid + tag（与 `core_log_file_test.cpp` 的 `scratch_dir` 同一手法）
///
/// 不用计数器 / `this` 指针：本仓 L8 禁“可变的进程级全局状态”，L6 禁 `reinterpret_cast`，
/// 而那两种写法各自撞一条。pid 天然区分并行分片（分片是独立进程），tag 区分同进程内的用例。
/// 构造前先 remove_all：上一次异常退出留下的同名目录会让 `create_directories` 成功
/// 而里面残留旧文件，“用例自己造的现场”就不再干净了。
struct TempDir {
  std::string dir;

  explicit TempDir(std::string_view tag) {
    const auto base = std::filesystem::temp_directory_path() /
                      std::format("st_embed_{}_{}", static_cast<int>(st::process::current_id()),
                                  std::string(tag));
    std::error_code error;
    std::filesystem::remove_all(base, error);
    std::filesystem::create_directories(base, error);
    dir = base.string();
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }
  TempDir(const TempDir&) = delete;
  auto operator=(const TempDir&) -> TempDir& = delete;
};

/// 造一份极简模板目录（`generate_embeds` 只要求这两个文件名存在、占位符可替换）。
///
/// **源模板刻意按真实模板的形状写**（`std::string_view(${GENERATED_BYTE_ARRAY}, ${FILESIZE})`）：
/// 零字节占位当初的报错正是 `std::string_view(, 0)` 这种**形状**上的错误，
/// 模板本身与 `byte_array_literal` 的产物的交互才是被守的东西。
/// 早期这里写成 `"// ${IDENTIFIER} ${FILENAME}\n${GENERATED_BYTE_ARRAY}"`（不包一层 string_view），
/// 于是“生成物能不能编译”这条断言在这份模板上**无从成立**，用例白红一轮。
[[nodiscard]] auto make_templates(const std::string& root) -> std::string {
  const std::string dir = st::fs::join(root, "templates");
  (void)st::fs::create_directories(dir);
  (void)st::fs::write_text(
      st::fs::join(dir, "embed.hpp.in"),
      "// generated\n${EMBEDDED_FILES_DECLARATIONS}\n${EMBEDDED_FILES_RETURNS}\n");
  (void)st::fs::write_text(st::fs::join(dir, "embed_source.cpp.in"),
                           "// ${IDENTIFIER} ${FILENAME} ${FILESIZE}\n"
                           "std::string_view(\n            ${GENERATED_BYTE_ARRAY}\n"
                           "        , ${FILESIZE}\n        ), \"${FILENAME}\"\n");
  return dir;
}

[[nodiscard]] auto make_request(const std::string& root, const std::string& templates)
    -> st::pkg::EmbedRequest {
  st::pkg::EmbedRequest request;
  request.work_directory = root;
  request.template_directory = templates;
  request.runtime_source = st::fs::join(templates, "embed_impl.cpp");
  request.build_subdir = "dev";
  request.scope = "probe";
  request.with_runtime = false;  // 本组只验资源与声明，不引入运行时
  return request;
}

}  // namespace

/// 必顶资源存在 → 正常生成，且不计入占位。
ST_TEST(embed_required_present_generates) {
  TempDir temp("st_embed_req_ok");
  const std::string root = temp.dir;
  const std::string templates = make_templates(root);
  (void)st::fs::create_directories(st::fs::join(root, "assets"));
  ST_REQUIRE(st::fs::write_text(st::fs::join(st::fs::join(root, "assets"), "a.txt"), "hello").has_value());

  auto request = make_request(root, templates);
  st::pkg::EmbedContribution contribution;
  contribution.base_directory = root;
  contribution.patterns = {"assets/a.txt"};
  contribution.identifier_prefix = "probe";
  contribution.origin = "测试";
  request.contributions.push_back(std::move(contribution));

  const auto output = st::pkg::generate_embeds(request);
  ST_REQUIRE(output.has_value());
  ST_CHECK_EQ(output->file_count, std::size_t{1});
  ST_CHECK_EQ(output->placeholder_count, std::size_t{0});
  ST_CHECK(!output->include_dir.empty());
}

/// 必顶资源缺失 → **硬失败**（这条是“路径写错”的唯一警报）。
ST_TEST(embed_required_missing_fails) {
  TempDir temp("st_embed_req_missing");
  const std::string root = temp.dir;
  const std::string templates = make_templates(root);

  auto request = make_request(root, templates);
  st::pkg::EmbedContribution contribution;
  contribution.base_directory = root;
  contribution.patterns = {"assets/does-not-exist.png"};
  contribution.identifier_prefix = "probe";
  contribution.origin = "测试";
  request.contributions.push_back(std::move(contribution));

  const auto output = st::pkg::generate_embeds(request);
  ST_REQUIRE(!output.has_value());
  ST_CHECK(output.error().message.find("does-not-exist.png") != std::string::npos);
}

/// 可选资源缺失 → **照常生成**（零字节占位），并计入 `placeholder_count`。
///
/// 判据除了“返回成功”，还必须**确认占位的查找键就是声明的字面路径**——
/// 否则 `b::embed<"assets/missing.png">()` 仍会 `static_assert` 失败，
/// 而“构建成功”会变成一个假的绿灯。
///
/// ## 本用例为什么还验“生成物里有合法字面量”（2026-10-29 补）
///
/// 首版只验到“`generate_embeds` 返回成功 + 查找键在头里”，于是漏了一个致命问题：
/// 零字节时 `byte_array_literal` 返回了空串，填进模板变成
/// `std::string_view(, 0)` —— **34 个生成单元全部编译不能过**。
/// 报错位置在生成代码里（`expected primary-expression before '(' token`），
/// 看上去像模板坏了，而不是“资源缺失应该降级”。
/// 真实构建（`st build gedou`，立绘被移走）才把它暴露出来。
/// 所以这里加一条**与模板形状绑定**的断言：零字节也必须产出合法字面量。
ST_TEST(embed_optional_missing_compiles_with_placeholder) {
  TempDir temp("st_embed_opt_missing");
  const std::string root = temp.dir;
  const std::string templates = make_templates(root);

  auto request = make_request(root, templates);
  st::pkg::EmbedContribution contribution;
  contribution.base_directory = root;
  contribution.patterns = {"assets/missing-a.png", "assets/missing-b.png"};
  contribution.identifier_prefix = "probe";
  contribution.origin = "测试（可选）";
  contribution.optional = true;
  request.contributions.push_back(std::move(contribution));

  const auto output = st::pkg::generate_embeds(request);
  ST_REQUIRE(output.has_value());
  ST_CHECK_EQ(output->placeholder_count, std::size_t{2});
  // 声明头里必须**确实**有这两条查找键（这是编译通过的前提）
  const auto header = st::fs::read_text(st::fs::join(output->include_dir, "battery/embed.hpp"));
  ST_REQUIRE(header.has_value());
  ST_CHECK(header->find("assets/missing-a.png") != std::string::npos);
  ST_CHECK(header->find("assets/missing-b.png") != std::string::npos);
  // 派生的字节数组源必须含**合法字面量**：空序列也得是 `""`（不能是空串）。
  // 判据钉在“模板里的 `${GENERATED_BYTE_ARRAY}` 位置不是空白”这件事上——
  // 它直接对应 `std::string_view(<这里>, 0)` 能否编过（见用例注释）。
  for (const auto& source : output->sources) {
    const auto text = st::fs::read_text(source);
    ST_REQUIRE(text.has_value());
    ST_CHECK(text->find("std::string_view(") != std::string::npos);
    // 形状：`std::string_view(` 之后必须紧跟引号（`\n            "` 或 `""`），
    // 不能是换行后的逗号/空白。
    const auto at = text->find("std::string_view(");
    const auto after = text->find_first_not_of(" \t\r\n", at + std::string_view("std::string_view(").size());
    ST_REQUIRE(after != std::string::npos);
    ST_CHECK((*text)[after] == '"');
  }
}

/// 可选资源**部分**存在 → 存在的按真实内容嵌、缺的补占位（两者互不干扰）。
ST_TEST(embed_optional_partially_present) {
  TempDir temp("st_embed_opt_partial");
  const std::string root = temp.dir;
  const std::string templates = make_templates(root);
  (void)st::fs::create_directories(st::fs::join(root, "assets"));
  ST_REQUIRE(st::fs::write_text(st::fs::join(st::fs::join(root, "assets"), "here.txt"), "data").has_value());

  auto request = make_request(root, templates);
  st::pkg::EmbedContribution contribution;
  contribution.base_directory = root;
  contribution.patterns = {"assets/here.txt", "assets/gone.txt"};
  contribution.identifier_prefix = "probe";
  contribution.origin = "测试（可选）";
  contribution.optional = true;
  request.contributions.push_back(std::move(contribution));

  const auto output = st::pkg::generate_embeds(request);
  ST_REQUIRE(output.has_value());
  ST_CHECK_EQ(output->file_count, std::size_t{2});       // 两条都进了声明表
  ST_CHECK_EQ(output->placeholder_count, std::size_t{1}); // 其中一条是占位
}

/// 可选 + **通配符** 未命中 → 不报错、也不生成占位（无字面键可补），只记日志。
///
/// 为何要钉：通配符形态缺文件时“看起来也该补个占位”，但补不了——
/// 不知道用户写了哪些文件名。若这里误报错，就退回“必顶”的行为了；
/// 若误生成一条带 `*` 的查找键，那是永远匹配不上的垃圾声明。
ST_TEST(embed_optional_glob_missing_is_skipped) {
  TempDir temp("st_embed_opt_glob");
  const std::string root = temp.dir;
  const std::string templates = make_templates(root);

  auto request = make_request(root, templates);
  st::pkg::EmbedContribution contribution;
  contribution.base_directory = root;
  contribution.patterns = {"assets/*.png"};
  contribution.identifier_prefix = "probe";
  contribution.origin = "测试（可选）";
  contribution.optional = true;
  request.contributions.push_back(std::move(contribution));

  const auto output = st::pkg::generate_embeds(request);
  ST_REQUIRE(output.has_value());
  ST_CHECK_EQ(output->placeholder_count, std::size_t{0});
  // 没有资源 → 连声明目录都不该产出（`pending` 为空是正常状态）
  ST_CHECK(output->include_dir.empty());
}
