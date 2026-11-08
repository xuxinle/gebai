# 组件补全台账（2026-11-08 起）

## 为什么要有这份台账

本轮任务是「补全桌面 UI 常用组件」，分五包交付。台账记录**每包的状态、缺口、以及
撞到的框架级问题**——这些东西不适合只留在对话里（对话会过去），也不适合写成
`BACKLOG.md` 的「已完成」条目（那是缺陷账，不是进度账）。

组件是否「有展示卡」由 `tests/support/gallery_exhibits.hpp` 与
`tests/gallery_component_parity_test.cpp` 强制对账，本文件不重复那份清单。

---

## 补全前的基线（2026-11-08 实测）

注册表 **51 项**（`ST_UI_COMPONENT` 行数，脚本点过）：

```
Avatar Badge Button Card Checkbox Chip CodeEditor ColorPicker CommandPalette
CompletionPopup Dialog DiffView Divider FileDialog Heading Icon Input KeyValueRow
ListItem List MarkdownView MenuPanel MenuBar ContextMenu MergeView Panel Spacer
ProgressBar Radio ScrollBar ScrollView Select SelectPanel Slider SlowContent
Spinner SplitView Switch Table Tabs Terminal Text TextArea ThemePicker TimelineView
TitleBar Toast Tooltip Tree Viewport WindowFrame
```

### 缺口全集（全仓无同名类，不是重名）

| 类别 | 缺口 |
|---|---|
| 导航定位 | Breadcrumb（此前只在 `file_dialog.cpp` 内自绘）、Pagination、Steps、SegmentedControl、NavRail/Sidebar |
| 反馈状态 | EmptyState、Banner/Alert、Skeleton、ProgressRing、StatusDot、AvatarGroup、Gauge |
| 输入表单 | NumberInput、SearchBox、TagInput、DatePicker/Calendar、FormRow、RadioGroup/ToggleGroup、MultiSelect/ComboBox |
| 结构容器 | Accordion/Collapsible、Toolbar、StatusBar、Drawer、Popover、DividerLabel |
| 展示微件 | Sparkline、Rating、Kbd、Link、CopyButton、SplitButton |

`DESIGN.md` 早就写着「规划中 5 个」——`IconButton` / `Link` / `Dropdown` /
`SegmentedControl` / `Sparkline`——它们一直没落地。本轮活包把其中两个
（`SegmentedControl`、`Link`）排进计划。

---

## 包一 · 导航与定位（已完成）

| 组件 | 文件 | 一句话 |
|---|---|---|
| `Breadcrumb` | `breadcrumb.hpp/.cpp` | 分段路径；末段=当前位置不可点；超宽折叠中段但**强制保留首段与末段** |
| `Pagination` | `pagination.hpp/.cpp` | 页码条；窗口固定 `2s+1` 且**靠边时向中间撑开**（长度恒定） |
| `Steps` | `steps.hpp/.cpp` | 步骤条；显式三态（含「跳步」）+ `set_current` 便捷入口；横/纵两向 |
| `SegmentedControl` | `segmented_control.hpp/.cpp` | 分段控件；滑块 `motion_normal` 滑动；键盘 ←/→ 循环 |

测试：`tests/ui_nav_components_test.cpp`（12 用例 / 200 断言，全绿）。
gallery：组件页新增「导航与定位」卡（`#card-nav`）。截图核验通过。

### 交付过程中修掉的三个真缺陷

1. **`Breadcrumb` 折叠宽度算错**（`…` 自己没算进预算 + 末段在窄宽下被整个丢掉了）。
   修法：从末尾往前装之前**先把末段放进预算**——末段是导航锚点，丢它等于控件只说
   “你在某处”。
2. **`Pagination` 折叠规则让页码条长度跳变**（首/末页时窗口被夹窄）。
   修法：`lo = clamp(p-s, 1, n-2s)`——靠边时窗口向中间撑开，长度恒定。
   这条**有逆向验证**：把 `1` 改回 `2` 后 `nav_pagination_window_rules` 立刻红。
3. **`SegmentedControl` 只画外壳、文字与滑块全空**（`apply_theme` 每帧清空段宽缓存，
   而 `paint_content` 里的 `relayout()` 拿不到 `RenderContext` 去重建 → 直接返回）。
   属性面全对，只有截图能发现。修法：`paint_content` 开头 `rebuild_widths(context)`。

