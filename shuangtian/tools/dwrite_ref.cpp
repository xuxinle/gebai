// dwrite_ref.cpp —— DirectWrite 参照生成器（像素级对标专用，不进框架构建）。
//
// 产出与霜天探针同内容、同字号、同布局的 DWrite 渲染图（PPM P6，PIL 可直接读）：
// Windows 桌面现代应用（Chrome/Edge/VS Code/Win11 XAML）字体渲染的原生真源。
//   natural : NATURAL 测量 + ClearType 亚像素（Chrome/Edge 正文路径）
//   gdi     : GDI_CLASSIC 测量 + ClearType（传统整像素对齐）
//   gray    : NATURAL 测量 + 灰度抗锯齿
// 渲染参数（gamma/增强对比/ClearType 强度/像素几何）取**系统当前调校**
// （CreateRenderingParams —— 用户 ClearType 设置，与真实桌面一致）。
//
// 用法：dwrite_ref.exe <rows.tsv> <outdir> [--scale 1.5] [--mode all|natural|gdi|gray]
//                       [--theme light|dark] [--w 640] [--h 504.6]
// TSV 列：label\tsize\ttop\tmono\tbold\ttext（UTF-8，由 tools/gen_dw_ab.py 生成）
#define INITGUID
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Row {
  std::string label;
  std::wstring text;
  float size = 15.0f;
  float top = 0.0f;
  bool mono = false;
  bool bold = false;
};

static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}

static bool parse_tsv(const char* path, std::vector<Row>* rows) {
  FILE* f = nullptr;
  if (::fopen_s(&f, path, "rb") != 0 || f == nullptr) {
    std::printf("cannot open rows: %s\n", path);
    return false;
  }
  std::string buf;
  char chunk[4096];
  size_t n = 0;
  while ((n = ::fread(chunk, 1, sizeof chunk, f)) > 0) buf.append(chunk, n);
  ::fclose(f);
  size_t start = 0;
  while (start < buf.size()) {
    size_t e = buf.find('\n', start);
    if (e == std::string::npos) e = buf.size();
    std::string line = buf.substr(start, e - start);
    start = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t t[5];
    int ti = 0;
    size_t p = 0;
    while (ti < 5) {
      const size_t nt = line.find('\t', p);
      if (nt == std::string::npos) break;
      t[ti++] = nt;
      p = nt + 1;
    }
    if (ti < 5) continue;
    Row r;
    r.label = line.substr(0, t[0]);
    r.size = (float)::atof(line.substr(t[0] + 1, t[1] - t[0] - 1).c_str());
    r.top = (float)::atof(line.substr(t[1] + 1, t[2] - t[1] - 1).c_str());
    r.mono = line.substr(t[2] + 1, t[3] - t[2] - 1) == "1";
    r.bold = line.substr(t[3] + 1, t[4] - t[3] - 1) == "1";
    r.text = utf8_to_wide(line.substr(t[4] + 1));
    rows->push_back(r);
  }
  return !rows->empty();
}

