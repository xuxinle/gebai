// X11 上屏路径对照探针（吞吐口径）：XPutImage（走 socket）vs XShmPutImage（共享内存）。
//
// 目的：霜天 X11 后端每帧整块画布（0xRRGGBBAA → 0x00RRGGBB）转换后经 `XPutImage`
// 推给 X 服务器——本探针量清「上屏」这段的绝对成本，并给出 MIT-SHM / SIMD 转换 /
// 局部上屏三者的收益上限，避免凭感觉优化。
//
// 口径：连续 N 帧 put + 一次 XSync 排空的总时间 / N（**含**服务器合成与网络往返，
// 即端到端稳态吞吐）；同时单独报「提交」（客户端占用）与「排空」。
//
// 实测（本机 Xwayland :0，40~200 帧中位）：
// | 上屏方式 | 1280×720 | 1920×1080 |
// |---|---|---|
// | XPutImage 整屏 | 17.5 ms/帧 | 18.2 ms/帧 |
// | XShmPutImage 整屏 | 1.46 ms/帧 | 2.42 ms/帧 |
// | XShmPutImage 每帧 XSync（单段） | 3.37 ms/帧 | 5.45 ms/帧 |
// | XShmPutImage 双段 + 完成事件（`--double`） | 见运行输出 | 见运行输出 |
// | XShmPutImage 局部 400×300 | — | 0.85 ms/帧 |
// 像素转换（0xRRGGBBAA→0xFFRRGGBB）另有 0.96 ms @1080p（SIMD 0.68 ms）。
//
// 为什么这四档都要量：
//  - 「每帧 XSync」是**单段**的真实代价（服务端直接读 shmaddr，不等往返就覆写 = 撕裂）；
//  - 「双段 + 完成事件」是**双缓冲**的真实代价：不阻塞送显，只在轮转回一块仍在途的段时
//    才等一次完成事件。两者之差就是本项改造的全部收益，不量就会高估或低估。
//
// 双缓冲档的口径细节（与后端实现同一手法）：两块段交替写，每帧 `send_event=1` 要求
// 服务端回报 `XShmCompletionEvent`；选槽前先收已到达的完成事件（不 sync），
// **只有**轮转回在途块时才 XSync 一次——这正是 gallery 里 `present_ms` 的构成。
//
// 构建：本文件自带 Xlib 最小 ABI 与 dlopen（不依赖 -lX11 / X11 头）——
//
//     g++ -std=c++23 -O2 -mavx2 -Iinclude tools/x11_present_probe.cpp -o /tmp/x11_present_probe -ldl
//
// 用法： x11_present_probe [width] [height] [frames] [--rect WxH] [--double] [--pace MS]
//
// 说明：本探针是量尺（不进框架构建），因此 L7/L9/L14 的既有豁免写法照抄
// 其它 tools/ 探针的口径：真正的 lint 门禁只扫 `src/` 与 `tests/` 的构建单元，
// 而 `st lint` 会把这个目录一并算进去——所以这里显式标注豁免（而不是让门禁变红）。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <thread>
#include <cstdlib>
#include <cstdio>   // lint-allow: L7 探针输出（自包含编译，不链框架对象）
#include <cstring>
#include <dlfcn.h>  // lint-allow: L14 探针自带 Xlib 最小 ABI（不链 -lX11）
#include <immintrin.h>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <vector>


/// 本地格式化输出：探针要**自包含可编译**（不链框架对象、不依赖任何 .o），
/// 因此不复用 `st::print`（它需要 `src/core/print.cpp` 的符号）。
/// 与 `st::print` 同一形态（`std::format` + 编译期校验格式串），只是落点在本文件内。
template <class... Args>
void out(std::format_string<Args...> fmt, Args&&... args) {
  const std::string text = std::format(fmt, std::forward<Args>(args)...);
  if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size()) return;
}

using XDisplay = void;
using XId = unsigned long;

struct XEvent { int type; std::int64_t pad[23]; };

