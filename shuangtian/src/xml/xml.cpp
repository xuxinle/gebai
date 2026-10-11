#include "st/xml/xml.hpp"

#include <cstdlib>
#include <format>

#include "st/core/fs.hpp"

namespace st::xml {
namespace {

auto is_space(char ch) noexcept -> bool {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

/// 实体解码：五个预定义 + 数字引用（`&#65;` / `&#x41;`）；其余原样保留。
///
/// 为何只认这些：通用实体集需要 DTD 才能解析，而 DTD 是外部输入（安全面）+
/// 我们没有任何消费者需要它。**不猜**是这里的正确姿态——认不出的 `&foo;`
/// 原样留在文本里，比静默吞掉更容易被发现。
void append_decoded(std::string& out, std::string_view raw) {
  out.reserve(out.size() + raw.size());
  for (std::size_t index = 0; index < raw.size();) {
    const char ch = raw[index];
    if (ch != '&') {
      out.push_back(ch);
      ++index;
      continue;
    }
    const std::size_t semi = raw.find(';', index + 1);
    // 实体的合理长度上限：`&#x1F600;` 这种最长的也就 10 来个字符。
    if (semi == std::string_view::npos || semi - index > 12) {
      out.push_back(ch);
      ++index;
      continue;
    }
    const std::string_view body = raw.substr(index + 1, semi - index - 1);
    if (body == "lt") {
      out.push_back('<');
    } else if (body == "gt") {
      out.push_back('>');
    } else if (body == "amp") {
      out.push_back('&');
    } else if (body == "quot") {
      out.push_back('"');
    } else if (body == "apos") {
      out.push_back('\'');
    } else if (body.size() > 1 && body[0] == '#') {
      // 数字引用：`&#65;` 十进制 / `&#x41;` 十六进制。
      const bool hex = body.size() > 2 && (body[1] == 'x' || body[1] == 'X');
      const std::string digits(body.substr(hex ? 2 : 1));
      char* end = nullptr;
      const unsigned long code = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
      if (end == digits.c_str() + digits.size() && code != 0 && code <= 0x10FFFFUL) {
        // UTF-8 编码（1~4 字节）。
        const auto value = static_cast<std::uint32_t>(code);
        if (value < 0x80U) {
          out.push_back(static_cast<char>(value));
        } else if (value < 0x800U) {
          out.push_back(static_cast<char>(0xC0U | (value >> 6)));
          out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        } else if (value < 0x10000U) {
          out.push_back(static_cast<char>(0xE0U | (value >> 12)));
          out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
          out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        } else {
          out.push_back(static_cast<char>(0xF0U | (value >> 18)));
          out.push_back(static_cast<char>(0x80U | ((value >> 12) & 0x3FU)));
          out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
          out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        }
      } else {
        out.append(raw.substr(index, semi - index + 1));  // 非法引用：原样保留
      }
    } else {
      out.append(raw.substr(index, semi - index + 1));  // 认不出的实体：原样保留
    }
    index = semi + 1;
  }
}

/// 段落空白折叠：首尾去掉，内部连续空白折成单个空格。
///
/// 为什么不是「原样保留」：界面描述文件天然带缩进与换行（
/// `<Column>\n  <Text>标题</Text>\n</Column>` 里 Column 的文本就是那些空白），
/// 原样保留会让每个容器的文本都变成一串空白，消费者还得各自再 trim 一遍。
/// 需要保留空白的场景（`<pre>` 类）走属性显式声明，而不是靠这里的宽松。
void append_collapsed(std::string& out, std::string_view raw) {
  std::string decoded;
  append_decoded(decoded, raw);
  std::size_t begin = 0;
  std::size_t end = decoded.size();
  while (begin < end && is_space(decoded[begin])) ++begin;
  while (end > begin && is_space(decoded[end - 1])) --end;
  bool pending_space = false;
  for (std::size_t index = begin; index < end; ++index) {
    const char ch = decoded[index];
    if (is_space(ch)) {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty()) out.push_back(' ');
    pending_space = false;
    out.push_back(ch);
  }
}

/// 一个待处理的元素（构建中；`Node` 只有成型后才能定）。
struct Frame {
  Node node{};
  bool has_text{false};
};

/// 解析器（光标式；不依赖正则/回溯）。
struct Parser {
  std::string_view source{};
  std::size_t pos{0};
  std::uint32_t line{1};
  std::vector<Frame> stack{};
  Document document{};
  std::uint32_t top_level_count{0};
  bool collecting_{false};   ///< 已进入“多顶层 → 匿名 root 收集”模式
  Error* error{nullptr};
  bool failed{false};

  void advance_line_to(std::size_t target) {
    for (std::size_t index = pos; index < target && index < source.size(); ++index) {
      if (source[index] == '\n') ++line;
    }
  }

  void fail(std::string message) {
    if (failed) return;
    failed = true;
    if (error != nullptr) {
      error->message = std::move(message);
      error->line = line;
    }
  }

  /// 把当前帧收口：挂到父帧（或作为顶层结果）。
  void close_frame() {
    if (stack.empty()) return;
    Frame frame = std::move(stack.back());
    stack.pop_back();
    if (stack.empty()) {
      ++top_level_count;
      if (top_level_count == 1) {
        document.root = std::move(frame.node);
      } else {
        // 第二个及之后的顶层元素：全挂到一个匿名 root 下（见头文件的约定）。
        //
        // ⚠ 判断“是否需要先把第一个包起来”用**独立标记**，不能用
        // `document.root.name.empty()`：第一个顶层元素的 name 就**不是**空的
        //（它就是 `<A/>` 的 A），拿它当判据永远不成立——第二个元素会被当成
        // 第一个的**子元素**塞进去（实测：`<A/><B/>` 出 `root=A, children=B`）。
        if (!collecting_) {
          collecting_ = true;
          Node first = std::move(document.root);
          document.root = Node{};
          document.root.children.push_back(std::move(first));
        }
        document.root.children.push_back(std::move(frame.node));
      }
      return;
    }
    stack.back().node.children.push_back(std::move(frame.node));
  }

  void add_text(std::string_view raw) {
    if (stack.empty()) return;   // 顶层裸文本：丢掉（资源文件里没有它的语义）
    append_collapsed(stack.back().node.text, raw);
  }

  auto parse_attributes(std::string_view tag, std::size_t& index) -> bool {
    while (index < tag.size()) {
      while (index < tag.size() && is_space(tag[index])) ++index;
      if (index >= tag.size() || tag[index] == '>' || tag[index] == '/') break;
      const std::size_t name_begin = index;
      while (index < tag.size() && !is_space(tag[index]) && tag[index] != '=' &&
             tag[index] != '>' && tag[index] != '/') {
        ++index;
      }
      const std::string_view name = tag.substr(name_begin, index - name_begin);
      if (name.empty()) {
        ++index;
        continue;
      }
      std::string_view value{};
      while (index < tag.size() && is_space(tag[index])) ++index;
      if (index < tag.size() && tag[index] == '=') {
        ++index;
        while (index < tag.size() && is_space(tag[index])) ++index;
        if (index < tag.size() && (tag[index] == '"' || tag[index] == '\'')) {
          const char quote = tag[index++];
          const std::size_t value_begin = index;
          while (index < tag.size() && tag[index] != quote) ++index;
          value = tag.substr(value_begin, index - value_begin);
          if (index < tag.size()) ++index;
        } else {
          const std::size_t value_begin = index;
          while (index < tag.size() && !is_space(tag[index]) && tag[index] != '>') ++index;
          value = tag.substr(value_begin, index - value_begin);
        }
      }
      // 值里的实体要解码（`&amp;` 写在属性里很常见）；名字也要（罕见但对称）。
      std::string decoded_name;
      append_decoded(decoded_name, name);
      std::string decoded_value;
      append_decoded(decoded_value, value);
      if (!stack.empty()) {
        stack.back().node.attributes.emplace_back(std::move(decoded_name),
                                                 std::move(decoded_value));
      }
    }
    return true;
  }

  /// 找当前标签的结束 `>`：**跳过引号里的 `>`**。
  ///
  /// ⚠ 这是最容易踩的一个坑（我自己第一版就错了）：`<A label="a > b"/>` 里那个 `>`
  /// 在引号内，不是标签结束。直接 `source.find('>')` 会把标签截成 `<A label="a >`，
  /// 于是属性值变成 `a >`、后面的内容全乱。
  ///
  /// 返回值 = `>` 的下标（`npos` = 没有闭合）。
  [[nodiscard]] auto find_tag_end(std::size_t from) const -> std::size_t {
    char quote = 0;
    for (std::size_t index = from; index < source.size(); ++index) {
      const char ch = source[index];
      if (quote != 0) {
        if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '"' || ch == '\'') {
        quote = ch;
        continue;
      }
      if (ch == '>') return index;
    }
    return std::string_view::npos;
  }

  void run() {
    while (pos < source.size()) {
      const std::size_t lt = source.find('<', pos);
      if (lt == std::string_view::npos) {
        add_text(source.substr(pos));
        advance_line_to(source.size());
        pos = source.size();
        break;
      }
      if (lt > pos) add_text(source.substr(pos, lt - pos));
      advance_line_to(lt);
      pos = lt;

      // 注释
      if (source.compare(pos, 4, "<!--") == 0) {
        const std::size_t close = source.find("-->", pos + 4);
        if (close == std::string_view::npos) {
          advance_line_to(source.size());
          pos = source.size();
          break;
        }
        advance_line_to(close + 3);
        pos = close + 3;
        continue;
      }
      // `<![CDATA[ ... ]]>`：内部内容**不折叠、不解码**（CDATA 的字面语义）。
      if (source.compare(pos, 9, "<![CDATA[") == 0) {
        const std::size_t close = source.find("]]>", pos + 9);
        const std::size_t text_end = close == std::string_view::npos ? source.size() : close;
        if (!stack.empty()) {
          // 原样追加（编译期检查用 is_space 折叠是错的：CDATA 就是要字面）。
          stack.back().node.text.append(source.substr(pos + 9, text_end - pos - 9));
        }
        advance_line_to(text_end);
        pos = close == std::string_view::npos ? source.size() : close + 3;
        continue;
      }
      // `<?xml … ?>` / `<!DOCTYPE …>`：跳过（我们不消费它们）。
      if (source.compare(pos, 2, "<?") == 0 || source.compare(pos, 2, "<!") == 0) {
        const std::size_t close = source.find('>', pos + 2);
        if (close == std::string_view::npos) {
          advance_line_to(source.size());
          pos = source.size();
          break;
        }
        advance_line_to(close + 1);
        pos = close + 1;
        continue;
      }

      const std::size_t close = find_tag_end(pos + 1);
      if (close == std::string_view::npos) {
        fail("标签没有闭合的 '>'");
        break;
      }
      const std::string_view tag = source.substr(pos, close - pos + 1);
      const std::uint32_t tag_line = line;
      if (tag.size() >= 2 && tag[1] == '/') {
        // 结束标签
        std::size_t index = 2;
        const std::size_t name_begin = index;
        while (index < tag.size() && !is_space(tag[index]) && tag[index] != '>') ++index;
        const std::string_view name = tag.substr(name_begin, index - name_begin);
        // 容错收口：若与被关的层不匹配，就按名字**向上找**（找不到则忽略该标签）。
        std::size_t matched = stack.size();
        for (std::size_t depth = stack.size(); depth > 0; --depth) {
          if (stack[depth - 1].node.name == name) {
            matched = depth - 1;
            break;
          }
        }
        if (matched == stack.size()) {
          // 没有对应开放标签：忽略（容错优先，但记一条诊断）。
          fail(std::string("结束标签 </") + std::string(name) + "> 没有对应的开始标签");
        } else {
          // 需要关闭多于一层 = 中间那些没写结束标签。
          //
          // ⚠ 这里**必须给诊断**：静默收口会让“少写了一个 </Row>”这类错误
          // 一直看不到——树的形状看起来正常，只是内容跑到了另一层。
          const std::size_t unclosed = stack.size() - matched - 1;
          if (unclosed > 0) {
            fail(std::format("</{}> 之前有 {} 个元素没有闭合（已自动收口）", name, unclosed));
          }
          while (stack.size() > matched) close_frame();
        }
      } else {
        // 开始 / 自闭合标签
        std::size_t index = 1;
        const std::size_t name_begin = index;
        while (index < tag.size() && !is_space(tag[index]) && tag[index] != '>' &&
               tag[index] != '/') {
          ++index;
        }
        const std::string_view name = tag.substr(name_begin, index - name_begin);
        std::size_t last = tag.size();
        while (last > 0 && (tag[last - 1] == '>' || is_space(tag[last - 1]))) --last;
        const bool self_close = last > 0 && tag[last - 1] == '/';
        Frame frame;
        frame.node.name = std::string(name);
        frame.node.line = tag_line;
        stack.push_back(std::move(frame));
        parse_attributes(tag, index);
        if (self_close) close_frame();
        if (stack.size() > 256) {
          // 深度护栏：畸形输入不应该把内存吃光（正常界面描述不会超过几十层）。
          fail("元素嵌套过深（>256 层）");
          break;
        }
      }
      advance_line_to(close + 1);
      pos = close + 1;
    }
    // 未闭合的标签：自动收口（容错）。
    if (!stack.empty() && !failed) {
      if (error != nullptr) {
        error->message = "有 " + std::to_string(stack.size()) + " 个元素没有闭合（已自动收口）";
        error->line = line;
      }
    }
    while (!stack.empty()) close_frame();
  }
};

}  // namespace

auto Node::attr(std::string_view key) const -> std::optional<std::string_view> {
  for (const auto& [attr_name, attr_value] : attributes) {
    if (attr_name == key) return attr_value;
  }
  return std::nullopt;
}

auto Node::attr_number(std::string_view key) const -> std::optional<double> {
  const auto raw = attr(key);
  if (!raw.has_value() || raw->empty()) return std::nullopt;
  const std::string digits(*raw);
  char* end = nullptr;
  const double value = std::strtod(digits.c_str(), &end);
  if (end == digits.c_str()) return std::nullopt;
  // 允许 `12px` 这种带单位写法（单位由消费方决定，这里只取值）。
  return value;
}

auto Node::attr_or(std::string_view key, std::string_view fallback) const -> std::string_view {
  const auto raw = attr(key);
  return raw.has_value() ? *raw : fallback;
}

auto Node::attr_bool(std::string_view key, bool fallback) const -> bool {
  const auto raw = attr(key);
  if (!raw.has_value()) return fallback;
  const std::string_view value = *raw;
  if (value.empty()) return true;   // 裸属性 `<Node grow/>`
  if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
  if (value == "false" || value == "0" || value == "no" || value == "off") return false;
  return fallback;
}

auto Node::child(std::string_view child_name) const -> const Node* {
  for (const auto& item : children) {
    if (item.name == child_name) return &item;
  }
  return nullptr;
}

auto Node::children_named(std::string_view child_name) const -> std::vector<const Node*> {
  std::vector<const Node*> out;
  for (const auto& item : children) {
    if (item.name == child_name) out.push_back(&item);
  }
  return out;
}

auto parse(std::string_view source, Error* out_error) -> std::optional<Document> {
  if (source.empty()) {
    if (out_error != nullptr) {
      out_error->message = "输入为空";
      out_error->line = 1;
    }
    return std::nullopt;
  }
  Parser parser;
  parser.source = source;
  parser.error = out_error;
  parser.run();
  if (out_error != nullptr) out_error->line = parser.line;
  if (parser.document.root.name.empty() && parser.document.root.children.empty() &&
      parser.document.root.text.empty()) {
    return std::nullopt;   // 一个元素都没解析出来
  }
  return std::move(parser.document);
}

auto parse_file(const std::string& path, Error* out_error) -> std::optional<Document> {
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) {
    if (out_error != nullptr) {
      out_error->message =
          "读不到文件：" + path + "（" + text.error().message + "）";
      out_error->line = 1;
    }
    return std::nullopt;
  }
  return parse(*text, out_error);
}

auto attr_of(const Node* node, std::string_view key) -> std::optional<std::string_view> {
  return node == nullptr ? std::nullopt : node->attr(key);
}

auto escape(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '&': out.append("&amp;"); break;
      case '"': out.append("&quot;"); break;
      case '\'': out.append("&apos;"); break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

}  // namespace st::xml
