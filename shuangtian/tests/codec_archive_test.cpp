// 霜天 codec 单测 —— 归档（tar / tar.gz / zip）读取与 ustar 打包。
//
// 覆盖：tar_write↔tar_read 往返（含长路径 prefix 拆分与目录条目）、手工构造 ustar 记录的交叉校验、
// 系统 python3 zipfile 生成的 zip 固定向量（stored + deflate + 目录）、archive_read 自动识别
// （zip / gzip+tar / tar）、解包到目录、以及 **路径逃逸拒绝**（`../`、绝对路径、深层 `..`）与
// 坏数据防御（截断 / 位翻转 / 校验和损坏）。
//
// 说明：ST_TEST / ST_CHECK 等函数式宏是 `CONVENTIONS.md` §3.6 登记的测试框架例外。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/archive.hpp"
#include "st/codec/deflate.hpp"
#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/test/test.hpp"

namespace {

using st::codec::ArchiveEntry;
using st::codec::Bytes;

/// 十六进制文本 → 字节（固定向量用）。
[[nodiscard]] auto from_hex(std::string_view text) -> Bytes {
  auto parsed = st::hash::from_hex(text);
  if (!parsed) return Bytes{};
  return *parsed;
}

/// 字面量文本 → 字节。
[[nodiscard]] auto as_bytes(std::string_view text) -> Bytes {
  Bytes out;
  out.reserve(text.size());
  for (const char ch : text) out.push_back(static_cast<std::uint8_t>(ch));
  return out;
}

/// 确定性伪随机字节（xorshift32）。
[[nodiscard]] auto pseudo_random(std::size_t size, std::uint32_t seed) -> Bytes {
  Bytes data(size, 0U);
  std::uint32_t state = seed == 0U ? 1U : seed;
  for (std::size_t at = 0; at < size; ++at) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    data[at] = static_cast<std::uint8_t>((state >> 24U) & 0xFFU);
  }
  return data;
}

/// 在条目表中按路径查找（找不到返回 nullptr）。
[[nodiscard]] auto find_entry(const std::vector<ArchiveEntry>& entries, std::string_view path)
    -> const ArchiveEntry* {
  for (const ArchiveEntry& entry : entries) {
    if (entry.path == path) return &entry;
  }
  return nullptr;
}

/// zip 中做等长条目改名（构造逃逸用例；只改字节，长度不变故结构仍合法）。
[[nodiscard]] auto rename_zip_entry(Bytes zip, std::string_view from, std::string_view to)
    -> Bytes {
  ST_CHECK_EQ(from.size(), to.size());
  for (std::size_t at = 0; at + from.size() <= zip.size(); ++at) {
    bool matched = true;
    for (std::size_t offset = 0; offset < from.size(); ++offset) {
      if (zip[at + offset] != static_cast<std::uint8_t>(from[offset])) {
        matched = false;
        break;
      }
    }
    if (!matched) continue;
    for (std::size_t offset = 0; offset < to.size(); ++offset) {
      zip[at + offset] = static_cast<std::uint8_t>(to[offset]);
    }
    at += from.size() - 1U;
  }
  return zip;
}

/// 写 tar 八进制字段（末字节 NUL）——测试侧独立实现。
void write_octal(std::span<std::uint8_t> field, std::uint64_t value) {
  const std::size_t digits = field.size() - 1;
  for (std::size_t index = 0; index < digits; ++index) {
    const std::size_t slot = digits - 1 - index;
    field[slot] = static_cast<std::uint8_t>(
        static_cast<unsigned>('0') +
        static_cast<unsigned>((value >> static_cast<unsigned>(index * 3U)) & 7U));
  }
  field.back() = 0U;
}

