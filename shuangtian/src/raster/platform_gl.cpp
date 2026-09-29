/// OpenGL 3D 实现（Windows：WGL + 离屏 FBO；其他平台如实报 Unsupported）。
///
/// 本文件是**平台层**：系统头（windows.h/GL）只允许出现在 `platform_*` 里，
/// 业务代码只通过 `gl.hpp` 的接口使用（见 `docs/cross_platform.md`）。

#include "st/raster/gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"

#if defined(_WIN32) && __has_include("opengl/gl.h")
#define ST_HAS_OPENGL 1
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(ST_HAS_OPENGL)
#define GLAD_GL_IMPLEMENTATION
#include "opengl/gl.h"
#endif

namespace st::raster::gl {

#if !defined(ST_HAS_OPENGL)

// ——— 没有 OpenGL 源码（未拉取）或非 Windows ———
// 关键：**编译期整块排除**，接口照常存在并如实报不可用。
// 这样"缺源码"不会变成"整个框架编不过"——那会把一个可选能力变成硬依赖。
auto has_opengl() noexcept -> bool { return false; }
auto available() noexcept -> bool { return false; }
auto probe() -> Result<DeviceInfo> {
  return unexpected(ErrorCode::Unsupported,
                    "本机没有 OpenGL 加载器源码：运行 tools/fetch_opengl.ps1（或 .sh）拉取到 "
                    "third_party/opengl/（该目录不进版本库）");
}
auto Mesh::cube(float) -> Mesh { return {}; }
auto Mesh::sphere(float, int) -> Mesh { return {}; }
auto Mesh::box(float, float, float) -> Mesh { return {}; }
auto Scene3D::create(int, int) -> Result<std::unique_ptr<Scene3D>> {
  return unexpected(ErrorCode::Unsupported, "OpenGL 3D 需要 OpenGL 加载器与 WGL 上下文");
}

#else

namespace {

/// 隐藏窗口 + WGL 上下文（进程级共享）。
///
/// 为什么要一个窗口：WGL 的入口是**设备上下文（HDC）**，而 HDC 只能从窗口取。
/// 这个窗口从不显示（`WS_POPUP` 且不 `ShowWindow`），纯粹作为上下文载体；
/// 渲染全部发生在 FBO 上，因此**无头环境同样可用**（与 D3D11 离屏路径一致）。
struct Context {
  HMODULE opengl32{nullptr};
  /// wgl* 入口点**动态解析**（不链接 opengl32.lib）：
  /// 与 D3D11 后端同一口径——运行时缺 DLL 只是"这个后端不可用"，
  /// 而不是链接期失败（那会让没装 GL 驱动的机器连框架都编不出来）。
  HGLRC(WINAPI* create_context)(HDC){nullptr};
  BOOL(WINAPI* make_current)(HDC, HGLRC){nullptr};
  BOOL(WINAPI* delete_context)(HGLRC){nullptr};
  PROC(WINAPI* get_proc_address)(LPCSTR){nullptr};
  HWND window{nullptr};
  HDC dc{nullptr};
  HGLRC rc{nullptr};
  bool ok{false};
  std::string error{};
};

/// 统一入口点解析（定义见下）：含 `wglGetProcAddress` → `opengl32` 导出表的回退。
[[nodiscard]] auto gl_proc(const Context& ctx, const char* name) -> void*;
/// 初始化期间"正在构造的上下文"（见加载回调处的说明：避免静态初始化重入死锁）。
Context* g_initializing{nullptr};

auto gl_context() -> Context& {
  static Context instance = [] {
    Context ctx;
    ctx.opengl32 = ::LoadLibraryW(L"opengl32.dll");
    if (ctx.opengl32 == nullptr) {
      ctx.error = "找不到 opengl32.dll";
      return ctx;
    }
    const auto resolve = [&ctx](const char* name) -> void* {
      const auto symbol = ::GetProcAddress(ctx.opengl32, name);
      return reinterpret_cast<void*>(symbol);
    };
    ctx.create_context = reinterpret_cast<HGLRC(WINAPI*)(HDC)>(resolve("wglCreateContext"));
    ctx.make_current = reinterpret_cast<BOOL(WINAPI*)(HDC, HGLRC)>(resolve("wglMakeCurrent"));
    ctx.delete_context = reinterpret_cast<BOOL(WINAPI*)(HGLRC)>(resolve("wglDeleteContext"));
    ctx.get_proc_address = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(resolve("wglGetProcAddress"));
    if (ctx.create_context == nullptr || ctx.make_current == nullptr ||
        ctx.get_proc_address == nullptr) {
      ctx.error = "opengl32.dll 缺少 wgl 入口点";
      return ctx;
    }
    // 只注册一次窗口类；重复注册会失败但无害（同一进程内）
    static const wchar_t* kClass = L"ShuangtianGlOffscreen";
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    (void)::RegisterClassW(&wc);
    ctx.window = ::CreateWindowExW(0, kClass, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
    if (ctx.window == nullptr) {
      ctx.error = std::format("创建离屏窗口失败（错误码 {}）", ::GetLastError());
      return ctx;
    }
    ctx.dc = ::GetDC(ctx.window);
    if (ctx.dc == nullptr) {
      ctx.error = "取窗口 DC 失败";
      return ctx;
    }
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int format = ::ChoosePixelFormat(ctx.dc, &pfd);
    if (format == 0 || ::SetPixelFormat(ctx.dc, format, &pfd) == 0) {
      ctx.error = "选择/设置像素格式失败";
      return ctx;
    }
    ctx.rc = ctx.create_context(ctx.dc);
    if (ctx.rc == nullptr) {
      ctx.error = "wglCreateContext 失败（无 OpenGL 驱动？）";
      return ctx;
    }
    if (ctx.make_current(ctx.dc, ctx.rc) == 0) {
      ctx.error = "wglMakeCurrent 失败";
      return ctx;
    }
    // 加载器用 wglGetProcAddress 取现代入口点（1.1 之外的函数都不在 opengl32.dll 的导出表里）
    // glad 的加载回调：统一走 `gl_proc`（含 opengl32 回退）。
    // 传 `wglGetProcAddress` 直接进去是错的——GL 1.1 的函数一个都取不到。
    // 加载回调处于初始化过程中：把"正在构造的对象"放进文件级指针，
    // 回调经由它取入口点——**不要**在回调里再取 `gl_context()`（那是重入，会死锁）。
    g_initializing = &ctx;
    const int version = gladLoadGL(+[](const char* name) -> GLADapiproc {
      Context* target = g_initializing;
      if (target == nullptr) return nullptr;
      return reinterpret_cast<GLADapiproc>(gl_proc(*target, name));
    });
    g_initializing = nullptr;
    if (version == 0) {
      ctx.error = "gladLoadGL 失败：驱动未提供可用的 OpenGL 入口点";
      return ctx;
    }
    ctx.ok = true;
    return ctx;
  }();
  return instance;
}

/// 取一个 GL 入口点。
///
/// Windows 上这是**两步**，而第二步极易漏：
/// 1. `wglGetProcAddress` 只能取到 **1.1 之后**的函数；
/// 2. `glGetString`/`glClear`/`glEnable` 这类 1.1 函数**必须**从 opengl32.dll 的导出表取。
/// 只做第 1 步的症状是"加载器返回失败"或"部分函数指针为空"——看起来像驱动有问题。
/// 另外 `wglGetProcAddress` 失败时会返回 1/2/3/-1 这些**哨兵值**（不是 nullptr），
/// 必须把它们当失败处理，否则会拿到野指针并在调用时崩。
/// ⚠ 参数是 `Context&` 而不是内部去取 `gl_context()`：
/// 本函数会在 `gl_context()` 的**静态初始化过程中**被调用（加载器回调），
/// 那时再调 `gl_context()` 就是重入静态初始化——MSVC 下直接**死锁**（不是报错）。
/// 这是实测踩到的：测试卡住不返回，看栈才知道。
[[nodiscard]] auto gl_proc(const Context& ctx, const char* name) -> void* {
  if (ctx.get_proc_address != nullptr) {
    void* symbol = reinterpret_cast<void*>(ctx.get_proc_address(name));
    if (symbol != nullptr && symbol != reinterpret_cast<void*>(1) &&
        symbol != reinterpret_cast<void*>(2) && symbol != reinterpret_cast<void*>(3) &&
        symbol != reinterpret_cast<void*>(-1)) {
      return symbol;
    }
  }
  if (ctx.opengl32 != nullptr) {
    return reinterpret_cast<void*>(::GetProcAddress(ctx.opengl32, name));
  }
  return nullptr;
}

/// 顶点着色器：MVP + 模型矩阵（法线用模型矩阵的左上 3×3 变换）。
constexpr const char* kVertexShader = R"glsl(#version 330 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 v_normal;
out vec3 v_color;
void main() {
  v_normal = mat3(u_model) * in_normal;
  v_color = in_color;
  gl_Position = u_mvp * vec4(in_position, 1.0);
}
)glsl";

/// 片元着色器：一个方向光 + 环境项 + 轻微边缘光。
///
/// 为什么自己算光照而不是只画纯色：三维场景没有明暗就没有**体积感**——
/// 立方体的相邻面会同色、看起来像一个扁平多边形。光照是"看起来像 3D"的最小代价。
constexpr const char* kFragmentShader = R"glsl(#version 330 core
in vec3 v_normal;
in vec3 v_color;
out vec4 out_color;
void main() {
  vec3 n = normalize(v_normal);
  vec3 light_dir = normalize(vec3(0.4, 0.8, 0.6));
  float diffuse = max(dot(n, light_dir), 0.0);
  float ambient = 0.35;
  // 边缘光（rim）：让轮廓在暗面上也能读出来
  float rim = pow(1.0 - max(n.z, 0.0), 2.0) * 0.25;
  vec3 lit = v_color * (ambient + diffuse * 0.75) + vec3(rim);
  out_color = vec4(clamp(lit, 0.0, 1.0), 1.0);
}
)glsl";

[[nodiscard]] auto compile_shader(GLenum type, const char* source) -> Result<GLuint> {
  const GLuint shader = glCreateShader(type);
  if (shader == 0) return unexpected(ErrorCode::Io, "glCreateShader 失败");
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    return unexpected(ErrorCode::Invalid,
                      std::format("着色器编译失败：{}", log.c_str()));
  }
  return shader;
}

