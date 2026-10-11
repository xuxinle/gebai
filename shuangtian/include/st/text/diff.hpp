#pragma once

/// **结构化差异**（`DESIGN.md` §4.6）：把「两个版本的文本差在哪」变成**数据**，而不是一坨 patch 文本。
///
/// ## 为什么要有这一层（对标歌白文件工作台的差异视图）
///
/// gbcode 先前的差异是「把 `git diff` 的 unified patch 塞进一个只读编辑器」——
/// 靠 `text::highlight` 的 `diff_like` 给整行着色。它只能**看**：
/// 没有行号映射、没有两栏对齐、没有「下一个差异」、没有「这块是新增/删除/替换」、
/// 更不能按块操作（逐块暂存 / 还原 / 接受左侧）。这些正是 IDEA 与歌白工作台
/// 差异视图的全部内容。
///
/// 本层提供它们**共同的地基**：一份与渲染无关的结果。
/// 上层（`ui::DiffView`、gbcode 的比较视图、将来的 gutter、逐块暂存）都读它，
/// 于是「同一个差异」在列表、并列视图、导出 patch 三处**结构同源**，
/// 不会出现「并列视图按块导航、导出却按另一套行号」这种分叉。
///
/// ## 三层坐标系（混一次就全错，这是本文件最重要的约定）
///
/// | 层 | 坐标系 | 谁在用 |
/// |---|---|---|
/// | `DiffLine` | 两侧各自的**行号（1 起）**，`0` 表示本侧不存在 | 渲染、跳转、逐块操作 |
/// | `DiffRow` | 两栏视图的**行**（0 起的数组下标） | 并列视图排版（左栏/右栏各指回一个 `DiffLine`） |
/// | unified patch | `@@ -a,b +c,d @@` 头里的行号 | `parse_unified` 的输入、`to_unified` 的输出 |
///
/// 三者的换算只在本文件里做。**不要**让调用方自己从 `DiffLine` 的下标推行号：
/// 那是「纯删除块在改侧没有行号」（`new_line == 0`）这类语义最容易出错的地方。
///
/// ## 与 git 的关系
///
/// 本层**不认识 git**：输入是两段文本或一段 unified patch 文本，输出是行级结构。
/// 「哪两端」「是不是三点 merge-base」「根提交要不要当空树」是调用方（git 服务）的事。
/// 这条分界是有意的——它让本层能被纯文本用例完整覆盖，也让 git 之外的场景
///（文件对比、配置对比、暂存区对比、将来三方合并的 base 一侧）直接复用。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::text {

/// 一行的差异类型。
enum class DiffLineKind : std::uint8_t {
  Context,  ///< 两侧都有（未改动）
  Added,    ///< 只在新侧
  Removed,  ///< 只在旧侧
};

/// 差异里的一行。
///
/// `old_line` / `new_line` **1 起**，`0` = 「本行在那一侧不存在」：
/// - `Context`：两侧都有行号；
/// - `Added`：`old_line == 0`；
/// - `Removed`：`new_line == 0`。
struct DiffLine {
  DiffLineKind kind{DiffLineKind::Context};
  std::string text{};       ///< 行内容（**不含换行符**）
  std::size_t old_line{0};  ///< 旧侧行号（1 起；0 = 旧侧没有这一行）
  std::size_t new_line{0};  ///< 新侧行号（1 起；0 = 新侧没有这一行）
  /// 该侧文件在**这一行**没有末行换行（git 的 `\ No newline at end of file`）。
  /// 这是「两个文件末尾换行不同」唯一能被表达出来的地方——纯行内容比较看不见它。
  bool no_newline{false};

