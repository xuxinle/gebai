#pragma once

/// **撤销/重做栈**（`st::edit`）：把「可回退的编辑历史」做成与文档模型无关的原语。
///
/// ## 为什么它值得在框架层
///
/// 镂月裁云（`examples/louyue`）先写过一份完整的命令式撤销：`Command` + 像素补丁 +
/// 整层快照，含「历史中间落新命令丢弃未来」「空命令不入栈」「历史面板要的那份数据」。
/// 这些语义**与图像无关**——文本编辑器、表格、时间轴、配置界面要的是同一套，
/// 而它们每一条都对应一个「错了不会报错、只会悄悄错」的坑（见下表）。
/// 让每个应用各写一遍，等于把坑也各复制一遍。
///
/// | 语义 | 不做会怎样（都是静默错） |
/// |---|---|
/// | **历史中间落新命令 → 丢弃未来** | 撤销三步后再画一笔，重做链里还留着旧的三步；再按重做，画面回到「旧的三步」——与用户眼前的当前状态对不上，而没有任何报错 |
/// | **空命令不入栈** | 原地重复落笔（像素没变）也进历史：面板上出现一串「画笔」，连按撤销画面却不动，用户会判定「撤销坏了」 |
/// | **游标与条目一起可查** | 只有 `size()` 是画不出历史面板的——面板要显示「现在停在第几步、后面还有几步可重做」 |
///
/// ## 与 `ui::CodeEditor` 内部那份的关系（为什么不合并）
///
/// 编辑器组件自己有一份 `push_undo` + 整文本快照。它**不迁到本层**：它的「一步」
/// 是组件内部状态（文本 + 光标 + 选区）的快照，粒度与合并窗口由编辑器手感决定，
/// 而且它不对外提供历史查询。本层服务的是**应用自己的文档模型**——那才是
/// 「命令对象」有意义的地方（应用知道该存像素补丁还是整层快照，组件不知道）。
///
/// ## 契约（用之前必须知道的三条）
///
/// 1. **`push` 表示「这一步已经做过了」**：它不再执行一次。先落笔、再拿前后状态入栈，
///    是补丁式命令的常态（louyue 的每一笔如此）。要「执行并记录」用 `apply`。
/// 2. **目标必须比栈活得久**：栈只存目标的引用（`CommandContext&`）。
/// 3. **命令的 `undo`/`redo` 必须可重入**：撤销后又重做、重做后又撤销是常规路径。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace st::edit {

/// 命令作用的**目标**（文档/模型）的公共基类。
///
/// 为什么是「基类 + 单点向下转换」而不是把撤销栈做成模板：模板会把整份实现
/// （游标维护、未来丢弃、合并窗口）塞进每个应用的头文件里，而这段逻辑恰恰是
/// 最不该被复制的那部分；它一旦进头文件，也就无法脱离任何应用单独测试。
/// 代价是命令实现里出现一次 `context_as<具体类型>`——但命令**本来就**知道自己
/// 操作的是哪种文档，这个转换点是它天然该有的信息。
///
/// 用法：`class Document : public st::edit::CommandContext { ... };`
class CommandContext {
 public:
  CommandContext() = default;
  virtual ~CommandContext() = default;
};

/// 把命令上下文取回**具体类型**（与 `ui::Element::owner_as` 同一套路）。
///
/// ⚠ 不做运行期类型检查（不引入 RTTI 开销与动态库边界问题）：调用方必须是
/// 「往这条栈里放的就是这个类型的命令」——栈与文档是一对一的，这一点由构造方式保证。
template <class T>
[[nodiscard]] auto context_as(CommandContext& context) -> T& {
  return static_cast<T&>(context);
}
template <class T>
[[nodiscard]] auto context_as(const CommandContext& context) -> const T& {
  return static_cast<const T&>(context);
}

/// 一条可撤销的命令。
///
/// 接口刻意窄：`undo`/`redo` 各自把状态换回去。具体是「像素补丁」还是「整层快照」
/// 由实现决定——撤销栈不关心，这正是它跨应用可复用的前提。
class Command {
 public:
  virtual ~Command() = default;

  /// 撤销：把目标恢复到本命令执行**之前**的状态。
  virtual void undo(CommandContext& context) = 0;
  /// 重做（以及 `apply` 时的首次执行）：把目标改成执行**之后**的状态。
  virtual void redo(CommandContext& context) = 0;

  /// 历史面板上显示的名字。用**用户看得懂的动作词**（「画笔」而不是「PixelPatch」）：
  /// 这一串字符串就是撤销面板的全部内容，写成类名等于没写。
  [[nodiscard]] virtual auto name() const -> std::string = 0;

  /// 本命令是否**没有造成任何变化**。
  ///
  /// 「无变化」只有命令自己知道（像素前后一致、图层快照逐位相同…），
  /// 所以判定放在这里，而**执行丢弃**放在栈里（见 `UndoStack::push`）——
  /// 应用各处不再需要各写一次 `if (!changed()) return;`。
  [[nodiscard]] virtual auto is_empty() const -> bool { return false; }

  /// 尝试把**紧随其后**的同一条命令并入自己（连续打字 = 一步）。
  ///
  /// 返回 `true` 表示已吸收，`next` 随即被栈丢弃。默认不合并——**合并是少数派**：
  /// 一笔画、一次图层删除天然是独立的一步，只有「同一动作的连续重复」才该并。
  ///
  /// 实现里通常需要认出 `next` 的具体类型（`dynamic_cast`）并比对「是不是同一个目标」：
  /// 把两笔不同图层的绘制并起来会让撤销错位。类型不符就返回 `false`。
  [[nodiscard]] virtual auto merge(const Command& next) -> bool {
    (void)next;
    return false;
  }
};