[[nodiscard]] auto link_program() -> Result<GLuint> {
  auto vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
  if (!vertex) return forward_error(vertex.error());
  auto fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!fragment) {
    glDeleteShader(*vertex);
    return forward_error(fragment.error());
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, *vertex);
  glAttachShader(program, *fragment);
  glLinkProgram(program);
  // 链接后立刻可删（程序对象已持有它们的副本）
  glDeleteShader(*vertex);
  glDeleteShader(*fragment);
  GLint status = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    return unexpected(ErrorCode::Invalid, std::format("着色器链接失败：{}", log.c_str()));
  }
  return program;
}

/// 已上传的网格（按顶点/索引数据地址做键：`Mesh` 常在调用方长期持有）。
struct UploadedMesh {
  const void* key{nullptr};
  std::size_t vertex_count{0};
  std::size_t index_count{0};
  GLuint vao{0};
  GLuint vbo{0};
  GLuint ibo{0};
};

class GlScene final : public Scene3D {
 public:
  GlScene(int width, int height) : width_(width), height_(height) {}
  /// 进程退出时不主动销毁上下文：`gl_context()` 是惰性静态对象，
  /// 销毁顺序与静态析构不确定，提前删上下文会让其它场景的析构调 GL 崩掉。
  ~GlScene() override {
    if (!gl_context().ok) return;
    for (auto& mesh : meshes_) {
      glDeleteBuffers(1, &mesh.vbo);
      glDeleteBuffers(1, &mesh.ibo);
      glDeleteVertexArrays(1, &mesh.vao);
    }
    if (depth_ != 0) glDeleteRenderbuffers(1, &depth_);
    if (color_ != 0) glDeleteTextures(1, &color_);
    if (fbo_ != 0) glDeleteFramebuffers(1, &fbo_);
    if (program_ != 0) glDeleteProgram(program_);
    if (pixels_.size() > 0) glDeleteBuffers(0, nullptr);  // 占位：像素在 CPU 侧
  }

