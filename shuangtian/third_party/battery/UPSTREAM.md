# battery/ — batterycenter/embed（编译期资源嵌入）

| 项 | 值 |
|---|---|
| 上游 | https://github.com/batterycenter/embed |
| 版本 | **v1.2.19**（tag，非 main） |
| 许可 | **Apache-2.0**（见 `LICENSE`） |
| 用途 | 把资源文件（图标/默认配置/着色器/本地化文件…）在**编译期**嵌入可执行文件 |
| 上游形态 | 整个库是一个 `CMakeLists.txt`（457 行）：CMake 在 configure 阶段把文件转成 C++ 字节数组 |

## 我们怎么用它

上游把「**运行时**」与「**构建期生成**」两件事揉在一个 CMake 文件里。我们的构建系统是自研
`stpm`（直驱编译器，**刻意不依赖 CMake**），因此按职责切开：

| 部分 | 处置 |
|---|---|
| `embed_impl.cpp`（138 行，热重载运行时） | **原样 vendor**，编译进目标 |
| `embed.hpp.in`（119 行，公开 API 模板） | vendor 为模板，占位符语义与上游逐字一致 |
| `embed_source.cpp.in`（22 行，单文件生成模板） | vendor 为模板 |
| 上游的 CMake 生成逻辑 | **由 `st build` 原生实现**（见 `shuangtian/src/pkg/embed.cpp`） |

于是使用者拿到的是**与上游完全相同的 API**，而构建期零 CMake 依赖：

```cpp
#include "battery/embed.hpp"                 // stpm 生成到 build/<profile>/embed/<target>/include/
b::embed<"assets/banner.txt">().str()        // consteval 路径检查 + 编译期嵌好的字节
b::embed<"assets/app.json">().get(cb)        // 开发期：磁盘文件变了就回调（B_PRODUCTION_MODE 下为一次性调用）
```

## 与上游的行为一致性

以下语义**逐项对齐上游**，升级时可直接比对上游 `CMakeLists.txt`：

| 语义 | 规则 |
|---|---|
| 标识符 | `tolower(<target>_<filename>)`，非 `[a-zA-Z0-9_]` 一律替换为 `_`；同一目标内不得重复 |
| 查找键 | `b::embed<"...">()` 的模板参数 = **使用者书写的相对路径原文**（如 `assets/banner.txt`） |
| 编译期检查 | 路径不在嵌入集合内 → `static_assert` 报 `No such file or directory`（`_false()` 技巧） |
| 生产模式 | 定义 `B_PRODUCTION_MODE` 后不携带绝对路径（体积更小、不泄露构建机路径）；热重载随之关闭 |
| 声明文件 | 生成 `include/battery/embed.hpp`，含各文件的 `static EmbeddedFile` 声明与 `if constexpr` 返回链 |

## 修改声明（Apache-2.0 §4(b)）

`embed.hpp.in` / `embed_source.cpp.in` / `embed_impl.cpp` 三个文件与上游相比，**仅在文件头追加了
一段来源与修改说明注释**；模板正文逐字未改。上游的 CMake 生成逻辑未被复制（我们用 stpm 原生实现）。
