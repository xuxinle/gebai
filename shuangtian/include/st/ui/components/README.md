# 组件库：一个组件一个头，用到才进产物

本目录（`include/st/ui/components/`）放霜天的全部 UI 组件头。
这份文档回答三件事：**怎么用**、**为什么这么设计**、**加组件要做什么**。

---

## 一、怎么用

### 三种写法，看你要做什么

**① 直接 `new` 组件（最常见）——一行 import 都不用写**

```cpp
#include "st/ui/components/button.hpp"

auto btn = std::make_unique<st::ui::Button>("确定");
// 到 main 之后，Button 已经自动注册好了：component_registry::contains("Button") == true
```

组件实现的 `.cpp` 末尾有静态登记块；你一 `new`，那个 `.o` 就进了产物，
登记随之在 `main` 之前完成。实测见第二节。

**② 要在运行期按名字建（XML / 脚本 / `ui.create`）——需要 import**

```cpp
#include "st/ui/components/button.hpp"
st::ui::import_components<st::ui::Button>();   // 我没 new 它，但运行期要按名建
```

这里 import 的作用是**把那个 `.o` 拉进产物**（即使你一次也不 `new`）。

**③ 开发期 / 不确定用哪些——全量**

```cpp
#include "st/ui/components/all.hpp"
st::ui::import_all_components();   // 全部组件进产物，XML 里写什么标签都能用
```

### XML 应用

XML 的标签是**运行期**才决定的，编译期枚举不出来——所以不能指望"自动注册"。
两条路：

- `all.hpp` + `import_all_components()`：全部标签可用（推荐）；
- 自己按需 include 用到的组件，逐个 `import_components<T>()`。

写了没导入的标签**不会静默消失**：`XmlBuildReport::unknown_tags` 会如实报出来
（见 `docs/xml-ui.md`）。

### 按需效果

```cpp
#include "st/ui/components/button.hpp"
#include "st/ui/components/text.hpp"
st::ui::import_components<st::ui::Button, st::ui::Text>();
```

产物里就只会有这两个组件（以及它们真正依赖的东西）。没 include、没导入、
也没 `new` 过的组件**整块不进产物**。

---

## 二、注册是怎么发生的：两层机制（前一版文档在这里说错过）

> ⚠ 本节曾写成一个错误结论：「include 不能自动注册，所以必须显式导入」。
> 2026-11-08 重新用独立最小实验测出：**组件被真正使用时，注册确实会自动发生**。
> 下面是把两种情形分开测后的准确说法。

### 先看一条硬约束

注册必须发生在**运行期**（`dsl::make_element(name)` 是按字符串查表），而
"组件的代码在不在产物里"由**链接器**按引用决定：

> **静态库里没人引用的 `.o` 不进产物**——这正是"按需"的机制本身。

把这个约束和两种使用方式对上，结论就清楚了。

### 情形一：应用真的用到了组件 → **自动注册**

```cpp
#include "st/ui/components/terminal.hpp"
auto t = std::make_unique<st::ui::Terminal>();   // 就这一步
```

`Terminal` 的构造函数在 `terminal.cpp` 里，所以这一步让 `terminal.o` 进了产物；
那个 `.cpp` 末尾的静态登记块随之在 `main` 之前执行。**一行 `import` 都不用写。**

实测（临时探针，只 `new` 了 `Terminal`，不写任何 import）：

```
造出 type=Terminal
未写 import，Terminal 已注册=1        ← AUTO_OK
```

为什么能这样：静态库里"`.o` 进不进产物"取决于**有没有引用它的符号**。
`new Terminal` 引用构造函数，这就是引用。

### 情形二：只 include，不用 → **不注册**（而且这是对的）

实测（include 了 `terminal.hpp`，但只 `new Text`）：

```
没用 Terminal，它已注册=0             ← ON_DEMAND_OK
产物中 Terminal 成员符号数: 0
```

这正是"按需"成立的原因。**如果你要用 `Terminal` 却没 `new` 它**（典型：
XML 里写了 `<Terminal>`、脚本里 `ui.create("Terminal")`），那么它本来就不在
产物里，必须靠显式导入把它拉进来：

```cpp
st::ui::import_components<st::ui::Terminal>();   // 我没有 new 它，但运行期要按名建
```

