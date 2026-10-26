# 模块分层与已知违规

> 这份文档记录**当前分层状态**、**8 处已知违规**，以及每一处**为什么不改**。
> 目的：避免下一个人（或下一个会话）把已经论证过的方案重新试一遍。

## 1. 分层规则

唯一硬约束：**下层不得依赖上层**。层号越小越底层。

| 层 | 模块 |
|---|---|
| 0 | `core` |
| 1 | `math` `codec` `xml` `net` `edit` `md` `lsp` `pkg` |
| 2 | `text` |
| 3 | `raster` `ext` |
| 4 | `control` |
| 5 | `ui` |
| 6 | `app` `shell` |
| 8 | `probe` `test`（工具性质，不受层序约束）|

**`st_core` 库 = 层 0–1 里不（传递地）触及 `ui`/`raster` 的模块**：core / codec / math / net /
xml / test / edit / lsp / pkg，共 9 个（详见 `AS_LIBRARY.md`）。

## 2. 当前 8 处违规

2026-10-26 实测。历史：17 → 10（json/async 归位 core）→ 8（Path 下沉 math）。

| 违规 | 处数 | 状态 |
|---|---|---|
| `control(4) → ui(5)` | 5 | **不改**，见 §3.1 |
| `text(2) → raster(3)` | 2 | **不改**，见 §3.2 |
| `md(1) → text(2)` | 1 | **不改**，见 §3.3 |

## 3. 逐条论证

### 3.1 `control → ui`（5 处）：抽接口会削弱能力，不抽

**用了什么**（按次数）：`EventKind`(23) / `Element`(20) / `ThemeMode`(8) / `Selector`(5) /
`element_to_json`(4) / `Panel`(3) / `Event`(3) / `SemanticsNode`(2) / `SemanticsFlags`(2) /
`apply_properties`(2) / `ScriptHost`(2) / 其余各 1。

**看起来可行的方案**：抽一个 `IElement` 接口（只有 id / type / bounds / props / children），
让 `control` 依赖接口而非 `ui::Element`。

**为什么这个方案不成立**，三条可核验的理由：

1. **`Element` 不是数据，是带行为的基类**。实测 `element.hpp` 971 行、`class Element`
   有虚函数（`type()` / `role()` / `add_child()` / `insert_child()` / 析构），
   还持有 `parent_` / `children_` 的所有权（`std::unique_ptr`）。
   控制通道要做的操作——`invoke_element`（触发动作）、`apply_properties`（写属性）、
   `element_snapshot`（取快照）——**全都要调它的虚函数**。抽接口等于把 `Element` 的
   公开面整体复制一份，然后保证两份永远同步。这不是解耦，是**制造一个必然漂移的镜像**。

2. **控制通道的价值恰恰在于"能操作真实元素"**。它是给智能体用的：
   `find(Selector)` → `invoke(action)` → 读回结果。`Selector` 本身依赖
   `ui::selector.hpp` 的匹配语义（含 `#id` / `Type[attr]` / `:focused` / `^=` 等），
   而那套语义与 `Element` 的 `type()` / 属性表强耦合。抽掉之后，
   `control` 只能操作"某种抽象节点"，而智能体需要的是**这个应用里这个按钮**。

3. **`ScriptHost` / `dsl` 是 UI 侧的执行入口**。控制通道的 `script` 方法（`--enable-script`）
   直接调 UI 的脚本宿主。这不是依赖错误，是**功能本身跨层**。

**结论**：这不是"分类错误"，是真实的能力需求。要让 `control` 独立，
得先给自绘 UI 造一套等价于 DOM 的稳定外部模型——那是一个**产品级项目**，
不是重构。**不做。**

### 3.2 `text → raster`（2 处）：抽象早就在了，剩下的是归属问题

**先纠正一个常见误解**：容易以为"文本为了通用性付出了虚调用代价"。**事实相反**：

- `TextRenderer::draw` 的签名是 `draw(raster::Surface& canvas, ...)` —— **接口**，不是 `Canvas`
- `Surface::blend_coverage_bitmap` **本来就是 `virtual`**（`surface.hpp:299`），
  软件 `Canvas` 与 GPU `GpuCanvas` 各一份实现

也就是说**那次虚调用一直在热路径上**，与本次重构无关。所谓"抽接口"不会新增任何间接层。

**剩下真正依赖的**：

| 符号 | 性质 | 频次 |
|---|---|---|
| `Paint` / `BlendMode` / `CoverageFormat` / `PaintOp` | 纯数据/枚举 | 每字形 |
| `Canvas scratch`（2 处）| 字形位图生成 | **冷路径**：有 LRU 缓存（`TextRenderer::Cache`），只在某字形首次出现时走 |

**结论**：2 处违规里，一处是纯数据、一处是冷路径有缓存。**都不是性能问题**。
若要消除，只需把 `Surface` / `Paint` 等定义挪到独立模块（如 `st/gfx`）——
**零性能变化，纯归属调整**。但这没有性能收益，按"性能优先"不做。**不做。**

### 3.3 `md → text`（1 处）：语法高亮要文本类型

`md/highlight.hpp` 需要 `text` 的文本类型做语法高亮。这不是能轻易换掉的东西，
除非把"文本度量"抽象出去（见 §3.2，同样只是归属问题）。**不做。**

## 4. 已经做过的归位（有实际收益）

| 改动 | 依据 | 效果 |
|---|---|---|
| `json` / `json_fwd` / `json.cpp`：`ext` → `core` | 414 行、**只依赖 core**、被 60 个文件引用（含底层 `lsp`）| `lsp`/`pkg` 得以进 `st_core`；违规 17→10 |
| `async`：`app` → `core` | 228 行、**零 st 依赖**（纯异步执行器）| 消除 `ui → app` 违规 |
| `Path` / `PathCommand` / `Polyline`：`raster` → `math` | 636 行、**只依赖 math/geometry + cmath**、**一个绘制符号都不引用** | `text` 对 raster 的依赖从 8 符号降到 6（全是真光栅化）；违规 10→8 |

**判断标准**（三条同时成立才做）：① 该文件**不引用**目标层的任何符号；
② 只依赖更底层；③ 现状属于**分类错误**而非真实需求。
`control → ui` 三条都不满足，`text → raster` 只有第 ③ 条勉强成立（剩下的是归属而非错误）。

## 5. 复现

```powershell
# 分层违规扫描（判据：下层 include 上层）
# 当前基线 8 处。脚本见 tools/（按需现写；扫描 include/ 与 src/ 的跨模块 #include）
```
