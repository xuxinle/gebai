// dw_ab_vscode_ref.cpp —— 实测 VS Code（Electron/Chromium）的字体渲染参数（仅探测用）。
//
// 为什么要实测而不是查资料：Chromium 的 DirectWrite 后端在
//   · 不同版本            （Skia 的 DW 后端改过多次）
//   · 不同 --force-color-profile / 缩放
//   · 是否开 ClearType     （Windows 的 ClearType 开关会影响 DWrite 默认）
// 下参数都不同。**参数错了，参照就错了**，复刻出来的东西必然不对。
//
// 做法：用 DWrite + D2D 渲染同一段代码文本，扫「抗锯齿模式 × gamma × 对比度」，
// 找出与 VS Code 实际像素最接近的组合。
//
// 用法：dw_ab_vscode_ref.exe <rows.tsv> <outdir> [--scale 1.5]
//   （输出一组候选 PNG，配合 Python 侧与 VS Code 截图比 RMS）
#define INITGUID
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

struct Row {
  std::string label;
  std::wstring text;
  float size = 14.0f;
  float top = 0.0f;
  bool mono = false;
  bool bold = false;
};

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}

bool parse_tsv(const char* path, std::vector<Row>* rows) {
  FILE* f = nullptr;
  if (::fopen_s(&f, path, "rb") != 0 || f == nullptr) return false;
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

bool write_ppm(const char* path, const unsigned char* rgb, int w, int h) {
  FILE* f = nullptr;
  if (::fopen_s(&f, path, "wb") != 0 || f == nullptr) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  const size_t wrote = ::fwrite(rgb, 1, (size_t)w * h * 3, f);
  ::fclose(f);
  return wrote == (size_t)w * h * 3;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: dw_ab_vscode_ref <rows.tsv> <outdir> [--scale 1.5]\n");
    return 1;
  }
  const char* rows_path = argv[1];
  const char* out_dir = argv[2];
  float scale = 1.5f;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--scale" && i + 1 < argc) scale = (float)::atof(argv[++i]);
  }
  std::vector<Row> rows;
  if (!parse_tsv(rows_path, &rows)) return 1;

  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ID2D1Factory* d2d = nullptr;
  IDWriteFactory* dw = nullptr;
  ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory, nullptr, (void**)&d2d);
  ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_IDWriteFactory, (IUnknown**)&dw);
  if (d2d == nullptr || dw == nullptr) { std::printf("factory failed\n"); return 1; }

  IDWriteRenderingParams* sys = nullptr;
  dw->CreateRenderingParams(&sys);
  std::printf("system: gamma=%.3f contrast=%.3f ct=%.3f geom=%d\n",
              (double)sys->GetGamma(), (double)sys->GetEnhancedContrast(),
              (double)sys->GetClearTypeLevel(), (int)sys->GetPixelGeometry());

  // VS Code / Chromium 在 Windows 上的候选组合（由浅到深扫）
  struct Cfg {
    const char* tag;
    D2D1_TEXT_ANTIALIAS_MODE aa;
    float gamma;
    float contrast;
    float ct;
  };
  const Cfg cfgs[] = {
      {"gray-g1.0-c0.0", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.0f, 0.0f, 0.0f},
      {"gray-g1.0-c0.5", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.0f, 0.5f, 0.0f},
      {"gray-g1.0-c1.0", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.0f, 1.0f, 0.0f},
      {"gray-g1.4-c0.0", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.4f, 0.0f, 0.0f},
      {"gray-g1.8-c0.5", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.8f, 0.5f, 0.0f},
      {"gray-g1.8-c1.0", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.8f, 1.0f, 0.0f},
      {"gray-g2.2-c0.0", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 2.2f, 0.0f, 0.0f},
      {"gray-g2.2-c1.0", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 2.2f, 1.0f, 0.0f},
      {"gray-g1.8-c1.5", D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, 1.8f, 1.5f, 0.0f},
      {"ct-g1.8-c0.5",   D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE, 1.8f, 0.5f, 1.0f},
  };

  const float logical_w = 640.0f;
  const float logical_h = 200.0f;
  const int phys_w = (int)(logical_w * scale + 0.5f);
  const int phys_h = (int)(logical_h * scale + 0.5f);

  for (const Cfg& cfg : cfgs) {
    ID2D1DCRenderTarget* rt = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES props;
    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 96.0f * scale;
    props.dpiY = 96.0f * scale;
    props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    if (FAILED(d2d->CreateDCRenderTarget(&props, &rt)) || rt == nullptr) continue;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = phys_w;
    bi.bmiHeader.biHeight = -phys_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC sdc = ::GetDC(nullptr);
    HBITMAP dib = ::CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, sdc);
    HDC mdc = ::CreateCompatibleDC(nullptr);
    ::SelectObject(mdc, dib);
    RECT rc = {0, 0, phys_w, phys_h};
    rt->BindDC(mdc, &rc);
    rt->SetTextAntialiasMode(cfg.aa);

    IDWriteRenderingParams* rp = nullptr;
    dw->CreateCustomRenderingParams(cfg.gamma, cfg.contrast, cfg.ct,
                                    sys->GetPixelGeometry(),
                                    DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &rp);
    if (rp != nullptr) rt->SetTextRenderingParams(rp);

    rt->BeginDraw();
    D2D1_COLOR_F bgc = {0.055f, 0.067f, 0.082f, 1.0f};   // VS Code Dark+ 底
    rt->Clear(bgc);
    ID2D1SolidColorBrush* brush = nullptr;
    D2D1_COLOR_F fgc = {0.831f, 0.831f, 0.847f, 1.0f};   // #D4D4D8
    rt->CreateSolidColorBrush(fgc, &brush);
    if (brush != nullptr) {
      for (const Row& r : rows) {
        if (r.top > logical_h - 10.0f) continue;
        IDWriteTextFormat* fmt = nullptr;
        const wchar_t* fam = r.mono ? L"Consolas" : L"Microsoft YaHei";
        dw->CreateTextFormat(fam, nullptr, r.bold ? DWRITE_FONT_WEIGHT_BOLD
                                                  : DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             r.size, L"zh-CN", &fmt);
        if (fmt != nullptr) {
          IDWriteTextLayout* lay = nullptr;
          if (SUCCEEDED(dw->CreateTextLayout(r.text.c_str(), (UINT32)r.text.size(), fmt,
                                             logical_w - 8.0f, r.size * 2.0f, &lay))) {
            const D2D1_POINT_2F o = {4.0f, r.top};
            rt->DrawTextLayout(o, lay, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            lay->Release();
          }
          fmt->Release();
        }
      }
      brush->Release();
    }
    rt->EndDraw();

    std::vector<unsigned char> rgb((size_t)phys_w * phys_h * 3);
    const unsigned char* src = (const unsigned char*)bits;
    for (int y = 0; y < phys_h; ++y) {
      const unsigned char* s = src + (size_t)y * phys_w * 4;
      unsigned char* d = &rgb[(size_t)y * phys_w * 3];
      for (int x = 0; x < phys_w; ++x) {
        d[x * 3 + 0] = s[x * 4 + 2];
        d[x * 3 + 1] = s[x * 4 + 1];
        d[x * 3 + 2] = s[x * 4 + 0];
      }
    }
    char path[512];
    sprintf_s(path, sizeof path, "%s/dwv-%s.ppm", out_dir, cfg.tag);
    write_ppm(path, rgb.data(), phys_w, phys_h);
    std::printf("wrote %s\n", path);

    if (rp != nullptr) rp->Release();
    ::DeleteDC(mdc);
    ::DeleteObject(dib);
    rt->Release();
  }
  sys->Release();
  dw->Release();
  d2d->Release();
  ::CoUninitialize();
  return 0;
}