/// 手工构造一条 ustar 记录（**独立于 tar_write 的实现**，用于交叉校验与逃逸用例）。
[[nodiscard]] auto handcrafted_tar_record(std::string_view name, std::string_view data) -> Bytes {
  Bytes out(512, 0U);
  const std::span<std::uint8_t> header(out);
  for (std::size_t at = 0; at < name.size() && at < 100; ++at) {
    header[at] = static_cast<std::uint8_t>(name[at]);
  }
  write_octal(header.subspan(100, 8), 0644U);
  write_octal(header.subspan(108, 8), 0U);
  write_octal(header.subspan(116, 8), 0U);
  write_octal(header.subspan(124, 12), data.size());
  write_octal(header.subspan(136, 12), 0U);
  for (std::size_t at = 148; at < 156; ++at) header[at] = 0x20U;  // 校验字段先填空格
  header[156] = static_cast<std::uint8_t>('0');
  const std::string_view magic = "ustar";
  for (std::size_t at = 0; at < magic.size(); ++at) {
    header[257 + at] = static_cast<std::uint8_t>(magic[at]);
  }
  header[263] = static_cast<std::uint8_t>('0');
  header[264] = static_cast<std::uint8_t>('0');
  std::uint64_t sum = 0;
  for (const std::uint8_t byte : out) sum += static_cast<std::uint64_t>(byte);
  for (std::size_t index = 0; index < 6; ++index) {
    header[148 + (5 - index)] = static_cast<std::uint8_t>(
        static_cast<unsigned>('0') +
        static_cast<unsigned>((sum >> static_cast<unsigned>(index * 3U)) & 7U));
  }
  header[154] = 0U;
  header[155] = 0x20U;

  out.insert(out.end(), data.begin(), data.end());
  while (out.size() % 512U != 0U) out.push_back(0U);
  return out;
}

/// 由系统 python3 zipfile 生成：a.txt(stored) + dir/(目录) + dir/b.bin(deflate, 192 字节)。
inline constexpr std::string_view kSystemZipHex =
    "504b0304140000000000000021006a39e0d0050000000500000005000000612e747874616c706861"
    "504b030414000000000000002100000000000000000000000000040000006469722f"
    "504b030414000000080000002100b0d1e36645000000c0000000090000006469722f622e62696e"
    "6360646266616563e7e0e4e2e6e1e5e3171014121611151397909492969195935750545256515553"
    "d7d0d4d2d6d1d5d33730343236313533b7b0b4b2b6b1b5b3671860fd00"
    "504b01021403140000000000000021006a39e0d00500000005000000050000000000000000000000800100000000612e747874"
    "504b01021403140000000000000021000000000000000000000000000400000000000000000000008001280000006469722f"
    "504b0102140314000000080000002100b0d1e36645000000c000000009000000000000000000000080014a0000006469722f622e62696e"
    "504b050600000000030003009c000000b60000000000";

/// 系统 zip 中 `dir/b.bin` 的期望内容：0..63 重复三遍。
[[nodiscard]] auto expected_zip_binary() -> Bytes {
  Bytes expected;
  expected.reserve(192);
  for (int round = 0; round < 3; ++round) {
    for (unsigned value = 0; value < 64U; ++value) {
      expected.push_back(static_cast<std::uint8_t>(value));
    }
  }
  return expected;
}

/// 构造往返用例的条目表（含目录、长路径与二进制内容）。
[[nodiscard]] auto sample_entries() -> std::vector<ArchiveEntry> {
  std::vector<ArchiveEntry> entries;
  entries.push_back(ArchiveEntry{"dir/", true, 0U, Bytes{}});
  entries.push_back(ArchiveEntry{"dir/a.txt", false, 5U, as_bytes("alpha")});
  entries.push_back(ArchiveEntry{"b.bin", false, 0U, pseudo_random(700, 7U)});
  entries.push_back(ArchiveEntry{"empty.txt", false, 0U, Bytes{}});
  const std::string deep = std::string(80, 'p') + "/" + std::string(90, 'n') + ".txt";
  entries.push_back(ArchiveEntry{deep, false, 3U, as_bytes("xyz")});
  return entries;
}

}  // namespace

