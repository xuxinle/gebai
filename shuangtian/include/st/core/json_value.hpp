#pragma once

/// 自研 JSON：**值类型 / 保序对象 / 零抛出访问**（`st/core/json.hpp` 的实现核心，勿直接包含）。
///
/// ## 为什么不继续用 nlohmann/json
///
/// 本框架曾用 nlohmann/json 3.12.0（25,526 行单头）。它的**接口语义**缺陷在本仓反复造成
/// 实际事故（`CONVENTIONS.md` §3.8、`docs/PITFALLS.md` 记录在案），全是"编译不报错、运行期才炸"：
///
/// | 缺陷 | 本仓实测后果 |
/// |---|---|
/// | `at()` 键缺失抛 `out_of_range` | 框架不设通用异常边界 → 直接 `std::terminate`（鼠标事件的修饰键缺席把整个应用干掉） |
/// | const `operator[]` 键缺失是 **UB** | 取可选字段时崩溃或读到随机内存（控制通道回包构造失败） |
/// | 花括号是"造数组"信号 | `Json x{Json::object()}` 得到 `[{}]`，之后按对象用即抛 `type_error.305` |
/// | `get<T>()` 类型不符抛 `type_error` | 用例里未捕获异常带走整个测试进程（1268 条在第 800 条处消失） |
///
/// 自研实现的约束（每一句都是**可测的承诺**，见 `tests/ext_json_test.cpp`）：
///
/// 1. **零抛出**：除内存耗尽（`bad_alloc`）外，没有成员函数会抛异常。越界/缺键/类型不符
///    一律退化为"空值"——`const operator[]` 缺键返回共享 null 节点（**不是 UB**），
///    `as_i64()` 对字符串返回 0。想区分"没有"与"是 0"就用 `json_find` / `is_null()`。
/// 2. **无歧义构造**：`Json{}` = null，`Json::object()` = `{}`，`Json::array()` = `[]`。
///    没有 `initializer_list` 构造——花括号永远不会变成数组（要数组写 `Json::array({1,2})`）。
/// 3. **保序**：对象按插入顺序存储（`vector`），因为清单/lock 是给人读、被 git diff 的产物。
/// 4. **数字保真**：整数/无符号/浮点分开存。`9007199254740993`(2^53+1)、雪花 ID、纳秒时间戳
///    逐位往返；`1.0` 不写成 `1`，`-0.0` 保持 `-0.0`（"是浮点"本身是语义）。
///
/// ## 布局与声明顺序（**改这个头必读**）
///
/// 递归类型：`Json` 的对象成员是 `vector<JsonMember>`，而 `JsonMember` 里又有 `Json`。
/// 标准库对 `vector<T>` 支持"不完整 T"（`T` 在**第一次引用成员**前完整即可），因此顺序必须是：
///
/// 1. `class Json;` + `struct JsonMember;`（前向声明）
/// 2. `using JsonObject = std::vector<JsonMember>;` / `using JsonArray = std::vector<Json>;`
/// 3. `class Json { ... }`（成员是两个 `vector`——此刻元素类型还不完整，这是允许的）
/// 4. `struct JsonMember { std::string first; Json second; };`（此刻 `Json` 已完整）
/// 5. `class JsonItems`（要用 `Json::members()`）
///
/// 把第 4 步提到第 3 步之前，会立刻得到 `field 'second' has incomplete type 'st::Json'`。
///
/// ## 性能取向
///
/// 对象是 `vector`：`find` 为 O(n)。实测仓内对象普遍 ≤ 20 键，线性扫描在缓存友好的顺序内存上
/// 与哈希表差异在纳秒级；而保序带来的"无关读取-回写不产生满屏 diff"是每天兑现的价值。

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "st/core/error.hpp"

