主题传播：从「每帧整树重算」改为「版本变更 + 新元素上树 + 样式输入变更」

## 问题：一条补偿的正反馈

`UiRoot::layout()` 把主题下发（`apply_theme_tree`）放在脏检查**之前**且**无条件**，
而 `paint()` 第一行就是 `layout()` ⇒ **每帧 × 每个元素**都在重算主题。

由此链出一条正反馈：

    每帧重算
      → 组件 apply_theme 把 style_ 重置成主题值
      → 调用方的显式声明被吃掉
      → 只好在 RenderExtras 里存一份存档，让 apply_declared_overrides 事后回放
      → 存档又被每帧重算改写
      → 再存一份"原始值"（补偿的二次方）

最后一步的实证是 `top_highlight_base_alpha`，它的注释原文：

> ⚠ 强度必须从存档读，不能从 style_.top_highlight.a 读……存档经本函数改写后 a
> 已经乘过 strength，若下一帧再乘一次会逐帧衰减到 0。

## 证据：字段增长率

RenderExtras 里 `_override` / `_declared` / `_base_alpha` 字段数：

    2026-10-30 ~ 11-17     4 个
    2026-11-19            12 个   ← 一天之内 +8
    2026-11-20 ~ 11-22    12 个

触发它的是**单次**功能开发（`72d3bc8f` / `d1d158e6`「小白液态玻璃特效」）。
也就是说：同类视觉特性每做一次，这套机制就加深一层。判据不是"现在有多少字段"，
而是"下一次同类开发还会不会涨"。

## 改法：两个单调版本号 + 一条显式声明

元素重算主题的条件变成三选一：

| 触发 | 机制 |
|---|---|
| 主题内容变了 | `UiRoot::set_theme` 递增 `theme_version_`；元素 `applied_theme_version_ != theme_version_` |
| 新元素上树 / 从别的树搬来 | `applied_theme_version_ == 0`（摘除时 `reset_applied_theme_version()` 清回 0） |
| 元素自己的样式输入变了 | setter 调 `request_theme_refresh()`，`UiRoot` 遍历时消费 |

第三条件是本次勘察新发现的，任务书里没有。全仓有 **31 处** setter 只改成员
（`shadow_level_` / `size_` / `tone_` / `row_height_` …）再标脏，靠下一帧的
`apply_theme` 把成员翻译成 `style_`。只做前两个条件会把"声明被覆盖"换成
"setter 静默失效"（设了参数、`get` 回读对、画面不变）——比原问题更难查。
所以一并补齐，并加机械守门（见下）。

### 为什么不用 `Theme::operator==`

`Theme` 有 30+ 颜色 + 30+ 尺度 + `std::string font_family` + `std::optional<Gradient>`。
逐字段比一遍比直接重算还慢，而且**漏一个字段 = 换肤静默不生效**——那正是本次
要消灭的那类缺陷。版本号是 O(1) 且不可能漏。

### 为什么摘子树必须清版本号

`applied_theme_version_` 只记数字、不知道属于哪棵树，而所有 `UiRoot` 的
`theme_version_` 都从 1 起。子树从树 A 挂到树 B 时版本号常恰好相等 ⇒ 漏算。
实测存在的搬家路径（不是理论风险）：`control/server.cpp` 的 `ui.create`（包一层再放回）、
`st_desktop` 换 Dock、`UiRoot::set_content`。

## 删掉的 8 个字段（逐个说明）

| 字段 | 它原来防什么 | 为何不再需要 |
|---|---|---|
| `glass_tint_declared` | `apply_theme_glass` 每帧把 tint 写回主题值，`set tint:0.9` 立刻被盖回 0.22 | 该函数不再每帧跑；标记合并进 `Style::backdrop_tint_declared`（与 `backdrop_tint` 同处，且不占 RenderExtras 的惰性分配） |
| `top_highlight_color_declared` | 决定高光合成用不用主题色 | 保留语义，但移进 `PaintDeclarations`（同类声明聚在一处） |
| `top_highlight_base_alpha` | 补偿的二次方：合成会回写存档，存档里的 `a` 已乘过 strength，下帧再乘会衰减到 0 | 合成**不再回写存档**，强度恒为原始值 |
| `fill_override_from_declaration` | 玻璃档与显式 `fill` 共用 `fill_override`，第二帧分不清谁写的 | `apply_theme_glass` 不再写存档 ⇒ 存档恒等于"调用方声明了什么"，单一事实源 |
| `fill_override` / `border_width_override` / `border_color_override` / `shadow_override` / `top_highlight_override` | 组件 `apply_theme` 会把这 5 项重算成主题值，显式声明必须盖过它 | 收进 `PaintDeclarations` 聚合：`RenderExtras` 字段数不再因同类属性增长 |

净效果：`RenderExtras` 的 override 类字段 **12 → 4**（剩下 4 个是任务书要求不动的
文本排版覆盖），而"下一次同类特性会不会让它涨"的答案是**不会**——新属性落在
`PaintDeclarations` 里，那是个聚合成员，不增加 `RenderExtras` 的字段数。

## 保留的 4 个字段（真实原因）

`text_color_override` / `text_tone_override` / `text_size_override` / `text_weight_override`：
属于**排版覆盖**，与 `appearance` 机制耦合，且存在 `apply_theme` 之外的写入路径
（`set_text_*` 直接改存档、`apply_text_overrides` 在统一层两处回放）。本次不动。