第 3 条已配像素级回归（`nav_segmented_paints_content_not_just_the_shell`），
它断言「选中段中心不是凹槽底色」+「未选中段有墨迹」——正是那个缺陷的画面特征。

### 一条设计口径修正（`indicator_rect()`）

`SegmentedControl::indicator_rect()` 早先返回「上次绘制时的滑块位置」，于是
「刚点完、还没重绘」时它给的是**上一段**的矩形——属性面与 `index()` 自相矛盾。
现改为返回**目标矩形**（= 当前段的矩形），动画中间位置只活在 `paint_content` 的局部
变量里。判据：**能同时被“读”和“画”的两个量不该是同一个字段**。

---

## 包二 · 反馈与状态（5 个，已交付）

| 组件 | 为什么需要它 | 关键几何/行为 |
|---|---|---|
| `EmptyState` | 「什么都没有」是最常被漏做的界面；没有空态时用户读成「坏了/没加载完」 | 图标-标题-说明-动作四块堆叠；四档尺寸（24/36/52/72 图标联动字号与留白）；无动作文案时**不留空命中区** |
| `Banner` | 需要用户**一直看见**的提示（版本更新/权限/离线）。与 Toast 的关键差别是**占位**（在内容流里，用户绕不过它） | 左缘 3px 色条 + 软色调底；四档色调→默认图标；关闭 × 与动作按钮不重叠；长文**折行增高** |
| `Skeleton` | 骨架的价值全在**形状与真实内容同构**；手摆拧巴的灰块会让切换时内容跳动 | 三形态（rect/circle/text）；行高/行距固定→与真文本同高；呼吸相位**由 `context.time_seconds` 决定**（可截图回归），首帧最亮 |
| `StatusDot` | 状态在界面上就是一个点；难点不在画圆，而在**与文字墨迹对齐**与语义色映射 | 五档状态色互不相同；点对齐**墨迹中心**（不是行盒中心，否则偏下 1~2px）；`Busy` 自动呼吸，切走时关掉 |
| `AvatarGroup` | 「协作者/在线成员」就是一叠头像；重叠几何与溢出规则各写一遍就会尺寸一变大变形 | 重叠 `kOverlapRatio=0.28`；溢出按**可用宽**算到放得下为止（不是固定砍到 3 个）；`+N` 用次级色 |

### 回归护栏与逆向验证

`tests/ui_feedback_components_test.cpp`：**23 用例 / 97 断言**。

两条做过**逆向验证**（把实现改坏→确认变红→恢复）：

| 用例 | 故意怎么弄坏 | 结果 |
|---|---|---|
| `avatar_group_avatars_overlap_by_ratio` | 把重叠步长改成 `avatar_size_`（不重叠） | 立刻红（两条断言） |
| `status_dot_busy_pulses_and_leaving_stops_it` | 删掉 `pulse_ = state_value_ == State::Busy` | 立刻红 |

### 交付中修正的一处判断

头像字形的取字规则曾一度改成「CJK 保留全名」，截图后发现反而更差（28px 圆里两个汉字
必须缩到看不清，或被 `ellipsize` 载成只剩名而丢了姓）。最终定案：**一律取第一个非空白
码点**，CJK 就是姓氏（微信/支付宝同款做法）。记录在这里，避免下个人重跑这个弯路。

---

## 包三 · 输入与表单（5 个，已交付）