  [[nodiscard]] auto initialize() -> Status {
    if (!gl_context().ok) {
      return unexpected(ErrorCode::Unsupported, std::format("OpenGL 上下文不可用：{}", gl_context().error));
    }
    auto program = link_program();
    if (!program) return forward_error(program.error());
    program_ = *program;
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      return unexpected(ErrorCode::Io, std::format("离屏帧缓冲不完整（0x{:X}）", status));
    }
    pixels_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4U, 0U);
    glEnable(GL_DEPTH_TEST);   // 深度缓冲是"真三维"与"贴图"的分界：先画的会被后画的挡住
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);   // 双面可见：模型绕序不确定时不该整面消失
    return ok();
  }

  void begin_frame(math::Color clear_color) override {
    if (!gl_context().ok) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glEnable(GL_DEPTH_TEST);
    glClearColor(static_cast<float>(clear_color.r) / 255.0f, static_cast<float>(clear_color.g) / 255.0f,
                 static_cast<float>(clear_color.b) / 255.0f,
                 static_cast<float>(clear_color.a) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_calls_ = 0;
  }

  void set_camera(const Camera& camera) override {
    const float aspect = height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_) : 1.0f;
    const float fov = camera.fov_y_degrees * 3.14159265358979f / 180.0f;
    view_ = math::Mat4::look_at(camera.eye, camera.target, camera.up);
    projection_ = math::Mat4::perspective(fov, aspect, camera.near_z, camera.far_z);
  }

  void draw_mesh(const Mesh& mesh, const math::Mat4& model) override {
    if (!gl_context().ok || mesh.is_empty() || program_ == 0) return;
    const UploadedMesh& uploaded = upload(mesh);
    if (uploaded.vao == 0) return;
    const math::Mat4 mvp = projection_ * view_ * model;
    glUseProgram(program_);
    glUniformMatrix4fv(glGetUniformLocation(program_, "u_mvp"), 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(glGetUniformLocation(program_, "u_model"), 1, GL_FALSE, model.m);
    glBindVertexArray(uploaded.vao);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(uploaded.index_count), GL_UNSIGNED_INT,
                   nullptr);
    glBindVertexArray(0);
    ++draw_calls_;
  }

  void end_frame(Surface& target, math::Rect destination, float opacity = 1.0f) override {
    if (!gl_context().ok) return;
    glFinish();
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    composite(target, destination, opacity);
  }

  [[nodiscard]] auto width() const noexcept -> int override { return width_; }
  [[nodiscard]] auto height() const noexcept -> int override { return height_; }
  [[nodiscard]] auto draw_calls() const noexcept -> std::uint64_t override { return draw_calls_; }

 private:
  auto upload(const Mesh& mesh) -> const UploadedMesh& {
    // 按数据地址缓存：`Mesh` 通常由调用方长期持有（如每帧同一个立方体）
    const void* key = mesh.vertices.data();
    for (const auto& existing : meshes_) {
      if (existing.key == key && existing.vertex_count == mesh.vertex_count()) return existing;
    }
    UploadedMesh entry;
    entry.key = key;
    entry.vertex_count = mesh.vertex_count();
    entry.index_count = mesh.indices.size();
    glGenVertexArrays(1, &entry.vao);
    glBindVertexArray(entry.vao);
    glGenBuffers(1, &entry.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, entry.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
                 mesh.vertices.data(), GL_STATIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(9 * sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(6 * sizeof(float)));
    glGenBuffers(1, &entry.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, entry.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
                 mesh.indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    meshes_.push_back(entry);
    return meshes_.back();
  }

  /// 合成：GL 的像素原点在**左下**，画布在左上——必须逐行翻转。
  ///
  /// ⚠ 这里**不能**逐像素调 `Surface::set_pixel`：
  /// `GpuCanvas::set_pixel` 每次写入都会触发一次**全屏回读**
  /// （2560×1600 的 32bpp 就是 16 MB），192k 个像素就是 192k 次全屏回读——
  /// 表现是主线程 CPU 打满、帧数不增（等价于卡死）。
  /// 正确做法是先在软件暂存画布上做一次线性转换（直接写内存），
  /// 再**一次 blit** 交给目标：GPU 目标是"上传一张纹理 + 画一个四边形"。
  void composite(Surface& target, math::Rect destination, float opacity) {
    const int dest_w = static_cast<int>(std::lround(destination.width));
    const int dest_h = static_cast<int>(std::lround(destination.height));
    if (dest_w <= 0 || dest_h <= 0) return;
    if (staging_ == nullptr || staging_->physical_width() != width_ ||
        staging_->physical_height() != height_) {
      staging_ = std::make_unique<Canvas>(width_, height_);
    }
    // 读回的像素是**直通** RGBA（GL 约定），画布内部是**预乘** 0xRRGGBBAA
    const std::span<std::uint32_t> out = staging_->pixels();  // 非 const 重载可直接写
    const auto scale = static_cast<float>(std::clamp(opacity, 0.0f, 1.0f));
    for (int y = 0; y < height_; ++y) {
      const int src_y = height_ - 1 - y;   // 翻转 Y
      for (int x = 0; x < width_; ++x) {
        const std::size_t index =
            (static_cast<std::size_t>(src_y) * static_cast<std::size_t>(width_) +
             static_cast<std::size_t>(x)) *
            4U;
        const auto alpha = static_cast<std::uint32_t>(
            static_cast<float>(pixels_[index + 3U]) * scale);
        const auto premultiply = [alpha](std::uint8_t channel) -> std::uint32_t {
          return (static_cast<std::uint32_t>(channel) * alpha + 127U) / 255U;
        };
        out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
            static_cast<std::size_t>(x)] =
            (premultiply(pixels_[index + 0U]) << 24U) | (premultiply(pixels_[index + 1U]) << 16U) |
            (premultiply(pixels_[index + 2U]) << 8U) | alpha;
      }
    }
    target.draw_canvas(*staging_, destination, DrawOptions{.opacity = 1.0f});
  }

  int width_{0};
  int height_{0};
  GLuint fbo_{0};
  GLuint color_{0};
  GLuint depth_{0};
  GLuint program_{0};
  math::Mat4 view_{math::Mat4::identity()};
  math::Mat4 projection_{math::Mat4::identity()};
  std::vector<UploadedMesh> meshes_{};
  std::vector<std::uint8_t> pixels_{};
  /// CPU 侧暂存（Y 翻转 + 直通→预乘转换）：见 `composite` 的说明。
  mutable std::unique_ptr<Canvas> staging_{};
  mutable std::uint64_t composite_calls_{0};
  std::uint64_t draw_calls_{0};
};

}  // namespace

