# 声明式 UI（`st::ui::dsl` + `declarative.js`）—— 设计方案 v2

> 对标 **Jetpack Compose**（`@Composable` / `remember` / `State<T>` / 重组）与
> **鸿蒙 ArkTS**（`@Component` / `@State` / `@Builder` / build()）。
> 一句话：**描述界面随状态变化的最终形态，框架负责把状态变化翻译成对保留模式元素树的
> 最小修改**。命令式（C++ Element 树）继续存在——它是真值树，声明式是它之上的编排层。
>
> **v2 修订**：C++ 与 JS **双宿主**——C++ DSL 为一等公民（编译期类型安全、零跨界），
> JS 层服务运行时与 AI（QuickJS 复用 ScriptHost）。两套重组器实现，**同一份语义规范**
> （同一组测试场景双宿主跑），共用元素工厂与落地管线。

## 0. 定位与不变式（为什么这样做）

霜天已有的资产决定了这条路线的形状：

| 已有资产 | 在声明式层中的角色 |
|---|---|
| 保留模式 `Element`/`UiRoot` + 32 组件 + `Panel` flex | **真值树**：布局/绘制/事件/脏区照旧 |
| `apply_properties` / `element_snapshot` / `invoke_element`（三路共用） | **统一的写入口与读出口**：diff 结果经它落地，不另立旁路 |
| `sync_items/sync_tabs/sync_nodes`（按 key 复用） | 列表组件的**数据驱动刷新**已验证的形态 |
| `ScriptHost` + QuickJS（可选依赖，默认关） | **JS 宿主**：AI 运行时写声明式 UI 的载体 |

四条不变式（贯穿后续所有设计，违反任何一条都要回头改设计而不是绕过去）：

1. **真值树唯一**：声明式层**永远不绕过** `apply_properties`/元素树 API 改界面。
   状态 →（重组）→ VDOM →（diff）→ 变更集 →（`apply_properties` + 树操作）→ 真值树。
2. **一套语义、四个入口**：C++ 命令式 / **C++ 声明式** / 协议 `get·set·invoke` /
   **JS 声明式（+既有脚本命令式）**，看到的界面完全一致（声明式元素同样有 id、属性面、
   语义树——`tree`/`find` 照常工作）。
3. **不挂即零开销**：不用声明式（C++ 不 mount 声明式根 / 应用不带 `--ui declarative`）
   时零运行时开销；JS 腿在 `ST_FEATURE_SCRIPT` 关闭时整层不存在（见 §8）。
4. **双宿主同一语义**：C++ 与 JS 重组器行为由同一份语义规范钉住——同组测试场景
   （diff/调度/状态传播）在两个宿主各跑一遍，结果必须一致（防「两套真相」漂移）。

## 1. 用户代码形态

### 1.1 C++ 声明式（一等公民：`st::ui::dsl`）

**ArkTS 风格（struct + State 成员 + build()）——C++ 的自然映射**：

```cpp
// 形态示意（完整实例见 examples/codeeditor —— 整个 IDE 就是一份 Component）
#include "st/ui/dsl.hpp"

struct CounterPage : st::ui::dsl::Component {
  st::ui::dsl::State<int> count{0};              // ≈ @State count = 0
  st::ui::dsl::State<std::string> label{"计数器"};

  void build(st::ui::dsl::Composer& c) override {   // ≈ build()
    using namespace st::ui::dsl;
    row(c, {.gap = 12, .padding = 16}, [&] {
      text(c, [&] {                                // 惰性闭包 = 状态读取点（自动追踪）
        return std::format("{}：点击了 {} 次", label.value(), count.value());
      });
      button(c, {.label = "+1"}, [this] {          // 事件：写状态 → 触发重组
        count.set(count.value() + 1);
      });
      switch_(c, {.checked_lazy = [&] { return count.value() > 3; }});
    });
  }
};

int main(int argc, char** argv) {
  return st::app::run(argc, argv, [](st::app::App& app) {
    app.mount<CounterPage>();       // 声明式挂载：Component → 重组器 → 真值树
  });
}
```