ST_TEST(codec_tar_write_read_roundtrip) {
  const std::vector<ArchiveEntry> entries = sample_entries();
  auto packed = st::codec::tar_write(entries);
  ST_REQUIRE(packed.has_value());
  ST_CHECK_EQ(packed->size() % 512U, std::size_t{0});

  auto read_back = st::codec::tar_read(*packed);
  ST_REQUIRE(read_back.has_value());
  ST_CHECK_EQ(read_back->size(), entries.size());
  for (std::size_t index = 0; index < entries.size() && index < read_back->size(); ++index) {
    ST_CHECK_EQ((*read_back)[index].path, entries[index].path);
    ST_CHECK_EQ((*read_back)[index].is_dir, entries[index].is_dir);
    ST_CHECK((*read_back)[index].data == entries[index].data);
  }

  // archive_read 自动识别 tar
  auto detected = st::codec::archive_read(*packed);
  ST_REQUIRE(detected.has_value());
  ST_CHECK_EQ(detected->size(), entries.size());

  // gzip + tar（.tar.gz）自动识别
  auto gzipped = st::codec::gzip_deflate(*packed, 6);
  ST_REQUIRE(gzipped.has_value());
  auto from_gzip = st::codec::archive_read(*gzipped);
  ST_REQUIRE(from_gzip.has_value());
  ST_CHECK_EQ(from_gzip->size(), entries.size());
  const ArchiveEntry* long_entry = find_entry(*from_gzip, entries.back().path);
  ST_REQUIRE(long_entry != nullptr);
  ST_CHECK(long_entry->data == entries.back().data);
}

ST_TEST(codec_tar_read_handcrafted_record) {
  Bytes archive = handcrafted_tar_record("hello.txt", "hello tar");
  archive.insert(archive.end(), 1024, 0U);
  auto entries = st::codec::tar_read(archive);
  ST_REQUIRE(entries.has_value());
  ST_CHECK_EQ(entries->size(), std::size_t{1});
  ST_CHECK_EQ(entries->front().path, std::string("hello.txt"));
  ST_CHECK_EQ(entries->front().size, std::uint64_t{9});
  ST_CHECK(entries->front().data == as_bytes("hello tar"));

  // 校验和损坏 → 必须报错
  Bytes corrupted = archive;
  corrupted[0] = static_cast<std::uint8_t>(corrupted[0] ^ 0x20U);
  auto corrupted_read = st::codec::tar_read(corrupted);
  ST_CHECK(!corrupted_read.has_value());
  ST_CHECK(corrupted_read.error().code == st::ErrorCode::Parse);

  // 数据块截断 → 必须报错
  auto truncated = st::codec::tar_read(std::span<const std::uint8_t>(archive).first(600));
  ST_CHECK(!truncated.has_value());
}

ST_TEST(codec_zip_read_system_vector) {
  const Bytes zip = from_hex(kSystemZipHex);
  auto entries = st::codec::zip_read(zip);
  ST_REQUIRE(entries.has_value());
  ST_CHECK_EQ(entries->size(), std::size_t{3});

  const ArchiveEntry* text = find_entry(*entries, "a.txt");
  ST_REQUIRE(text != nullptr);
  ST_CHECK(!text->is_dir);
  ST_CHECK(text->data == as_bytes("alpha"));

  const ArchiveEntry* directory = find_entry(*entries, "dir/");
  ST_REQUIRE(directory != nullptr);
  ST_CHECK(directory->is_dir);

  const ArchiveEntry* binary = find_entry(*entries, "dir/b.bin");
  ST_REQUIRE(binary != nullptr);
  ST_CHECK(binary->data == expected_zip_binary());

  // archive_read 自动识别 zip
  auto detected = st::codec::archive_read(zip);
  ST_REQUIRE(detected.has_value());
  ST_CHECK_EQ(detected->size(), std::size_t{3});
}

