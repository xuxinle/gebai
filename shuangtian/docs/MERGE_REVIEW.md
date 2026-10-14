# 合并前复核清单 · `optimize/ui-viewport-2026`

> 16 笔提交 / 47 文件 / +4583 −99 行。**未动 master、未 push。**
> 复核日期 2026-10-14。逐笔列出：改了什么 → 怎么验证 → 风险面 → 回滚点。

## 一、生产代码改动总览（23 文件 / +649 −82）

| 文件 | 性质 | 风险 |
|---|---|---|
| `src/ui/components/tree.cpp` | 视口剔除 + 行宽缓存 | **中**（缓存一致性） |
| `src/ui/components/basic.cpp` | 多行 Text 排版缓存 + 逐行剔除 | **中**（缓存一致性） |
| `src/ui/components/table.cpp` | 行/列视口剔除 + 属性改名 | 低 |
| `src/ui/components/markdown_view.cpp` | 组首行剔除 + 二分 + `group_head_row` | **中**（曾引入漏画，已修+护栏） |
| `src/ui/components/tabs.cpp` | 前缀和 + 标签级二分 | 低 |
| `src/ui/components/list.cpp` | 项宽缓存 | 低 |
| `src/ui/components/file_dialog.cpp` | 滚轮方向 + `scroll` 读取口 | 低 |
| `src/ui/components/completion_popup.cpp` | 滚轮方向 + `scroll` 读取口 | 低 |
| `src/ui/components/merge_view.cpp` | `scroll` 读取口 | 低 |
| `src/ui/components/input.cpp` / `color_picker.cpp` | 属性改名 / 去重声明 | 低 |
| `src/ui/actions.cpp` | 通用可设集改名 | 低 |
| `src/shell/backend.cpp` | 无头 renderer 判据 | **中**（曾吞掉显式 auto） |
| `src/control/server.cpp` | `capture_dirs` 哨兵 → `optional` | **中**（安全语义） |
| `include/st/app/app.hpp` | `renderer` 缺省 → 空串 | 低（但有契约含义） |
| `include/st/control/control.hpp` | `capture_dirs` → `optional` | 低 |
| 其余 8 个头文件 | 新增只读访问器 / 视口契约 / 缓存字段 | 低 |

## 二、逐笔复核（重点：**我自己引入又修掉的**）

### ⚠️ 笔 4 `4af0c5c` MarkdownView 二分 —— 曾引入一处漏画

**当时发生了什么**：加二分后，`paint_content` 只遍历与视口相交的行。而代码块/引用块的
背景只画在**组首行**那一次 —— 组首行已滚出视口时，背景**整块不画**。
实测：40 行代码块滚到中段后 `code_bg` 像素 **0**、非白像素也 **0**（整个视口全空）。

**为何当时没被自己的护栏拦住**：那几条护栏断言的是"**画了哪些行**"（记录型端口数 `draw`），
而组背景是 `paint_group` 里的 `fill_rect_shape`，**不经过 `TextPort::draw`** ——
对"哪些行被画"完全不可见。这是**护栏口径盲区**。

**现在的状态**：`Row` 增 `group_head_row`（本组首行下标）；`paint_content` 分两种情形
（首行在区间内 → 它自己画；首行在区间前 → 由本行补画）。
护栏 `md_code_block_background_survives_scrolling`（**像素级**），已反证能抓住。

**复核结论**：✅ 已修且有真能抓住的护栏。**这是本次复核最该盯的一笔**。

### ⚠️ 笔 3/5/7 缓存类改动（Tree / ListItem / Text）—— 复核判定

三者都是"**按某个键缓存测量结果**"。复核重点：**缓存键是否覆盖所有会让结果变化的输入**。

| 组件 | 缓存键 | 失效时机 | 判定 |
|---|---|---|---|
| `Tree` 行宽 | 行数 + 字号 | `sync_nodes`/`clear_nodes` 显式失效 | ✅ |
| `ListItem` 项宽 | 项数 + 字号 | `sync_items` 显式失效 | ✅ |
| `Text` 折行 | 内容 + 宽度 + 字号 | 三者比对 | ⚠️ 见下 |

**`Text` 的缓存键复核（本次复核新查的一项）**：

- 候选遗漏维度 ①**字体族** —— **不成立**：`Theme::font_family` 只用于**主题序列化**
  （`theme_io.cpp`），**没有接到字体渲染**（`FontStack::system_default()` 自己选字体）。
  即换 `font_family` 根本不会改变实际字宽。
- 候选遗漏维度 ②**端口/渲染器替换**（如 DPI 切换重建 `TextRenderer`）
  —— 这类变化走 `UiRoot::mark_dirty_all()`，而它会**逐元素标记 `layout_dirty_`**，
  于是下一帧 `Text::measure` 会重新进入缓存填充分支…**但缓存判据是内容/宽度/字号**，
  这三者都没变 ⇒ 严格说**不会重建**，只是"恰好重新度量一次"（度量在缓存命中时仍会跑，
  取到的行宽可能不准，但**折行行数**不会错，因为折行只依赖 wrap，而 wrap 走的是新端口）。