| 组件 | 为什么需要它 | 关键行为 |
|---|---|---|
| `NumberInput` | 数值语义的行为（非法输入、区间、步进、精度）分散到调用方就会各写一遍 | **值与文本分开存**：`value()` 永远合法且已夹取，`text()` 是用户正在敲的原文；`on_change` 只在值**真变**时派发（否则回调里 set_value 会递归） |
| `SearchBox` | 搜索框的四个部件（放大镜/清除/计数/键位）手搭 `Input` 时容易漏 | 清除 × **只在有内容时**出现且不侵输入区；`Esc` 空内容时**不吃**（应交给上层）；超宽时保留**尾部**（刚敲的字信息量最大） |
| `FormRow` | 一行表单看起来是 Row(Text, Control)，但标签列宽/垂直对齐/必填星号/错误占位四件事都得专门做 | 标签列可**显式共享宽**（上下行对齐），也可自动；显式给宽时两行控件起点必一致；错误文案**参与布局**（不浮在上面盖下一行） |
| `RadioGroup` | `Radio` 明说“不做分组”，于是互斥/键盘导航/索引映射三件事各写一遍 | 选中**唯一**（数据结构保证）；按**值**重定位（重排后选中不漂）；方向键移动并选中、**跳过禁用项** |
| `TagInput` | 「标签与输入框同排且折行」是必然碰到的难题 | 标签折行、输入位跟随并可折行；空草稿时**退格删最后一个标签**（肌肉记忆）；去重，超限不加；粘贴 `a,b,c` 可一次加多个 |

### 回归护栏与逆向验证

`tests/ui_form_components_test.cpp`：**31 用例 / 152 断言**。

| 用例 | 故意怎么弄坏 | 结果 |
|---|---|---|
| `number_input_clamps_value_to_range` | 去掉两处区间夹取 | 立刻红 |
| `radio_group_selection_follows_value_across_reorder` | 把按值重定位改成 `if (false)` | 见下 |

**第二条逆向验证一次就抓出了真缺陷**，值得记下来：

1. 第一次写用例时重排是 `{甲,乙,丙} → {丙,乙,甲}`，而「乙」前后都在下标 1——
   「按值重定位」与「按下标保留」**完全等价**，把实现改坏测试依然绿（假绿，`§7.1`）。
2. 把重排改成 `{甲,乙,丙} → {甲,丙,乙}`（乙换到下标 2）后重跑：**红了**。
3. 但恢复实现后**仍然红**——这才发现 `set_options` 里是真 bug：它先
   `options_ = std::move(options)` 再取 `options_[index_]`，读到的是**新列表同一下标**
   的值，所以「按值重定位」从一开始就没生效过。修成先取值再换列表，转绿。

> 教训：逆向验证不只是“证明测试有效”，它也会**先于生产环境抓到实现本身的错**。

---

## 包四 · 结构与容器（4 个，已交付）

| 组件 | 为什么需要它 | 关键行为 |
|---|---|---|
| `DividerLabel` | 「线 + 文字 + 线」最容易错的不是画线，而是留白分配 | 线居中于**行盒**（不是墨迹）；两侧留白相等（`Align::Center`）；长标题夹到可用宽（不把线挤没）；右侧动作贴右缘且不与线重叠 |
| `Accordion` | 折叠面板必掎到四件事：单选/多选、收起边界、动画高度、标题行角色 | 单选**互斥**（打开第二个自动收起第一个）；切到单选取只保留一个；`allow_collapse` 决定点已展开项收不收；**整行可点**（不是只有箭头）；展开动画在 `arrange` 里推进 |
| `Toolbar` | 「一排按钮」不够：溢出要**折进菜单**而不是被裁半个 | 从**右往左**折叠；`pinned` 永不折；分隔线比按钮**矮**（否则像边框）；报告折叠数 == 实际隐藏数 |
| `StatusBar` | 状态栏有一条只为它成立的规则：左右分流、中间空白才是被压的那段 | 左区贴左、右区贴右且从右往左排；中间空白夹到 0（不让两侧重叠）；可点段按**段**算命中区 |

### 回归护栏与逆向验证

`tests/ui_structure_components_test.cpp`：**25 用例 / 89 断言**。

| 用例 | 故意怎么弄坏 | 结果 |
|---|---|---|
| `accordion_single_mode_keeps_one_open` | `apply_mode` 改成 `if (true) return` | 立刻红 |
| `toolbar_narrow_reports_same_count_as_hidden` | 折叠集合只计数不真折 | **没变红** |

第二行要如实记下来：在「6 × 50px 挤 180px」这个场景里，
「从右往左折」与「按宽度自然放不下的那几个」**本来就是同一个解**，
两种实现无法被区分（不是测试缺陷，是场景无区分力）。
但这次逆向验证换来一个真实重构：原实现有两套判据（折叠集合 + “放不下就跳过”兜底），
两套恰好同解时**互相掩盖**——改坏哪一套都绿。已删掉兜底，只留单一判据（见 `toolbar.cpp` 注释）。