**Compose 函数风格（无 struct，就地组合）**：

```cpp
st::ui::dsl::compose(app, [&](Composer& c) {
  auto& query = c.state<std::string>("");
  auto results = c.memo([&] { return search_index.find(query.value()); },
                        c.deps(query));
  column(c, {.gap = 8}, [&] {
    input(c, {.placeholder = "搜索…", .value = query.value(),
              .on_input = [&](std::string_view v) { query.set(std::string(v)); }});
    for_each(c, results.value(), [](const Item& it) { return it.id; }, [&](const Item& it) {
      list_item(c, {.text = it.name});
    });
  });
});
```

C++ DSL 的形态决策：

| 决策 | 理由 |
|---|---|
| **Props 是强类型结构体**（指定初始化器 `{.gap = 12}`），非字符串映射 | 编译期检查；diff 用结构体相等比较（零字符串开销）；落地时序列化走 `apply_properties`（语义钉子不变，见 §5） |
| **惰性 prop 用闭包**（`[&]{ return ...; }`） | 依赖收集的读取点——闭包内读 State 自动登记订阅；与 JS 侧「props 可为闭包」同一语义 |
| **Component = 框架所有权**（mount 后由宿主持有 `shared_ptr`） | 悬垂防护：闭包捕获 `this` 安全，重组任意时刻回调不悬垂（§5.4） |
| 事件回调是普通 `std::function` | 与既有组件 `on_click` 等回调形态一致 |

### 1.2 JS 声明式（运行时宿主：AI 可写、免编译）

**Compose 风格**：

```js
// app.js —— st run app.js / 控制通道 script 下发
compose('Counter', () => {
  const count = useState(0)                    // ≈ remember { mutableStateOf(0) }
  return Row({ gap: 12, padding: 16 }, [
    Text(() => `点击了 ${count.value} 次`),      // 闭包 = 状态读取点，自动追踪
    Button({ label: '+1' }, () => { count.value++ }),
    Switch({ checked: count.value > 3 }),
  ])
})
```

**ArkTS 风格（轻转译，§7）**：

```js
struct Counter {                               // ≈ @Component struct Counter
  @State count = 0
  @Prop label = '计数器'
  @Builder
  build() {
    Row({ gap: 12 }, [
      Text(this.label + '：' + this.count),
      Button('+1').onClick(() => { this.count++ })
    ])
  }
}
```

两种风格、两个宿主，编译到**同一棵中间表示**（§2 VDOM）——「C++ struct 风格 / C++ 函数
风格 / JS compose / JS struct」四种写法同一运行语义。

### 1.3 事件与异步（两个宿主同一套 hooks 名）

```cpp
// C++
auto data = c.resource([](const std::string& q) { return st::fs::list(q); }, query);
// data.value() → { status: pending|ok|error, value?, error? }；结果落地自动重组
```

```js
// JS
const data = useResource(async (q) => (q ? st.fs.list(q) : []), query)
```

`resource/useResource`：异步取数 → 内部 StateCell 落地 → 触发重组；输入变化自动取消
旧请求（代次计数，旧代次丢弃）。`For/for_each` 是内置控制流（ArkTS `ForEach` /
Compose `key` 的合体）：数据数组按 key 复用真值树子元素（语义同 `sync_items`），
不是逐帧重建。

## 2. 运行时架构（双宿主、四层）

```
        C++ 宿主（编译期）                    JS 宿主（运行时，QuickJS）
┌────────────────────────────┐  ┌────────────────────────────────┐
│ L4  Component/build、compose │  │ L4  compose()/struct 代码      │
│ L3  C++ 重组器（调度/依赖）    │  │ L3  JS 重组器（调度/依赖）      │
│ L2  C++ VDOM（类型化 Props） │  │ L2  JS VDOM（动态 props）      │
└──────────┬─────────────────┘  └──────────┬─────────────────────┘
           └──────────────┬───────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│  L1 元素工厂（C++，共享）: create(type) / apply_props / mount     │
├─────────────────────────────────────────────────────────────────┤
│  L0 真值树   Element / UiRoot / Panel / 32 组件（现状不动）       │
└─────────────────────────────────────────────────────────────────┘
```