namespace st {
namespace json_detail {

/// 值类别（与 JSON 规范一一对应；`Discarded` 是"解析失败"哨兵）。
enum class Kind : std::uint8_t {
  Null,
  Boolean,
  /// 有符号整数。
  Integer,
  /// 无符号整数（只有超出 `int64` 的正数才落这里）。
  Unsigned,
  /// 浮点。
  Float,
  String,
  Array,
  Object,
  /// 解析失败哨兵（`is_discarded()`；除序列化外一切读取退化为默认值）。
  Discarded,
};

}  // namespace json_detail

class Json;
struct JsonMember;
class JsonItems;

/// 保序对象（键值对按插入顺序存放）。
using JsonObject = std::vector<JsonMember>;
/// 数组。
using JsonArray = std::vector<Json>;

/// JSON 值（**零抛出**；见文件头）。
///
/// 构造入口只有三类：标量构造（`Json(1)` / `Json("s")`…）、`Json::object()`、
/// `Json::array()` / `Json::array({...})`。**没有**接受花括号的隐式构造——这是刻意的。
class Json {
 public:
  /// 对象迭代器（解引用得 `JsonMember`）。
  using const_object_iterator = JsonObject::const_iterator;
  /// 数组迭代器。
  using array_iterator = JsonArray::iterator;
  using const_array_iterator = JsonArray::const_iterator;

  /// 统一只读迭代器：对象遍历给成员值，数组遍历给元素。
  ///
  /// 需要键时请用 `items()`（那里有 `(key, value)` 结构化绑定）。
  class const_iterator {
   public:
    const_iterator() = default;
    explicit const_iterator(const_object_iterator it, const_object_iterator end) : object_(it), object_end_(end) {
      as_object_ = true;
    }
    explicit const_iterator(const_array_iterator it, const_array_iterator end) : array_(it), array_end_(end) {
      as_object_ = false;
    }

    [[nodiscard]] auto is_object() const noexcept -> bool { return as_object_; }

    /// 当前成员的键 / 值（**仅对象遍历有效**）。
    ///
    /// 旧库的 `for (auto it = obj.begin(); it != obj.end(); ++it) it.key()` 就是这个形态：
    /// 它不是 `items()`，而是直接在 Json 上迭代对象。为免几十处调用点逐个改写，
    /// 这里提供同名访问器；非对象（数组/标量）时返回空键 / 空值。
    [[nodiscard]] auto key() const -> const std::string&;
    [[nodiscard]] auto value() const -> const Json&;

    /// 随机访问（`*(arr.begin() + 1)` 这类旧写法）：只对**数组**成立；对象上偏移无效。
    friend auto operator+(const_iterator it, std::ptrdiff_t offset) noexcept -> const_iterator {
      if (!it.as_object_) it.array_ += offset;
      return it;
    }
    friend auto operator+(std::ptrdiff_t offset, const_iterator it) noexcept -> const_iterator {
      return it + offset;
    }
    friend auto operator-(const_iterator it, std::ptrdiff_t offset) noexcept -> const_iterator {
      if (!it.as_object_) it.array_ -= offset;
      return it;
    }
    friend auto operator-(const const_iterator& lhs, const const_iterator& rhs) noexcept -> std::ptrdiff_t {
      if (lhs.as_object_ || rhs.as_object_) return 0;
      return lhs.array_ - rhs.array_;
    }

    /// 解引用：对象 → 成员的值；数组 → 元素本身。
    [[nodiscard]] auto operator*() const -> const Json&;
    auto operator++() -> const_iterator&;
    auto operator++(int) -> const_iterator;
    [[nodiscard]] auto operator==(const const_iterator& other) const noexcept -> bool;
    [[nodiscard]] auto operator!=(const const_iterator& other) const noexcept -> bool {
      return !(*this == other);
    }

   private:
    bool as_object_{true};
    const_object_iterator object_{};
    const_object_iterator object_end_{};
    const_array_iterator array_{};
    const_array_iterator array_end_{};
  };

  // ── 构造 ──────────────────────────────────────────────────────────────

  /// null（`Json{}` 也是 null——要空对象写 `Json::object()`）。
  Json() noexcept = default;
  Json(std::nullptr_t) noexcept {}  // NOLINT(*-explicit-constructor)：null 天然可隐式