### 所以 `import` 的含义是什么

| 你想做的事 | 写法 | 需要 `import` 吗 |
|---|---|---|
| 直接 `new` 一个组件 | `std::make_unique<Button>(...)` | **不需要**（自动） |
| 运行期按**名字**建（XML / 脚本 / `ui.create`） | `import_components<Button>()` | **需要** |

`import_components<T>()` 表达的不是"我要用它"，而是
**"我要在运行期靠名字找到它"**——这是个额外诉求：它对
`detail::component_importer<T>::run` 产生真实引用，于是那个 `.o` 被拉进产物，
即使你一次也没 `new` 过它。**"导入"与"链进来"是同一件事。**

### 两条边界（实测量出来的）

**① 只 include 不用，就是不注册。** 这不是缺陷：`<Terminal>` 这种标签是
**运行期**才决定要不要建的，编译期不知道。要覆盖这条路径，就显式导入。

**② 自动注册发生在 `main` 之前，但也只早于 `main`。**

实测（应用自己也有个全局对象，组件的 `.o` 确实进了产物）：

```
[全局对象构造期] 注册表 0 项      ← 应用的静态对象可能先跑
[main 期]        注册表 2 项      ← 到 main 就有了
```

跨 TU 静态初始化顺序不可控。所以**不要在某个全局对象的构造函数里按名字建组件**
（那会拿到空注册表）；放到 `main` 之后、或某个惰性初始化函数里就好。

### 曾经走过的弯路（别重试）

这三条都**编译链接全绿、运行时注册表是空的**，它们验的全部是
**头文件里的 `inline` 变量**（没人 ODR-use 时链接器可整块丢弃）：

| 写法 | 断在哪 |
|---|---|
| 头里 `inline const bool x = [](){...}();` | 没有 TU 对它 ODR-use → 链接器可整块丢弃 |
| 同上 + 实现文件里取一次地址 | 只保住**那一份**；"哪个 `.o` 进产物"仍由别处决定 |
| 头里声明非 inline 函数、`.cpp` 里定义 + 静态登记 | 头里那个声明无人引用时，`.cpp` 不进产物 |

⚠ **关键区别（这也是我上次搞错的地方）**：**`.cpp` 里命名空间作用域的静态对象**
和上面第三条不一样——它进 `.init_array`，**只要该 `.o` 进了产物就必然构造**。
上面第三条"失败"的真正原因是「没人引用那个 `.o`」，而不是「静态初始化不可靠」。
这两件事被混为一谈了很久。

还试过把登记项放进自定义链接段（`__attribute__((section))` + `__start/__stop`），
想做到零时序依赖的自动注册——**在 PE/Mach-O 下段边界符号不存在，实测直接段错**。
放弃。

### 可执行证据

- `tools/import_probe.cpp`：只 include/导入 `Panel`、`Text`、`Button`。实测
  `components=8`（注册表里只有 `basic.cpp` 那一个目标文件里的组件）、
  `terminal_present=0`、**产物中 Terminal 符号数 0**。
  这是"按需"在**产物**层面的证据。
- 上面的两个探针（自动注册 / 不注册）是"注册时机"层面的证据。

（`components=8` 是因为 `basic.cpp` 一个文件实现了 7 个基础组件，加上 `Panel`。
按文件粒度按需，而不是按组件粒度——那是 `.cpp` 的组织方式决定的，见下文。）

---

## 三、为什么**不**把指针依赖改成前向声明（一条经验，别重跑）

> 本节是一条**已经做过又撤掉的尝试**的结论。写在这里是因为“该不该前向声明”
> 每隔一阵就会有人重新想一遍，而这次把代价量干净了。

### 当时的做法与收益

把 `window_frame.hpp -> title_bar.hpp`、`title_bar.hpp -> theme_picker.hpp`、
`scroll_view.hpp -> scroll_bar.hpp` 三处（头里只用到 `T*`）改成 `class T;` 前向声明，
并刚掉 `chip.hpp -> icon_view.hpp` 这类真·假依赖。

收益：那几个头被依赖的 TU 数是 11 / 14 / 17 个，而 `title_bar.hpp` 本身 258 行。
**构建时间上量不出来**。语义上确实更准（“只用一个指针”不等于“要知道这个类的一切”），
但这份准确度换不来任何可观测改善。

