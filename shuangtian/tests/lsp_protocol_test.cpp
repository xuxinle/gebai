/// LSP 地基测试（阶段 1）：协议分帧/JSON-RPC 编解码 + 子进程双向通信。
///
/// 为什么这两件事测在一起：它们是**同一条链路的上下半**——分帧层的结果要经
/// 双向管道送进送出，任何一侧坏了整条链都不通。用真子进程（`cat` 回显）
/// 而非 mock：mock 能证明“我以为对”，真进程能证明“确实对”。

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "st/core/process.hpp"
#include "st/lsp/protocol.hpp"
#include "st/test/test.hpp"

namespace {

using st::lsp::FrameReader;
using st::lsp::Message;
using st::lsp::Position;

/// 构造一条线格式消息（测试里的“手工字节”，用来验证解析端）。
[[nodiscard]] auto wire(std::string_view body) -> std::string {
  return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

}  // namespace

ST_TEST(lsp_frame_reader_parses_single_message) {
  FrameReader reader;
  reader.feed(wire(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"a":1}})"));
  const auto message = reader.next();
  ST_REQUIRE(message.has_value());
  ST_CHECK(message->has_id);
  ST_CHECK_EQ(message->id.get<std::int64_t>(), std::int64_t{1});
  ST_CHECK_EQ(message->method, std::string("initialize"));
  ST_CHECK(message->is_request());
  ST_CHECK(!message->is_response());
  ST_CHECK_EQ(st::json_get_i64(message->body, "a"), std::int64_t{1});
  // 取完一条后没有残留。
  ST_CHECK(!reader.next().has_value());
  ST_CHECK_EQ(reader.pending_bytes(), std::size_t{0});
}

ST_TEST(lsp_frame_reader_handles_split_and_batched_bytes) {
  // 情形一：字节被任意切分（真实管道一次读 4096 字节，消息会被切断）。
  const std::string bytes =
      wire(R"({"jsonrpc":"2.0","id":"req-1","method":"textDocument/hover","params":{}})");
  for (std::size_t split = 1; split < bytes.size(); ++split) {
    FrameReader reader;
    reader.feed(std::string_view(bytes).substr(0, split));
    ST_CHECK(!reader.next().has_value());   // 半个消息不该被交出去
    ST_CHECK_EQ(reader.pending_bytes(), split);
    reader.feed(std::string_view(bytes).substr(split));
    const auto message = reader.next();
    ST_REQUIRE(message.has_value());
    ST_CHECK_EQ(message->method, std::string("textDocument/hover"));
    // id 是字符串时原样保留（LSP 允许数字与字符串两种）。
    ST_CHECK_EQ(message->id.get<std::string>(), std::string("req-1"));
  }

  // 情形二：一次喂进多条（批量响应），必须逐条吐出。
  FrameReader reader;
  reader.feed(wire(R"({"jsonrpc":"2.0","method":"a","params":{}})"));
  reader.feed(wire(R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})"));
  reader.feed(wire(R"({"jsonrpc":"2.0","method":"b","params":{}})"));
  const auto first = reader.next();
  ST_REQUIRE(first.has_value());
  ST_CHECK_EQ(first->method, std::string("a"));
  ST_CHECK(first->is_notification());
  const auto second = reader.next();
  ST_REQUIRE(second.has_value());
  ST_CHECK(second->is_response());
  ST_CHECK(st::json_get_bool(second->body, "ok"));
  const auto third = reader.next();
  ST_REQUIRE(third.has_value());
  ST_CHECK_EQ(third->method, std::string("b"));
  ST_CHECK(!reader.next().has_value());
}

ST_TEST(lsp_frame_reader_tolerates_extra_headers_and_lf_only) {
  // 多余头字段（`Content-Type`）要容忍。
  FrameReader reader;
  const std::string body = R"({"jsonrpc":"2.0","method":"x","params":{}})";
  reader.feed("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\nContent-Length: " +
              std::to_string(body.size()) + "\r\n\r\n" + body);
  const auto message = reader.next();
  ST_REQUIRE(message.has_value());
  ST_CHECK_EQ(message->method, std::string("x"));

  // 少数实现只发 `\n\n`（协议要求 `\r\n\r\n`，容忍比报错好）。
  FrameReader lf;
  lf.feed("Content-Length: " + std::to_string(body.size()) + "\n\n" + body);
  const auto lf_message = lf.next();
  ST_REQUIRE(lf_message.has_value());
  ST_CHECK_EQ(lf_message->method, std::string("x"));
}