为什么两套重组器而不是一套：

| | C++ 宿主 | JS 宿主 |
|---|---|---|
| 重组执行位置 | 进程内，零跨界 | QuickJS 内，diff 纯 JS 完成 |
| 跨界次数 | **0**（直接树操作） | 每帧 ≤1 次批量提交（复用 ScriptHost 变更集管线） |
| props 形态 | 类型化结构体（编译期检查） | 动态对象（运行时灵活） |
| 服务对象 | C++ 应用（gallery/vsedit/第三方） | AI 运行时搭界面、热更新逻辑 |

共享的部分：L1 元素工厂、状态语义规范（§4）、调度护栏参数（§4.2）、diff 对齐规则
（key/位置）、以及**同一组验收测试场景**（§9）。两套实现的 diff 算法各 ~400 行，
语义由规范钉住——这是「同一语义、两处实现」的自觉权衡（强行共享会让 C++ 侧付出
跨界或 JS 侧付出运行时生成成本，两头不讨好）。

## 3. L1 元素工厂（C++ 薄桥，两宿主共用）

```cpp
// include/st/ui/element_factory.hpp（新增）
namespace st::ui {
struct ElementFactory {
  /// 类型名 → 元素构造（唯一注册表；C++ 声明式、JS 声明式、协议三条路共用）
  [[nodiscard]] auto create(std::string_view type) const -> std::unique_ptr<Element>;
  /// 应用属性包（内部走 ui::apply_properties —— 一套语义的落点）
  static auto apply(Element& el, const st::Json& props) -> st::Json;
  /// 树操作：挂载 / 摘除（add_child / insert_child / remove_child / set_child / overlay）
  static auto mount(UiRoot&, Element* parent /*nullptr=root*/, std::size_t index,
                    std::unique_ptr<Element>) -> Element*;
  static auto unmount(UiRoot&, Element&) -> std::unique_ptr<Element>;
};
[[nodiscard]] auto element_factory() -> ElementFactory&;   // 全局注册表（启动期注册 32 组件）
}
```

- 注册表内容 = 组件类型名（tree 输出同款）+ 构造函数指针 + 构造参数约定
  （`Select` options、`Table` columns 等**非属性面可后置**的构造期属性，§10 差距表）；
- **协议在线建 UI 顺带解锁**：工厂落地后 `script` 的 `ui_create('Button', {...})`
  水到渠成（AI 在线搭界面能力，BACKLOG 挂此项）；
- 属性包里 `id`/`key` 先行截获（`set_id`/`set_key`），其余走属性面。

## 4. 状态系统（两宿主同一语义）

```cpp
// C++                                        // JS（同一语义的 JS 形态）
State<T> s{init};                             const s = useState(init)
s.value()                    // 读：登记依赖      s.value
s.set(v)                     // 写：失效 + 调度    s.value = v
memo<T>(c, fn, deps)         // 依赖未变不重算       useMemo(fn, deps)
effect(c, fn, deps)          // 副作用，可返回清理   useEffect(fn, deps)
ref<T>(c, init)              // 跨重组稳定（非响应式） useRef(init)
resource<T>(c, fetch, input) // 异步状态            useResource(fetch, input)
persisted<T>(c, key, init)   // 会话级持久（按名字） usePersisted(key, init)
```

### 4.0 两个契约（不知道就会写错）

**① hooks 按调用点序号对齐槽位——不能写在条件分支里。**

`memo`/`effect`/`ref`/`resource` 共享一个游标（C++ 侧 `Composer::hook_index()`，JS 侧
`hookCursor`）：同一 build 里第 N 个 hook 调用 ↔ 第 N 个槽，跨重组复用。条件分支里
调用它就会错位——错位不是崩溃而是**静默串味**（第 2 个 hook 拿第 1 个的缓存）。
唯一按**名字**取槽的是 `persisted`：它的身份是业务 key，条件剪掉再声明也能拿回旧值。

