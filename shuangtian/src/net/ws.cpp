/// WebSocket（RFC 6455）实现：帧编解码 + 握手 + 会话。
/// SHA-1 自带最小实现（RFC 3174；握手 Accept 的固定 20 字节摘要——不引入大依赖）。

#include "st/net/ws.hpp"

#include <array>
#include <cstring>
#include <random>
#include <span>

#include "st/core/base64.hpp"

namespace st::net::ws {

// ── SHA-1（握手 Accept 专用最小实现）────────────────────────────────────

namespace {

struct Sha1Digest {
  std::array<std::uint8_t, 20> bytes{};
};

[[nodiscard]] auto sha1(std::span<const std::uint8_t> data) -> Sha1Digest {
  std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
               h4 = 0xC3D2E1F0;
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;

  std::vector<std::uint8_t> message(data.begin(), data.end());
  message.push_back(0x80);
  while (message.size() % 64U != 56U) message.push_back(0x00);
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xFFU));
  }

  auto left_rotate = [](std::uint32_t value, std::uint32_t count) -> std::uint32_t {
    return (value << count) | (value >> (32U - count));
  };

  for (std::size_t chunk = 0; chunk < message.size(); chunk += 64U) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t index = 0; index < 16; ++index) {
      w[index] = (static_cast<std::uint32_t>(message[chunk + index * 4U]) << 24U) |
                 (static_cast<std::uint32_t>(message[chunk + index * 4U + 1U]) << 16U) |
                 (static_cast<std::uint32_t>(message[chunk + index * 4U + 2U]) << 8U) |
                 static_cast<std::uint32_t>(message[chunk + index * 4U + 3U]);
    }
    for (std::size_t index = 16; index < 80; ++index) {
      w[index] = left_rotate(w[index - 3] ^ w[index - 8] ^ w[index - 14] ^ w[index - 16], 1U);
    }
    std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (std::size_t index = 0; index < 80; ++index) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (index < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (index < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (index < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const std::uint32_t temp = left_rotate(a, 5U) + f + e + k + w[index];
      e = d;
      d = c;
      c = left_rotate(b, 30U);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }
  Sha1Digest digest;
  const std::array<std::uint32_t, 5> words = {h0, h1, h2, h3, h4};
  for (std::size_t word = 0; word < words.size(); ++word) {
    digest.bytes[word * 4U + 0U] = static_cast<std::uint8_t>((words[word] >> 24) & 0xFFU);
    digest.bytes[word * 4U + 1U] = static_cast<std::uint8_t>((words[word] >> 16) & 0xFFU);
    digest.bytes[word * 4U + 2U] = static_cast<std::uint8_t>((words[word] >> 8) & 0xFFU);
    digest.bytes[word * 4U + 3U] = static_cast<std::uint8_t>(words[word] & 0xFFU);
  }
  return digest;
}

}  // namespace

// ── 帧编解码 ────────────────────────────────────────────────────────────

auto frame_encode(const Frame& frame, std::uint32_t mask_key, bool masked)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  // 第一字节：FIN + RSV(0) + opcode
  out.push_back((frame.fin ? 0x80U : 0x00U) | static_cast<std::uint8_t>(frame.opcode));
  // 长度档位
  const std::size_t length = frame.payload.size();
  std::uint8_t mask_bit = masked ? 0x80U : 0x00U;
  if (length <= 125U) {
    out.push_back(static_cast<std::uint8_t>(mask_bit | length));
  } else if (length <= 0xFFFFU) {
    out.push_back(static_cast<std::uint8_t>(mask_bit | 126U));
    out.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(length & 0xFFU));
  } else {
    out.push_back(static_cast<std::uint8_t>(mask_bit | 127U));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(length) >> shift) & 0xFFU));
    }
  }
  if (masked) {
    const std::array<std::uint8_t, 4> key = {static_cast<std::uint8_t>(mask_key >> 24U),
                                             static_cast<std::uint8_t>(mask_key >> 16U),
                                             static_cast<std::uint8_t>(mask_key >> 8U),
                                             static_cast<std::uint8_t>(mask_key)};
    for (std::size_t index = 0; index < 4; ++index) out.push_back(key[index]);
    for (std::size_t index = 0; index < length; ++index) {
      out.push_back(frame.payload[index] ^ key[index % 4U]);
    }
  } else {
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  }
  return out;
}