  /// **行内字符区间**（`[begin, end)`，字节偏移；空 = 无行内差异）。
  ///
  /// 为何要它：纯行级差异下「改一个词」表现为「整行删 + 整行加」，视图里看不出到
  /// 底改了什么。有了它，编辑器能在行内再叠一层底色（VSCode/IDEA 的“字级高亮”）。
  ///
  /// 只对**配对后的**改行有意义（见 `pair_and_refine`）；`Context` 行恒为空。
  /// 两侧各自算各自的区间：旧侧是“被改掉的那段”，新侧是“新写成的那段”，
  /// 两者的长度与位置都不一致（这正是它需要两个字段而不是一个的原因）。
  std::vector<std::pair<std::size_t, std::size_t>> inline_old{};
  std::vector<std::pair<std::size_t, std::size_t>> inline_new{};

  /// 相等比较：`dsl::State<T>` 靠它判“要不要重组”（差异数据变没变）。
  auto operator==(const DiffLine& other) const -> bool = default;
};

/// 一个差异块（git 的 hunk）。
struct DiffHunk {
  std::size_t old_start{0};  ///< 旧侧起始行号（1 起；0 行 = 空侧，见 `old_count == 0`）
  std::size_t old_count{0};
  std::size_t new_start{0};
  std::size_t new_count{0};
  /// `@@ … @@` 之后的**函数上下文**（git 会填「这一块落在哪个函数里」）。
  /// 两段文本直接比较时为空；解析 patch 时原样带出（差异视图用它做 hunk 标题）。
  std::string section{};
  std::vector<DiffLine> lines{};

  auto operator==(const DiffHunk& other) const -> bool = default;
};

/// 两栏（并列）视图的一行：左栏与右栏各指回所属 hunk 里的一个 `DiffLine`。
///
/// `-1` = 该侧这一行要画**填充**（对齐用的空行），这是并列视图的关键语义：
/// 「删 3 行、加 1 行」必须占 3 行，否则两侧会错位——而错位后人眼无法对照。
struct DiffRow {
  std::size_t hunk{0};       ///< 所属 hunk 下标（0 起）
  std::ptrdiff_t left{-1};   ///< 左栏在本 hunk `lines` 中的下标；-1 = 填充
  std::ptrdiff_t right{-1};  ///< 右栏同上
};

/// 一个文件的差异。
struct FileDiff {
  std::string old_path{};  ///< 旧路径（新增文件为空）
  std::string new_path{};  ///< 新路径（删除文件为空）
  bool added{false};       ///< 整文件新增
  bool deleted{false};     ///< 整文件删除
  bool renamed{false};     ///< 重命名/复制
  bool binary{false};      ///< 二进制（无行级内容——不要拿它当"没有差异"）
  /// 降级/异常原因（**空 = 一切正常**）。非空时 `hunks` 可能为空，
  /// 而「为空」不等于「没有差异」：见 `message` 的文案。
  std::string message{};
  std::vector<DiffHunk> hunks{};
  std::size_t additions{0};  ///< 新增行数
  std::size_t deletions{0};  ///< 删除行数

  /// 有没有可逐行展示的差异。
  [[nodiscard]] auto has_line_changes() const noexcept -> bool { return !hunks.empty(); }

  /// 相等比较：`dsl::State<T>` 靠它判“要不要重组”（差异数据变没变）。
  /// 字段全比——差异本体不大（结构 + 行文本），而“以为没变”会让视图停在旧内容上。
  auto operator==(const FileDiff& other) const -> bool = default;
};

/// 一份差异文档（可能含多个文件：`git diff` 的输出就是多文件）。
struct DiffDocument {
  std::vector<FileDiff> files{};
  std::size_t additions{0};
  std::size_t deletions{0};
  bool binary{false};  ///< 任一文件是二进制
  /// 解析期**如实**留下的告警（如「hunk 头声明 5 行，实际 3 行」）。
  ///
  /// 为什么要有：patch 是外部输入（git 的输出、用户粘贴的文本），
  /// 静默忽略结构异常会让上层显示出一个「看起来正常但少了几行」的差异。
  std::vector<std::string> warnings{};

  [[nodiscard]] auto empty() const noexcept -> bool { return files.empty(); }
};

// ————————————————————————————————————————————————————————————————————————————
// 文本 → 结构
// ————————————————————————————————————————————————————————————————————————————