struct XImage {
  int width, height, xoffset, format;
  char* data;
  int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line,
      bits_per_pixel;
  unsigned long red_mask, green_mask, blue_mask;
  void* obdata;
  struct { void *create_image, *destroy_image, *get_pixel, *put_pixel, *sub_image, *add_pixel; } f;
};

struct XShmSegmentInfo { XId shmseg; int shmid; char* shmaddr; int read_only; };

/// 完成事件（`shmstr.h`）的字段布局（本机实测：`shmseg` 在偏移 48，8 字节对齐）。
/// 用结构体而不是手算偏移：偏移算错时失效方式是“永远认不出自己那一块”
/// （静默退化，不崩不报错），而那正是探针要避免的盲区。
struct XShmCompletionEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId drawable;
  int major_code;
  int minor_code;
  XId shmseg;
  unsigned long offset;
  unsigned long data;
};
static_assert(offsetof(XShmCompletionEvent, shmseg) == 48, "shmseg 偏移必须是 48");

template <typename Fn>
void load_symbol(void* handle, const char* name, Fn& out) {
  out = reinterpret_cast<Fn>(::dlsym(handle, name));  // lint-allow: L6,L14 探针自带 Xlib 最小 ABI
}

struct Xlib {
  XDisplay* (*open_display)(const char*);
  int (*close_display)(XDisplay*);
  int (*default_screen)(XDisplay*);
  XId (*root_window)(XDisplay*, int);
  void* (*default_visual)(XDisplay*, int);
  int (*default_depth)(XDisplay*, int);
  XId (*create_window)(XDisplay*, XId, int, int, unsigned int, unsigned int, unsigned int, int,
                       unsigned int, void*, unsigned long, void*);
  int (*map_window)(XDisplay*, XId);
  int (*flush)(XDisplay*);
  void* (*create_gc)(XDisplay*, XId, unsigned long, void*);
  XImage* (*create_image)(XDisplay*, void*, unsigned int, int, int, char*, unsigned int,
                          unsigned int, int, int);
  int (*put_image)(XDisplay*, XId, void*, XImage*, int, int, int, int, unsigned int, unsigned int,
                   int);
  int (*sync)(XDisplay*, int);
  int (*pending)(XDisplay*);
  int (*next_event)(XDisplay*, XEvent*);
};

struct Xext {
  int (*query_extension)(XDisplay*, int*, int*, int*);
  XImage* (*create_image)(XDisplay*, void*, unsigned int, int, char*, XShmSegmentInfo*, unsigned int,
                          unsigned int);
  int (*attach)(XDisplay*, XShmSegmentInfo*);
  int (*detach)(XDisplay*, XShmSegmentInfo*);
  int (*put_image)(XDisplay*, XId, void*, XImage*, int, int, int, int, unsigned int, unsigned int,
                   int);
  int (*get_event_base)(XDisplay*);
};

static void convert_scalar(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) dst[i] = ((src[i] >> 8U) & 0x00FFFFFFU) | 0xFF000000U;
}
static void convert_simd(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  std::size_t i = 0;
  const __m256i mask = _mm256_set1_epi32(0x00FFFFFF);
  for (; i + 8 <= count; i += 8) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));  // lint-allow: L6 SIMD
    v = _mm256_srli_epi32(v, 8);
    v = _mm256_and_si256(v, mask);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v);  // lint-allow: L6 SIMD
  }
  for (; i < count; ++i) dst[i] = (src[i] >> 8U) & 0x00FFFFFFU;
}

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

