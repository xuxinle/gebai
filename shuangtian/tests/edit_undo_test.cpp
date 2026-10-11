/// `st::edit::UndoStack` 撤销栈原语测试。
///
/// 三条核心语义各自对应一个「静默错」的坑（见 `undo.hpp` 的表格），所以用例按它们组织：
/// ① 撤销/重做的基本链路；② **历史中间落新命令丢弃未来**；③ **空命令不入栈**；
/// ④ 合并策略（含「撤销后不复用旧合并窗口」这条最容易被漏的）。另外把「面板数据」
/// （`entries`/`cursor`）也钉住——它是历史面板的全部内容，错了界面就是假的。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/edit/undo.hpp"
#include "st/test/test.hpp"

namespace {

/// 测试用的最小文档：一个整数 + 一份操作日志（用来断言「命令到底跑没跑」）。
///
/// 为何要日志：光看 `value` 分不清「撤销了」与「重做了一个恰好相同的值」——
/// 而这类误判正是撤销栈最容易掩盖的错。
struct Doc : st::edit::CommandContext {
  int value{0};
  std::vector<std::string> log{};
};

/// 把 `value` 设成 `to` 的命令（`from` 在构造时记录，模拟「补丁式命令」）。
struct SetValue : st::edit::Command {
  SetValue(int from, int to, std::string label = "设置")
      : from_(from), to_(to), label_(std::move(label)) {}

  void undo(st::edit::CommandContext& context) override {
    auto& doc = st::edit::context_as<Doc>(context);
    doc.value = from_;
    doc.log.push_back("undo:" + label_);
  }
  void redo(st::edit::CommandContext& context) override {
    auto& doc = st::edit::context_as<Doc>(context);
    doc.value = to_;
    doc.log.push_back("redo:" + label_);
  }
  [[nodiscard]] auto name() const -> std::string override { return label_; }

  int from_{0};
  int to_{0};
  std::string label_{};
};

/// 空命令（`is_empty()` 恒真）：模拟「原地重复落笔，像素没变」。
struct Noop : st::edit::Command {
  void undo(st::edit::CommandContext&) override {}
  void redo(st::edit::CommandContext&) override {}
  [[nodiscard]] auto name() const -> std::string override { return "空操作"; }
  [[nodiscard]] auto is_empty() const -> bool override { return true; }
};

/// 可合并的命令：把连续两次「改同一个值」并成一步（模拟连续打字）。
struct Mergeable : st::edit::Command {
  Mergeable(int from, int to) : from_(from), to_(to) {}

  void undo(st::edit::CommandContext& context) override {
    st::edit::context_as<Doc>(context).value = from_;
  }
  void redo(st::edit::CommandContext& context) override {
    st::edit::context_as<Doc>(context).value = to_;
  }
  [[nodiscard]] auto name() const -> std::string override { return "连续输入"; }
  [[nodiscard]] auto merge(const st::edit::Command& next) -> bool override {
    // 只并同类：不同命令类型并起来会让撤销落到错误的目标上。
    const auto* other = dynamic_cast<const Mergeable*>(&next);
    if (other == nullptr) return false;
    to_ = other->to_;   // 吸收：终点延到后一条的终点
    return true;
  }

  int from_{0};
  int to_{0};
};

}  // namespace

ST_TEST(undo_stack_undo_redo_round_trip) {
  Doc doc;
  st::edit::UndoStack stack(doc);
  ST_CHECK(!stack.can_undo());
  ST_CHECK(!stack.can_redo());
  ST_CHECK(!stack.undo());   // 空栈上撤销：返回 false 且不动文档
  ST_CHECK(!stack.redo());
  ST_CHECK_EQ(doc.value, 0);

  // `push` = 「这一步已经做过了」：文档得先变，再入栈。
  doc.value = 1;
  ST_CHECK(stack.push(std::make_unique<SetValue>(0, 1)));
  doc.value = 2;
  ST_CHECK(stack.push(std::make_unique<SetValue>(1, 2)));
  ST_CHECK_EQ(stack.size(), std::size_t{2});
  ST_CHECK_EQ(stack.cursor(), std::size_t{2});

  // 撤销两次回到 0，重做两次回到 2。
  ST_CHECK(stack.undo());
  ST_CHECK_EQ(doc.value, 1);
  ST_CHECK(stack.undo());
  ST_CHECK_EQ(doc.value, 0);
  ST_CHECK(!stack.can_undo());
  ST_CHECK(stack.can_redo());
  ST_CHECK(stack.redo());
  ST_CHECK_EQ(doc.value, 1);
  ST_CHECK(stack.redo());
  ST_CHECK_EQ(doc.value, 2);
  ST_CHECK(!stack.can_redo());
  // 命令真的跑了（不是靠值恰好相等蒙混过关）。
  ST_CHECK_EQ(doc.log.size(), std::size_t{4});

  // `apply`：命令还没生效时用它（先执行再入栈）。
  doc.value = 5;
  ST_CHECK(stack.apply(std::make_unique<SetValue>(2, 5)));
  ST_CHECK_EQ(doc.value, 5);
  ST_CHECK_EQ(stack.size(), std::size_t{3});
}

