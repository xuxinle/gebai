// 霜天示例：用 JS 写组件控制逻辑（`codeeditor --enable-script` 启动时载入）。
//
// 这份文件的全部内容都是**编译期嵌入的真实资源**（`b::embed<"...">()`）：在编辑器里写、
// 有语法高亮、无需 C++ 原始字符串转义，构建时被嵌进可执行文件。
//
// 它演示两件事：
//   ① 「组件控制逻辑用 JS 写」比 C++ 短：下面这些联动若用 C++ 要写回调、连信号、管生命周期；
//   ② AI 可以用同一套 API 读写界面（`--enable-script` 后经控制通道 `script` 方法）。
//
// 性能约定：所有读写先在 JS 侧完成，**阶段末统一提交**（不是逐属性跨语言往返）。

// ── 1. 启动即生效：状态栏显示可用语言数与样例数 ──────────────────────────
$('#status').set({
  text: 'JS 逻辑已载入 · ' + count('Button') + ' 个按钮 · ' + count('CodeEditor') + ' 个编辑器',
});

// ── 2. 事件驱动：点语言标签 → 状态栏回显（无需 C++ 回调）──────────────────
on('Button[id^=tab-]', 'click', (event) => {
  // 我们的最小选择器语法支持 `[prop=value]` 精确匹配；这里演示读取命中元素自身信息
  $('#status').set({ text: '语言已切换（由 JS 捕获）' });
});

// ── 3. 声明式联动：监听编辑器的输入事件 ─────────────────────────────────
on('#editor', 'input', () => {
  const editor = $('#editor');
  $('#status').set({
    text: '编辑中 · ' + editor.props.lines + ' 行 · ' + editor.props.language,
  });
});

// ── 4. 定时器：每 2 秒刷新一次"心跳"，证明脚本在持续运行且不阻塞主循环 ──
every(2000, () => {
  state.heartbeats = (state.heartbeats ?? 0) + 1;
  $('#language-label').set({ text: 'hb ' + state.heartbeats });
});

// ── 5. 脚本侧状态：AI 可通过控制通道 `script {"state":true}` 读回 ────────
state.ready = true;
state.loaded_at = now();

log('示例 JS 逻辑已载入（状态可用 script/state 读回）');