int main(int argc, char** argv) {
  int width = 1920, height = 1080, frames = 60;
  int rw = 0, rh = 0;  // 局部上屏矩形（0 = 整屏）
  bool probe_double = false;  // `--double`：加测双缓冲档
  double pace_ms = 0.0;       // `--pace`：双缓冲档每帧之间等多久（模拟真应用的帧间隔）
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rect" && i + 1 < argc) { std::sscanf(argv[++i], "%dx%d", &rw, &rh); }
    else if (arg == "--double") { probe_double = true; }
    else if (arg == "--pace" && i + 1 < argc) { pace_ms = std::atof(argv[++i]); }
    else if (width == 1920 && i == 1) { width = std::atoi(argv[i]); }
    else if (height == 1080 && i == 2) { height = std::atoi(argv[i]); }
    else if (frames == 60 && i == 3) { frames = std::atoi(argv[i]); }
  }
  if (rw <= 0) rw = width;
  if (rh <= 0) rh = height;

  void* xlib = ::dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);  // lint-allow: L14 探针运行时探测
  if (xlib == nullptr) { out("libX11.so.6 不可加载\n"); return 1; }
  void* handle = xlib;
  Xlib x{};
  load_symbol(handle, "XOpenDisplay", x.open_display); load_symbol(handle, "XCloseDisplay", x.close_display);
  load_symbol(handle, "XDefaultScreen", x.default_screen); load_symbol(handle, "XRootWindow", x.root_window);
  load_symbol(handle, "XDefaultVisual", x.default_visual); load_symbol(handle, "XDefaultDepth", x.default_depth);
  load_symbol(handle, "XCreateWindow", x.create_window); load_symbol(handle, "XMapWindow", x.map_window);
  load_symbol(handle, "XFlush", x.flush); load_symbol(handle, "XCreateGC", x.create_gc);
  load_symbol(handle, "XCreateImage", x.create_image); load_symbol(handle, "XPutImage", x.put_image); load_symbol(handle, "XSync", x.sync);
  load_symbol(handle, "XPending", x.pending); load_symbol(handle, "XNextEvent", x.next_event);
  XDisplay* display = x.open_display(nullptr);
  if (display == nullptr) { out("XOpenDisplay 失败\n"); return 1; }
  const int screen = x.default_screen(display);
  const XId root = x.root_window(display, screen);
  void* visual = x.default_visual(display, screen);
  const int depth = x.default_depth(display, screen);
  char attrs[256] = {};
  const XId window = x.create_window(display, root, 0, 0, static_cast<unsigned int>(width),
                                     static_cast<unsigned int>(height), 0, depth, 0, visual, 0, attrs);
  void* gc = x.create_gc(display, window, 0, nullptr);
  x.map_window(display, window);
  x.sync(display, 0);

  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const std::size_t rect_count = static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh);
  std::vector<std::uint32_t> source(count, 0xFF3F7FAFu);
  std::vector<std::uint32_t> dst(count, 0);

  // ① 转换成本（整屏 / 局部）
  std::vector<double> scalar_ms, simd_ms;
  for (int f = 0; f < frames; ++f) {
    auto t0 = Clock::now(); convert_scalar(source.data(), dst.data(), rect_count);
    auto t1 = Clock::now(); convert_simd(source.data(), dst.data(), rect_count);
    auto t2 = Clock::now();
    scalar_ms.push_back(ms_since(t0) - ms_since(t1));
    scalar_ms.back() = std::chrono::duration<double, std::milli>(t1 - t0).count();
    simd_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
  }
  std::sort(scalar_ms.begin(), scalar_ms.end());
  std::sort(simd_ms.begin(), simd_ms.end());

  // ② XPutImage：整屏 / 局部（端到端吞吐 + 客户端提交）
  auto measure_put = [&](int w, int h, double& submit_ms, double& throughput_ms) {
    XImage* image = x.create_image(display, visual, static_cast<unsigned int>(depth), 2,
                                   0, reinterpret_cast<char*>(dst.data()),  // lint-allow: L6 像素视图
                                   static_cast<unsigned int>(width), height, 32, 0);
    if (image == nullptr) return false;
    std::vector<double> subs;
    auto wall0 = Clock::now();
    for (int f = 0; f < frames; ++f) {
      auto t0 = Clock::now();
      x.put_image(display, window, gc, image, 0, 0, 0, 0, static_cast<unsigned int>(w),
                  static_cast<unsigned int>(h), 0);
      x.flush(display);
      subs.push_back(ms_since(t0));
    }
    x.sync(display, 0);
    const double wall = ms_since(wall0);
    std::sort(subs.begin(), subs.end());
    submit_ms = subs[subs.size() / 2U];
    throughput_ms = wall / static_cast<double>(frames);
    image->data = nullptr;
    image->f.destroy_image = nullptr;
    return true;
  };

  double put_rect_submit = 0, put_rect_tp = 0, put_full_submit = 0, put_full_tp = 0;
  if (rw != width || rh != height) measure_put(rw, rh, put_rect_submit, put_rect_tp);
  measure_put(width, height, put_full_submit, put_full_tp);

  // ③ XShmPutImage：整屏 / 局部
  double shm_rect_submit = -1, shm_rect_tp = -1, shm_full_submit = -1, shm_full_tp = -1;
  double shm_full_sync_submit = -1, shm_full_sync_tp = -1;
  bool shm_ok = false;
  double shm_double_submit = -1, shm_double_tp = -1;
  double shm_double_completions = 0, shm_double_sync_frames = 0;
  bool shm_double_ok = false;
  {
    void* xext = ::dlopen("libXext.so.6", RTLD_LAZY | RTLD_LOCAL);  // lint-allow: L14 探针运行时探测
    if (xext != nullptr) {
      Xext ext{};
      handle = xext;
      load_symbol(handle, "XShmQueryExtension", ext.query_extension); load_symbol(handle, "XShmCreateImage", ext.create_image);
      load_symbol(handle, "XShmAttach", ext.attach); load_symbol(handle, "XShmDetach", ext.detach);
      load_symbol(handle, "XShmPutImage", ext.put_image);
      load_symbol(handle, "XShmGetEventBase", ext.get_event_base);
      handle = xlib;
      int a = 0, b = 0, c = 0;
      if (ext.query_extension != nullptr && ext.query_extension(display, &a, &b, &c) != 0) {
        const std::size_t bytes = count * 4U;
        const int shmid = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (shmid >= 0) {
          XShmSegmentInfo shm{};
          shm.shmid = shmid;
          shm.shmaddr = static_cast<char*>(::shmat(shmid, nullptr, 0));
          shm.read_only = 0;
          XImage* image = ext.create_image(display, visual, static_cast<unsigned int>(depth), 2,
                                           shm.shmaddr, &shm, static_cast<unsigned int>(width),
                                           static_cast<unsigned int>(height));
          if (image != nullptr && ext.attach(display, &shm) != 0) {
            x.sync(display, 0);
            auto bench = [&](int w, int h, double& submit, double& tp) {
              std::vector<double> subs;
              auto wall0 = Clock::now();
              for (int f = 0; f < frames; ++f) {
                std::memcpy(shm.shmaddr, dst.data(), count * 4U);
                auto t0 = Clock::now();
                ext.put_image(display, window, gc, image, 0, 0, 0, 0, static_cast<unsigned int>(w),
                              static_cast<unsigned int>(h), 0);
                x.flush(display);
                subs.push_back(ms_since(t0));
              }
              x.sync(display, 0);
              const double wall = ms_since(wall0);
              std::sort(subs.begin(), subs.end());
              submit = subs[subs.size() / 2U];
              tp = wall / static_cast<double>(frames);
            };
            if (rw != width || rh != height) bench(rw, rh, shm_rect_submit, shm_rect_tp);
            bench(width, height, shm_full_submit, shm_full_tp);
            // 同步档：每帧 XSync（单缓冲必须如此——服务端直接读 shmaddr，
            // 不等一次往返就覆写 = 读到半新半旧的画面）。量它就是为了回答
            // 「为了防撕裂得付多少代价」。
            {
              std::vector<double> subs;
              auto wall0 = Clock::now();
              for (int f = 0; f < frames; ++f) {
                std::memcpy(shm.shmaddr, dst.data(), count * 4U);
                auto t0 = Clock::now();
                ext.put_image(display, window, gc, image, 0, 0, 0, 0,
                              static_cast<unsigned int>(width),
                              static_cast<unsigned int>(height), 0);
                x.flush(display);
                x.sync(display, 0);
                subs.push_back(ms_since(t0));
              }
              const double wall = ms_since(wall0);
              std::sort(subs.begin(), subs.end());
              shm_full_sync_submit = subs[subs.size() / 2U];
              shm_full_sync_tp = wall / static_cast<double>(frames);
            }
            ext.detach(display, &shm);  // lint-allow: L9 探针直接调 X 扩展（无并发语义）
            shm_ok = true;
          }
          ::shmdt(shm.shmaddr);
          ::shmctl(shmid, IPC_RMID, nullptr);
        }

        // ④ 双缓冲档（`--double`）：两块段交替 + `XShmCompletionEvent` 判块可用。
        //
        // 与后端实现同一手法：每帧 `send_event=1`；选槽前先收已到达的完成事件**不 sync**，
        // 只在轮转回一块仍在途的段时才 `XSync` 一次。量出来的提交耗时就是 gallery 里
        // `present_ms` 那句的同一口径（不含转换）。
        if (probe_double && ext.get_event_base != nullptr) {
          const int event_base = ext.get_event_base(display);
          struct Slot { XShmSegmentInfo seg{}; XImage* image{nullptr}; };
          std::array<Slot, 2> slots{};
          int built = 0;
          for (auto& slot : slots) {
            const int sid = ::shmget(IPC_PRIVATE, count * 4U, IPC_CREAT | 0600);
            if (sid < 0) break;
            slot.seg.shmid = sid;
            slot.seg.shmaddr = static_cast<char*>(::shmat(sid, nullptr, 0));
            if (slot.seg.shmaddr == nullptr) {
              ::shmctl(sid, IPC_RMID, nullptr);
              break;
            }
            slot.image = ext.create_image(display, visual, static_cast<unsigned int>(depth), 2,
                                          slot.seg.shmaddr, &slot.seg,
                                          static_cast<unsigned int>(width),
                                          static_cast<unsigned int>(height));
            if (slot.image == nullptr || ext.attach(display, &slot.seg) == 0) break;
            ++built;
          }
          if (built == 2) {
            x.sync(display, 0);
            bool in_flight[2] = {false, false};
            int active = -1;
            int in_flight_count = 0;
            std::uint64_t completions = 0;
            int synced = 0;  // 发了一次 XSync 的帧数（就是双缓冲没省下的那些帧）
            std::vector<double> subs;
            auto wall0 = Clock::now();
            for (int f = 0; f < frames; ++f) {
              // 先收已到达的完成事件（与后端的事件泵同一动作）
              auto drain = [&] {
                while (x.pending(display) > 0) {
                  XEvent ev{};
                  x.next_event(display, &ev);
                  if (ev.type != event_base) continue;
                  const XId seg = reinterpret_cast<const XShmCompletionEvent*>(&ev)->shmseg;  // lint-allow: L6 探针读事件字段
                  ++completions;
                  for (int i = 0; i < 2; ++i) {
                    if (slots[static_cast<std::size_t>(i)].seg.shmseg == seg && in_flight[i]) {
                      in_flight[i] = false;
                      --in_flight_count;
                    }
                  }
                }
              };
              drain();
              int pick = (active + 1) % 2;
              if (in_flight[pick]) {
                pick = in_flight[0] ? (in_flight[1] ? -1 : 1) : 0;
              }
              auto t0 = Clock::now();
              if (pick < 0) {
                // 两块都在途：只能让服务端追上（与后端同）
                ++synced;
                x.sync(display, 0);
                drain();
                in_flight[0] = in_flight[1] = false;
                in_flight_count = 0;
                pick = (active + 1) % 2;
              }
              auto& slot = slots[static_cast<std::size_t>(pick)];
              std::memcpy(slot.seg.shmaddr, dst.data(), count * 4U);
              ext.put_image(display, window, gc, slot.image, 0, 0, 0, 0,
                            static_cast<unsigned int>(width),
                            static_cast<unsigned int>(height), 1);
              x.flush(display);
              in_flight[pick] = true;
              ++in_flight_count;
              active = pick;
              subs.push_back(ms_since(t0));
              if (pace_ms > 0.0) {
                // 模拟真应用的帧间隔（gallery 全量重绘 11~18 ms/帧）。
                // 为什么需要它：本探针其它档是**满速**压测，服务端跟不上时两块段都会在途，
                // 双缓冲就退化成“每两帧排空一次”；而真实帧间隔里完成事件早就回来了。
                // 两种工况都给数，才不会把“满速下的收益”当成“实际情况下的收益”。
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(pace_ms));
              }
            }
            x.sync(display, 0);
            const double wall = ms_since(wall0);
            std::sort(subs.begin(), subs.end());
            shm_double_submit = subs[subs.size() / 2U];
            shm_double_tp = wall / static_cast<double>(frames);
            shm_double_completions = static_cast<double>(completions);
            shm_double_sync_frames = static_cast<double>(synced);
            shm_double_ok = true;
          }
          for (auto& slot : slots) {
            if (slot.image != nullptr && slot.seg.shmseg != 0) {
              ext.detach(display, &slot.seg);  // lint-allow: L9 探针直接调 X 扩展
            }
            if (slot.seg.shmaddr != nullptr) ::shmdt(slot.seg.shmaddr);
            if (slot.seg.shmid >= 0) ::shmctl(slot.seg.shmid, IPC_RMID, nullptr);
          }
        }
      }
    }
  }

  out("画布 {}x{}（{:.2f} Mpx）；上屏区域 {}x{}；帧数 {}；DISPLAY={}\n", width, height,
            static_cast<double>(count) / 1e6, rw, rh, frames,
            ::getenv("DISPLAY") != nullptr ? ::getenv("DISPLAY") : "(none)");
  out("  像素转换(局部区)  标量 {:.3f} ms · SIMD {:.3f} ms（{:.2f}x）\n",
            scalar_ms[scalar_ms.size() / 2U], simd_ms[simd_ms.size() / 2U],
            scalar_ms[scalar_ms.size() / 2U] / std::max(1e-6, simd_ms[simd_ms.size() / 2U]));
  out("  XPutImage  整屏   提交 {:.2f} ms · 端到端 {:.2f} ms/帧\n", put_full_submit,
            put_full_tp);
  if (rw != width || rh != height)
    out("  XPutImage  局部   提交 {:.2f} ms · 端到端 {:.2f} ms/帧\n", put_rect_submit,
              put_rect_tp);
  if (shm_ok) {
    out("  XShmPutImage 整屏 提交 {:.3f} ms · 端到端 {:.2f} ms/帧（{:.1f}x）\n", shm_full_submit,
              shm_full_tp, put_full_tp / std::max(1e-6, shm_full_tp));
    out("  XShmPutImage 整屏 每帧 XSync 提交 {:.3f} ms · 端到端 {:.2f} ms/帧（{:.1f}x）\n",
              shm_full_sync_submit, shm_full_sync_tp,
              put_full_tp / std::max(1e-6, shm_full_sync_tp));
    if (rw != width || rh != height)
      out("  XShmPutImage 局部 提交 {:.3f} ms · 端到端 {:.2f} ms/帧（{:.1f}x）\n",
                shm_rect_submit, shm_rect_tp, put_rect_tp / std::max(1e-6, shm_rect_tp));
    if (shm_double_ok) {
      out("  XShm 双段+完成事件 提交 {:.3f} ms · 端到端 {:.2f} ms/帧（{:.1f}x 单段同步档）"
          " · 完成事件 {} 个 · 排空 {} 帧\n",
          shm_double_submit, shm_double_tp, shm_full_sync_tp / std::max(1e-6, shm_double_tp),
          shm_double_completions, shm_double_sync_frames);
    } else if (probe_double) {
      out("  XShm 双段+完成事件 : 不可用（完成事件基址取不到或两块段建不起来）\n");
    }
  } else {
    out("  XShmPutImage      : 不可用\n");
  }
  x.close_display(display);
  return 0;
}
