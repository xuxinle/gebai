# 组件库：一个组件一个头，按需导入

本目录（`include/st/ui/components/`）放霜天的全部 UI 组件头。
这份文档回答三件事：**怎么用**、**为什么这么设计**、**加组件要做什么**。

---

## 一、怎么用

### 开发期 / XML 应用：全量头

```cpp
#include "st/ui/components/all.hpp"

// 某个初始化函数里（**必须调**，见下文"为什么要导入"）：
st::ui::import_all_components();
```

一个 include 拿到全部组件，XML 里写什么标签都能用。

### 交付期：按需导入

```cpp
#include "st/ui/components/button.hpp"   // 只拿 Button
#include "st/ui/components/text.hpp"     // 只拿 Text

st::ui::import_components<st::ui::Button, st::ui::Text>();
```

**产物里就只会有这两个组件**（以及它们真正依赖的东西）。
没 include、没导入的组件整块不进产物。

### XML 应用要自己导入

XML 里的标签是运行期才决定的，编译期枚举不出来。所以：

- 用 `all.hpp` + `import_all_components()`：全部标签可用（推荐）；
- 或者自己按需 include 用到的组件，逐个 `import_components<T>()`。

写了没导入的标签**不会静默消失**：`XmlBuildReport::unknown_tags` 会如实报出来
（见 `docs/xml-ui.md`）。

---

## 二、为什么是"导入"而不是"include 就自动注册"

这一条踩过坑，写清楚免得下一个人再试一遍。

注册必须发生在**运行期**（`dsl::make_element(name)` 是按字符串查表），而"组件的
代码在不在产物里"由**链接器**按引用决定。两者之间有一条硬约束：

- **静态库里没人引用的 `.o` 不进产物**——这正是"按需"的机制本身；
- 于是"只 include 头、不引用任何符号"的组件不会进产物，它的**任何静态初始化
  都不会运行**；
- 想让注册"自动"发生就得引用它，那又让"用不用"变成编译期常量，按需失效。

实测过三条死路，全都**编译链接全绿、运行时注册表是空的**：

| 写法 | 断在哪 |
|---|---|
| 头里 `inline const bool x = [](){...}();` | 没有 TU 对它 ODR-use → 链接器可整块丢弃 → 初始化不发生 |
| 同上 + 实现文件里取一次地址 | 只保住**被取地址的那一份**；"哪个 `.o` 进产物"仍由别处决定 |
| 头里声明非 inline 函数、`.cpp` 里定义 + 静态登记 | `.cpp` 不进产物，定义与登记都不存在 |

所以本框架采用**显式导入**，与 Qt 的 `qmlRegisterType`、Flutter 的
`registerViewFactory` 同一形态。

**这一步不是形式主义**：`import_components<T>()` 引用
`detail::component_importer<T>::run`，那个定义在组件的 `.cpp` 里——所以调用它
= 对那个目标文件产生真实引用 = 链接器必须把它拉进来。**"导入"与"链进来"是同一件事。**

### 有可执行证据

`tools/import_probe.cpp` 是一个独立的验证程序：它只 include/导入 `Panel`、`Text`、
`Button` 三个组件。实测结果：

```
components=8          ← 注册表里只有 basic.cpp 那一个目标文件里的组件
panel=1 text=1 button=1
terminal_present=0    ← Terminal 查不到
产物中 Terminal 符号数: 0   ← 而且二进制里没有它的任何符号
```

（`components=8` 是因为 `basic.cpp` 一个文件实现了 7 个基础组件，加上 `Panel`。
按文件粒度按需，而不是按组件粒度——那是 `.cpp` 的组织方式决定的，见下文。）

---

## 三、include 关系怎么摆（这一节是「一组件一头」真正难的部分）

光把文件切开不等于依赖就干净了：拆完一问发现过 4 处**假依赖**（include 了但\.hpp\n里根本不用），还有 5 处是「只用指针」——本来前向声明就够，却把对方的\.hpp\n整个拉进了每一个用它的翻译单元。

### 规则

头里用到对方的方式，决定了能不能去掉 include：

| 头里的用法 | 需要什么 | 能不能去 include |
|---|---|---|
| 成员对象、值传参/返回、`std::vector<T>` 成员 | **完整定义** | 不能 |
| 继承（`class Heading : public Text`） | **完整定义** | 不能 |
| 嵌套类型（`CodeEditor::LineDecoration`） | **完整定义** | 不能 |
| 常量表达式里调静态成员（`Icon::ink_size`） | **完整定义** | 不能 |
| 仅 `T*` 成员 / 返回 `T*` / 形参 `T&` | **前向声明** | **能，而且应该** |

去 include 后，那个 ".cpp" 必须自己 include 它（它要构造/解引用）——这是该付的代价，
而且付在正确的地方：只有真正实现它的人扛这份依赖。

### 两个头：身份 vs 注册表

组件头只 include **`st/ui/component_id.hpp`**（只依赖 `<memory>`，装着
`ST_UI_COMPONENT` 宏与两个模板声明）；
**`st/ui/component_registry.hpp`** 是"真正登记/查表"的地方才要的
（`add` / `create` / `names` / `import_components`），它要 `<functional>`、
`<string>`、`<vector>` 等。

> 实测记一笔：只 include `button.hpp` 展开约 7.6 万行，其中 `element.hpp` 占绝对主体，
> `component_registry.hpp` 那几百行**不是瓶颈**。所以拆两个头**不是为了省编译时间**，
> 而是为了把依赖关系说准：`<functional>` 出现在 60 个组件头里，仅仅因为有人把
> 宏和注册表搞混了。

---

## 四、加一个组件要做什么

1. **新建头** `st/ui/components/<名字>.hpp`：只 include 它**自己需要**的东西
   （别 include `all.hpp`；别的组件头只在真有依赖时 include，且先对照上表判断
   是不是前向声明就够）；
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