### 截图才能发现的缺陷

`Accordion` 展开后**内容区是空白**（标题下方一大片空）。
原因：动画推进写在 `paint_content` 里，而 `arrange` 先于 `paint` 执行——
于是 `arrange` 时进度恒为 0 → 内容矩形为空 → 子内容**从不落位**。
属性面与 `content_rect()` 几何断言都绿（后者返回的是目标几何），只有截图看得见。
修法：动画推进改到 `arrange` 开头（几何先于绘制就绪）。

---

## 包五 · 展示微件（5 个，已交付）

| 组件 | 为什么需要它 | 关键行为 |
|---|---|---|
| `Sparkline` | 迷你图的价值是「一眼看出形状」，难点全在归一化与笔画面 | 纵轴按**数据自身** min/max 归一（不是 0..1）；全等数据落**中线**；笔画**内缩半线宽**（峰谷不被削）；三形态 line/area/bar |
| `ProgressRing` | 紧凑空间里的循环进度；与 `ProgressBar` 分工 | 弧从 **12 点顺时针**（不是数学零点 3 点）；轨道与进度分开画；不确定态画**转动的短弧**且不显示数字（不知道还多久就别拿假数字充数）；100% 只触发一次 |
| `Rating` | 四个只有它才需要的语义：半星/悬停预览/键盘/再点清零 | 半星用**裁剪**（不是画小星——小星形状不同，像字体回退错误）；悬停只**预览**不改值；`arrow`/`Home`/`End` 可调 |
| `Kbd` | 键帽有一批必须一致的规矩 | 平台修饰键名（`Ctrl`/`⌃`、`Win`/`⌘`）；键帽宽按内容量；分隔符 `+` 占位；键帽互不重叠 |
| `Link` | 与 `Button` 分工：链接是跳转不是动作 | 下划线贴**基线**（不是墨迹底——否则穿过 `g`/`p` 下伸部）；截断后下划线跟着缩；`↗` 不覆盖文字；组件**不自己打开浏览器**（只把意图交给宿主） |

### 回归护栏与逆向验证

`tests/ui_widget_components_test.cpp`：**35 用例 / 143 断言**。

| 用例 | 故意怎么弄坏 | 结果 |
|---|---|---|
| `sparkline_extremes_are_inset_by_half_stroke` | 把内缩改成 0（笔画贴边） | 立刻红（两条断言） |
| `rating_click_sets_value_with_half` | 去掉半星判定（一律整档） | 立刻红 |

---

## 五个包交付完毕：汇总

| 包 | 组件数 | 用例 | 断言 |
|---|---|---|---|
| 一 · 导航定位 | 4 | 12 | 200 |
| HtmlView | 1（+6 文件子系统） | 18 | 179 |
| 二 · 反馈与状态 | 5 | 23 | 97 |
| 三 · 输入与表单 | 5 | 31 | 152 |
| 四 · 结构与容器 | 4 | 25 | 89 |
| 五 · 展示微件 | 5 | 35 | 143 |
| **合计** | **24 个组件** | **144** | **860** |

### 五个包里修掉的真缺陷（按发现手段分类）

**只能靠截图发现的（属性面全对）**：

1. `SegmentedControl` 只画外壳不画内容；
2. `HtmlView` 内容画到组件界外（`clip_children` 不包 `paint_content`）；
3. 同行相邻行内元素的**底色盖住前一格末尾字**（分两趟画才修）；
4. `Accordion` 展开后内容区空白（动画在 `paint` 里推进，而 `arrange` 先执行）。

**测试/逆向验证发现的**：

5. `Breadcrumb` 折叠时丢末段；
6. `Pagination` 页码条长度跳变；
7. `parse_border` 把 `#2563eb` 当宽度（→ 边框 2563px）；
8. `border-left` 被当成四边描边；
9. `RadioGroup::set_options` 先换列表后取旧值（「按值重定位」从未生效）；
10. `AvatarGroup` 的 `visible_` **遮蔽** `Element::visible_`（`L13`）——
    它是真缺陷：改名后 Loop 条件变异，头像只画出一个。

---