ST_TEST(lsp_frame_reader_reports_bad_frame_without_jamming) {
  // 一条坏消息不该让整个会话瘫掉：如实记录原因，后续消息照常解析。
  FrameReader reader;
  reader.feed("garbage without header\r\n\r\n");
  reader.feed(wire("{not valid json}"));
  reader.feed(wire(R"({"jsonrpc":"2.0","method":"good","params":{}})"));
  // 前两次 next 可能返回 nullopt（坏帧被丢弃），第三次必须拿到好消息。
  bool got = false;
  for (int attempt = 0; attempt < 4 && !got; ++attempt) {
    const auto message = reader.next();
    if (message.has_value() && message->method == "good") got = true;
  }
  ST_CHECK(got);
  // 坏帧的原因看得到（不静默）。
  ST_CHECK(!reader.last_error().empty() || reader.pending_bytes() == 0);
}

ST_TEST(lsp_encode_message_round_trips) {
  const auto request = st::lsp::make_request(3, "textDocument/definition",
                                             [] {
                                               st::Json params = st::Json::object();
                                               params["x"] = 1;
                                               return params;
                                             }());
  const std::string encoded = st::lsp::encode_message(request);
  // 帧头格式正确（`Content-Length` + 空行 + 体）。
  ST_CHECK(encoded.rfind("Content-Length: ", 0) == 0);
  const std::size_t header_end = encoded.find("\r\n\r\n");
  ST_REQUIRE(header_end != std::string::npos);
  const std::string declared =
      encoded.substr(std::string("Content-Length: ").size(),
                     header_end - std::string("Content-Length: ").size());
  const std::string body = encoded.substr(header_end + 4);
  ST_CHECK_EQ(std::stoul(declared), body.size());

  // 回环：编码 → 解析 → 同一语义。
  FrameReader reader;
  reader.feed(encoded);
  const auto parsed = reader.next();
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_EQ(parsed->method, std::string("textDocument/definition"));
  ST_CHECK(parsed->is_request());

  // 通知：无 id。
  const std::string notification =
      st::lsp::encode_message(st::lsp::make_notification("initialized"));
  FrameReader notify_reader;
  notify_reader.feed(notification);
  const auto notify = notify_reader.next();
  ST_REQUIRE(notify.has_value());
  ST_CHECK(notify->is_notification());
  ST_CHECK(!notify->has_id);

  // 错误响应：code 与 message 保真。
  const std::string error =
      st::lsp::encode_message(st::lsp::make_error_response(9, -32601, "method not found"));
  FrameReader error_reader;
  error_reader.feed(error);
  const auto parsed_error = error_reader.next();
  ST_REQUIRE(parsed_error.has_value());
  ST_CHECK(parsed_error->is_error);
  ST_CHECK_EQ(parsed_error->error_code, -32601);
  ST_CHECK_EQ(parsed_error->error_message, std::string("method not found"));
}