**② `memo` 的依赖集同时承担“订阅”职责。**

`Deps{{&a, &b}}` 不只是比较用的指纹——它同时把 `a`/`b` 登记为本作用域的依赖。
若只在“真的重算”那帧才登记，命中缓存那帧作用域就会“忘了”依赖，依赖下一次
变化时无人订阅 → 界面静默停在旧值（实测踩到：`calls` 值停在 1 不动）。

### 4.0.1 副作用（effect）的执行时机

`effect` 在**本帧重组结束之后**执行（C++ 侧 `Composer::reconcile()` 末尾；JS 侧
`__d_reconcile()` 末尾），不在 build 中途。为何：副作用里写状态属于**连锁写**，
而本次重组的脏标记清理就在 build 之后——内联执行的那次写会被当成“本次已处理”
而吞掉，界面停在旧值（代码看着对、日志也没错，最难查的一类）。

重组末尾执行则写状态自然标脏；JS 侧还多一层**收敛循环**（≤4 轮，防 effect 每轮都写
状态的链子拖成死循环）——否则调一次 `tick()` 只能推进半拍（`useEffect` 里写的界面
永远滞后一帧；实测：`log=` 而不是 `log=1`）。依赖变化时**先跑上次的清理**再跑本次的体。

### 4.1 依赖收集与重组范围

- **读取点**：重组作用域（Component::build / compose 函数）执行期间读 State → 该作用域
  登记为订阅者（C++ 用 thread-local 当前 Composer；JS 用当前执行栈标记）；
- **失效传播**：写 State → 订阅作用域标脏 → `useMemo/依赖` 派生链沿依赖图传播；
- **重组单位 = 整个作用域**（与 Compose 一致：函数级、非表达式级）——简单正确优先，
  细粒度跳过（ComposableSkipping 级）为 v2 项；
- **惰性 prop 闭包**归属所在作用域：`text(c, [&]{ return ...count... })` 中 count 的
  读取把**外层作用域**登记为订阅者，count 变化 → 重跑该作用域 → 重新求值闭包。

### 4.2 调度与护栏（两宿主同参数）

写状态 → 标脏 → **下一帧帧首统一重组**（与 `UiRoot` 帧循环对齐；同帧多次写合并）。

| 护栏 | 默认 | 行为 |
|---|---|---|
| 单帧重组预算 | 4ms | 按作用域计：超了停手，剩下的**留在树上顺延下一帧**（各自仍带脏标记 `dirty()` 为真 → 下一帧补跑；不是丢弃）+ `budget_exceeded` 标记 |
| 作用域嵌套深度 | 64 | 超深**拒绝声明** + `stats.error` 上报（防递归 build 撞栈，见下） |
| 连锁写 | 隔帧 | 重组中再写状态记入下一帧，不内联递归（防死循环燃烧 CPU） |
| 错误隔离 | 冻结 | build 抛异常 → 该作用域保留上一帧 UI，上报 `on_error` + 协议事件（不白屏） |

**深度护栏为何必须有**：递归 build（组件在自己 build 里又声明自己——「列表里每项再渲染一个
同样的组件」写漏终止条件就会踩到）没有护栏时深度无界增长，最后是栈溢出（SIGSEGV），
现场只剩一大堆 `run_scope` 帧，很难指到真正的错处。护栏把它变成一条明确错误：
拒绝声明 + 写 `stats.error`（实测：去掉判断即 0xC00000FD，现场只有重复的递归帧）。

### 4.3 状态持久化与全局状态

- `persisted/usePersisted`：写穿透宿主状态仓（进程生命期内存态；协议 `script.state`
  可整体读回——AI 可观测）；落盘 v2；
