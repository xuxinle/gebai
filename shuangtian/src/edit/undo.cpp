#include "st/edit/undo.hpp"

#include <algorithm>
#include <chrono>

namespace st::edit {

namespace {

/// 当前时刻（毫秒）。抽出来是为了让合并窗口的判定在测试里可注入固定时间。
[[nodiscard]] auto now_ms() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

auto UndoStack::merge_window_open(std::int64_t now) const noexcept -> bool {
  if (!policy_.enabled) return false;
  if (!merge_armed_) return false;   // 撤销/重做/清空之后是新的一段，不复用旧的合并窗口
  if (commands_.empty() || cursor_ == 0U) return false;   // 栈顶没有「已做」的命令可并
  if (policy_.window_ms <= 0) return true;                // 不限时
  return (now - last_push_ms_) <= policy_.window_ms;
}

auto UndoStack::push(std::unique_ptr<Command> command, std::optional<std::int64_t> at_ms) -> bool {
  if (command == nullptr) return false;
  // **空命令不入栈**：这是唯一一处做这个判断的地方（见头文件）。
  // 放在这里而不是散在各调用点，是因为「原地重复落笔」这种情形每个应用都会有，
  // 而漏掉它的表现是「按撤销画面不动」——用户会判定撤销坏了。
  if (command->is_empty()) return false;

  const std::int64_t now = at_ms.value_or(now_ms());

  // 合并窗口：只尝试并进**栈顶那条已做**的命令。
  //
  // ⚠ 这里必须先判 `cursor_ == 0`（无已做命令）再取 `commands_[cursor_ - 1]`——
  // 少了这一步在「撤销到底后再输入」时会取到 `commands_[-1]`（越界）。
  if (merge_window_open(now) && cursor_ > 0U) {
    Command& top = *commands_[cursor_ - 1];
    if (top.merge(*command)) {
      last_push_ms_ = now;
      merge_armed_ = true;
      return true;   // 已吸收：不新增条目，游标不动
    }
  }

  // **历史中间落新命令 → 丢弃未来**：截到游标位置。
  // 这一条只有在本层实现才不会被某个调用方漏掉；漏掉的表现是「撤销三步后再画一笔，
  // 重做却回到旧的三步」——与眼前状态对不上，且没有任何报错。
  if (cursor_ < commands_.size()) {
    commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_), commands_.end());
  }
  commands_.push_back(std::move(command));
  cursor_ = commands_.size();
  last_push_ms_ = now;
  merge_armed_ = true;
  return true;
}

auto UndoStack::apply(std::unique_ptr<Command> command, std::optional<std::int64_t> at_ms) -> bool {
  if (command == nullptr) return false;
  // 先执行再入栈：`redo` 是「把目标改成执行之后的状态」，所以首次执行也用它。
  command->redo(*context_);
  return push(std::move(command), at_ms);
}

auto UndoStack::undo() -> bool {
  if (!can_undo()) return false;
  --cursor_;
  commands_[cursor_]->undo(*context_);
  // 撤销打破「连续序列」：之后接着输入的是新的一段（见头文件 `merge_armed_` 的说明）。
  merge_armed_ = false;
  return true;
}

auto UndoStack::redo() -> bool {
  if (!can_redo()) return false;
  commands_[cursor_]->redo(*context_);
  ++cursor_;
  merge_armed_ = false;
  return true;
}

void UndoStack::clear() noexcept {
  commands_.clear();
  cursor_ = 0U;
  last_push_ms_ = 0;
  merge_armed_ = false;
}

auto UndoStack::entries() const -> std::vector<HistoryEntry> {
  std::vector<HistoryEntry> out;
  out.reserve(commands_.size());
  for (std::size_t index = 0; index < commands_.size(); ++index) {
    out.push_back(HistoryEntry{.name = commands_[index]->name(), .done = index < cursor_});
  }
  return out;
}

auto UndoStack::names() const -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(cursor_);
  for (std::size_t index = 0; index < cursor_; ++index) {
    out.push_back(commands_[index]->name());
  }
  return out;
}

}  // namespace st::edit