  Json(bool value) noexcept : kind_(json_detail::Kind::Boolean), boolean_(value) {}
  // 整型重载覆盖全部宽度：漏一个就会让 `Json(42)` 在 int64/double 之间走歧义或误选 double
  Json(int value) noexcept : kind_(json_detail::Kind::Integer), scalar_{.integer_ = value} {}
  Json(long value) noexcept : kind_(json_detail::Kind::Integer), scalar_{.integer_ = value} {}
  Json(long long value) noexcept : kind_(json_detail::Kind::Integer), scalar_{.integer_ = value} {}
  Json(unsigned int value) noexcept : kind_(json_detail::Kind::Unsigned), scalar_{.unsigned_ = value} {}
  Json(unsigned long value) noexcept : kind_(json_detail::Kind::Unsigned), scalar_{.unsigned_ = value} {}
  Json(unsigned long long value) noexcept
      : kind_(json_detail::Kind::Unsigned), scalar_{.unsigned_ = value} {}
  Json(double value) noexcept : kind_(json_detail::Kind::Float), scalar_{.number_ = value} {}
  Json(float value) noexcept : kind_(json_detail::Kind::Float), scalar_{.number_ = value} {}
  Json(const char* value) : kind_(json_detail::Kind::String), text_(value == nullptr ? "" : value) {}
  Json(std::string value) : kind_(json_detail::Kind::String), text_(std::move(value)) {}
  // `std::string_view` 与 `std::string` 只能留一个：两者同时存在时，
  // `Json{{"k", true}}`（旧库的"造对象"写法）会同时匹配 `string(const char*, size_t)`
  // 与 `string_view(const char*)`，变成**二义调用**。而 `string_view` 又是必须的
  // （大量调用点传 `std::string_view`），所以删掉 `std::string` 重载（它会隐式转成 view）。
  Json(std::string_view value) : kind_(json_detail::Kind::String), text_(value) {}

  /// 由 `std::vector<std::string>` 造字符串数组。
  ///
  /// 为什么要它：清单/报告类代码普遍是 `node["names"] = some_string_vector;`——
  /// 旧库靠模板隐式转换支持这个写法。没有它，几十处调用点都要手写循环。
  Json(const std::vector<std::string>& items);
  Json(std::vector<std::string>&& items);
  /// 由 `std::vector<int64>` / `std::vector<double>` 造数字数组（同上，为了免改调用点）。
  Json(const std::vector<std::int64_t>& items);
  Json(const std::vector<double>& items);
  /// 由 `std::vector<float>` 造数字数组（多边形/点列这类几何字段的主力形态）。
  /// 存为**浮点**（不撮成 double 好——float→double 是精确的，往返后读回仍是原值）。
  Json(const std::vector<float>& items);
  /// 由 `std::vector<Json>` 造数组（如聊天消息列表：`payload["messages"] = messages;`）。
  Json(const std::vector<Json>& items);
  Json(std::vector<Json>&& items);

  // ── 工厂（**造容器只走这三个**，避免花括号歧义）──────────────────────

  /// 空对象 `{}`。
  [[nodiscard]] static auto object() -> Json;
  /// 共享 null 节点（只读）：缺键/越界/未初始化时的**可安全读取**的占位值。
  ///
  /// 为何需要它：`json_child` / `operator[] const` 都得“返回一个引用但键不存在”，
  /// 而且这两个现在是头文件里的 `inline` 函数（见下方注），不能去 `.cpp` 里要单例。
  /// 函数内 `static` 在 C++ 里天然是**进程内唯一**（不需要 inline 变量），可直接用。
  [[nodiscard]] static auto null_node() noexcept -> const Json& {
    static const Json instance;
    return instance;
  }
  /// 由键值对列表造对象（**保序**，按给定顺序插入）：`Json::object({{"k", 1}, {"z", 2}})`。
  ///
  /// 这是旧库 `Json{{...}}` 那类写法的替代：花括号构造已经取消（它会静默把对象变成数组），
  /// 想要"当场造对象"就用这个，语义一目了然。重复键后者覆盖前者的**值**、保留首次位置。
  [[nodiscard]] static auto object(std::initializer_list<std::pair<std::string_view, Json>> members) -> Json;
  /// 空数组 `[]`。
  [[nodiscard]] static auto array() -> Json;
  /// 由初始化列表造数组（**唯一**接受花括号的入口）：`Json::array({1, 2, "x"})`。
  [[nodiscard]] static auto array(std::initializer_list<Json> items) -> Json;

  // ── 类别查询 ──────────────────────────────────────────────────────────