- 跨组件通信 v1 = 模块级/宿主级单例 State + props 下传/事件上抛；不引入 store 全家桶
  （等真实示例暴露痛点再决定）。

## 5. C++ 宿主（`st::ui::dsl`）设计要点

### 5.1 Composer 与作用域树

```cpp
class Composer {
 public:
  // 作用域内可用：row/column/panel/text/button/…（32 组件全覆盖的薄包装）
  template <class T> auto state(T init) -> StateHandle<T>;   // 框架所有权
  template <class T> auto memo(auto fn, Deps deps) -> StateHandle<T>;
  void effect(auto fn, Deps deps);
  // 供包装函数用：进入/退出作用域、登记 VNode
  class ScopeGuard;   // RAII：作用域执行期设置 thread-local current
};
```

`build()` 重跑时，Composer 按 VDOM 位置对齐旧节点（复用/新建/销毁决策与 JS 侧同一
diff 规则）。VNode 是 C++ 结构（type + 类型化 props + children + 惰性闭包槽）。

### 5.2 类型化 Props 与「一套语义」的钉法

每个组件一个 Props 结构（`ButtonProps{ .label, .variant, .on_click, ... }`，指定初始化器），
diff 按成员相等比较；**落地路径统一**：变更成员 → `st::Json` → `apply_properties`。
这样协议 `get` 读到的属性面与声明式写入的完全一致（不变式 2 的落点）。闭包型成员
（`on_click`、惰性 text）不参与序列化，由 Composer 直接接既有组件回调接口
（`Button::set_on_click` 等——它们本就是组件的一等接口，不经属性面）。

### 5.3 for_each 与列表

```cpp
for_each(c, items, key_fn, item_fn);   // ≈ For/ForEach
```

数据数组 → 按 key 对齐复用（同 `sync_items` 语义）；`List/Table/Tabs` 的**数据型子项**
（非子元素）由专用包装承接（`list(c, {.items, .selected})` 内部调 `sync_items`），
v1 覆盖 List/Table/Tabs 三件套，其余组件 v1.1 逐个审查（§10）。

### 5.4 C++ 生命周期安全（悬垂防护，条款化）

| 风险 | 防护 |
|---|---|
| 闭包捕获栈上 State | `state()` 返回**框架所有权的 StateHandle**（shared_ptr 语义），捕获句柄安全；Composer 文档与 lint 禁止裸引用捕获栈对象（lint 规则挂 `st lint`，模式匹配 `[&]` + 局部 State 声明告警） |
| Component 挂载后被析构 | `mount` 后所有权归宿主（`shared_ptr<Component>`），应用侧只持弱引用或裸指针观察 |
| 回调时组件树正在重组 | 事件回调只发生在帧的事件段（重组在帧首完成）——时序由 UiRoot 帧循环保证，非锁 |
| effect 清理函数 | 作用域销毁/依赖变化时先调旧清理（RAII 链，防泄漏监听） |

### 5.5 调用方必须知道的三件事（写声明式代码前先读）

这三条都是“不知道就一定会写错、而且错得不像自己的错”的契约。

**① `mount` 是单根替换，`mount_into` 才是“挂一块”。**

- `dsl::mount(root, component)` 会**替掉 `UiRoot::content()`**——整个界面都是声明式的场合用它。
- 宿主界面（手搭外壳、只想让某一页/某一区域声明式）必须用
  `dsl::mount_into(root, host_element, component)`：只占 `host_element` 那个子位。
- 两者返回值都是 `DeclarativeHost*`（`unique_ptr`），**由调用方持有**；丢了它 = 不再重组。

**② 帧首必须推进宿主，而且 `pump_async` 要在判脏之前。**

```cpp
// 主循环每帧（无头与窗口一致）：
if (decl != nullptr) {
  (void)decl->pump_async();      // 先泵异步结果（它会把状态写回去）
  if (decl->dirty()) {           // 再判脏（异步落地后这里才为真）
    (void)decl->tick();          // 重组
    app.request_repaint();
  }
}
app.tick();
```