ST_TEST(undo_stack_push_after_undo_drops_the_future) {
  // 「撤销三步、再画一笔」之后，重做链里**不得**还留着旧的三步：
  // 留着的话再按重做会回到「旧的三步」，与眼前状态对不上，而没有任何报错。
  Doc doc;
  st::edit::UndoStack stack(doc);
  for (int step = 1; step <= 3; ++step) {
    doc.value = step;
    ST_CHECK(stack.push(std::make_unique<SetValue>(step - 1, step, "第" + std::to_string(step))));
  }
  ST_CHECK(stack.undo());
  ST_CHECK(stack.undo());
  ST_CHECK_EQ(doc.value, 1);
  ST_CHECK_EQ(stack.cursor(), std::size_t{1});
  ST_CHECK(stack.can_redo());

  // 落一条新命令：未来（第 2、3 步）必须被丢掉。
  doc.value = 9;
  ST_CHECK(stack.push(std::make_unique<SetValue>(1, 9, "新分支")));
  ST_CHECK_EQ(stack.size(), std::size_t{2});    // 只剩「第1步」+「新分支」
  ST_CHECK_EQ(stack.cursor(), std::size_t{2});
  ST_CHECK(!stack.can_redo());                  // 未来没了
  ST_CHECK_EQ(stack.entries().size(), std::size_t{2});
  ST_CHECK_EQ(stack.entries()[1].name, std::string("新分支"));

  // 撤销回去是 1（第 1 步之后的状态），而不是被丢掉的第 2 步。
  ST_CHECK(stack.undo());
  ST_CHECK_EQ(doc.value, 1);
}

ST_TEST(undo_stack_skips_empty_commands) {
  // 「原地重复落笔、像素没变」不该进历史：进了的话面板上出现一串「画笔」，
  // 连按撤销画面却不动——用户会判定撤销坏了。
  Doc doc;
  st::edit::UndoStack stack(doc);
  doc.value = 1;
  ST_CHECK(stack.push(std::make_unique<SetValue>(0, 1)));
  ST_CHECK(!stack.push(std::make_unique<Noop>()));
  ST_CHECK(!stack.push(nullptr));
  ST_CHECK_EQ(stack.size(), std::size_t{1});   // 只有那一条真命令
  ST_CHECK_EQ(stack.cursor(), std::size_t{1});
  // 空命令也**不改变**游标/未来（否则「撤销到底再落空命令」会把未来吃掉）。
  ST_CHECK(stack.undo());
  ST_CHECK(!stack.push(std::make_unique<Noop>()));
  ST_CHECK(stack.can_redo());
}

ST_TEST(undo_stack_merge_policy_windows_and_arming) {
  Doc doc;
  st::edit::UndoStack stack(doc);
  // 默认不合并：连续两步各占一条历史。
  doc.value = 1;
  ST_CHECK(stack.push(std::make_unique<Mergeable>(0, 1), 1000));
  doc.value = 2;
  ST_CHECK(stack.push(std::make_unique<Mergeable>(1, 2), 1010));
  ST_CHECK_EQ(stack.size(), std::size_t{2});

  // 打开合并 + 时间窗：窗口内的同类命令并成一条。
  stack.set_merge_policy(st::edit::MergePolicy{.enabled = true, .window_ms = 500});
  doc.value = 3;
  ST_CHECK(stack.push(std::make_unique<Mergeable>(2, 3), 2000));
  doc.value = 4;
  ST_CHECK(stack.push(std::make_unique<Mergeable>(3, 4), 2100));
  const std::size_t after_merge = stack.size();
  ST_CHECK_EQ(after_merge, std::size_t{3});   // 3 与 4 并成了一条

  // 撤销那一条回到 2（合并的起点），而不是 3——这正是「并成一步」的含义。
  ST_CHECK(stack.undo());
  ST_CHECK_EQ(doc.value, 2);

  // **撤销后不复用旧合并窗口**：撤销之后接着输入的是新的一段。
  // 只看时间窗会把这段悄悄并进那个刚被撤销过的命令里，效果与预期差一大截。
  stack.set_merge_policy(st::edit::MergePolicy{.enabled = true, .window_ms = 100'000});
  ST_CHECK(stack.redo());
  ST_CHECK_EQ(doc.value, 4);
  doc.value = 5;
  ST_CHECK(stack.push(std::make_unique<Mergeable>(4, 5), 2150));
  ST_CHECK_EQ(stack.size(), after_merge + 1U);   // 新的一条，没并进旧窗口

  // 超出时间窗也不合并。
  doc.value = 6;
  ST_CHECK(stack.push(std::make_unique<Mergeable>(5, 6), 900'000));
  ST_CHECK_EQ(stack.size(), after_merge + 2U);
}

ST_TEST(undo_stack_history_entries_mark_done_and_future) {
  // 历史面板要的那份数据：既列已做的，也列「还能重做什么」。
  Doc doc;
  st::edit::UndoStack stack(doc);
  for (int step = 1; step <= 3; ++step) {
    doc.value = step;
    ST_CHECK(stack.push(std::make_unique<SetValue>(step - 1, step, "动作" + std::to_string(step))));
  }
  ST_CHECK(stack.undo());
  const auto entries = stack.entries();
  ST_REQUIRE(entries.size() == 3U);
  ST_CHECK(entries[0].done);
  ST_CHECK(entries[1].done);
  ST_CHECK(!entries[2].done);   // 未来：可重做
  ST_CHECK_EQ(entries[2].name, std::string("动作3"));
  // `names()` 只给已做的（「撤销到某一步」这类面板用）。
  const auto past = stack.names();
  ST_REQUIRE(past.size() == 2U);
  ST_CHECK_EQ(past[1], std::string("动作2"));

  // `clear()`：表达「从现在起这是一份新文档」——不回滚已做的修改。
  const int before = doc.value;
  stack.clear();
  ST_CHECK_EQ(stack.size(), std::size_t{0});
  ST_CHECK(!stack.can_undo());
  ST_CHECK(!stack.can_redo());
  ST_CHECK(stack.entries().empty());
  ST_CHECK_EQ(doc.value, before);   // 文档没被回滚
}