  [[nodiscard]] auto is_null() const noexcept -> bool { return kind_ == json_detail::Kind::Null; }
  [[nodiscard]] auto is_boolean() const noexcept -> bool { return kind_ == json_detail::Kind::Boolean; }
  /// 兼容别名（历史代码/注释里出现过 `is_bool`）。
  [[nodiscard]] auto is_bool() const noexcept -> bool { return is_boolean(); }
  [[nodiscard]] auto is_number() const noexcept -> bool {
    return kind_ == json_detail::Kind::Integer || kind_ == json_detail::Kind::Unsigned ||
           kind_ == json_detail::Kind::Float;
  }
  [[nodiscard]] auto is_number_integer() const noexcept -> bool {
    return kind_ == json_detail::Kind::Integer || kind_ == json_detail::Kind::Unsigned;
  }
  [[nodiscard]] auto is_number_unsigned() const noexcept -> bool {
    return kind_ == json_detail::Kind::Unsigned;
  }
  [[nodiscard]] auto is_number_float() const noexcept -> bool { return kind_ == json_detail::Kind::Float; }
  [[nodiscard]] auto is_string() const noexcept -> bool { return kind_ == json_detail::Kind::String; }
  [[nodiscard]] auto is_array() const noexcept -> bool { return kind_ == json_detail::Kind::Array; }
  [[nodiscard]] auto is_object() const noexcept -> bool { return kind_ == json_detail::Kind::Object; }
  /// 结构化（对象或数组）。
  [[nodiscard]] auto is_structured() const noexcept -> bool { return is_array() || is_object(); }
  /// 标量（非对象/数组/哨兵）。
  [[nodiscard]] auto is_primitive() const noexcept -> bool { return !is_structured() && !is_discarded(); }
  /// 解析失败哨兵（只由解析器产生；调用方应视为"没有值"）。
  [[nodiscard]] auto is_discarded() const noexcept -> bool { return kind_ == json_detail::Kind::Discarded; }

  // ── 尺寸 ──────────────────────────────────────────────────────────────

  /// 对象=键数、数组=元素数、字符串=字节数、null=0、标量=1。
  [[nodiscard]] auto size() const noexcept -> std::size_t;
  [[nodiscard]] auto empty() const noexcept -> bool;

  // ── 原始值访问（**调用方先检查类别**；类别不符给中性默认值，绝不抛）─────

  [[nodiscard]] auto as_bool() const noexcept -> bool { return boolean_; }
  [[nodiscard]] auto as_i64() const noexcept -> std::int64_t;
  [[nodiscard]] auto as_u64() const noexcept -> std::uint64_t;
  [[nodiscard]] auto as_double() const noexcept -> double;
  /// 字符串引用（非字符串返回静态空串；**生命周期同本节点**）。
  [[nodiscard]] auto as_string() const noexcept -> const std::string&;

  /// 类型化取值（兼容旧写法 `node.get<T>()`）。
  ///
  /// ⚠ 语义与旧库**不同且更安全**：类型不符时返回**默认值**，而不是抛 `type_error`。
  /// 调用点普遍已经先判过类别（`if (value.is_boolean()) return value.get<bool>();`），
  /// 因此这里的默认值分支实际不可达；即便可达，也只退化为 0/空串，不会带走进程。
  ///
  /// 支持的类型与 `as_*` 一致：布尔、各宽度整数、`double`/`float`、`std::string`。
  /// **不支持**容器类型（旧库的 `get<vector<T>>`）：仓内无此用法，需要时请自己遍历 `elements()`。
  template <typename T>
  [[nodiscard]] auto get() const noexcept {
    if constexpr (std::is_same_v<T, bool>) {
      return is_boolean() ? boolean_ : false;
    } else if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(as_double());
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
      return static_cast<T>(as_i64());
    } else if constexpr (std::is_integral_v<T>) {
      return static_cast<T>(as_u64());
    } else if constexpr (std::is_same_v<T, std::string>) {
      return std::string(as_string());
    } else {
      static_assert(std::is_same_v<T, void>, "Json::get<T> 只支持布尔/整数/浮点/std::string");
    }
  }

  /// 对象成员表（非对象返回静态空表）。
  [[nodiscard]] auto members() const noexcept -> const JsonObject&;
  /// 数组元素表（非数组返回静态空表）。
  [[nodiscard]] auto elements() const noexcept -> const JsonArray&;

  /// 容器类型别名（兼容旧库 `Json::array_t` / `Json::object_t` 的写法）。
  using array_t = JsonArray;
  using object_t = JsonObject;
  using string_t = std::string;