**顺序反过来就是一个静默死锁**：异步结果到达时状态还没写、作用域不脏，
只在 `dirty()` 为真时才调 `tick()` 的话，`pump_async` 永远不会被调到——
`resource` 永远停在“计算中…”（实测踩到：异步卡片一直不收敛）。

**③ 重组会销毁重建元素：构建期存的指针不可跨帧使用。**

- `build()` 里 `custom<T>` 拿到的 `T*` 只在**本帧**有效；存到成员里下一帧可能悬垂。
  - 用法：每帧 `build` 开头置空、在 `build` 里重新取得，且**只在本帧后续代码里用**。
- 需要“跨帧生效”的效果（如“打开文件后跳到某行”）不能当场调，要**排队到下一帧**：
  记下待办 → 本帧末尾/下一帧 `build` 之后再执行（codeeditor 的 `pending_jump_` 就是这个模式）。
- **`set_text` 类接口会重置状态**（光标归零、撤销栈清空）：只能“值真变了”时写，
  不能每次重组都无脑写——否则用户打字会被每帧重置（实测踩到）。
  做法：记一个“当前已装载的 key”，与目标不一致时才写。

## 6. JS 宿主（`declarative.js`）设计要点

- VDOM/重组器纯 JS（~400 行，编译期嵌入，机制同 `script_api.js`）；diff 在 JS 内完成，
  落地时**每帧最多一次跨界**（变更集批量提交，复用 ScriptHost 的 pending 队列）；
- **复用 ScriptHost 的引擎、事件桥、定时器**——一个应用一个 JS 上下文，
  `$('#x').set(...)`（命令式）与声明式 diff 共存共享提交管线；
- 挂载点：`DeclarativeHost`（持有 ScriptHost + JS 重组器句柄）注册进 UiRoot 帧循环，
  与 C++ 宿主同一调度护栏参数（§4.2）；
- `useElement(selector)` 逃生舱：命令式读句柄（写句柄 v2）。

## 6.1 id/key 语义（与既有约定对齐，两宿主一致）

VNode 的 `key` 直接映射 `Element::set_key`（§4.5.1.1 业务身份）；自动 id 规则照旧
（`Type@key` / `Type[i]`）——**声明式产出的元素树与手搭的树在选择器/协议/语义树层面
不可区分**。

## 6.2 真值树被外部改动（自愈）

协议 `set` 直接改了声明式产出的元素 → 下次重组发现「VDOM 认为在、真值树没了」→
按丢失重建（单向数据流的自愈路径）。反向（声明式与协议改同一元素）由不变式 1 兜底
——都走 `apply_properties`，无第二修改路径。

## 7. ArkTS 风格转译器（仅 JS 宿主）

C++ 侧 struct 风格是**原生形态**（无需转译）；JS 侧 struct 风格经 `arkts_compat.js`
转译（~200 行结构化文本变换：`struct X {` 抓取、`@State/@Prop/@Builder/build()` 识别、
重排为 `compose()`）。边界外**明确报错**引导改写（错误带行号与建议片段）。
**不做完整 ArkTS 方言**（完整方言需要 TS 编译器 + 装饰器元编程；霜天目标是形态兼容
——AI 迁移成本低、心智模型一致——不是语法完全兼容）。转译层薄、可删、可换。

## 8. 构建集成与零开销

- `declarative.js`/`arkts_compat.js` 以 `b::embed` 嵌入；C++ 侧 `dsl.hpp/cpp`、
  `element_factory.cpp`、`declarative_host.cpp` 为常驻库代码（**无 JS 依赖**——
  C++ 声明式在 `ST_FEATURE_SCRIPT` 关闭时完整可用）；
- JS 腿整体打在 `ST_FEATURE_SCRIPT` 条件编译内；C++ 重组器不 mount 声明式根时，
  帧循环无额外成本（无注册即无回调）；