auto FrameDecoder::try_parse_one() -> std::optional<Frame> {
  if (buffer_.size() < 2U) return std::nullopt;
  const std::uint8_t first = buffer_[0];
  const std::uint8_t second = buffer_[1];
  const bool fin = (first & 0x80U) != 0U;
  // RSV 位非零 = 扩展协商失败（我们没协商扩展）——fail the connection
  if ((first & 0x70U) != 0U) {
    error_ = "RSV 位非零（未协商扩展）";
    return std::nullopt;
  }
  const auto opcode = static_cast<Opcode>(first & 0x0FU);
  const bool masked = (second & 0x80U) != 0U;
  std::uint64_t length = second & 0x7FU;
  std::size_t header_size = 2U;
  if (length == 126U) {
    if (buffer_.size() < 4U) return std::nullopt;
    length = (static_cast<std::uint64_t>(buffer_[2]) << 8U) | buffer_[3];
    header_size = 4U;
  } else if (length == 127U) {
    if (buffer_.size() < 10U) return std::nullopt;
    length = 0;
    for (int index = 2; index < 10; ++index) {
      length = (length << 8U) | buffer_[static_cast<std::size_t>(index)];
    }
    if (length > (1ULL << 31U)) {   // 实用上限（2GiB 帧足够任何用途）
      error_ = "帧长超实用上限";
      return std::nullopt;
    }
    header_size = 10U;
  }
  if (masked) header_size += 4U;
  if (buffer_.size() < header_size + static_cast<std::size_t>(length)) return std::nullopt;

  Frame frame;
  frame.fin = fin;
  frame.opcode = opcode;
  const std::size_t payload_offset = header_size;
  if (masked) {
    const std::size_t key_offset = payload_offset - 4U;
    const std::array<std::uint8_t, 4> key = {buffer_[key_offset], buffer_[key_offset + 1U],
                                             buffer_[key_offset + 2U], buffer_[key_offset + 3U]};
    frame.payload.resize(static_cast<std::size_t>(length));
    for (std::uint64_t index = 0; index < length; ++index) {
      frame.payload[static_cast<std::size_t>(index)] =
          buffer_[payload_offset + static_cast<std::size_t>(index)] ^ key[index % 4U];
    }
  } else {
    frame.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                         buffer_.begin() + static_cast<std::ptrdiff_t>(payload_offset + length));
  }
  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(payload_offset + length));
  return frame;
}

auto FrameDecoder::feed(std::span<const std::uint8_t> data) -> std::vector<Frame> {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  std::vector<Frame> frames;
  while (!failed()) {
    auto frame = try_parse_one();
    if (!frame.has_value()) break;
    frames.push_back(std::move(*frame));
  }
  return frames;
}

// ── 握手 ────────────────────────────────────────────────────────────────

auto make_handshake_key() -> std::string {
  // 16 随机字节（random_device——握手键不需要密码学级熵，防缓存碰撞即可）
  std::random_device device;
  std::array<std::uint8_t, 16> raw{};
  for (auto& byte : raw) byte = static_cast<std::uint8_t>(device());
  return base64_encode(raw);
}

auto expected_accept(std::string_view client_key) -> std::string {
  // RFC 6455 §4.2.2：key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" 的 SHA-1 → base64
  const std::string combined = std::string(client_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const Sha1Digest digest = sha1(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(combined.data()), combined.size()));   // lint-allow: L6 字节级协议编解码：char 是字节别名，跨 reinterpret 是 payload 的真实布局
  return base64_encode(digest.bytes);
}

auto client_handshake_request(std::string_view host, std::uint16_t port,
                              std::string_view path, std::string_view client_key) -> std::string {
  const bool default_port = port == 80;
  std::string request;
  request += "GET " + std::string(path.empty() ? "/" : path) + " HTTP/1.1\r\n";
  request += "Host: " + std::string(host) + (default_port ? "" : ":" + std::to_string(port)) + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + std::string(client_key) + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += "\r\n";
  return request;
}

auto server_handshake_response(std::string_view client_key) -> std::string {
  return "HTTP/1.1 101 Switching Protocols\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " +
         expected_accept(client_key) + "\r\n\r\n";
}

auto validate_server_handshake(std::string_view response, std::string_view client_key) -> bool {
  // 状态行 + 头（大小写不敏感子串判定——握手应答头少，逐项找够用且不引解析器）
  const std::string text(response);
  auto contains_ci = [&text](std::string_view needle) {
    std::string lower_text = text;
    std::string lower_needle(needle);
    for (auto& c : lower_text) c = static_cast<char>(std::tolower(c));
    for (auto& c : lower_needle) c = static_cast<char>(std::tolower(c));
    return lower_text.find(lower_needle) != std::string::npos;
  };
  if (!contains_ci("101")) return false;
  if (!contains_ci("upgrade: websocket")) return false;
  // Accept 必须精确匹配（大小写敏感——base64 字符集）
  const std::string accept_header = "sec-websocket-accept: " + expected_accept(client_key);
  return contains_ci(accept_header);
}

// ── 会话 ────────────────────────────────────────────────────────────────

auto WsSession::send_frame(Opcode opcode, std::span<const std::uint8_t> payload) -> bool {
  Frame frame;
  frame.opcode = opcode;
  frame.payload.assign(payload.begin(), payload.end());
  const std::uint32_t mask_key = client_ ? 0x11223344U : 0U;   // 客户端方向必须掩码
  const auto bytes = frame_encode(frame, mask_key, client_);
  return stream_.write_all(bytes);
}