### 代价（这是撤掉的真原因）

前向声明把错误从“编译期看得见”推到了“会在下次改代码时突然冒出来”，而且
**错不在改的那一行**：

**场景 A：往 `.cpp` 里加了用到完整定义的代码**（模型最常做的事）

```
auto bar = std::make_unique<TitleBar>(std::move(title));
```

实测报错（GCC 16，本仓）：

```
error: invalid use of incomplete type 'class st::ui::TitleBar'
note: forward declaration of 'class st::ui::TitleBar'
```

问题不在“看不懂”，而在：

- 同一个错误**重复 12 次**（每个用到它的成员函数各一次）；
- 夹一条**误导性的级联错误**：`cannot convert 'unique_ptr<TitleBar>' to
  'unique_ptr<Element>'`——它会把人引向“转换写错了”，而不是“缺 include”；
- 编译器**从不提示该 include 什么**（实测：报错里没有任何 include 建议）。

**场景 B：在头里加了值成员**

```cpp
error: field 'bar_' has incomplete type 'st::ui::TitleBar'
note: forward declaration of 'class st::ui::TitleBar'
```

这条倒是很干净、位置也对。所以场景 B 不是问题，**场景 A 才是**。

### 一个可行但不愿背的缓解办法

把“需要完整定义时 include 哪个头”写在**前向声明同一行的注释**里。编译器的 note
会把出错那一行的源码打出来（实测有效）：

```
note: forward declaration of 'class st::ui::TitleBar'
 49 | class TitleBar;  // 前置声明：在 .cpp 里用它请 include "st/ui/components/title_bar.hpp"
```

管用，但要 60 多个头 × 每一处前向声明都写对、永远不许漂。
**用“永不漂移的人工约定”去换“量不出来的构建收益”，不划算。**

### 结论（以后照这个判）

| 情况 | 做法 |
|---|---|
| 头里只用 `T*` / `T&` / 返回 `T*` | **照旧 include**。本仓不采用前向声明换收益 |
| 头里**根本不用**那个类型 | **删掉 include**（这种是纯多余，无条件该删） |
| 头里真的需要完整定义 | include（本来就得） |

第二行是本次唯一留下的改动：`chip.hpp` 曾经 include `icon_view.hpp` 却根本不用
（Chip 的图标是运行期经 `IconRegistry` 按名取的）。这种“纯多余”不换任何可读性，
删掉没有争议。

⚠ 一条必须记住的反例：`button.hpp` 里也搜不到 `IconView` 字样，但它调了
`Icon::ink_size(...)`（常量表达式），**必须**完整定义。删了立刻报
`'Icon' has not been declared`。**“头里搜不到那个类名”不等于“不依赖那个头”。**

---

## 四、加一个组件要做什么

1. **新建头** `st/ui/components/<名字>.hpp`：只 include 它自己需要的东西
   （别 include `all.hpp`；别的组件头只在真有依赖时 include——注意上节那条，
   只用指针也照 include，不要为了“更纯粹”改前向声明）；
2. **在头末尾加一行**：

   ```cpp
   ST_UI_COMPONENT(::st::ui::MyWidget, "MyWidget")
   ```

   需要构造参数的组件用 `ST_UI_COMPONENT_WITH(类型, "名字", 默认构造表达式)`；

3. **在组件实现的 `.cpp` 末尾加一行**：

   ```cpp
   ST_UI_COMPONENT_IMPL(::st::ui::MyWidget);
   ```

   并（推荐）在同一个文件里自导入，保证"实现进产物 = 已登记"：

   ```cpp
   namespace {
   [[maybe_unused]] const bool kImported = []() {
     st::ui::import_components<::st::ui::MyWidget>();
     return true;
   }();
   }
   ```

4. **如果是基础组件族**，还要在对应的聚合头（`basic.hpp` / `input.hpp` / …）
   里 include 它；**如果希望它出现在全量头里**，在 `all.hpp` 的 include 区与
   `import_all_components()` 的类型列表里各加一行。

5. **如果它有视觉形态**，在 `tests/support/gallery_exhibits.hpp` 登记一张展示卡
   （或在 `kGalleryExempt` 里写一条理由）。`tests/gallery_component_parity_test.cpp`
   会断言"注册表 − 豁免 == 展示集"，漏了会红。