  /// 容器/字符串引用（兼容旧库 `get_ref<const Json::array_t&>()`）。
  ///
  /// 与 `elements()` / `members()` **同一语义**（非目标类别返回静态空表，永不抛），
  /// 保留这个名字只是因为旧代码如此写、逐字改写会引入无意差异。新代码直接用
  /// `elements()` / `members()` / `as_string()`。
  template <typename Ref>
  [[nodiscard]] auto get_ref() const noexcept -> decltype(auto) {
    if constexpr (std::is_same_v<Ref, const JsonArray&> || std::is_same_v<Ref, JsonArray>) {
      return elements();
    } else if constexpr (std::is_same_v<Ref, const JsonObject&> || std::is_same_v<Ref, JsonObject>) {
      return members();
    } else if constexpr (std::is_same_v<Ref, const std::string&> || std::is_same_v<Ref, std::string>) {
      return as_string();
    } else {
      static_assert(std::is_same_v<Ref, void>, "Json::get_ref<T> 只支持容器/字符串引用");
    }
  }

  // ── 对象访问（**零抛出**：缺键返回共享 null 节点，不是 UB）────────────

  /// 查成员（不存在或非对象返回 `nullptr`）。返回指针指向本节点内部。
  ///
  /// ## 为何定义在头文件（不放到 .cpp）
  ///
  /// 这是**最热的函数**：`json_get_*` / `json_child` / `json_path` / `operator[]` 全部经它。
  /// 放在 `.cpp` 里意味着跨 TU 无法内联，而内联的收益不在“省一次函数调用”（那只是几 ns）——
  /// 在于**键是字面量时编译器能把比较特化**。实测同一套数据、同一份代码：`.cpp` 版 8.2 ns/次，
  /// 头文件内联版 4.9 ns/次（与 header-only 的旧库同级）。框架里绝大多数调用都是
  /// `find("id")` 这种字面量键，所以这不是玄学。
  ///
  /// 函数体在**类定义之后**：类体内 `JsonMember` 还是不完整类型（无法取 `member.second`）。
  [[nodiscard]] auto find(std::string_view key) const noexcept -> const Json*;
  /// 同上，可写版本。
  [[nodiscard]] auto find_mutable(std::string_view key) noexcept -> Json*;
  /// 键是否存在（等价 `find(key) != nullptr`）。
  [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool { return find(key) != nullptr; }

  /// 带默认值的读取（兼容旧库 `obj.value("k", fallback)`）。
  ///
  /// 语义：键存在**且类型匹配**时用它的值，否则用 `fallback`。类型不匹配也算"没给"——
  /// 这比"拿 0 当默认值"更贴近调用意图（配置写错时回退到默认值而不是变成 0）。
  ///
  /// ⚠ `T = Json` 是一个**必须单独处理**的分支：即“取一个子节点，缺了就给我这个默认节点”。
  /// 漏了它的后果不是编译错误，而是**静默拿到 fallback**（序列化时表现为子节点全部丢失）。
  template <typename T>
  [[nodiscard]] auto value(std::string_view key, T fallback) const -> T {
    const Json* found = find(key);
    if (found == nullptr) return fallback;
    if constexpr (std::is_same_v<T, Json>) {
      return *found;
    } else if constexpr (std::is_same_v<T, std::string>) {
      return found->is_string() ? found->as_string() : fallback;
    } else if constexpr (std::is_same_v<T, const char*>) {
      return fallback;  // 指针型默认值无法安全持有：返回调用方给的字符串常量
    } else if constexpr (std::is_same_v<T, bool>) {
      return found->is_boolean() ? found->as_bool() : fallback;
    } else if constexpr (std::is_floating_point_v<T>) {
      return found->is_number() ? static_cast<T>(found->as_double()) : fallback;
    } else if constexpr (std::is_integral_v<T>) {
      return found->is_number() ? static_cast<T>(found->as_i64()) : fallback;
    } else {
      return fallback;
    }
  }

  /// 解析 JSON 文本（兼容旧写法 `Json::parse(text)`）。
  ///
  /// **与旧库的语义差异（刻意）**：失败时返回 `is_discarded()` 为真的哨兵值，不抛异常。
  /// 需要错误消息时用 `st::json_parse`（返回 `Result`，带行列号定位）——这是推荐通道；
  /// 这个静态入口只为了不让旧调用点（测试里的 `Manifest::parse_json(Json::parse(text), …)`）中断。
  [[nodiscard]] static auto parse(std::string_view text) -> Json;

  /// 与布尔比较：`*value != false` 这类写法（检查"开了没有"）。
  ///
  /// 语义：布尔节点比它的值；**null / 空容器也算 false**（与脚本语言的"空即假"一致，
  /// 而且这正是调用点的意图："这个 capability 给了或给了空对象都算没开"）。
  friend auto operator==(const Json& value, bool other) noexcept -> bool { return value.as_truthy() == other; }
  friend auto operator!=(const Json& value, bool other) noexcept -> bool { return value.as_truthy() != other; }
  friend auto operator==(bool other, const Json& value) noexcept -> bool { return value.as_truthy() == other; }
  friend auto operator!=(bool other, const Json& value) noexcept -> bool { return value.as_truthy() != other; }

  /// 结构相等（两个 Json 逐层比）。
  ///
  /// 为什么必须有：没有它，`ST_CHECK(a == b)`（两个 Json）会只剩"与 bool 比"的重载可参，
  /// 于是报出"4 个候选都不匹配"的难读错误。数字跨类别比较（整型 vs 浮点）按**数值**相等，
  /// 因为 `1` 与 `1.0` 在 JSON 语义上是同一个数（序列化时才区分存储形态）。
  /// 对象比较**不看键序**（键序是存储细节，不是值语义）。
  friend auto operator==(const Json& lhs, const Json& rhs) noexcept -> bool { return lhs.structurally_equal(rhs); }
  friend auto operator!=(const Json& lhs, const Json& rhs) noexcept -> bool { return !lhs.structurally_equal(rhs); }

  /// 结构相等实现（公开以便递归调用；外部用 `==` 即可）。
  [[nodiscard]] auto structurally_equal(const Json& other) const noexcept -> bool;

  /// 真值语义：布尔取自身；null/空串/零/空容器为假；其余（非空串、非零数、非空容器）为真。
  [[nodiscard]] auto as_truthy() const noexcept -> bool {
    switch (kind_) {
      case json_detail::Kind::Boolean:
        return boolean_;
      case json_detail::Kind::Null:
      case json_detail::Kind::Discarded:
        return false;
      case json_detail::Kind::String:
        return !text_.empty();
      case json_detail::Kind::Integer:
        return scalar_.integer_ != 0;
      case json_detail::Kind::Unsigned:
        return scalar_.unsigned_ != 0;
      case json_detail::Kind::Float:
        return scalar_.number_ != 0.0;
      case json_detail::Kind::Array:
        return !array_.empty();
      case json_detail::Kind::Object:
        return !object_.empty();
    }
    return false;
  }

  /// 读成员；缺键返回共享 null 节点（**不是 UB**，可安全判 `is_null()`）。
  [[nodiscard]] auto operator[](std::string_view key) const noexcept -> const Json&;

  /// 读/写成员（**写**：不存在则插入到末尾，保持保序语义；非对象先转为空对象）。
  auto operator[](std::string_view key) noexcept -> Json&;
  auto operator[](const std::string& key) noexcept -> Json& { return (*this)[std::string_view(key)]; }

  /// 字符串下标（`const char*`）。
  ///
  /// ⚠ 这里**不能**只保留 `const char*` 重载：字面量 `0` 是空指针常量，`json[0]` 会
  /// 在 `const char*` 与 `size_t` 之间变成**二义调用**（编译期报错，不是静默选错）。
  /// 因此整型下标走下面那条模板（精确匹配，胜过指针转换）。
  auto operator[](const char* key) noexcept -> Json& {
    return (*this)[std::string_view(key == nullptr ? "" : key)];
  }

  // ── 数组访问 ──────────────────────────────────────────────────────────

  /// 下标读；越界或非数组返回共享 null 节点（**不是 UB**）。
  [[nodiscard]] auto operator[](std::size_t index) const noexcept -> const Json&;
  /// 下标读写；越界时**自动扩展**（用 null 填充）——便于 `arr[0] = x`。
  auto operator[](std::size_t index) noexcept -> Json&;

  /// 整型下标（**精确匹配**：让 `json[0]` 不会被解析成 `const char*` 空指针）。
  /// 没有这条模板，`arr[0]` 在 `size_t` 与 `const char*` 之间二义（字面量 0 两路都成立）。
  template <typename Index, std::enable_if_t<std::is_integral_v<Index>, int> = 0>
  [[nodiscard]] auto operator[](Index index) const noexcept -> const Json& {
    return (*this)[static_cast<std::size_t>(index)];
  }
  template <typename Index, std::enable_if_t<std::is_integral_v<Index>, int> = 0>
  auto operator[](Index index) noexcept -> Json& {
    return (*this)[static_cast<std::size_t>(index)];
  }

  /// 追加元素（非数组时先转成空数组）。
  auto push_back(Json value) -> void;
  /// 追加空元素并返回引用。
  auto emplace_back() -> Json&;
  /// 批量追加（`append(list)` 等价于逐个 `push_back`）。
  auto append(const std::vector<Json>& items) -> void;
  /// 取末元素；空/非数组返回共享 null（不抛）。
  [[nodiscard]] auto back() const noexcept -> const Json&;
  auto back() noexcept -> Json&;
  /// 取首元素；空/非数组返回共享 null。
  [[nodiscard]] auto front() const noexcept -> const Json&;
  auto front() noexcept -> Json&;

  // ── 遍历 ──────────────────────────────────────────────────────────────

  /// 序列化为 JSON 文本（兼容旧写法 `node.dump()` / `node.dump(2)`）。
  ///
  /// `indent == 0`（缺省）紧凑输出；传入缩进宽度则美化。与 `st::json_dump` 等价——
  /// 保留成员版只因旧代码如此写（`json.dump(2)` 改成函数调用会丢掉"这个值能自序列化"的直觉）。
  [[nodiscard]] auto dump(std::uint32_t indent = 0) const -> std::string;

  /// 统一只读遍历（对象 → 成员的值，数组 → 元素）；对 null/标量得到空区间。
  [[nodiscard]] auto begin() const -> const_iterator;
  [[nodiscard]] auto end() const -> const_iterator;
  /// 可变遍历（数组元素可改；对象请走 `members_mutable()`）。
  [[nodiscard]] auto begin() -> array_iterator;
  [[nodiscard]] auto end() -> array_iterator;

  /// 对象成员迭代器（`find` 返回 `const Json*`，取"成员位置"时可用它做下标比对）。
  [[nodiscard]] auto members_begin() const noexcept -> const_object_iterator;
  [[nodiscard]] auto members_end() const noexcept -> const_object_iterator;

  /// 键值对遍历（`for (const auto& [key, value] : node.items())`）；非对象为空区间。
  ///
  /// 定义在类外（`JsonItems` 定义在本类**之后**，类体内无法定义这个函数体）。
  [[nodiscard]] auto items() const noexcept -> JsonItems;

  // ── 修改 ──────────────────────────────────────────────────────────────

  /// 清空内容（保留原类别：对象清成 `{}`、数组清成 `[]`；标量清成 null）。
  auto clear() noexcept -> void;
  /// 删除成员；返回是否真的删了。
  auto erase(std::string_view key) -> bool;
  /// 删除数组元素；越界返回 false。
  auto erase_index(std::size_t index) -> bool;

  // ── 内部（访问层与解析器用）──────────────────────────────────────────

  [[nodiscard]] auto members_mutable() noexcept -> JsonObject&;
  [[nodiscard]] auto elements_mutable() noexcept -> JsonArray&;

 private:
  json_detail::Kind kind_{json_detail::Kind::Null};
  bool boolean_{false};
  /// 三个数值**互斥**（kind_ 决定哪个有效）：union 而不是三个并行字段。
  ///
  /// 收益：`sizeof(Json)` 112 → 96 字节，且标量值的读写落在同一缓存行上。
  /// “保真”靠的是 kind_ 区分 Integer / Unsigned / Float，而不是靠“各自存一份”。
  ///
  /// 必须显式 `{0}` 初始化：否则 `Json(false)` 这类只设 kind_/boolean_ 的构造路径上，
  /// GCC 会报 `-Wmaybe-uninitialized`（实测在 script_host.cpp 等三个单元上就是 `-Werror` 建败）。
  union {
    std::int64_t integer_;
    std::uint64_t unsigned_;
    double number_;
  } scalar_{0};
  std::string text_;
  // 两个容器的元素类型此刻仍不完整：标准库对 `vector<T>` 明确支持这种用法
  // （`T` 只需在"第一次引用成员"前完整，而那时 `JsonMember` 已在下方定义）。
  JsonObject object_;
  JsonArray array_;
};

/// 对象的一个键值对（**保序**存储元素；`items()` 与迭代器都指向它）。
///
/// 位置必须在 `Json` **之后**：它的 `second` 是完整的 `Json`。
/// 不用 `std::pair<const std::string, Json>`：`first` 是 const 就无法 `erase`
/// （vector::erase 要求元素可赋值）。代价是"遍历时别改 key"的纪律。
struct JsonMember {
  std::string first;
  Json second;
};

// ── Json 中必须看到完整 `JsonMember` 的成员（定义放在这里而不是类体内）────────

inline auto Json::find(std::string_view key) const noexcept -> const Json* {
  if (kind_ != json_detail::Kind::Object) return nullptr;
  for (const JsonMember& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

inline auto Json::find_mutable(std::string_view key) noexcept -> Json* {
  if (kind_ != json_detail::Kind::Object) return nullptr;
  for (JsonMember& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

/// `items()` 的返回类型：`for (const auto& [key, value] : node.items())`。
class JsonItems {
 public:
  explicit JsonItems(const Json& owner) noexcept : owner_(owner) {}

  class item {
   public:
    explicit item(const JsonMember& member) noexcept : member_(&member) {}
    /// 结构化绑定下标 0：键。
    [[nodiscard]] auto get_key() const -> const std::string& { return member_->first; }
    /// 结构化绑定下标 1：值。
    [[nodiscard]] auto get_value() const -> const Json& { return member_->second; }
    /// 访问器别名（旧库 `it.key()` / `it.value()` 两种写法都有，均支持）。
    [[nodiscard]] auto key() const -> const std::string& { return member_->first; }
    [[nodiscard]] auto value() const -> const Json& { return member_->second; }

   private:
    const JsonMember* member_;
  };

  class iterator {
   public:
    iterator() = default;
    explicit iterator(JsonObject::const_iterator it) noexcept : it_(it) {}
    [[nodiscard]] auto operator*() const noexcept -> item { return item(*it_); }
    /// 迭代器直接给键/值（`for (auto it = map.begin(); ...) it.key()` 的旧写法）。
    [[nodiscard]] auto key() const -> const std::string& { return it_->first; }
    [[nodiscard]] auto value() const -> const Json& { return it_->second; }
    auto operator++() -> iterator& {
      ++it_;
      return *this;
    }
    auto operator++(int) -> iterator {
      iterator copy = *this;
      ++it_;
      return copy;
    }
    [[nodiscard]] auto operator==(const iterator& other) const noexcept -> bool { return it_ == other.it_; }
    [[nodiscard]] auto operator!=(const iterator& other) const noexcept -> bool { return it_ != other.it_; }
    /// 与 `const_iterator` 的互相对齐（`found != node.end()` 这类写法靠它）。
    [[nodiscard]] auto base() const noexcept -> JsonObject::const_iterator { return it_; }

   private:
    JsonObject::const_iterator it_{};
  };

  [[nodiscard]] auto begin() const noexcept -> iterator { return iterator(owner_.members().begin()); }
  [[nodiscard]] auto end() const noexcept -> iterator { return iterator(owner_.members().end()); }

 private:
  const Json& owner_;
};

/// 自由函数版 `items()`（`for (... : st::json_items(node))` 与成员版等价）。
[[nodiscard]] inline auto json_items(const Json& owner) noexcept -> JsonItems { return JsonItems(owner); }

/// `Json::items()` 的定义（必须晚于 `JsonItems`——见类内注释）。
[[nodiscard]] inline auto Json::items() const noexcept -> JsonItems { return JsonItems(*this); }

// ── 结构化绑定协议（`auto [key, value] = item;`）───────────────────────────
//
// 只给 `tuple_size` / `tuple_element` 不够：绑定语法还要求可调用的 `get<N>`。

template <std::size_t Index>
[[nodiscard]] auto get(const JsonItems::item& entry) -> decltype(auto) {
  static_assert(Index < 2, "JsonItems::item 只有两个字段（键、值）");
  if constexpr (Index == 0) {
    return entry.get_key();
  } else {
    return entry.get_value();
  }
}

}  // namespace st

namespace std {

template <>
struct tuple_size<st::JsonItems::item> : integral_constant<size_t, 2> {};

template <>
struct tuple_element<0, st::JsonItems::item> {
  using type = const std::string&;
};

template <>
struct tuple_element<1, st::JsonItems::item> {
  using type = const st::Json&;
};

}  // namespace std