**复核结论**：✅ **当前行为正确**。原因是 `wrap` 只在缓存未命中时调用，而那个分支一旦进入
就用**当时的**端口 ⟹ 通过 `mark_dirty_all` + 宽度/内容变化能到达。
**但它不是靠机制保证的**（靠"DPI 切换伴随的其它变化恰好会失效缓存"）——
已记入 `docs/BACKLOG.md` 作为**脆弱点**：若将来出现"只换端口、其余全不变"的路径，
缓存会静默不失效。**建议**：把 `RenderContext::text` 指针也纳入缓存键（一行改动）。

### ⚠️ 笔 2 `9a70292` / 笔 6 `3128f8e` —— 属性/契约面改动

- `table.cpp` / `input.cpp` / `markdown_view.cpp` 的 `scroll_offset` → `scroll`：
  **破坏性改名**（旧名写不进去）。已同步 `actions.cpp` 的通用可设集与 `ui_table_test.cpp`。
  复核确认：全库 `git grep '"scroll_offset"'` 只剩契约测试里的**反向断言**（要求它不存在）。
- `color_picker.cpp` 去掉重复声明 `"color"`：`property_names()` 曾出现两次，
  会让"属性清单"类遍历（如 `list_components` 消费方）出错。

### ⚠️ 笔 15 `ef214a9` —— 滚轮方向与 `--renderer auto`

两处都是**用户报告的缺陷**，且都不是孤例：

- 滚轮方向：**同类三处**（`file_dialog` / `completion_popup` / `table`）。根因是
  `wheel_delta` 的符号约定只在注释里，而这两处**连读取口都没有** ⇒ 测不出。
- `--renderer auto`：根因是 `AppOptions::renderer` 缺省就是 `"auto"`
  ⇒「没传」与「显式传 auto」**字符串上不可区分**，`backend.cpp` 的判据无法兑现注释承诺。

**复核发现的一处附带问题**：`ui_table_test.cpp` 的 `ui_table_horizontal_scroll`
**用 `delta=+1`（向上滚）断言 `offset==48`（offset 增大）** —— 与约定相反，
把 bug 固化成了期望值。已改。

### ⚠️ 笔 16 `7f6f68b` —— 结构性排查

- A 类：方向语义全库一致，但约定**只在注释 + 一条只覆盖 ScrollView 的用例**里
  → 新增表驱动契约测试。
- B 类：`capture_dirs` 注释承诺「空 = 禁止落盘」，实现是「空 = 用默认白名单」
  ⇒ **安全语义反向**。**同一文件里 `token` 已有同型问题并已修**——只改了一半。

## 三、验证矩阵（每笔都跑过的门禁）

| 项 | 命令 | 结果 |
|---|---|---|
| 全量测试（dev） | `st test` | **1414 passed / 0 failed** |
| 全量测试（ASan） | `st test --san` | 同上（零报告） |
| lint | `st lint` | 2 违反 + 1 提示（**与基线一致**） |
| 文档一致性 | `python3 tools/check_docs.py` | 全绿 |
| 性能门禁（10 条） | `st test scale_gate` | 比值 0.96~2.27（判据 5.0） |

## 四、反证记录（"护栏真能抓住缺陷"的验证）

| 反证动作 | 期望 | 实际 |
|---|---|---|
| `Tree` 恢复全量遍历 | scale_gate 红 | ✅ 比值 1.00 → 12.20 |
| 去掉 MarkdownView 组背景补画 | 像素护栏红 | ✅ `at_middle > 0` 失败 |
| `MergeView` 方向改 `+` | 契约表红 | ✅（首次未能抓住，修护栏后抓住） |
| `capture_dirs` 判据改回 `empty()?默认` | 护栏红 | ✅ |
| `file_dialog` 方向改 `+` | 护栏红 | ✅ |
| `backend` 判据改回 `empty()‖=="auto"` | 护栏红 | ✅ |
| 二分位置改错（模拟单调性失效） | 滚动用例红 | ✅ 2 条 |

## 五、风险面与回滚

**合并顺序建议**（低风险 → 高风险）：

1. `abd5935` / `756e3d6` / `da135b2` —— 纯文档
2. `fcc0752` —— 只加测试 + 一处去重
3. `7d91f2b` … `a0ab0aa` —— 视口剔除与缓存（**逐笔独立**，可单独回滚）
4. `ef214a9` / `7f6f68b` —— 缺陷修复（含**破坏性属性改名**，需下游确认）

**需要下游注意的一处**：`scroll_offset` → `scroll` 属性改名。若外部脚本/协议调用方
（`set {"name":"scroll_offset","value":...}`）存在，需同步。当前全库已无引用。

**已知既有红灯（与本次改动无关）**：`independent_project_builds_links_and_runs`
在本容器会挂住（`docs/BACKLOG.md` 已记录的环境限制）。

## 六、遗留（已记入 `docs/BACKLOG.md`，不在本分支修）

1. `Text` 折行缓存的键**不含 `RenderContext::text` 端口** —— 当前行为正确但**脆弱**
   （见 §二 笔 3/5/7）。建议后续把端口指针纳入键。
2. 性能门禁 `scale_gate` 只覆盖 10 个组件；`DiffView` 未纳入（当前健康）。
