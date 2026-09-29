#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/md/markdown.hpp"
#include "st/md/stream.hpp"

namespace {

[[nodiscard]] auto txt(std::string_view value) -> std::string { return std::string(value); }

/// 结构序列化（含嵌套、行号、行内序列）：流式切片与一次性解析必须逐字节相同。
void dump_into(const std::vector<st::md::Block>& blocks, int depth, std::string& out) {
  const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
  for (const st::md::Block& block : blocks) {
    out += indent;
    out += st::md::to_string(block.kind);
    out += " lvl=";
    out += std::to_string(block.level);
    out += " line=";
    out += std::to_string(block.start_line);
    out += " lang=" + block.language;
    out += " code=<" + block.code + ">";
    if (block.ordered) out += " ordered";
    if (block.task_item) out += " task";
    if (block.checked) out += " checked";
    for (const std::string& cell : block.header) out += " H<" + cell + ">";
    for (const std::vector<std::string>& row : block.rows) {
      out += " R[";
      for (const std::string& cell : row) out += "<" + cell + ">";
      out += "]";
    }
    for (const st::md::Inline& node : block.inlines) {
      out += " I{";
      out += st::md::to_string(node.kind);
      out += ":" + node.text + "|" + node.url + "|" + node.title;
      out += "}";
    }
    out += "\n";
    dump_into(block.children, depth + 1, out);
  }
}

[[nodiscard]] auto dump(const std::vector<st::md::Block>& blocks) -> std::string {
  std::string out;
  dump_into(blocks, 0, out);
  return out;
}

/// 含**全部块类型**的样本文档（末尾空行 ⇒ 全部块闭合）。
inline constexpr std::string_view kFullDocument = R"MD(# 霜天 Markdown 流式样本

段落文本，含 **加粗**、*斜体*、`code`、~~删除~~ 与 [链接](https://example.com "标题") 和 ![图](a.png)。
中文 😀 与 emoji 🚀 混排的续行。

- 无序项 A
  - 嵌套项 a
  - 嵌套项 b
- [x] 已完成
- [ ] 未完成

1. 有序一
2. 有序二

> 引用段落
>
> - 引用内列表

```cpp
int main() { return 0; }
```

```python title=demo
def f(x):
    return x  # 注释
```

    indented code

| 名称 | 数量 | 说明 |
| :--- | ---: | :---: |
| 苹果 | 3 | 甜的 |
| pear | 12 | 脆的 |

---

<div class="x">
html block
</div>

尾段（含裸链接 https://st.dev/x 与 <https://st.dev/y>）。

)MD";

/// 按固定步长切片喂入，返回最终 snapshot 的结构序列化。
[[nodiscard]] auto stream_dump(std::string_view document, std::size_t step) -> std::string {
  st::md::MdStream stream;
  for (std::size_t pos = 0; pos < document.size(); pos += step) {
    const std::size_t count = step < document.size() - pos ? step : document.size() - pos;
    (void)stream.feed(document.substr(pos, count));
  }
  return dump(stream.snapshot());
}

}  // namespace

// ⑦ 流式切片等价性：1 / 3 / 7 字节切片最终结果 == 一次性解析
ST_TEST(md_stream_equivalence_1_3_7_bytes) {
  const std::string expected = dump(st::md::parse(kFullDocument));
  ST_CHECK(!expected.empty());

  for (const std::size_t step : {std::size_t{1}, std::size_t{3}, std::size_t{7}}) {
    st::md::MdStream stream;
    std::vector<st::md::Block> latest;
    for (std::size_t pos = 0; pos < kFullDocument.size(); pos += step) {
      const std::size_t count =
          step < kFullDocument.size() - pos ? step : kFullDocument.size() - pos;
      latest = stream.feed(kFullDocument.substr(pos, count));
    }
    ST_CHECK_EQ(dump(latest), expected);
    ST_CHECK_EQ(dump(stream.snapshot()), expected);
    ST_CHECK_EQ(stream.source(), std::string(kFullDocument));
    ST_CHECK(!stream.pending());
  }
}

ST_TEST(md_stream_equivalence_irregular_chunks) {
  const std::string expected = dump(st::md::parse(kFullDocument));
  // 确定性伪随机切分（无时钟/无网络，可复现）：覆盖"切在 UTF-8 中间"等各种边界
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  for (int round = 0; round < 6; ++round) {
    st::md::MdStream stream;
    std::size_t pos = 0;
    while (pos < kFullDocument.size()) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const std::size_t step = 1 + static_cast<std::size_t>((state >> 33) % 17);
      const std::size_t count = step < kFullDocument.size() - pos ? step : kFullDocument.size() - pos;
      (void)stream.feed(kFullDocument.substr(pos, count));
      pos += count;
    }
    ST_CHECK_EQ(dump(stream.snapshot()), expected);
  }

  // 极细分片：每次 1 字节（含 UTF-8 中间字节）
  ST_CHECK_EQ(stream_dump(kFullDocument, 1), expected);
  ST_CHECK_EQ(stream_dump(kFullDocument, 2), expected);
  ST_CHECK_EQ(stream_dump(kFullDocument, 5), expected);
}

