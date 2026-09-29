#include "st/md/stream.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::md {
namespace {

[[nodiscard]] auto count_newlines(std::string_view text) noexcept -> std::size_t {
  std::size_t count = 0;
  for (const char raw : text) {
    if (raw == '\n') ++count;
  }
  return count;
}

}  // namespace

void MdStream::reset() {
  source_.clear();
  committed_.clear();
  blocks_.clear();
  tail_begin_ = 0;
  tail_line_ = 0;
  pending_ = false;
}

auto MdStream::feed(std::string_view chunk) -> std::vector<Block> {
  source_.append(chunk);

  // 防御：不变量是 tail_begin_ <= source_.size()（只追加解析保证）；越界即整体重来，绝不越读。
  if (tail_begin_ > source_.size()) {
    committed_.clear();
    tail_begin_ = 0;
    tail_line_ = 0;
  }

  const std::string_view tail(source_.data() + tail_begin_, source_.size() - tail_begin_);
  const detail::ParseResult parsed = detail::parse_incremental(tail, tail_line_);

  if (parsed.sealed_blocks > 0) {
    committed_.reserve(committed_.size() + parsed.sealed_blocks);
    for (std::size_t index = 0; index < parsed.sealed_blocks; ++index) {
      committed_.push_back(parsed.blocks[index]);
    }
  }

  const std::size_t sealed_bytes = parsed.sealed_bytes > tail.size() ? tail.size() : parsed.sealed_bytes;
  tail_line_ += count_newlines(tail.substr(0, sealed_bytes));
  tail_begin_ += sealed_bytes;

  blocks_.clear();
  blocks_.reserve(committed_.size() + (parsed.blocks.size() - parsed.sealed_blocks));
  for (const Block& block : committed_) blocks_.push_back(block);
  for (std::size_t index = parsed.sealed_blocks; index < parsed.blocks.size(); ++index) {
    blocks_.push_back(parsed.blocks[index]);
  }

  pending_ = parsed.sealed_blocks < parsed.blocks.size();
  return blocks_;
}

auto MdStream::snapshot() const -> const std::vector<Block>& { return blocks_; }

auto MdStream::pending() const noexcept -> bool { return pending_; }

auto MdStream::source() const -> std::string { return source_; }

}  // namespace st::md