- 示例（2026-10-03 示例整合后的现状）：**`examples/codeeditor/`（整个 IDE 由一份
  `Component::build()` 描述）+ `examples/gallery/` 的「声明式」页**（用 `mount_into`
  把声明式子树挂进手搭页壳——演示状态驱动表单 / key 对齐列表 / 异步 `resource` / 条件内容）。
  两者覆盖「整页声明式」与「页内一块声明式」两种形态。

> **早先的四个小示例（`counter` / `counter-js` / `todo-js` / `codeeditor-dsl`）已删除**：
> 它们的验证价值已被 `tests/ui_dsl_test.cpp`（C++ 十六用例，含 overlay 生命周期 /
> 异步 resource 与取消 / 嵌套作用域树 / 构造期属性组件 / 多作用域细粒度 / 子树挂载 /
> 载体内部件不被裁剪）、`tests/ui_declarative_host_test.cpp`（JS 十一用例）、
> `tests/ui_declarative_parity_test.cpp`（双宿主一致性）完整覆盖。
> 保留示例的门槛是「演示形态 + 能当靶场」，而不是「把单测再跑一遍」。

## 9. 测试与验证策略

| 层 | 测试 |
|---|---|
| 元素工厂 | 每组件 create + apply 全属性面往返（snapshot == 输入）；未知类型报错 |
| C++ 重组器 | `st test` 单测：作用域对齐/props diff/`for_each` key 复用（头插·重排·删除后身份不变）/调度护栏（预算顺延/深度拦截/连锁/冻结） |
| JS 重组器 | QuickJS 宿主内单测：同上场景 + 片段摊平（`forEach` 结果塞进 kids 数组）+ 事件绑定跨帧不重绑 / 卸载反注册 + 换页不叠树 + 「跨界次数」断言（N 次状态写 → 每帧 ≤1 次批量提交） |
| **双宿主一致性** | 同一组场景 fixture（JSON 描述：状态序列 → 期望 tree 快照），C++/JS 各跑，结果必须逐字节一致（不变式 4 的可验证形态）；现含列表/条件裁剪/key 身份跨插入四组 |
| 集成（无头） | 声明式应用照常可被协议驱动：`tree` 断言结构、`invoke click` 后 `get text` 断言传播、连续 100 次点击 → 恰 100 次变化（见 `tools/codeeditor_e2e.py` 与 `tools/framework_gaps_e2e.py`） |
| 回归 | gallery/codeeditor 全场景截图对比（不挂声明式时零差异） |
| 谬误注入 | build 抛异常 → 冻结 + 事件；递归 build → 深度拦截（**用例已落地**：去掉判断即栈溢出）；悬垂捕获 → lint 报警 |

**验收标准**（2026-10-03 全部达成，记录如下）：① ~~counter 三形态无头+窗口跑通~~
→ **改为**：声明式语义由 `tests/ui_dsl_test.cpp` + `ui_declarative_host_test.cpp`
+ `ui_declarative_parity_test.cpp` 三大件全绿（示例不再承担单测职责）；
② **gallery 新增「声明式页」**（状态驱动表单 + key 对齐列表 + 异步 `resource` + 条件内容，
经 `dsl::mount_into` 子树挂载）——✅ 已落地（页壳手搭 + 内容区声明式，两者共存于同一界面）；
③ 双宿主一致性 fixture 全绿——✅；④ 测试全绿（592 用例）+ DESIGN.md §4.5.2 + README 特性表
+ BACKLOG 勾销——✅。

**打磨轮（2026-10-04）**——把四个「文档写了、代码没跟上」的点补实，均为先探针实测
再动手，并逐个做了反向验证（回退修复看用例变红）：