ST_TEST(md_stream_prefix_stability) {
  // 同一前缀无论怎么切片，快照必须一致（解析纯函数 + 只追加）
  const std::string_view prefix = kFullDocument.substr(0, 120);
  const std::string reference = dump(st::md::parse(prefix));

  st::md::MdStream whole;
  (void)whole.feed(prefix);
  ST_CHECK_EQ(dump(whole.snapshot()), reference);

  st::md::MdStream byte_wise;
  for (const char raw : prefix) {
    const std::string_view one(&raw, 1);
    (void)byte_wise.feed(one);
  }
  ST_CHECK_EQ(dump(byte_wise.snapshot()), reference);

  // 重复喂同一前缀（追加到同一流）也不崩、不漂移
  st::md::MdStream repeated;
  (void)repeated.feed(prefix);
  const std::string first = dump(repeated.snapshot());
  (void)repeated.feed("");
  ST_CHECK_EQ(dump(repeated.snapshot()), first);
}

ST_TEST(md_stream_unclosed_fence_is_code_block) {
  st::md::MdStream stream;
  (void)stream.feed("# 标题\n\n```cpp\nint x = 1;\n");
  ST_REQUIRE(stream.snapshot().size() == 2);
  ST_CHECK_EQ(txt(st::md::to_string(stream.snapshot()[1].kind)), txt("code_block"));
  ST_CHECK_EQ(txt(stream.snapshot()[1].language), txt("cpp"));
  ST_CHECK_EQ(txt(stream.snapshot()[1].code), txt("int x = 1;\n"));
  ST_CHECK(stream.pending());  // 围栏未闭合

  (void)stream.feed("int y = 2;\n```\n\n");
  ST_REQUIRE(stream.snapshot().size() == 2);
  ST_CHECK_EQ(txt(stream.snapshot()[1].code), txt("int x = 1;\nint y = 2;\n"));
  ST_CHECK(!stream.pending());  // 围栏闭合且后随空行

  // 未闭合表格 / 未完段落：同样以"已收到部分"呈现且 pending 为真
  st::md::MdStream tail;
  (void)tail.feed("| a | b |\n| --- | --- |\n| 1 |");
  ST_REQUIRE(tail.snapshot().size() == 1);
  ST_CHECK_EQ(txt(st::md::to_string(tail.snapshot()[0].kind)), txt("table"));
  ST_CHECK(tail.pending());

  st::md::MdStream paragraph;
  (void)paragraph.feed("未完的一段");
  ST_REQUIRE(paragraph.snapshot().size() == 1);
  ST_CHECK_EQ(txt(st::md::to_string(paragraph.snapshot()[0].kind)), txt("paragraph"));
  ST_CHECK(paragraph.pending());
  (void)paragraph.feed("\n\n");
  ST_CHECK(!paragraph.pending());
}

ST_TEST(md_stream_utf8_split_bytes) {
  const std::string source = "# 中文 😀\n\n汉字续行\n";
  const std::string expected = dump(st::md::parse(source));

  st::md::MdStream stream;
  for (std::size_t pos = 0; pos < source.size(); pos += 2) {
    const std::size_t count = std::min<std::size_t>(2, source.size() - pos);
    (void)stream.feed(std::string_view(source).substr(pos, count));
  }
  ST_CHECK_EQ(dump(stream.snapshot()), expected);
  ST_CHECK_EQ(stream.source(), source);

  // 逐字节喂入：中间态不得崩溃/越界，终态每个文本都是合法 UTF-8
  st::md::MdStream step;
  for (const char raw : source) {
    (void)step.feed(std::string_view(&raw, 1));
  }
  ST_CHECK_EQ(dump(step.snapshot()), expected);
  for (const st::md::Block& block : step.snapshot()) {
    for (const st::md::Inline& node : block.inlines) {
      ST_CHECK(st::utf8_is_valid(node.text));
    }
  }
}

ST_TEST(md_stream_reset_and_source) {
  st::md::MdStream stream;
  (void)stream.feed("第一段\n\n第二段\n\n");
  ST_CHECK_EQ(stream.source(), txt("第一段\n\n第二段\n\n"));
  ST_REQUIRE(stream.snapshot().size() == 2);
  ST_CHECK(!stream.pending());

  stream.reset();
  ST_CHECK(stream.source().empty());
  ST_CHECK(stream.snapshot().empty());
  ST_CHECK(!stream.pending());

  // reset 后与全新实例行为一致
  st::md::MdStream fresh;
  (void)stream.feed("# 标题\n\n");
  (void)fresh.feed("# 标题\n\n");
  ST_CHECK_EQ(dump(stream.snapshot()), dump(fresh.snapshot()));
}

ST_TEST(md_stream_incremental_matches_tail_parse) {
  // 长文档（多段）逐片喂入：每次快照都等于对当前全文的一次性解析
  std::string document;
  for (std::size_t index = 0; index < 60; ++index) {
    document += "## 小节 ";
    document += std::to_string(index);
    document += "\n\n段落内容 ";
    document += std::to_string(index);
    document += " 带 `code` 与 **加粗**。\n\n";
    if (index % 7 == 0) document += "- 项一\n- 项二\n\n";
    if (index % 11 == 0) document += "```js\nlet a = 1;\n```\n\n";
  }

  st::md::MdStream stream;
  std::size_t pos = 0;
  while (pos < document.size()) {
    const std::size_t count = std::min<std::size_t>(9, document.size() - pos);
    (void)stream.feed(std::string_view(document).substr(pos, count));
    pos += count;
    if (pos % 200 < 9) {
      ST_CHECK_EQ(dump(stream.snapshot()), dump(st::md::parse(std::string_view(document).substr(0, pos))));
    }
  }
  ST_CHECK_EQ(dump(stream.snapshot()), dump(st::md::parse(document)));
}