auto WsSession::send_text(std::string_view text) -> bool {
  return send_frame(Opcode::Text,
                    std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));   // lint-allow: L6 同上：文本载荷的字节视图
}

auto WsSession::send_binary(std::span<const std::uint8_t> data) -> bool {
  return send_frame(Opcode::Binary, data);
}

auto WsSession::send_ping(std::string_view payload) -> bool {
  return send_frame(Opcode::Ping,
                    std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));   // lint-allow: L6 同上：ping 载荷的字节视图
}

auto WsSession::send_close(std::uint16_t code, std::string_view reason) -> bool {
  if (close_sent_) return true;
  std::vector<std::uint8_t> payload;
  payload.push_back(static_cast<std::uint8_t>(code >> 8U));
  payload.push_back(static_cast<std::uint8_t>(code & 0xFFU));
  payload.insert(payload.end(), reason.begin(), reason.end());
  const bool sent = send_frame(Opcode::Close, payload);
  if (sent) close_sent_ = true;
  return sent;
}

auto WsSession::pump_frames() -> bool {
  read_buffer_.resize(4096);
  const int got = stream_.read_some(read_buffer_);
  if (got <= 0) return false;
  // ⚠ feed 返回的帧不能丢：FrameDecoder 是「无内部帧队列」设计——
  // 解析结果只经返回值交付（buffer_ 里只是未消费字节）。旧写法 (void)feed 把帧
  // 全扔了，表现为「消息永远收不到」（实测：会话对回环卡死在第一条消息）。
  auto fresh = decoder_.feed(std::span<const std::uint8_t>(
      read_buffer_.data(), static_cast<std::size_t>(got)));
  pending_frames_.insert(pending_frames_.end(),
                         std::make_move_iterator(fresh.begin()),
                         std::make_move_iterator(fresh.end()));
  return !decoder_.failed();
}

auto WsSession::try_read_message() -> std::optional<Message> {
  for (;;) {
    if (decoder_.failed()) return std::nullopt;
    // 新到字节先进 pending（pump 已并入）；这里直接消费队列。
    auto frames = std::move(pending_frames_);
    pending_frames_.clear();
    bool progress = !frames.empty();
    while (!frames.empty()) {
      Frame frame = std::move(frames.front());
      frames.erase(frames.begin());
      switch (frame.opcode) {
        case Opcode::Ping:
          // 自动回 Pong（RFC §5.5.2：必须回，载荷原样）
          (void)send_frame(Opcode::Pong, frame.payload);
          progress = true;
          continue;
        case Opcode::Pong:
          continue;   // 心跳应答——应用层要语义时自行计时（本层静默）
        case Opcode::Close: {
          close_received_ = true;
          Message message;
          message.opcode = Opcode::Close;
          if (frame.payload.size() >= 2U) {
            message.close_code = static_cast<std::uint16_t>(
                (frame.payload[0] << 8U) | frame.payload[1]);
          }
          if (frame.payload.size() > 2U) {
            message.text.assign(reinterpret_cast<const char*>(frame.payload.data()) + 2,   // lint-allow: L6 同上：Close 原因的字节还原
                                frame.payload.size() - 2U);
          }
          // 回 Close（完成关闭握手；已发过则不重发）
          if (!close_sent_) (void)send_close(1000);
          return message;
        }
        case Opcode::Continuation: {
          if (!fragmenting_) {
            return std::nullopt;   // 协议错：无起始片的续片
          }
          fragment_buffer_.insert(fragment_buffer_.end(), frame.payload.begin(),
                                  frame.payload.end());
          if (frame.fin) {
            Message message;
            message.opcode = fragment_opcode_;
            if (fragment_opcode_ == Opcode::Text) {
              message.text.assign(reinterpret_cast<const char*>(fragment_buffer_.data()),   // lint-allow: L6 同上：分片重组的字节还原
                                  fragment_buffer_.size());
            } else {
              message.binary = std::move(fragment_buffer_);
            }
            fragment_buffer_.clear();
            fragmenting_ = false;
            return message;
          }
          continue;
        }
        case Opcode::Text:
        case Opcode::Binary: {
          if (frame.fin) {
            Message message;
            message.opcode = frame.opcode;
            if (frame.opcode == Opcode::Text) {
              message.text.assign(reinterpret_cast<const char*>(frame.payload.data()),   // lint-allow: L6 同上：文本消息的字节还原
                                  frame.payload.size());
            } else {
              message.binary = std::move(frame.payload);
            }
            return message;
          }
          fragmenting_ = true;
          fragment_opcode_ = frame.opcode;
          fragment_buffer_ = std::move(frame.payload);
          continue;
        }
      }
    }
    if (!progress) return std::nullopt;
  }
}

auto WsSession::read_message() -> std::optional<Message> {
  for (;;) {
    if (auto message = try_read_message(); message.has_value()) return message;
    if (close_received_ || decoder_.failed()) return std::nullopt;
    if (!pump_frames()) return std::nullopt;
  }
}

}  // namespace st::net::ws