auto has_opengl() noexcept -> bool { return true; }

auto available() noexcept -> bool { return gl_context().ok; }

auto probe() -> Result<DeviceInfo> {
  if (!gl_context().ok) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("OpenGL 上下文不可用：{}", gl_context().error));
  }
  DeviceInfo info;
  const auto read_string = [](GLenum name) -> std::string {
    const auto* value = reinterpret_cast<const char*>(glGetString(name));
    return value != nullptr ? std::string(value) : std::string{};
  };
  info.vendor = read_string(GL_VENDOR);
  info.renderer = read_string(GL_RENDERER);
  info.version = read_string(GL_VERSION);
  info.glsl = read_string(GL_SHADING_LANGUAGE_VERSION);
  GLint max_texture = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
  info.max_texture_size = static_cast<int>(max_texture);
  // 显存：NVX_gpu_memory_info 是常见 NVIDIA 扩展，但**别的驱动没有**，
  // 而加载器不一定收录它。所以按扩展串运行期判断 + 动态取入口点——
  // 硬编码宏会在非 NVIDIA 上编不过（把一个可选信息变成硬依赖）。
  if (const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
      extensions != nullptr &&
      std::string_view(extensions).find("GL_NVX_gpu_memory_info") != std::string_view::npos) {
    using GetIntegeriFn = void (*)(GLenum, GLint*);
    (void)0;  // glGetIntegerv 是 1.1 函数，直接可用（glad 已绑定）
    const auto get_integer = glGetIntegerv;
    {
      GLint vram = 0;
      get_integer(0x9047 /* GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX */, &vram);
      info.vram_mb = static_cast<long long>(vram) / 1024;
    }
  }
  return info;
}

