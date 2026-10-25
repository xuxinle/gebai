# 用 XML + JS 写应用（xmlapp 通路）

> 面向**不写 C++ 也能做应用**：界面写在 XML 里、逻辑写在 JS 里，
> 真实能力（读文件、跑命令、查库）由 C++ 通过桥暴露给脚本。

## 最小例子

一个能跑的应用 = **三个文件**：

```
myapp/
  main.cpp        ← 只做装配与暴露能力（约 60 行，见下文模板）
  ui/main.xml     ← 界面长什么样
  ui/app.js       ← 界面怎么动
```

`ui/main.xml`：

```xml
<Column id="root" gap="8" padding="12" grow="true" surface="base">
  <Row gap="8" align="center">
    <Text id="title" size="18">我的应用</Text>
    <Button id="go" label="运行" icon="play" variant="primary"/>
  </Row>
  <Input id="needle" placeholder="输入关键词" grow="true"/>
  <Progress id="bar" value="0"/>
</Column>
```

`ui/app.js`：

```js
$('#go').on('click', () => {
  // 调 C++ 提供的能力（桥的名字由 main.cpp 决定）
  const count = app.search.run($('#needle').value);
  $('#bar').set({ value: 100 });
  log('找到 ' + count + ' 条');
});
```

`main.cpp` 里只有三件事（完整模板见 `examples/xmlapp/main.cpp`）：

```cpp
// ① 壳层先挂（窗框/标题栏）：XML 树由 HotReload 换根，落点是它当前的父元素
app.set_content(std::move(frame));
app.start();

// ② 暴露 C++ 能力：脚本里就是 `app.search.run(…)`
bridge.expose("search.run", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
  return st::Json(do_search(st::json_as_string(args[0])));
});

// ③ 装载 XML 树 + JS 逻辑（并**改文件即生效**，见下）
st::ui::HotReload::Options options;
options.ui_dir = "ui";
options.watch = true;
st::ui::HotReload hot(app.root(), *app.script(), bridge, options);
hot.refresh_all();      // 主循环里再加一句 `if (hot.poll() > 0) app.request_repaint();`
```

> **调界面不必重启**：`HotReload` 让改 `main.xml` / `app.js` 后保存即生效（换树 /
> 重装逻辑，原子替换、写错不清界面）。完整说明见 [`hot-reload.md`](hot-reload.md)。

## XML 里能写什么

### 标签

标签名 = 组件注册名，另有几个短名别名（更顺手）：

| XML 写 | 实际组件 | 说明 |
|---|---|---|
| `<Row>` / `<Column>` | `Panel`（方向不同） | 行/列布局容器 |
| `<Icon>` | `IconView` | 图标 |
| `<Separator>` | `Divider` | 分隔线 |
| `<Progress>` | `ProgressBar` | 进度条 |
| 其余 | 同名注册名 | `Text`/`Button`/`Input`/`Checkbox`/`Table`/`Tabs`… |

认不出的标签**跳过并报告**（`XmlBuildReport::unknown_tags`）——不会静默消失。
想要"写错就早失败"（CI / 工具链）用 `XmlBuildOptions{.strict = true}`。

### 属性

两层，名字与**协议 `set` 完全一致**（所以 `set` 能改的，XML 也能写）：

**① 布局与外观**（任何元素都能写）：

| 属性 | 值 | 说明 |
|---|---|---|
| `gap` / `padding` / `padding_x` / `padding_y` / `margin` | 数字 | 间距（px） |
| `width` / `height` / `min_width` / `min_height` / `max_width` / `max_height` | 数字 | 尺寸 |
| `grow` / `shrink` / `wrap` | `true`/`false` | 主轴伸缩 |
| `align` / `valign` / `justify` | `start`/`center`/`end`/`stretch`/`space_between`… | 对齐 |
| `surface` | `none`/`base`/`alt`/`sunken` | 语义表面（跟主题走） |
| `bg` / `fg` | `#rgb`/`#rrggbb`/`#rrggbbaa` / CSS 色名 | 背景 / 文字色（字面值） |
| `radius` / `border_width` / `border_color` / `opacity` | 数字 / 色值 | 外观 |
| `font_size` / `font_weight` / `text_align` / `monospace` | — | 排版 |