## 机械守门（替代"改代码时记得"）

`request_theme_refresh()` 仍是约定型接口，靠 setter 自己调。本框架已经杀过一次
这类反模式（`refresh_theme` 的注释："那是靠约定保护的不变式，新人加组件时必漏"），
所以补了 `tests/ui_theme_input_setter_contract_test.cpp`：源码扫描全部组件的
setter，若它写了 `apply_theme` 会读取的成员、却既没调 `request_theme_refresh()`
也没直接写 `style_`，即报违约并给出修法。

审计结果：47 个 setter 被写入 `request_theme_refresh()`（31 个第一次修 +
`set_enabled` 等交互态 + 后续补的 `Connector` 两处）。

## 逆向验证结果（最关键的一节）

三个层次的回退，各自让不同的测试变红：

| 回退什么 | 变红的测试 |
|---|---|
| 每帧重算条件写回 `if (true \|\| …)` | `theme_apply_not_per_frame`（`apply_count` 1 → 2） |
| 删 `set_enabled` 的 `request_theme_refresh()` | `disabled_state_still_reaches_style_across_frames` |
| 删 `Card::set_shadow_level` 的 `request_theme_refresh()` | `setter_still_reaches_style_across_frames` |
| 删 `take_content` 的版本清零 | `detached_element_reattached_gets_refresh` |
| 删 `apply_declared_paint`（声明回放） | `tools/verify_theme_runtime.py` 判据 3（像素 248 → 0） |

### ⚠ 一条必须记下的教训：我第一版回归测试是假绿的

我先写的运行时验证用 `get top_highlight` 回读断言。它 PASS——
但把 `apply_declared_paint` **整个删掉**，它**依旧 PASS**。
原因：`set` 本身就写进了 `Style`，`get` 读的也是 `Style`，观测路径绕过了被测机制。
再加"连跑 12 帧"也不够——主题没重算，声明本来就不会被盖。

唯一能抓住它的观测是「**切一次主题** + **看像素**」：切主题会让 `Card::apply_theme`
真的重算并把 `top_highlight` 按档位抹成透明。改用像素后，删回放立刻从 248 → 0。

已写进 `CONVENTIONS.md` §7.1.1：断言必须挂在"修复前后行为真的不同"的那条路径上。

## 性能数据（诚实说明：这不是性能优化）

`gallery --headless` 稳态 `paint_ms`（60 样本中位，各跑 4 轮）：

| 版本 | 轮 1 | 轮 2 | 轮 3 | 轮 4 |
|---|---|---|---|---|
| 改前 HEAD `e6b25a2d` | 7.131 | 5.473 | 5.243 | 7.847 |
| 改后 | 2.500 | 1.894 | 1.872 | 1.814 |

大约 5.5 ms → 1.9 ms。但这个数字**不要当卖点**：

- 它主要来自"不再每帧对每个元素做一遍样式赋值 + `mark_dirty`"，是**顺带**结果；
- 瓶颈本来就在光栅化（三档阴影快照实测 0.009 µs/元素 = 63 元素 0.6 µs），
  主题重算从来不是主要成本；
- 本次的价值是**消除复杂度增长**：切断"每修一个同类 bug 就加深一层补偿"的正反馈。

## 结构性对比

| 指标 | 改前 | 改后 |
|---|---|---|
| `include/st/ui/element.hpp` 行数 | 1992 | **1991** |
| `RenderExtras` override 类字段 | 12 | **4** |
| 每帧 `apply_theme` 调用次数（无主题变更） | 每元素每帧 | **0** |
| `st build gbcode` / `gallery` | 过 | 过 |
| `st lint` 违规 | 32 | **32**（无新增） |
| `st test` | 3 个既有红 | 3 个既有红（零新增） |
| 新增测试文件 / 用例 | — | 2 个文件 / 8 条用例 |

## 验证命令

    st build gbcode --profile dev        # 过
    st build gallery --profile dev       # 过
    st test                              # 3 个既有红（git 超时 + 2 个 emoji 字体整形），零新增
    st lint                              # 32 项，与基线相同
    python tools/verify_theme_runtime.py # 7 条判据全过（含跨主题变更的像素验证）
    python tools/measure_paint_ms.py <exe>

⚠ `st test --san` **本机跑不了**：当前工具链是 MinGW（`D:/program/mingw/bin/g++.exe`），
不带 ASan/UBSan 运行库；`st.pkg` 声明的 clang 交叉工具链未安装（`st doctor` 确认）。
这一项如实记为"未验证"，不是"通过"。

## 一侧发现但未做（已记 BACKLOG P1）

1. **`Element::dirty_` 在生产路径里从不清零**：`clear_dirty()` 只被测试调用；
   `arrange` 只清 `layout_dirty_`。所以 `dirty_` 事实上是个"永远脏"的死标志。
   这正是本次不能用"元素脏就重算主题"来消掉那 31 个钩子的原因——合并等于永久重算。
2. **`request_theme_refresh()` 仍是约定型**：扫描测试能**发现**漏写，但不能让它
   **不可能漏**。结构性修法是给 `apply_theme` 读的成员套一个"改了就必须声明"的包装。