auto Scene3D::create(int width, int height) -> Result<std::unique_ptr<Scene3D>> {
  if (width <= 0 || height <= 0) return unexpected(ErrorCode::Invalid, "渲染尺寸必须为正");
  if (!gl_context().ok) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("OpenGL 上下文不可用：{}", gl_context().error));
  }
  auto scene = std::make_unique<GlScene>(width, height);
  if (auto status = scene->initialize(); !status) return forward_error(status.error());
  return std::unique_ptr<Scene3D>(std::move(scene));
}

// ——— 网格生成 ———

namespace {

void push_vertex(std::vector<float>& out, math::Vec3 position, math::Vec3 normal,
                 math::Vec3 color) {
  out.insert(out.end(), {position.x, position.y, position.z, normal.x, normal.y, normal.z,
                         color.x, color.y, color.z});
}

}  // namespace

auto Mesh::box(float x, float y, float z) -> Mesh {
  Mesh mesh;
  const float hx = x * 0.5f;
  const float hy = y * 0.5f;
  const float hz = z * 0.5f;
  // 六面各自不同色：**一眼能看出朝向**（单色立方体转过 90° 看不出来，
  // 于是"模型矩阵对不对"这种问题会被漏掉）
  struct Face {
    math::Vec3 normal;
    math::Vec3 color;
    math::Vec3 corners[4];
  };
  const math::Vec3 faces_color[6] = {{1.0f, 0.42f, 0.38f}, {0.42f, 0.78f, 1.0f},
                                     {1.0f, 0.85f, 0.36f}, {0.52f, 0.92f, 0.58f},
                                     {0.78f, 0.55f, 1.0f}, {1.0f, 0.62f, 0.85f}};
  const math::Vec3 normals[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0},
                                 {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  const math::Vec3 quad[6][4] = {
      {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}},
      {{hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz}},
      {{hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}},
      {{-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}},
      {{-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}, {-hx, hy, -hz}},
      {{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}, {-hx, -hy, hz}},
  };
  for (int face = 0; face < 6; ++face) {
    const auto base = static_cast<std::uint32_t>(face * 4);
    for (int corner = 0; corner < 4; ++corner) {
      push_vertex(mesh.vertices, quad[face][corner], normals[face], faces_color[face]);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base + 0U, base + 1U, base + 2U, base + 0U, base + 2U, base + 3U});
  }
  return mesh;
}