ST_TEST(codec_zip_rejects_corruption) {
  const Bytes zip = from_hex(kSystemZipHex);

  // 任意截断都必须被拒（EOCD 必然残缺）
  std::size_t accepted = 0;
  for (std::size_t cut = 0; cut < zip.size(); ++cut) {
    auto entries = st::codec::zip_read(std::span<const std::uint8_t>(zip).first(cut));
    if (entries) ++accepted;
  }
  ST_CHECK_EQ(accepted, std::size_t{0});

  // 单字节位翻转：不崩溃；若仍可解则条目数与内容必须正确（CRC-32 兜底）
  std::size_t still_valid = 0;
  for (std::size_t at = 0; at < zip.size(); ++at) {
    Bytes corrupted = zip;
    corrupted[at] = static_cast<std::uint8_t>(corrupted[at] ^ 0x3CU);
    auto entries = st::codec::zip_read(corrupted);
    if (entries) {
      ++still_valid;
      ST_CHECK_EQ(entries->size(), std::size_t{3});
      const ArchiveEntry* text = find_entry(*entries, "a.txt");
      if (text != nullptr) ST_CHECK(text->data == as_bytes("alpha"));
    }
  }
  ST_CHECK(still_valid < zip.size());

  // 非 zip 数据（自动识别会落到 tar 分支）
  ST_CHECK(!st::codec::archive_read(Bytes(700, 0xA5U)).has_value());
}

ST_TEST(codec_archive_extract_to_directory) {
  auto dir = st::fs::make_temp_dir("st-codec-archive");
  ST_REQUIRE(dir.has_value());

  const Bytes zip = from_hex(kSystemZipHex);
  const st::Status extracted = st::codec::archive_extract_to(zip, *dir);
  ST_REQUIRE(extracted.has_value());
  ST_CHECK(st::fs::is_regular_file(st::fs::join(*dir, "a.txt")));
  ST_CHECK(st::fs::is_regular_file(st::fs::join(*dir, "dir/b.bin")));
  ST_CHECK(st::fs::is_directory(st::fs::join(*dir, "dir")));

  auto text = st::fs::read_bytes(st::fs::join(*dir, "a.txt"));
  ST_REQUIRE(text.has_value());
  ST_CHECK(*text == as_bytes("alpha"));
  auto binary = st::fs::read_bytes(st::fs::join(*dir, "dir/b.bin"));
  ST_REQUIRE(binary.has_value());
  ST_CHECK(*binary == expected_zip_binary());

  // tar 同样可解包
  const std::vector<ArchiveEntry> entries = sample_entries();
  auto packed = st::codec::tar_write(entries);
  ST_REQUIRE(packed.has_value());
  const std::string tar_dir = st::fs::join(*dir, "tar_out");
  const st::Status tar_extracted = st::codec::archive_extract_to(*packed, tar_dir);
  ST_REQUIRE(tar_extracted.has_value());
  auto tar_text = st::fs::read_bytes(st::fs::join(tar_dir, "dir/a.txt"));
  ST_REQUIRE(tar_text.has_value());
  ST_CHECK(*tar_text == as_bytes("alpha"));

  const st::Status cleaned = st::fs::remove_all(*dir);
  ST_CHECK(cleaned.has_value());
}

