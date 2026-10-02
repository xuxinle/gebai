// 霜天声明式 UI 示例：待办清单（JS 宿主，ArkTS 风格 + 异步取数 + key 对齐列表）。
//
// 用途：M4 的「真实复杂度声明式页」——一个表单 + 列表 + 派生统计 + 异步状态，
// 全部用声明式描述；控制通道可驱动验证（tree/get/invoke），与手搭页同一套语义。
//
// 用法：
//   todo-js --headless --frames 3 --control-port 0

#include <cstdio>
#include <memory>
#include <string>

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/declarative_host.hpp"
#include "st/ui/script_host.hpp"

namespace {

/// 待办页（ArkTS 风格：大写组件 + 链式修饰）。
constexpr std::string_view kTodoJs = R"JS(
// 假异步：模拟"从后端拉取列表"（微任务，宿主每帧泵）
const loadTodos = (filter) => Promise.resolve(
  [
    {id: 't1', text: '读霜天 DESIGN.md', done: true},
    {id: 't2', text: '写一个声明式页面', done: false},
    {id: 't3', text: '跑通控制通道验证', done: false},
  ].filter((item) => filter === 'all' || (filter === 'done') === item.done)
);

let draft = null;      // 输入框内容
let todos = null;      // 真实列表（本地增删）
let filter = null;     // 过滤条件

compose('TodoPage', () => {
  if (draft === null) draft = useState('');
  if (filter === null) filter = useState('all');
  if (todos === null) {
    todos = useState([]);
    // 初次装载：异步取数（结果落地自动触发重组）
    loadTodos('all').then((list) => { todos.value = list; });
  }

  const done_count = todos.value.filter((item) => item.done).length;
  const total = todos.value.length;

  return Column([
    Text('待办清单').prop('font_size', 20),
    Text(() => '已完成 ' + done_count + ' / ' + total),
    Row([
      Input(draft.value).onInput((text) => { draft.value = text; }).grow(),
      Button('添加').onClick(() => {
        const text = draft.value.trim();
        if (!text) return;
        todos.value = todos.value.concat([{id: 'n' + (total + 1), text: text, done: false}]);
        draft.value = '';
      }),
    ]).gap(8),
    Row([
      Button('全部').onClick(() => { filter.value = 'all'; }),
      Button('未完成').onClick(() => { filter.value = 'open'; }),
      Button('已完成').onClick(() => { filter.value = 'done'; }),
    ]).gap(6),
    Text(() => '当前过滤：' + filter.value),
    // key 对齐：勾选/删除时，其余项元素身份保持不变
    ForEach(
      todos.value.filter((item) => filter.value === 'all' || (filter.value === 'done') === item.done),
      (item) => item.id,
      (item) => Row([
        Checkbox(item.text, item.done).onChange(() => {
          todos.value = todos.value.map((row) =>
            row.id === item.id ? {id: row.id, text: row.text, done: !row.done} : row);
        }),
        Button('删除').onClick(() => {
          todos.value = todos.value.filter((row) => row.id !== item.id);
        }),
      ]).gap(8)
    ),
  ]).gap(10).padding(20);
});
)JS";

}  // namespace

auto run_app(int argc, char** argv) -> int {
  st::app::CommonOptions common;
  if (auto parsed = st::app::parse_common_options(argc, argv, common); !parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
    return 1;
  }
  common.app.title = "霜天声明式待办";
  common.app.enable_script = true;
  st::app::Application app("todo-js", "0.1.0", common.app);

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }
  auto* script = app.script();
  if (script == nullptr || !script->valid()) {
    std::fprintf(stderr, "脚本宿主未就绪\n");
    return 1;
  }
  auto decl = st::ui::DeclarativeHost::attach(*script, app.root());
  if (decl == nullptr) {
    std::fprintf(stderr, "声明式宿主接入失败\n");
    return 1;
  }
  if (auto status = decl->run(std::string(kTodoJs), "<todo-js>"); !status) {
    std::fprintf(stderr, "声明式代码执行失败: %s\n", status.error().message.c_str());
    return 1;
  }

  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    (void)decl->tick();
    app.tick();
    ++frames;
    if (common.max_frames > 0 && frames >= common.max_frames) break;
    if (common.max_ms > 0 &&
        st::time::now_ms() - frame_start_ms >= static_cast<std::int64_t>(common.max_ms)) {
      break;
    }
    app.pace_loop(frame_start_ms);
  }
  return 0;
}

ST_MAIN(run_app)