| 项 | 实测的旧行为 | 现行为 |
|---|---|---|
| JS `forEach` 塞进 kids 数组 | 顶层 0 元素、Text=0，只一行 `__d_create 需要类型名`（**静默失效**） | 数组与 `__fragment` 都摊平（同写法与大写 `ForEach` 等价） |
| JS 事件绑定 | 每帧全树注销重绑（三帧绑定 id：b1..b3 → b4..b6 → b7..b9） | 按**元素身份**持有，复用帧零事件成本（绑定 id 跨帧不变） |
| JS 场景卸载 | 再 `compose` 一页会叠在旧页下、hook 槽从旧页往后取、effect 清理永不跑 | `compose` 先卸整层；`Host::unmount_declarative()` 发清理 |
| C++ `for_each` | **编译不过**（两参数未用 `-Werror`）+ 每帧重建全部项 | 按 key 跨位置复用（头插/重排/删除后身份不变，`elements_moved` 可观测） |
| 深度护栏 / 帧预算 | `max_depth` 无任何读取点；预算只事后置标志 | 递归 build 被拒声明 + 上报；超预算作用域留脏顺延下一帧 |

测试计数随之：637 用例全绿（声明式三件合计 52 用例，其中本轮 +26）；lint 0 违规；
mingw 交叉编译通过；`tools/st_visual_check.py` dev+san × 两应用 × 亮/暗 × DPI2.0
**0 失败步、无 sanitizer 报告**；`tools/codeeditor_e2e.py` 九项全过。

> 超出原计划的部分：**`examples/codeeditor` 整份界面也改成了声明式**（与 `codeeditor-dsl`
> 合并为一份，两份合计 2758 行 → 1605 行）；`counter` / `counter-js` / `todo-js`
> 三个小示例随整合删除。

## 10. 实施计划（四阶段，每阶段独立可验证）

| 阶段 | 内容 | 交付物 | 验收 |
|---|---|---|---|
| **M1 元素工厂**（地基） | `element_factory` + 32 组件注册 + 构造参数差距表 | 工厂单测全绿 | create/apply 往返一致 |
| **M2 C++ 声明式核心** | `dsl.hpp/cpp`（Composer/State/Component/Props）+ C++ 重组器 | C++ 单测全绿 | 点击/状态传播/护栏生效 |
| **M3 JS 宿主** | `declarative.js` + ScriptHost 集成 + compose 风格 | JS 单测 + 跨界次数断言 | 双宿主一致性 fixture 全绿 |
| **M4 补全与示例** | resource/persisted + 调度护栏全量 + `arkts_compat.js` + gallery 声明式页 + 文档 | gallery 声明式页 + codeeditor 整页声明式 | ArkTS 风格跑通；端到端全绿 |

依赖链：M1 → M2 → M3 → M4（M2 完成时 C++ 应用已可全量使用声明式；M3/M4 服务 JS 与
迁移体验）。

## 11. 风险与开放问题

| 风险 | 缓解 |
|---|---|
| 组件构造期属性（Select options 等）非属性面可后置 | M1 逐组件审查出差距表；缺属性面的组件在 M1 补属性面（量大则降级为专用包装器） |
| 双重组器实现漂移 | 语义规范 + 一致性 fixture（§9）钉住；diff 算法小（各 ~400 行）可全文互审 |
| C++ 闭包悬垂 | StateHandle 框架所有权 + Component 宿主所有权 + lint 规则（§5.4）；谬误注入测试 |
| QuickJS 性能（大列表） | For 走 sync_items 路径；预算调度兜底；真实瓶颈出现前不优化 |
| 类型化 Props 与属性面往返的字符串开销 | 仅**变更成员**序列化；C++ 无跨界，开销可忽略；profiling 证伪前不优化 |
| ArkTS 转译边界 | 明确报错 + compose fallback；转译器 ≤200 行可整体替换 |

## 12. 与外部世界的对齐（非目标声明）

- **不做** Flutter/SwiftUI 式「声明式即一切」：保留模式真值树是根基（控制通道、语义树、
  损坏区都长在它上面），声明式是编排层不是替代品；
- **不做** 完整 ArkTS 方言与装饰器编译器（§7 诚实边界）；
- **不做** 多窗口声明式（BACKLOG P2 多窗口先行，声明式层随后接入）；
- **不做** v1 细粒度重组跳过（整作用域重跑，简单正确优先）。