auto Mesh::cube(float size) -> Mesh { return box(size, size, size); }

auto Mesh::sphere(float radius, int segments) -> Mesh {
  Mesh mesh;
  const int rings = std::max(segments / 2, 3);
  const int slices = std::max(segments, 3);
  for (int ring = 0; ring <= rings; ++ring) {
    const float v = static_cast<float>(ring) / static_cast<float>(rings);
    const float phi = v * 3.14159265358979f;         // 0..π（从北极到南极）
    for (int slice = 0; slice <= slices; ++slice) {
      const float u = static_cast<float>(slice) / static_cast<float>(slices);
      const float theta = u * 2.0f * 3.14159265358979f;
      const math::Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta)};
      // 纬度渐变上色：让球面的曲率在明暗之外还有第二重线索
      const math::Vec3 color{0.35f + 0.55f * (1.0f - v), 0.55f + 0.35f * v, 0.95f - 0.35f * v};
      push_vertex(mesh.vertices, normal * radius, normal, color);
    }
  }
  for (int ring = 0; ring < rings; ++ring) {
    for (int slice = 0; slice < slices; ++slice) {
      const auto a = static_cast<std::uint32_t>(ring * (slices + 1) + slice);
      const auto b = static_cast<std::uint32_t>(a + static_cast<std::uint32_t>(slices) + 1U);
      mesh.indices.insert(mesh.indices.end(), {a, b, a + 1U, a + 1U, b, b + 1U});
    }
  }
  return mesh;
}

#endif  // ST_HAS_OPENGL

}  // namespace st::raster::gl