ST_TEST(lsp_position_maps_utf16_columns) {
  // BMP 内中文（1 码元 3 字节）+ BMP 外 emoji（2 码元 4 字节）都必须对。
  const std::string text = "ab中文cd\nsecond\n🙂x\n";
  // 第 1 行：a(0) b(1) 中(2) 文(3) c(4) d(5)
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 0, .character = 0}),
              std::size_t{0});
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 0, .character = 2}),
              std::size_t{2});   // "ab" 之后
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 0, .character = 4}),
              std::size_t{8});   // "ab中文" = 2 + 3 + 3 字节
  // 第 2 行首字符（"second" 的 's'）。
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 1, .character = 0}),
              std::size_t{11});
  // 第 3 行：emoji 占 2 个 UTF-16 码元；列 2 应落在 emoji 之后。
  const std::size_t emoji_start = text.find("🙂");
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 2, .character = 0}), emoji_start);
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 2, .character = 2}),
              emoji_start + 4);
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 2, .character = 3}),
              emoji_start + 5);   // "x"

  // 反向：字节偏移 → 位置。
  const auto back = st::lsp::offset_to_position(text, emoji_start + 4);
  ST_CHECK_EQ(back.line, std::size_t{2});
  ST_CHECK_EQ(back.character, std::size_t{2});
  // 越界夹取（server 的 range 可能比我们手里的版本新）。
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 99, .character = 0}), text.size());
  ST_CHECK_EQ(st::lsp::position_to_offset(text, Position{.line = 0, .character = 999}),
              std::size_t{10});   // 第 1 行末尾（换行前）

  // JSON 往返。
  const Position position{.line = 3, .character = 7};
  const auto json = st::lsp::position_to_json(position);
  const auto parsed = st::lsp::position_from_json(json);
  ST_CHECK_EQ(parsed.line, std::size_t{3});
  ST_CHECK_EQ(parsed.character, std::size_t{7});
  const st::lsp::Range range{.start = position, .end = Position{.line = 4, .character = 1}};
  const auto round = st::lsp::range_from_json(st::lsp::range_to_json(range));
  ST_CHECK_EQ(round.start.character, std::size_t{7});
  ST_CHECK_EQ(round.end.line, std::size_t{4});
}

ST_TEST(stream_handle_supports_bidirectional_pipes) {
  // 真子进程双向通信：`/bin/sh -c 'cat'` 把 stdin 原样回显到 stdout。
  // 这验证的正是 LSP 需要的形态（同一对管道上写请求、读响应）。
  st::process::StreamHandle handle;
  handle.open("/bin/sh", {"-c", "cat"}, "");
  ST_REQUIRE(handle.valid());
  ST_CHECK(handle.can_write());

  const std::string payload = "hello\nworld\n";
  ST_CHECK(handle.write(payload));
  // 读回两行（回显）。
  std::string line;
  ST_REQUIRE(handle.read_line(line));
  ST_CHECK_EQ(line, std::string("hello"));
  ST_REQUIRE(handle.read_line(line));
  ST_CHECK_EQ(line, std::string("world"));

  // 写多段（对应多条 LSP 消息），顺序保真。
  ST_CHECK(handle.write("second-round\n"));
  ST_REQUIRE(handle.read_line(line));
  ST_CHECK_EQ(line, std::string("second-round"));

  // 发完关 stdin → 子进程 EOF 退出（`cat` 的正常收尾路径）。
  handle.close_write();
  ST_CHECK(!handle.can_write());
  const int code = handle.finish();
  ST_CHECK_EQ(code, 0);
}

ST_TEST(stream_handle_reports_write_failure_after_process_exit) {
  // 子进程立刻退出：之后再写必须**如实返回 false**（不静默丢弃）。
  st::process::StreamHandle handle;
  handle.open("/bin/sh", {"-c", "exit 3"}, "");
  ST_REQUIRE(handle.valid());
  const int code = handle.finish();
  ST_CHECK_EQ(code, 3);
  // 进程已退：写会失败（管道无读者 → EPIPE/SIGPIPE 被忽略后返回 false）。
  // 允许“第一次写侥幸成功”（内核缓冲），但反复写必然失败。
  bool wrote_all = true;
  for (int attempt = 0; attempt < 64 && wrote_all; ++attempt) {
    wrote_all = handle.write(std::string(4096, 'x'));
  }
  ST_CHECK(!wrote_all);
}

ST_TEST(stream_handle_write_works_without_stdin_pipe_regression) {
  // 回归：未 open 时 write 不崩、返回 false；read_line 仍返回 false。
  st::process::StreamHandle handle;
  ST_CHECK(!handle.valid());
  ST_CHECK(!handle.can_write());
  ST_CHECK(!handle.write("x"));
  std::string line;
  ST_CHECK(!handle.read_line(line));
  handle.close_write();   // 幂等，不崩

  // 终端场景（只读、不写 stdin）仍要正常工作：`echo` 输出照读。
  st::process::StreamHandle echo;
  echo.open("/bin/sh", {"-c", "echo terminal-path"}, "");
  ST_REQUIRE(echo.valid());
  ST_REQUIRE(echo.read_line(line));
  ST_CHECK_EQ(line, std::string("terminal-path"));
  ST_CHECK_EQ(echo.finish(), 0);
}