按批准的计划：反馈与状态 / 输入与表单 / 结构与容器 / 展示微件。
每包交付口径与包一相同：头+实现+注册 → 聚合头 → 测试 → gallery 卡 →
构建 + 全量测试 + lint → 截图核验。

---

## HtmlView · HTML 预览（基础 CSS 子集）

**定位**：预览智能体产出的 HTML 产物（文档/报告/卡片），**不是**跑真实网页。
所以取“确定性、可截屏回归、零依赖”，而不是内嵌系统 WebView。

三层分工：

| 层 | 文件 | 职责 |
|---|---|---|
| 解析 | `include/st/html/dom.hpp` + `src/html/dom.cpp` | 容错建树（隐式闭合、空元素、实体、`<pre>` 空白、`<style>` 提取） |
| 样式 | `include/st/html/style.hpp` + `src/html/style.cpp` | 选择器匹配、特异性/顺序/`!important` 层叠、可继承属性、UA 默认样式 |
| 组件 | `html_view.hpp` + `html_view.cpp` | 块级流 + 行内混排折行、表格、列表、滚动、链接点击 |

**为何不复用 `st::xml`**：`st::xml` 把文本拼成单一串（丢了「文本在哪两个子元素之间」），
又把 `<` 当标签开始（`<style>a > b {}</style>` 会被误解）。两条都是行内混排的硬需求。

### 能力边界（写全，不含糊）

支持：标签/`.class`/`#id`/`*`/后代/子代/选择器列表、层叠与继承、块级流与行内折行、
列表、表格（等分列宽）、`<pre>` 与 `white-space` 四种取值、颜色/字号/字重/行高/
边距/内边距/边框（含 `border-left` 只画左缘）/背景/圆角/对齐/下划线。

**明确不支持**（忽略该条声明，不影响其余）：`<script>` 执行、外部 CSS/图片（不联网）、
`float`/`position`/`flex`/`grid`/`transform`/动画、伪类与属性选择器（写了就丢那条选择器）。

### 交付中修掉的六个真缺陷

1. **行内片段各占一行**（`<p>a <b>b</b></p>` 变成三行）。根因：用 `lines[] + styles[]`
   一一对应。修法：改行内流（片段串 + 硬换行标记，折行统一在 `finish_inline_flow`）。
2. **行内元素推进了块游标**（`<code>` 的 padding 让下一个片段的 y 跳）→ 文字互相覆盖。
   修法：行内内容只追加进流，不推游标。
3. **`border-left` 被当成矩形描边**（画成四边框）→ 读起来像“套了个框”。
   修法：`border_left_only` 标志，只在左缘出墨。
4. **`parse_border` 把 `#2563eb` 当宽度**（`#` 被跳过后 `2563` 可解析）→ 边框宽 2563px。
   修法：先判颜色再判长度，且 `#`/`rgb(` 开头的字面量绝不当作长度。
5. **文档背景没铺**：网页文字按白底设计，直接画在暗色 `surface` 上是一片看不见的深灰。
   修法：组件自己铺文档底色（取 `<body>` 的 `background`，没有则白）。
6. **内容画到组件界外**（往上滚时正文压在卡片标题上）。根因：`style_.clip_children`
   只包裹**子元素**的绘制，`paint_content` 在裁剪之前。修法：自绘滚动内容自己 push/pop 裁剪。

另修一条绘制顺序：**背景与文字分两趟画**（同行相邻行内元素的底色会盖住前一格的末尾字）。

### 回归护栏

`tests/ui_html_view_test.cpp`（18 用例 / 179 断言）：解析 4 条、样式 5 条、组件 9 条。
其中三条是**像素/几何级**的，它们各自对应上面一条真实缺陷：

| 用例 | 抓住的形态 |
|---|---|
| `html_css_border_left_draws_only_left_edge` | 四边框 vs 单边（横扫一行统计红像素的 x 范围） |
| `html_view_inline_runs_do_not_overlap` | 同行片段重叠（同一 y 的片段矩形必须不相交） |
| `html_view_inline_piece_is_not_truncated` | 片段尾字被裁（盒宽必须 ≥ 文字宽） |

前两条做过**逆向验证**（把修复改回去 → 断言立刻红）。

