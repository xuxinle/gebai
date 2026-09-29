#pragma once

/// 语法高亮引擎（`DESIGN.md` §4.5）：**规则驱动 + 运行时可注册**，纯词法着色，零第三方依赖。
///
/// 设计要点
/// - **规则即数据**：一门语言 = 一个 `LanguageSpec`（关键字/类型/内建表 + 注释与字符串定界 +
///   若干语法特征开关）。新增语言**不需要写代码**，只需给一份规则；
/// - **内置主流语言**（C/C++/Rust/Go/Python/JS/TS/Java/C#/Kotlin/Swift/Ruby/PHP/Lua/SQL/
///   HTML/XML/CSS/JSON/YAML/TOML/INI/Shell/Dockerfile/Makefile/Markdown/Diff/Protobuf…），
///   按规范名 / 别名 / **文件扩展名**三种方式解析；
/// - **自定义语言**：应用可构造 `LanguageSpec` 并注册（`LanguageRegistry`），可覆盖内置同名语言；
/// - **纯词法**：不做语法分析，因此不限语言、不会因半截代码失败——正好适配编辑器"逐行增量重算"；
/// - **线程安全**：注册表读写有锁；扫描为纯函数，可并行调用。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::text {

/// 词法着色类别（UI 按主题色板映射到颜色）。
enum class TokenKind : std::uint8_t {
  Plain,        ///< 未着色（标识符/空白/未知内容）
  Keyword,      ///< 关键字（`if`/`fn`/`def`/`SELECT`…）
  Type,         ///< 类型名（`int`/`Vec`/`string`…）
  String,       ///< 字符串字面量（含模板串/三引号串/原始串，可跨行）
  Number,       ///< 数字字面量
  Comment,      ///< 注释（行/块，可跨行）
  Function,     ///< 函数名（定义处，或后随 `(` 的调用）
  Operator,     ///< 运算符
  Punctuation,  ///< 标点（括号/逗号/分号等）
  Preprocessor, ///< 预处理指令行（C/C++ `#include` 等）
  Builtin,      ///< 内建名/常量（`true`/`None`/`nil`…）
  Attribute,    ///< 装饰器/注解/HTML 属性（`@Override`/`data-*`）
  Key,          ///< 对象键（JSON/YAML/TOML 的 `"k":` 左侧）
  Tag,          ///< 标记语言标签名（HTML/XML `<div`）
  Inserted,     ///< Diff 新增行（`+ …`）
  Deleted,      ///< Diff 删除行（`- …`）
};

/// 类别短名（稳定：主题映射、日志、测试断言用）。
[[nodiscard]] constexpr auto to_string(TokenKind kind) noexcept -> std::string_view {
  switch (kind) {
    case TokenKind::Plain: return "plain";
    case TokenKind::Keyword: return "keyword";
    case TokenKind::Type: return "type";
    case TokenKind::String: return "string";
    case TokenKind::Number: return "number";
    case TokenKind::Comment: return "comment";
    case TokenKind::Function: return "function";
    case TokenKind::Operator: return "operator";
    case TokenKind::Punctuation: return "punctuation";
    case TokenKind::Preprocessor: return "preprocessor";
    case TokenKind::Builtin: return "builtin";
    case TokenKind::Attribute: return "attribute";
    case TokenKind::Key: return "key";
    case TokenKind::Tag: return "tag";
    case TokenKind::Inserted: return "inserted";
    case TokenKind::Deleted: return "deleted";
  }
  return "unknown";
}

/// 着色片段：相对输入 `code` 的字节半开区间 `[begin, end)`。
struct Token {
  TokenKind kind{TokenKind::Plain};
  std::size_t begin{0};
  std::size_t end{0};
};

