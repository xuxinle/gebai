# 改文件即生效（热重载）

> XML 写界面、JS 写逻辑的这条通路（见 [`xml-ui.md`](xml-ui.md)），**改完保存就见效**：
> 不重编、不重启进程。调一个间距的一轮从「改 → 切终端 → 起进程 → 看」压成一次保存。

## 一条命令

```bash
# 在窗口里改（默认开 watch，改 XML/JS 保存即生效）
./build/dev/bin/st run xmlapp --profile dev

# 无头 + 控制通道（自动化/像素断言同样吃热重载）
./build/dev/bin/xmlapp --headless --watch --control-port 0 --control-file /tmp/ctl.json
```

`st run` 现在对**壳层源码没改**的应用**跳过构建**：XML/JS 是运行期读的文件，
改它们不需要编译。要强制重建用 `st build xmlapp` 或 `st run … --force`。
（判据：目标第一条 `sources` 里的壳层源文件与产物比 mtime —— 任一源比产物新才重建。）

## 改了会发生什么

| 改了 | 生效方式 | 代价（必须知道） |
|---|---|---|
| `ui/main.xml` | 重建整棵界面树并**换根**（同一父元素位置） | 旧树销毁 → 输入框内容/滚动位置/勾选态等**运行期状态丢失** |
| `ui/app.js` | 清掉旧绑定/定时器/脚本状态后**重新装载** | 脚本侧状态回到初始（与重启进程等价） |

界面重建后**脚本会自动重放一遍**（旧树上的绑定已随旧元素消失，不重放就没有点击响应）。

## 三条硬约束

### ① 原子：写错不清空

- **XML 语法错 / 认不出的标签** → 界面**保持旧版**，错误带文件名与行号进 stderr：
  ```
  [hot] 界面重载失败（旧界面保持，改回来会自动生效）：…/main.xml 建树失败：认不出的标签: <Buton>
  ```
- **JS 语法错** → 全部页**先做语法检查**，任一页错就原地返回，旧逻辑继续跑：
  ```
  [hot] 逻辑重载失败（旧逻辑保持运行）：…/app.js：… line 1: unexpected token
  ```

判据是「写文件的人正在看窗口」：一次手滑把应用变成白屏，比一次报错难查得多。

**但认不出的属性只告警、不拒绝**——那是"丢一个属性值"，元素还在、界面大致完整，
与"丢整棵子树"的后果不同，故处置也不同。

### ② 内容指纹，不看时间戳

判「文件变了没」用**内容哈希**（FNV-1a），不是 mtime。理由见
`CONVENTIONS.md` §7.4：`git checkout` / `copy2` 恢复文件后 mtime 可能比产物还旧，
用 mtime 的判据会出现"保存了却没反应"——而那是这条通路唯一会让人放弃它的症状。
几 KB 文本 250ms 一次哈希，成本可忽略。

### ③ 降级如实

- 重载后**不保留**界面运行期状态（不是"状态迁移"，是"重新建"）。需要保的东西属于
  脚本层，由脚本自己写出去（`app.*` 能力），本层不猜。
- 新 XML 里删掉的 id，其旧绑定由 `ScriptHost::prune_dead_bindings` 回收（不静默累积）。

## 怎么用（C++ 侧三步）

```cpp
// ① 壳层先挂（窗框/标题栏），XML 树交给 HotReload 管
app.set_content(std::move(frame));
app.start();

// ② 桥负责 expose，热重载负责 XML/JS 的装载与换根
st::ui::ScriptBridge bridge(*app.script(), &app.root());
store.install(bridge);
st::ui::HotReload::Options options;
options.ui_dir = ui_dir;
options.watch = true;            // 默认关：生产应用不为调试便利每帧扫文件
st::ui::HotReload hot(app.root(), *app.script(), bridge, options);

// ③ 开局装载一次 + 主循环里推一下
if (auto status = hot.refresh_all(); !status) { 报错并退出 }
while (!app.quit_requested()) {
  if (hot.poll() > 0) app.request_repaint();
  app.tick();
}
```

三条可选能力：

| 能力 | 用法 |
|---|---|
| 脚本里一句话重载 | `hot_reload()`（宿主需 `bridge.expose("hot.reload", …)`，见示例） |
| 诊断面 | `hot.status()` → `{xml, script, watch, ui_loaded, ui_visible, ui_refreshes, script_refreshes, bindings, last_summary, last_error}` |
| 界面可见性（开发期看背景） | `hot.set_ui_visible(false)` —— 只改 XML 根的 `visible`，**壳层不受影响** |

`status()` 建议同时经 `bridge.expose("hot.status", …)` 交出去：控制通道一句
`script {code: "JSON.stringify(app.hot.status())"}` 就能问"重载了几次、错在哪"。

## 参考实现

- `include/st/ui/hot_reload.hpp` / `src/ui/hot_reload.cpp`（本层）
- `examples/xmlapp/main.cpp`（`--watch` / `--no-watch`、`app.hot.reload`）
- `examples/xiangqi/main.cpp`（常驻面板：默认不 watch，`--watch` 打开）
- 回归：`tests/ui_hot_reload_test.cpp`（原子/换树/清场/指纹各一条）、
  `tools/hot_reload_e2e.py`（真进程：改文件 → 控制通道看界面变没变）
- 设计：`DESIGN.md` §4.2.24

## 常见坑

| 现象 | 原因 |
|---|---|
| 改了 JS 不生效 | 没开 `--watch`；或应用自己 `bridge.load` 后又用 `hot` 装了一遍（两者都写日志，看 `last_summary` 用的是哪份） |
| 改 JS 后点一下触发两次 | 宿主重复装载同一页（`refresh_script` 已经清场，别在外部再 `bridge.load`） |
| 改 XML 后输入框内容没了 | 预期行为（换树）；要保留就写进脚本状态并在重放时恢复 |
| 新 XML 里删了元素，JS 报 `not a function` | 脚本对空句柄调了 `.on()`：先判 `$('#x').exists`（示例脚本就是这么写的） |
| 属性写了没生效但不报错 | 看 stderr 的 `[hot] … 未生效的属性 N 处`（报告含标签名与行号） |