/// LCS 动态规划的单元上限（超出即退化为整块替换）。约 16 MiB（`uint32_t` 单元）。
inline constexpr std::size_t kMaxLcsCells = 4'000'000;

/// 差异块的默认上下文行数（与 git 默认一致）。
inline constexpr std::size_t kDefaultContext = 3;

/// 按行切分（**保留空行**，去掉行尾换行符）。
///
/// 语义与编辑器/表格同一口径：`"a\nb\n"` → `{"a", "b"}`（末尾换行不产生空行），
/// `"a\nb"` → `{"a", "b"}`，`""` → `{}`，`"\n"` → `{""}`。
/// 行尾的 `\r` 一并去掉（Windows 工作区读到的 CRLF 不该被当成内容差异）。
[[nodiscard]] auto split_lines(std::string_view text) -> std::vector<std::string_view>;

/// **两段文本 → 差异块**（不经过 git：本地比较、未保存内容对照、将来三方合并的 base 侧都用它）。
///
/// - 行匹配用**最长公共子序列**，因此「中间插一行」只会产生 1 行新增，而不是「后面全改了」；
/// - 差异行按 git 的惯例排：**先全部删除、再全部新增**（同一块内）；
/// - 上下文各留 3 行（与 git 默认一致），相邻/重叠的改动块合并成一个 hunk；
/// - 文件末尾换行不同（`"a\n"` vs `"a"`）会产出一个 hunk——纯行内容比较看不见它，
///   而这类差异在提交界面上必须看得见；
/// - **规模上限**：两侧行数乘积超过 `kMaxLcsCells` 时**不再求 LCS**，
///   退化为「整文件替换」一个 hunk（如实反映"差异很大"，不做静默截断）。
[[nodiscard]] auto diff_hunks(std::string_view old_text, std::string_view new_text,
                              std::size_t context = kDefaultContext) -> std::vector<DiffHunk>;

/// 把 hunk 交错成**两栏行**（并列视图排版用）。
///
/// `hunk_index` 原样写进每行的 `hunk` 字段（调用方在整篇文档里串联多 hunk 时用）。
[[nodiscard]] auto build_rows(const DiffHunk& hunk, std::size_t hunk_index)
    -> std::vector<DiffRow>;

/// **行内字符级差异**：给定一对「旧行 / 新行」，算出各自被改动的区间。
///
/// 返回 `{old_ranges, new_ranges}`（字节偏移 `[begin, end)`，已排序、已合并相邻）。
///
/// 算法与行级差异**同一套 LCS**（只是元素从行变成了 UTF-16 码位）——不一致的算法会让
/// “行级说没改、字符级说改了”这类矛盾出现。按码位而不是字节切：UTF-8 的字节级 LCS
/// 会把一个汉字切碎，区间落到半个字符上，编辑器的高亮就会错位。
///
/// 规模上限与行级同姿态：超过 `kMaxInlineCells` 就**不做字符级**（返回空区间 = 退化为
/// 整行高亮），而不是静默给一个看着对、实际错的区间。
[[nodiscard]] auto diff_inline(std::string_view old_line, std::string_view new_line)
    -> std::pair<std::vector<std::pair<std::size_t, std::size_t>>,
                 std::vector<std::pair<std::size_t, std::size_t>>>;

/// 把一段差异里**相邻的删除行与新增行配对**，并为每对填上行内区间（`inline_old`/`inline_new`）。
///
/// 为何需要“配对”：字符级差异只在“这两行是同一行的两个版本”时才有意义。git 的
/// 排法是一块里**先全删、再全加**，所以配对规则是「同一 hunk 内第 i 对删/加行」；
/// 两侧数量不等时，多出来的那些**不配对**（保持纯行级高亮）——按顺序硬配会把
/// “新增了 3 行”误读成“这 3 行都是别行的改动”。
///
/// 原地修改传入的各 hunk（`DiffLine` 的区间字段）。
void refine_inline_diffs(std::vector<DiffHunk>& hunks);