/// 合并策略：把连续同类操作并成一条（如连续打字、连续调同一个滑杆）。
///
/// 为什么要**时间窗**而不只靠「同类」：仅凭同类会把「敲一行、想两分钟、再敲一行」
/// 并成一步，而用户按一次撤销想退回的是「刚写的那段」，不是「从上次停顿起的全部」。
struct MergePolicy {
  /// 默认**关**：多数编辑操作期望「一次操作 = 一步历史」，合并是显式选择。
  bool enabled{false};
  /// 相邻两步的**最大间隔**（毫秒）；超过即不合并。`<= 0` = 不限时。
  std::int64_t window_ms{0};
};

/// 历史面板的一条。
struct HistoryEntry {
  std::string name{};
  /// `true` = 已做（游标左侧）；`false` = 可重做（游标右侧的「未来」）。
  ///
  /// 两种都要给：面板既要列出已做的步骤，也要在撤销后把「还能重做什么」显示出来，
  /// 否则用户不知道重做键是不是有东西可重做。
  bool done{false};
};

/// 撤销栈：命令列表 + 游标 + 合并策略 + 历史查询。
///
/// 游标语义：`[0, cursor)` 是**已做**的步骤，`[cursor, size)` 是可重做的「未来」。
/// 「历史中间落新命令丢弃未来」就是 `push` 时把列表截到 `cursor`——
/// 这条规则只有在这一层实现，才不会被某个调用方漏掉。
class UndoStack {
 public:
  /// 绑定命令作用的目标。⚠ 目标必须比栈活得久（栈只存引用）。
  explicit UndoStack(CommandContext& context) noexcept : context_(&context) {}

  UndoStack(const UndoStack&) = delete;
  auto operator=(const UndoStack&) -> UndoStack& = delete;
  UndoStack(UndoStack&&) = delete;
  auto operator=(UndoStack&&) -> UndoStack& = delete;

  void set_merge_policy(MergePolicy policy) noexcept { policy_ = policy; }
  [[nodiscard]] auto merge_policy() const noexcept -> MergePolicy { return policy_; }

  /// 记入一条**已经生效**的命令（不重复执行）。返回是否真的建立了历史条目。
  ///
  /// 返回 `false` 的两种情况——都**不**改变历史：
  /// - `command == nullptr`；
  /// - 命令自己是空命令（`is_empty()`）；同时**这是唯一一处**执行「空命令不入栈」的地方。
  ///
  /// `at_ms` 只参与合并窗口判定；省略则取当前时刻（测试可传固定值以脱离时钟）。
  [[nodiscard]] auto push(std::unique_ptr<Command> command,
                          std::optional<std::int64_t> at_ms = std::nullopt) -> bool;

  /// 执行（`redo`）并记入。给「命令还没生效」的调用方用。
  [[nodiscard]] auto apply(std::unique_ptr<Command> command,
                           std::optional<std::int64_t> at_ms = std::nullopt) -> bool;

  /// 撤销一步；无可撤销时返回 `false`（**不**改变目标）。
  [[nodiscard]] auto undo() -> bool;
  /// 重做一步；无可重做时返回 `false`。
  [[nodiscard]] auto redo() -> bool;

  /// 清空历史（如「新建文档」「重新载入」）。**不**回滚任何已做的修改——
  /// 它表达的是「从现在起这是一份新文档」，回滚是撤销的事。
  void clear() noexcept;

  [[nodiscard]] auto can_undo() const noexcept -> bool { return cursor_ > 0U; }
  [[nodiscard]] auto can_redo() const noexcept -> bool { return cursor_ < commands_.size(); }

  /// 条目总数（**含**可重做的「未来」）——历史面板的列表长度。
  [[nodiscard]] auto size() const noexcept -> std::size_t { return commands_.size(); }
  /// 当前游标位置：已做的步数，也是历史面板要标出的「当前位置」。
  [[nodiscard]] auto cursor() const noexcept -> std::size_t { return cursor_; }

  /// 全部条目（含未来），带 `done` 标记——历史面板的数据源。
  [[nodiscard]] auto entries() const -> std::vector<HistoryEntry>;
  /// 已做步骤的名字（`[0, cursor)`）。给「撤销到某一步」这类只关心过去的面板用。
  [[nodiscard]] auto names() const -> std::vector<std::string>;

 private:
  /// 现在允许把新命令并入栈顶吗（策略开关 + 连续序列 + 时间窗）。
  [[nodiscard]] auto merge_window_open(std::int64_t now_ms) const noexcept -> bool;

  CommandContext* context_{nullptr};
  std::vector<std::unique_ptr<Command>> commands_{};
  std::size_t cursor_{0};
  MergePolicy policy_{};
  /// 上一条命令落栈的时刻（合并窗口的起点；无命令时无意义）。
  std::int64_t last_push_ms_{0};
  /// 「连续序列」标记：中间发生过撤销/重做/清空就熄灭，直到下一次 `push`。
  ///
  /// 为什么要它（而不是只看时间窗与同类）：撤销之后接着输入是**新的一段**，
  /// 用户心里的「上一步」已经被他改主意了。仅凭时间窗会把这段悄悄并进
  /// 那个刚被撤销过的命令里，于是撤销一步的效果与预期差一大截。
  bool merge_armed_{false};
};

}  // namespace st::edit
