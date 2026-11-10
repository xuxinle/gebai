---
title: 写界面 · 三条路怎么选
summary: 声明式（dsl）、XML+JS、手搭元素树——同一棵树、同一套协议，差别在生命周期契约与交付形态。
---

# 写界面：三条路怎么选

三条路产出**同一棵元素树**——`tree`/`find`/`get`/`set`/`invoke` 对三者完全一致（不变式：一套语义、多个入口）。
所以选路只看"谁维护这份界面"和"改一次要多久生效"。

| 路 | 适合 | 生效方式 | 关键契约 |
|---|---|---|---|
| **声明式**（`st::ui::dsl`，C++） | 界面随状态变化、结构复杂（列表/表单/条件内容） | 改代码后重编 | 每帧 `pump_async()` 必须**先于** `dirty()` 判断；元素指针不可跨帧 |
| **XML + JS**（`mount_from_xml` + `ScriptBridge`） | 想"改一行看效果"不重编、把界面交给非 C++ 参与 | 保存即重载（`--watch`） | 写到没导入的标签**不会静默消失**（`XmlBuildReport::unknown_tags` 如实报） |
| **手搭**（`std::make_unique<T>()` + `add_child`） | 外壳、低频结构、自绘面板宿主 | 改代码后重编 | 自己管生命周期与重排 |

实际项目里三者常混用：**外壳手搭 + 内容区声明式子树**（`dsl::mount_into`）是 gbcode 的形态；
**纯 XML+JS** 是 `apps/xiangqi` 的形态；**高频自绘面**（编辑器、三维、时间轴）用手搭 + `custom<T>` 挂进声明式树。

## 声明式：三条不知道就一定会写错的契约

**① `mount` 是单根替换，`mount_into` 才是"挂一块"。**

- `dsl::mount(root, component)` 会替掉 `UiRoot::content()`——整个界面都是声明式时用它。
- 宿主界面（手搭外壳、只想让某一块声明式）必须用 `dsl::mount_into(root, host_element, component)`。
- 两者都返回 `DeclarativeHost*`（`unique_ptr`），**由调用方持有**；丢了它 = 不再重组。

**② 帧首必须先推进宿主，而且 `pump_async()` 在判脏之前。**

```cpp
if (decl != nullptr) {
  (void)decl->pump_async();   // 先泵异步结果（它会把状态写回去）
  if (decl->dirty()) {        // 再判脏（异步落地后这里才为真）
    (void)decl->tick();
    app.request_repaint();
  }
}
```

顺序反过来就是一个**静默死锁**：异步结果到达时状态还没写、作用域不脏，于是 `pump_async` 永远不被调到，
`resource` 永远停在"计算中…"。

**③ 重组会销毁重建元素：构建期存的指针不可跨帧使用。**

- `build()` 里 `custom<T>` 拿到的 `T*` 只在**本帧**有效。
- 需要"跨帧生效"的效果（如"打开文件后跳到某行"）要**排队到下一帧**，不能当场调。
- `set_text` 类接口会重置状态（光标归零、撤销栈清空）：只能在"值真变了"时写，
  否则用户打字会被每帧重置。

## 声明式：三条最容易踩的写法坑

- **给组件加子元素只能 `custom_container<T>`，不能 `custom<T>`**；它的三个参数各有各的活
  （子声明回调用**外层** composer；配置回调才拿 `T&`）。写错就是 `error: 'class X' has no member named 'composer'`。
- **`BoxProps` 没有 `tone` 字段**：文字色调/字号/字重走 `custom<Text>` 的 setter。
- **`BoxProps` 字段有固定初始化顺序**：设计符顺序写错会报 `designator order … does not match declaration order`，
  按报错提示调顺序即可，不要重写整行。

## JS 声明式（运行时/AI 写的）另有四个点

1. `forEach()` 返回**裸数组**，直接塞进 kids 是合法写法（框架会摊平）。
2. 大写 `Text(...)` 返回**链对象**（`.padding()` 可用）；小写 `text(...)` 返回**裸 VNode**（key 经 `{key: …}` 传）。
3. 属性面写后要**下一帧**才看得见——要断言新值先 `tick()`（`wait`/`get` 默认已处理）。
4. `compose()` 换页会先卸旧页，不要指望"再 compose 一次"是叠加。

## 元素身份：`id` 与 `key` 分工

- **`id`** 给外部世界（协议/选择器/测试）定位用；不写则自动生成（`Type@key` / `Type[i]`）。
- **`key`** 是业务身份，重组时按它对齐（列表增删不串台）。
- 二者都不写时元素仍可用，但**自动化定位会退化成按序号猜**——要测的界面请显式给 `id`。

## 更细

- 声明式完整设计（VDOM/重组/异步 resource/overlay 生命周期）：`docs/declarative.md`
  （**§5.5 是硬契约**，写代码前必读）
- XML + JS 通路：`docs/xml-ui.md`
- 改文件即生效（热重载语义）：`docs/hot-reload.md`
- 组件清单与属性面：`components/index.md`
- 属性/通用属性口径：`overview/properties.md`