/// 一门语言的**词法规则**（全部字段可自定义；空表/关开关即关闭对应特性）。
///
/// 最小可用定义只需要名字 + 关键字表；其余留空即等价于「纯标识符 + 数字」的最小语言。
/// 字符串字段为**拥有语义**（`std::string`），因此规则可在运行时构造、修改、注册。
struct LanguageSpec {
  /// 规范名（建议小写，如 `mylang`）——注册表主键。
  std::string name{};
  /// 别名与**文件扩展名**（查找时一并匹配，全小写、不含点，如 `cpp`/`cxx`/`hpp`）。
  std::vector<std::string> aliases{};

  /// 关键字（含控制流与声明关键字）。
  std::vector<std::string> keywords{};
  /// 类型名（内建类型与常见泛型容器）。
  std::vector<std::string> types{};
  /// 内建常量/函数（`true`/`nullptr`/`len`…）。
  std::vector<std::string> builtins{};
  /// 定义关键字：其后紧跟的标识符视为**函数/类型名**（如 `fn`/`def`/`func`/`class`）。
  std::vector<std::string> definition_keywords{};
  /// 属性/装饰器起始符（如 `@`）——置空即关闭该特性。
  std::string attribute_prefix{};
  /// 翻译/注解关键字（Java/C# 风格，如写 `@` 之外的词形）。留空表示不用。

  /// 行注释起始符（如 `//`），空 = 无。
  std::string line_comment{};
  /// 第二种行注释起始符（如 SQL `--`、Lua `--`），空 = 无。
  std::string line_comment_alt{};
  /// 块注释定界（如 `/*` … `*/`）；两者须同时非空才启用。
  std::string block_comment_begin{};
  std::string block_comment_end{};
  /// 标记语言标签定界（如 `<` … `>`）；同时非空即启用「标签内着色 + 属性识别」。
  std::string tag_begin{};
  std::string tag_end{};
  /// 原始字符串前缀（如 `R"` / `r#"` / `@"` / `$"`）——命中即按定界符吃到匹配的收尾。
  std::vector<std::string> string_prefixes{};

  bool double_quote{true};       ///< `"…"` 字符串
  bool single_quote{true};       ///< `'…'` 字符串
  bool backtick_string{false};   ///< `` `…` ``（JS 模板串 / Shell 命令替换）
  bool triple_quote{false};      ///< `"""…"""` / `'''…'''`（Python 风格，可跨行）
  bool multiline_quoted{false};  ///< 普通引号串是否允许跨行（否则未闭合止于行尾）
  bool escape_in_single{true};   ///< 单引号串内 `\` 是否转义（Shell 单引号不转义）
  bool preprocessor{false};      ///< `#` 行首视为预处理指令
  bool function_call{true};      ///< 标识符后随 `(` 视为函数名
  bool case_insensitive{false};  ///< 关键字/类型/内建匹配是否忽略大小写（SQL/HTML）
  bool key_value_keys{false};    ///< 标识符后随 `:`（或 `=`）视为对象键 → `Key`
  bool css_like{false};          ///< CSS 风格：选择器/属性名/`#hex` 颜色按 `Key`/`Number` 处理
  bool markdown_like{false};     ///< Markdown 风格：行首 `#`/`>`/`-`/`*`/``` 与强调标记着色
  bool diff_like{false};         ///< Diff 风格：`+`/`-`/`@@` 行整体着色
};

/// 语言注册表（内置语言 + 运行时注册）。
///
/// - `global()` 为进程级默认实例（`highlight(code, name)` 使用它）；
/// - 也支持自建实例（测试隔离、按工程定制语言集）；
/// - 所有方法线程安全；`find()` 返回 `shared_ptr`，**取到手后即使被注销也依然有效**
///   （避免"注册表变更导致调用方悬垂"这一类问题）。
class LanguageRegistry {
 public:
  LanguageRegistry();

  /// 注册语言：`replace=false` 时同名/同别名冲突返回 false 且不改动注册表；
  /// `replace=true` 时覆盖同名语言（自定义语言覆盖内置语言的唯一途径）。
  auto register_language(LanguageSpec spec, bool replace = false) -> bool;
  /// 注销（内置语言同样可注销，便于"只留我关心的几种"）。返回是否确实移除。
  auto unregister(std::string_view name) -> bool;