// PPM P6：头 "P6\n<w> <h>\n255\n" + RGB 逐行。零依赖、零压缩、无 CRC。
static bool write_ppm(const char* path, const unsigned char* rgb, int w, int h) {
  FILE* f = nullptr;
  if (::fopen_s(&f, path, "wb") != 0 || f == nullptr) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  const size_t wrote = ::fwrite(rgb, 1, (size_t)w * h * 3, f);
  ::fclose(f);
  return wrote == (size_t)w * h * 3;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: dwrite_ref <rows.tsv> <outdir> [--scale 1.5] [--mode all] "
                "[--theme light] [--w 640] [--h 504.6]\n");
    return 1;
  }
  const char* rows_path = argv[1];
  const char* out_dir = argv[2];
  float scale = 1.5f;
  const char* mode = "all";
  const char* theme = "light";
  float logical_w = 640.0f;
  float logical_h = 504.6f;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--scale" && i + 1 < argc) scale = (float)::atof(argv[++i]);
    else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    else if (a == "--theme" && i + 1 < argc) theme = argv[++i];
    else if (a == "--w" && i + 1 < argc) logical_w = (float)::atof(argv[++i]);
    else if (a == "--h" && i + 1 < argc) logical_h = (float)::atof(argv[++i]);
  }
  std::vector<Row> rows;
  if (!parse_tsv(rows_path, &rows)) return 1;
  const bool dark = std::strcmp(theme, "dark") == 0;
  std::printf("rows=%zu scale=%.3f mode=%s theme=%s\n", rows.size(), scale, mode, theme);

  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::printf("CoInitializeEx failed\n");
    return 1;
  }
  int exit_code = 0;
  ID2D1Factory* d2d = nullptr;
  IDWriteFactory* dw = nullptr;
  do {
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory,
                                   nullptr, (void**)&d2d)) || d2d == nullptr) {
      std::printf("D2D1CreateFactory failed\n");
      exit_code = 1;
      break;
    }
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_IDWriteFactory,
                                     (IUnknown**)&dw)) || dw == nullptr) {
      std::printf("DWriteCreateFactory failed\n");
      exit_code = 1;
      break;
    }
    // 系统调校的渲染参数（用户 ClearType 设置的 gamma/对比度/几何）
    IDWriteRenderingParams* sys_params = nullptr;
    if (FAILED(dw->CreateRenderingParams(&sys_params)) || sys_params == nullptr) {
      std::printf("CreateRenderingParams failed\n");
      exit_code = 1;
      break;
    }
    float sys_gamma = sys_params->GetGamma();
    float sys_contrast = sys_params->GetEnhancedContrast();
    float sys_ct_level = sys_params->GetClearTypeLevel();
    const DWRITE_PIXEL_GEOMETRY sys_geometry = sys_params->GetPixelGeometry();
    // 可选覆盖：与霜天侧同组旋钮，用于回答"调校空间还剩多少"
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--gamma" && i + 1 < argc) sys_gamma = (float)::atof(argv[++i]);
      else if (a == "--contrast" && i + 1 < argc) sys_contrast = (float)::atof(argv[++i]);
      else if (a == "--ct" && i + 1 < argc) sys_ct_level = (float)::atof(argv[++i]);
    }
    std::printf("render params: gamma=%.3f contrast=%.3f ct=%.3f geom=%d\n",
                (double)sys_gamma, (double)sys_contrast, (double)sys_ct_level,
                (int)sys_geometry);

    struct ModeCfg {
      const char* name;
      DWRITE_RENDERING_MODE rm;        // 渲染模式（决定栅格化路径）
      D2D1_TEXT_ANTIALIAS_MODE aa;     // ClearType 亚像素 vs 灰度（设在 D2D 目标上）
    };
    std::vector<ModeCfg> cfgs;
    if (std::strcmp(mode, "all") == 0 || std::strcmp(mode, "natural") == 0)
      cfgs.push_back({"natural", DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                      D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE});
    if (std::strcmp(mode, "all") == 0 || std::strcmp(mode, "gdi") == 0)
      cfgs.push_back({"gdi", DWRITE_RENDERING_MODE_GDI_CLASSIC,
                      D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE});
    if (std::strcmp(mode, "all") == 0 || std::strcmp(mode, "gray") == 0)
      cfgs.push_back({"gray", DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                      D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE});

    const int phys_w = (int)(logical_w * scale + 0.5f);
    const int phys_h = (int)(logical_h * scale + 0.5f);

    for (const ModeCfg& cfg : cfgs) {
      // 软件渲染目标：内存 DC + DIB section（32bpp top-down），DPI = 96*scale。
      ID2D1DCRenderTarget* dcrt = nullptr;
      D2D1_RENDER_TARGET_PROPERTIES props;
      props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
      props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
      props.dpiX = 96.0f * scale;
      props.dpiY = 96.0f * scale;
      props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
      props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
      if (FAILED(d2d->CreateDCRenderTarget(&props, &dcrt)) || dcrt == nullptr) {
        std::printf("CreateDCRenderTarget failed\n");
        exit_code = 1;
        break;
      }
      BITMAPINFO bi;
      ZeroMemory(&bi, sizeof bi);
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = phys_w;
      bi.bmiHeader.biHeight = -phys_h;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      HDC screen_dc = ::GetDC(nullptr);
      HBITMAP dib = ::CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      ::ReleaseDC(nullptr, screen_dc);
      if (dib == nullptr || bits == nullptr) {
        std::printf("CreateDIBSection failed\n");
        dcrt->Release();
        exit_code = 1;
        break;
      }
      HDC mem_dc = ::CreateCompatibleDC(nullptr);
      const HGDIOBJ old_obj = ::SelectObject(mem_dc, dib);
      RECT rc = {0, 0, phys_w, phys_h};
      if (FAILED(dcrt->BindDC(mem_dc, &rc))) {
        std::printf("BindDC failed\n");
        exit_code = 1;
      } else {
        // 渲染参数：系统调校值 + 指定渲染模式；AA 模式设在目标上
        IDWriteRenderingParams* my_params = nullptr;
        if (SUCCEEDED(dw->CreateCustomRenderingParams(
                sys_gamma, sys_contrast, sys_ct_level, sys_geometry, cfg.rm,
                &my_params)) && my_params != nullptr) {
          dcrt->SetTextRenderingParams(my_params);
          dcrt->SetTextAntialiasMode(cfg.aa);
          my_params->Release();
        } else {
          std::printf("CreateCustomRenderingParams failed (fallback system params)\n");
        }
        dcrt->BeginDraw();
        // 手工构造颜色（不依赖 d2d1helper）
        D2D1_COLOR_F bg_c;
        bg_c.r = dark ? 0.055f : 1.0f;
        bg_c.g = dark ? 0.067f : 1.0f;
        bg_c.b = dark ? 0.082f : 1.0f;
        bg_c.a = 1.0f;
        dcrt->Clear(bg_c);
        ID2D1SolidColorBrush* brush = nullptr;
        D2D1_COLOR_F fg_c;
        fg_c.r = dark ? 0.910f : 0.059f;
        fg_c.g = dark ? 0.933f : 0.090f;
        fg_c.b = dark ? 0.976f : 0.165f;
        fg_c.a = 1.0f;
        dcrt->CreateSolidColorBrush(fg_c, &brush);
        if (brush != nullptr) {
          for (const Row& r : rows) {
            // 字号单位：DIP（逻辑 px）。mono → Consolas，其余 → 微软雅黑（与霜天字体链一致）
            IDWriteTextFormat* fmt = nullptr;
            const wchar_t* family = r.mono ? L"Consolas" : L"Microsoft YaHei";
            const DWRITE_FONT_WEIGHT weight =
                r.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
            if (FAILED(dw->CreateTextFormat(family, nullptr, weight,
                                            DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, r.size,
                                            L"zh-CN", &fmt)) || fmt == nullptr) {
              continue;
            }
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(dw->CreateTextLayout(r.text.c_str(), (UINT32)r.text.size(),
                                               fmt, logical_w - 8.0f, r.size * 2.0f,
                                               &layout)) && layout != nullptr) {
              const D2D1_POINT_2F origin = {4.0f, r.top};
              dcrt->DrawTextLayout(origin, layout, brush,
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
              layout->Release();
            }
            fmt->Release();
          }
          brush->Release();
        }
        const HRESULT ended = dcrt->EndDraw();
        if (ended != S_OK) std::printf("EndDraw hr=0x%08lX\n", (unsigned long)ended);
        // GDI 透明位图双写问题：D2D ClearType 写入的是预乘 alpha，但 DC 目标 alpha=IGNORE，
        // RGB 已是最终色。直接按 BGRA→RGB 读出。
        std::vector<unsigned char> rgb((size_t)phys_w * phys_h * 3);
        const unsigned char* src = (const unsigned char*)bits;
        for (int y = 0; y < phys_h; ++y) {
          const unsigned char* srow = src + (size_t)y * phys_w * 4;
          unsigned char* drow = &rgb[(size_t)y * phys_w * 3];
          for (int x = 0; x < phys_w; ++x) {
            drow[x * 3 + 0] = srow[x * 4 + 2];  // B→R
            drow[x * 3 + 1] = srow[x * 4 + 1];  // G
            drow[x * 3 + 2] = srow[x * 4 + 0];  // R→B
          }
        }
        char path[512];
        sprintf_s(path, sizeof path, "%s/dw-%s-%s.ppm", out_dir, cfg.name, theme);
        if (!write_ppm(path, rgb.data(), phys_w, phys_h)) {
          std::printf("write failed: %s\n", path);
          exit_code = 1;
        } else {
          std::printf("wrote %s (%dx%d)\n", path, phys_w, phys_h);
        }
      }
      ::SelectObject(mem_dc, old_obj);
      ::DeleteDC(mem_dc);
      ::DeleteObject(dib);
      dcrt->Release();
    }
    sys_params->Release();
  } while (false);
  if (dw != nullptr) dw->Release();
  if (d2d != nullptr) d2d->Release();
  ::CoUninitialize();
  return exit_code;
}