---

## 五、目录里都有什么

### 聚合头（开发期方便，交付前建议改回按需）

| 头 | 内容 |
|---|---|
| `all.hpp` | 全部组件 + `import_all_components()` |
| `basic.hpp` | 基础展示：`Text` / `Heading` / `Icon` / `Button` / `Card` / `Divider` / `KeyValueRow` |
| `feedback.hpp` | 反馈：`ProgressBar` / `Spinner` / `Badge` / `Chip` / `Avatar` / `Tooltip` |
| `toggle.hpp` | 开关：`Checkbox` / `Radio` / `Switch` |
| `input.hpp` | 输入：`Input` / `TextArea` |
| `list.hpp` | 列表：`List` / `ListItem` |
| `scroll.hpp` | 滚动：`ScrollView` / `ScrollBar` |
| `overlay.hpp` | 浮层：`Dialog` / `Toast` |
| `select.hpp` | 选择：`Select`（实现在 `select_box.hpp`）/ `SelectPanel` |
| `panel.hpp` | 框架容器：`Panel` / `Spacer`（定义在 `element.hpp`，本头只做登记） |

### 一组件一头

其余每个组件一个头：`terminal.hpp`、`table.hpp`、`code_editor.hpp`、`diff_view.hpp`
…… 名字即组件名。

### 连线：`Connector`

把**任意两个点/元素**连起来（流程图、节点编辑器、思维导图、状态机、拓扑图）。
它是**自绘单件**，不依赖任何容器：

```cpp
#include "st/ui/components/connector.hpp"

auto line = std::make_unique<st::ui::Connector>();
line->set_points({20, 60}, {180, 120});                 // 本地坐标（= 自身 bounds 内）
line->set_route(st::ui::Connector::Route::Curve);       // Straight / Orthogonal / Curve
line->set_arrow(st::ui::Connector::Arrow::End);         // None / Start / End / Both
line->set_dash_csv("6 4");                             // 空 = 实线
line->style().absolute = true;   // 铺满父级、不进 flex 流（推荐形态）
line->style().width = 200.0f;
line->style().height = 140.0f;
parent->add_child(std::move(line));
```

端点四种给法（优先级从高到低）：`set_source_fn/set_target_fn`（每次绘制现算的表达式）
→ `set_source/set_target(element, side)`（跟随元素，元素不可见时整条不画）
→ `set_points`（固定坐标）→ 什么都不设（用自身 bounds 的对角）。

三件容易踩的：

- **坐标是本地坐标**（原点 = 自身 `bounds` 左上角）。给元素端点时组件内部会把
  `bounds()` 的绝对坐标换成同一套坐标；自己算多边形时别再把原点加一次。
- **命中是沿线窄带**（`hit_tolerance`），不是整块 bounds——所以 `absolute` 铺满父级
  时它**不会吃掉**下层按钮的点击。要“点线选中”就在 `on_line_event` 上接回调。
- **虚线是路径层切段**：光栅层没有 dash（`StrokeStyle` 只有 cap/join），
  段数越多描边次数越多——界面里几十条线的规模完全够用。

回归：`tests/ui_connector_test.cpp`（几何/路由/虚线/箭头/命中/属性面），
逆向验证：`tools/connector_reverse_verify.py`。

### 不进声明式注册表的组件

三维族（`three_view.hpp` / `scene_view.hpp` / `graph_view.hpp` / `label_overlay.hpp`）
**不在**注册表里：声明式创建在无头环境没有意义，它们由 C++ 直接 `new` 并挂树。
各自的头末尾写了理由。

---

## 六、已知的粒度限制

**按需的粒度是 `.cpp` 文件，不是组件。** 一个 `.cpp` 实现了多个组件时，
导入其中任何一个都会把整个文件（以及里面其余组件）带进产物。

这是 `.cpp` 组织方式的直接后果，不是导入机制的问题。若确实需要更细的粒度，
把那个 `.cpp` 拆开即可（例如把 `basic.cpp` 拆成 `text.cpp` / `button.cpp` / …）。
当前**没有**这样做：一个文件里放同一族的几个小控件，编译与维护都更省事，
而它们通常也是一起用到的。
