// 纯 XML + JS 应用的逻辑（零 C++ UI 代码）。
//
// 分工：`ui/main.xml` 描述界面长什么样；本文件描述界面**怎么动**。
// 需要真实能力（持久化、跑命令、查库…）时调 C++——本示例通过 XML 建树找元素、
// 通过 `app.tasks.*`（宿主用 `ScriptBridge::expose` 暴露）落数据。
//
// 可用的宿主 API（都由 C++ 侧提供，见 main.cpp）：
//   app.tasks.list()            → [{text, done}, …]
//   app.tasks.add(text)         → 新列表
//   app.tasks.toggle(index)     → 新列表
//   app.tasks.remove_done()     → 新列表
//   app.tasks.count()           → {total, done}

(function () {
  'use strict';

  let filter = 'all';      // all | open | done
  let todos = app.tasks.list();

  // ── 渲染：把数据变成清单里的行 ────────────────────────────────────────────
  //
  // 这一段的写法有意保持"朴素"：它演示的是**用 XML 定结构 + 用 JS 填数据**，
  // 而不是引入一套前端框架。C++ 侧提供 `ui.build` 建子元素、`ui.clear` 清空容器、
  // `ui.set` 改属性——三个原语就够表驱动界面用了。

  function visible(item) {
    if (filter === 'open') return !item.done;
    if (filter === 'done') return item.done;
    return true;
  }

  function render() {
    const list = $('#list');
    list.clear();
    let index = 0;
    for (const item of todos) {
      const rowIndex = index++;
      if (!visible(item)) continue;
      // 每一行 = 勾选框 + 文本 + 删除按钮（都是框架组件，不是自绘 HTML）。
      list.append('Row', { gap: '8', padding_y: '4', align: 'center', id: 'row-' + rowIndex });
      $('#row-' + rowIndex).append('Checkbox', {
        checked: item.done ? 'true' : 'false',
        id: 'check-' + rowIndex,
      });
      $('#row-' + rowIndex).append('Text', {
        id: 'text-' + rowIndex,
        grow: 'true',
        text: item.text,
        text_color: item.done ? '#9ca3af' : '',
      });
      $('#row-' + rowIndex).append('Button', {
        id: 'del-' + rowIndex,
        label: '删除',
        variant: 'ghost',
        size: 'small',
      });
      // 事件：勾选切换完成态、删除移除该项。
      //
      // ⚠ 事件名是 **`click`**（框架的事件表里有 Click/DoubleClick/KeyDown…但**没有 Change**）。
      // 写 `'change'` 不会报错——只是永远不触发（`on` 只存"选择器 + 事件名"，
      // 宿主按事件名匹配分发）。这是本示例踩过的一次：点了勾选框计数不动。
      $('#check-' + rowIndex).on('click', () => {
        todos = app.tasks.toggle(rowIndex);
        render();
      });
      $('#del-' + rowIndex).on('click', () => {
        todos = app.tasks.remove(rowIndex);
        render();
      });
    }
    const counts = app.tasks.count();
    $('#counter').set({ text: '共 ' + counts.total + ' 项 · 完成 ' + counts.done });
    $('#progress').set({ value: counts.total === 0 ? 0 : Math.round(counts.done * 100 / counts.total) });
  }

  // ── 输入：回车或点「添加」 ────────────────────────────────────────────────
  function commit() {
    const text = ($('#draft').value || '').trim();
    if (text === '') return;
    todos = app.tasks.add(text);
    $('#draft').set({ value: '' });
    render();
  }
  $('#add').on('click', commit);
  $('#draft').on('keydown', (event) => {
    if (event.key === 'Enter') commit();
  });

  // ── 过滤 ────────────────────────────────────────────────────────────────
  function set_filter(next) {
    filter = next;
    const active = { variant: 'soft' };
    const idle = { variant: 'ghost' };
    $('#filter-all').set(next === 'all' ? active : idle);
    $('#filter-open').set(next === 'open' ? active : idle);
    $('#filter-done').set(next === 'done' ? active : idle);
    render();
  }
  $('#filter-all').on('click', () => set_filter('all'));
  $('#filter-open').on('click', () => set_filter('open'));
  $('#filter-done').on('click', () => set_filter('done'));
  $('#clear-done').on('click', () => {
    todos = app.tasks.remove_done();
    render();
  });

  render();
})();