  /// 查找：规范名 / 别名 / 文件扩展名（大小写不敏感）。未找到返回空。
  [[nodiscard]] auto find(std::string_view name_or_alias) const
      -> std::shared_ptr<const LanguageSpec>;
  /// 全部语言的规范名（升序）。
  [[nodiscard]] auto names() const -> std::vector<std::string>;
  /// 全部规则（按规范名升序）。
  [[nodiscard]] auto specs() const -> std::vector<std::shared_ptr<const LanguageSpec>>;
  /// 已注册语言数量。
  [[nodiscard]] auto size() const -> std::size_t;
  /// 清空全部语言（测试用；`reset_to_builtin()` 恢复内置集合）。
  void clear();
  void reset_to_builtin();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

/// 进程级默认注册表（启动即装载内置语言）。
[[nodiscard]] auto global_languages() -> LanguageRegistry&;

/// 内置语言规则集（不依赖注册表，便于"另建一个只含内置的注册表"）。
[[nodiscard]] auto builtin_languages() -> std::vector<LanguageSpec>;

/// 用给定规则着色（纯函数）。
///
/// 保证：输出区间**升序、互不重叠**；未着色字节不属于任何 token；未知/空语言时调用方应对策略
/// 由上层决定（本函数要求规则有效；`highlight(code, name)` 在查不到语言时返回单个 `Plain` 覆盖全文）。
[[nodiscard]] auto highlight_with(std::string_view code, const LanguageSpec& spec)
    -> std::vector<Token>;

/// 按语言名（规范名/别名/扩展名）着色；查不到语言或空输入 → 单个 `Plain` token 覆盖全文。
[[nodiscard]] auto highlight(std::string_view code, std::string_view language) -> std::vector<Token>;

/// 从文件路径推断语言（按扩展名与常见无扩展名文件名：`Makefile`/`Dockerfile`/`CMakeLists.txt`…）。
[[nodiscard]] auto language_from_path(std::string_view path) -> std::optional<std::string>;

/// 全部可识别的语言名（规范名 + 别名 + 扩展名，升序去重；语言选择器与文档同步用）。
[[nodiscard]] auto supported_languages() -> std::vector<std::string>;

// ————————————————————————————————————————————————————————————————————————————
// 便捷构造：内置语言集的常用片段（自定义语言时可直接复用，少写样板）
// ————————————————————————————————————————————————————————————————————————————

/// 空规则（只有名字）。
[[nodiscard]] auto make_language(std::string name) -> LanguageSpec;
/// 追加关键字（链式风格）。
auto with_keywords(LanguageSpec& spec, std::initializer_list<std::string_view> words) -> LanguageSpec&;
auto with_types(LanguageSpec& spec, std::initializer_list<std::string_view> words) -> LanguageSpec&;
auto with_builtins(LanguageSpec& spec, std::initializer_list<std::string_view> words) -> LanguageSpec&;
auto with_definition_keywords(LanguageSpec& spec, std::initializer_list<std::string_view> words)
    -> LanguageSpec&;
auto with_aliases(LanguageSpec& spec, std::initializer_list<std::string_view> names) -> LanguageSpec&;
/// 设定 C 风格注释（`//` + `/* */`）。
auto with_c_comments(LanguageSpec& spec) -> LanguageSpec&;
/// 设定行注释（如 `#`）。
auto with_line_comment(LanguageSpec& spec, std::string_view marker) -> LanguageSpec&;
/// 设定块注释定界。
auto with_block_comment(LanguageSpec& spec, std::string_view begin, std::string_view end)
    -> LanguageSpec&;
/// 设定标记语言标签定界（HTML/XML 风格）。
auto with_tags(LanguageSpec& spec, std::string_view begin, std::string_view end) -> LanguageSpec&;

}  // namespace st::text