/// 便捷：两段文本 → 一份单文件差异（含增删统计与末尾换行标记）。
[[nodiscard]] auto make_file_diff(std::string_view old_text, std::string_view new_text,
                                  std::string old_path = {}, std::string new_path = {})
    -> FileDiff;

/// 统计一组差异块的新增/删除行数。
[[nodiscard]] auto count_changes(const std::vector<DiffHunk>& hunks)
    -> std::pair<std::size_t, std::size_t>;

/// **解析 unified patch**（`git diff` 的标准输出；也可喂 `diff -u` 的产物）。
///
/// 覆盖：多文件、`@@` 头（含省略的 `,count`）、`---`/`+++` 路径、`new file mode` /
/// `deleted file mode` / `rename from|to`、`Binary files …` 与 `GIT binary patch`
///（→ `binary = true`）、`\ No newline at end of file`。
///
/// 两条容易做错、这里显式处理的规则：
/// 1. **`--- `/`+++ `/`diff ` 只在 hunk 之外才是头部**——hunk 里一行内容完全可能是
///    `-- 注释`（渲染成 `--- 注释`）。按行首前缀无条件判头部会把内容吃掉；
/// 2. **hunk 头是有权威的行号来源**，解析出的行数与之不符时按实际行收录、
///    并把差异写进 `warnings`（不静默）。
[[nodiscard]] auto parse_unified(std::string_view patch) -> DiffDocument;

// ————————————————————————————————————————————————————————————————————————————
// 结构 → 文本
// ————————————————————————————————————————————————————————————————————————————

/// 把 `FileDiff` 还原成 unified patch 文本（复制/导出/"应用这块补丁"的输入）。
[[nodiscard]] auto to_unified(const FileDiff& file) -> std::string;

/// 单个 hunk 的片段：`@@` 头 + 正文（**不含** `---`/`+++` 文件头）。
/// 要交给 `git apply` 时需自带文件头（用 `to_unified(FileDiff)`，或自行补两条）。
[[nodiscard]] auto to_unified(const DiffHunk& hunk) -> std::string;

/// **部分暂存**：从一段差异里挑出指定的行，构造一份能交给 `git apply --cached` 的补丁。
///
/// `selected` 的语义是「hunk.lines 的下标」（**不是**行号、也不是 hunk 下标）——
/// 那是调用方手里唯一稳定的身份（行号在改侧会为 0，见 `DiffLine` 的说明）。
/// 未选中的**新增行**被丢开，未选中的**删除行**要变成上下文（它仍存在，只是这次不删）；
/// 未选中的**上下文行**本身就不动。
///
/// 为何需要它：IDEA/VSCode 的核心肌肉记忆就是「只把这几行提交了」——而实现路径就是
/// 「构造只含选中改动的补丁 → `git apply --cached`」，所以本函数是那条路的地基。
///
/// 返回空串 = **选中的内容不足以形成有效补丁**（一行都没选，或只选了若干上下文行）：
/// 那种情况下硬造一个空 hunk 会让 `git apply` 报一个与用户意图无关的错。
///
/// `header` 非空时原样放在开头（用 `FileDiff` 拼 `--- a/x` `+++ b/x` 两条）；
/// 空 = 只返回 hunk 片段。
[[nodiscard]] auto build_partial_patch(const DiffHunk& hunk,
                                       const std::vector<std::size_t>& selected,
                                       std::string_view header = {}) -> std::string;

/// 与 `build_partial_patch` 同义，但直接对一份 `FileDiff` 工作（自己拼文件头）。
/// `selected` 是「文件内 hunk 下标 → 该 hunk 内选中的行下标」的映射；
/// 不存在的 hunk 下标被忽略（调用方不必先过滤）。
[[nodiscard]] auto build_partial_patch(const FileDiff& file,
                                       const std::vector<std::pair<std::size_t,
                                                                   std::vector<std::size_t>>>&
                                           selected) -> std::string;

}  // namespace st::text