**② 组件自己的属性面**：`label`（Button）、`value`（Input/Progress）、`checked`（Checkbox）、
`placeholder`、`icon`、`variant`/`size`（Button 档位）、`text`（Text）…每个组件
`property_names()` 里的名字都能写。

未识别的属性**进报告**（含标签名与行号）——"点了没反应"最常见的原因就是属性名打错，
静默忽略会让排查从"看日志"变成"读源码"。

### 文本

`<Text>你好</Text>` / `<Button>确定</Button>` 直接把文本给元素的文本属性；
`text="…"` 属性与它同义（属性优先）。

## JS 里有什么

脚本运行在 `st::ui::ScriptHost`（QuickJS）上，可用：

| API | 说明 |
|---|---|
| `$('#id')` | 取元素句柄（未命中返回"空句柄"：读属性得 `undefined`、写操作记一条日志） |
| `$$('Selector')` / `count('…')` | 按选择器取多个 / 计数 |
| `$('#id').set({…})` | 改属性（与协议 `set` 同一份实现，批量提交） |
| `$('#id').prop('x')` | 读属性 |
| `$('#id').on('click', fn)` / `.off(id)` | 绑/解事件（**重复绑定同一元素同一事件会先解旧的**） |
| `$('#id').append('Row', {…})` | **动态建子元素**（返回新元素句柄） |
| `$('#id').clear()` | 清空子元素 |
| `log('…')` | 打日志（出现在应用 stdout） |
| `tree()` | 整棵语义树快照 |
| `app.*` | **C++ 暴露的能力**（见下） |

可用事件名：`click` / `dblclick` / `mousedown` / `mouseup` / `mousemove` / `wheel` /
`keydown` / `keyup` / `input` / `focus` / `blur` / `hover` / `unhover`。
**绑一个不存在的事件名会直接报错**（以前是静默永不触发——那是"点了没反应"的经典来源）。

## 从 JS 调 C++（`ScriptBridge::expose`）

```cpp
st::ui::ScriptBridge bridge(*app.script(), &app.root());

// 名字带点号 → 脚本里是 `app.fs.read(...)`
bridge.expose("fs.read", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
  const auto text = st::fs::read_text(st::json_as_string(args[0]));
  if (!text.has_value()) {
    return st::unexpected(st::ErrorCode::NotFound, "读不到 " + st::json_as_string(args[0]));
  }
  return st::Json(*text);
});
```

- **失败会变成 JS 异常**（不是静默 `undefined`）——脚本里 `try/catch` 能接住，
  也就是"读文件失败"在 JS 里表现为异常而不是"拿到空字符串"。
- **名字不能以下划线开头**（`__` 是框架保留段）；点号分层不污染全局。
- 返回 `false` 时看 `bridge.last_error()` 拿原因（`expose` 只返回 bool 会把原因压平）。
- 反向（C++ 主动调 JS）：`bridge.call("on_message", {…})`。
- 装载文件：`bridge.load("ui/app.js")`——失败信息带**文件路径**（不是"脚本出错"）。
- 内建便利：`app.ui.set/invoke/query/exists/build/append/clear`（与协议同一份实现）。

## 常见坑

| 现象 | 原因 |
|---|---|
| 底部那条跑到窗口中间 | 根元素少了 `grow="true"`（它挂在 `WindowFrame::content()` 下） |
| 点了没反应（事件） | 事件名写错（如 `change`——框架只有 `click`）。现在会直接报错 |
| 点了勾选框又弹回原状 | 同一元素同一事件被绑了两次（重建式渲染下累积）。句柄的 `.on()` 已内置去重 |
| 属性写了没生效 | 看应用 stderr 的 `[xmlapp] 属性未生效：…`（报告里有标签名与行号） |
| JS 里 `xxx is not a function` | 桥没暴露（`bridge.exposed()` 可查）/ 名字写错 / 桥在 `start()` 之前建（必须在之后） |

## 参考

- 完整可运行示例：`examples/xmlapp/`（任务清单，零 C++ UI 代码）
- **改文件即生效**：`docs/hot-reload.md`（`ui::HotReload`，`DESIGN.md` §4.2.28）
- 端到端回归：`tools/xmlapp_e2e.py`、`tools/hot_reload_e2e.py`
- 设计说明：`DESIGN.md` §4.2.20（XML 建树）与 §4.2.21（脚本桥）
- C++ 声明式（不需要 XML 时的另一条路）：`docs/declarative.md`
