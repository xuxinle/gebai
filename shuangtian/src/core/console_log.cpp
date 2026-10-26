#include "st/core/console_log.hpp"

#include <cstdio>
#include <mutex>

#include "st/core/fs.hpp"

// lint-allow: L8 控制台镜像是**进程级基础设施**：`st::print` 是全局输出出口，
// 它没有 Context 可注入（调用方是整个进程的每一处 print）。与 `st::log` 的
// sink/listener 同类，按 §8 登记为进程级例外。
namespace st::console_log {
namespace {

std::mutex mirror_mutex{};
struct MirrorState {
  std::FILE* handle{nullptr};
  std::string file_path{};
  std::size_t written{0};
  bool reported_failure{false};
};
MirrorState state{};

}  // namespace

auto open(std::string_view path) -> bool {
  if (path.empty()) return false;
  {
    const std::scoped_lock lock(mirror_mutex);
    if (state.handle != nullptr) {
      std::fflush(state.handle);
      std::fclose(state.handle);
      state.handle = nullptr;
    }
    state.file_path = std::string(path);
    state.written = 0;
    state.reported_failure = false;
    // 父目录不存在时建出来：调用方（CLI）不必自己先 mkdir，
    // 否则「日志没落盘」会以“文件打不开”的形式静默发生。
    if (const std::size_t slash = state.file_path.find_last_of("/\\");
        slash != std::string::npos) {
      (void)fs::create_directories(state.file_path.substr(0, slash));
    }
    // "wb" 而不是 "ab"：每次运行写**自己的**日志文件（文件名带时间戳），
    // 追加语义会把上一次的输出混进来，事后无法判断哪一段属于本次。
    state.handle = std::fopen(state.file_path.c_str(), "wb");
    if (state.handle == nullptr) {
      std::fprintf(stderr, "[st] 控制台镜像打不开（只写终端）：%s\n", state.file_path.c_str());
      state.file_path.clear();
      return false;
    }
  }
  return true;
}

void close() {
  const std::scoped_lock lock(mirror_mutex);
  if (state.handle != nullptr) {
    std::fflush(state.handle);
    std::fclose(state.handle);
    state.handle = nullptr;
  }
  state.file_path.clear();
}

auto active() -> bool {
  const std::scoped_lock lock(mirror_mutex);
  return state.handle != nullptr;
}

auto path() -> std::string {
  const std::scoped_lock lock(mirror_mutex);
  return state.file_path;
}

void mirror(std::string_view text) noexcept {
  if (text.empty()) return;
  const std::scoped_lock lock(mirror_mutex);
  if (state.handle == nullptr) return;
  const std::size_t length = std::fwrite(text.data(), 1, text.size(), state.handle);
  if (length != text.size()) {
    if (!state.reported_failure) {
      state.reported_failure = true;
      // 用 stderr 直写（不能回 st::print——那会递归回本函数）
      std::fprintf(stderr, "[st] 控制台镜像写盘失败（后续只写终端）：%s\n",
                   state.file_path.c_str());
    }
    std::fclose(state.handle);
    state.handle = nullptr;
    return;
  }
  // 每条即 flush：进程被 OOM/取消杀掉时，已写的内容必须还在。
  // 构建日志本来就不长（几 KB 到几 MB），flush 的开销远小于"丢了要重跑"。
  std::fflush(state.handle);
  state.written += length;
}

auto bytes_written() -> std::size_t {
  const std::scoped_lock lock(mirror_mutex);
  return state.written;
}

}  // namespace st::console_log