ST_TEST(codec_archive_rejects_path_traversal) {
  auto dir = st::fs::make_temp_dir("st-codec-traversal");
  ST_REQUIRE(dir.has_value());
  const std::string outside = st::fs::join(st::fs::parent(*dir), "st-codec-evil.txt");
  const st::Status pre_clean = st::fs::remove_file(outside);  // 不存在则忽略
  ST_CHECK(pre_clean.has_value() || pre_clean.error().code == st::ErrorCode::Io);

  const std::vector<std::string> unsafe_paths{
      "../st-codec-evil.txt",
      "a/../../st-codec-evil.txt",
      "/tmp/st-codec-evil.txt",
      "..\\st-codec-evil.txt",
  };
  for (const std::string& unsafe : unsafe_paths) {
    Bytes archive = handcrafted_tar_record(unsafe, "boom");
    archive.insert(archive.end(), 1024, 0U);
    const st::Status status = st::codec::archive_extract_to(archive, *dir);
    ST_CHECK(!status.has_value());
    ST_CHECK(status.error().code == st::ErrorCode::Invalid);
  }
  ST_CHECK(!st::fs::exists(outside));
  ST_CHECK(!st::fs::exists("/tmp/st-codec-evil.txt"));

  // 同一净化逻辑对 zip 分支同样生效（等长改名构造逃逸条目）
  const Bytes evil_zip = rename_zip_entry(from_hex(kSystemZipHex), "a.txt", "../ab");
  auto zip_entries = st::codec::zip_read(evil_zip);
  ST_REQUIRE(zip_entries.has_value());
  ST_CHECK(find_entry(*zip_entries, "../ab") != nullptr);
  const st::Status zip_status = st::codec::archive_extract_to(evil_zip, *dir);
  ST_CHECK(!zip_status.has_value());
  ST_CHECK(zip_status.error().code == st::ErrorCode::Invalid);

  const st::Status cleaned = st::fs::remove_all(*dir);
  ST_CHECK(cleaned.has_value());
}

ST_TEST(codec_tar_write_rejects_unsafe_entries) {
  const std::vector<std::string> unsafe_paths{
      "", "/etc/passwd", "../evil.txt", "a/../../evil.txt", "..\\evil.txt", "C:\\evil.txt"};
  for (const std::string& unsafe : unsafe_paths) {
    std::vector<ArchiveEntry> entries;
    entries.push_back(ArchiveEntry{unsafe, false, 4U, as_bytes("boom")});
    auto packed = st::codec::tar_write(entries);
    ST_CHECK(!packed.has_value());
    ST_CHECK(packed.error().code == st::ErrorCode::Invalid);
  }

  // 形似而非逃逸的路径必须放行
  std::vector<ArchiveEntry> safe;
  safe.push_back(ArchiveEntry{"a/..b/c.txt", false, 4U, as_bytes("boom")});
  auto packed = st::codec::tar_write(safe);
  ST_REQUIRE(packed.has_value());
  auto read_back = st::codec::tar_read(*packed);
  ST_REQUIRE(read_back.has_value());
  ST_CHECK_EQ(read_back->size(), std::size_t{1});
  ST_CHECK_EQ(read_back->front().path, std::string("a/..b/c.txt"));
}

ST_TEST(codec_archive_empty_and_garbage_inputs) {
  ST_CHECK(!st::codec::archive_read(Bytes{}).has_value());
  ST_CHECK(!st::codec::tar_read(Bytes{}).has_value());
  ST_CHECK(!st::codec::zip_read(Bytes{}).has_value());
  ST_CHECK(!st::codec::archive_read(Bytes(2048, 0xA5U)).has_value());

  // 空归档（两块全零）解析为空条目表；解包到目录为成功且无文件
  Bytes empty_tar(1024, 0U);
  auto entries = st::codec::tar_read(empty_tar);
  ST_REQUIRE(entries.has_value());
  ST_CHECK_EQ(entries->size(), std::size_t{0});

  auto dir = st::fs::make_temp_dir("st-codec-empty");
  ST_REQUIRE(dir.has_value());
  ST_CHECK(st::codec::archive_extract_to(empty_tar, *dir).has_value());
  ST_CHECK(st::codec::archive_extract_to(empty_tar, "").error().code == st::ErrorCode::Invalid);
  const st::Status cleaned = st::fs::remove_all(*dir);
  ST_CHECK(cleaned.has_value());
}
