// imgproc 子代理项目：图像处理（C++ 边车常驻进程）。
// 典型场景——本地图片批处理：信息探测/灰度化/缩放/像素统计，不依赖任何运行时。
// 生态依赖：stb 单头库（stb_image 读、stb_image_write 写、stb_image_resize2 缩放），
// vendor 在语言目录 cpp/stb/（源码树即生效，构建脚本加 include 路径）。
// 基于语言目录共享基础框架（keqing/cpp/framework.hpp），本文件只写工具逻辑。
#include "../framework.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "../stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../stb/stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../stb/stb_image_resize2.h"

namespace {

using gebai::Json;
using gebai::ToolResult;

// 相对路径以 {agent_dir}（cwd）为基；~ 展开（宿主 cwd 为 manifest 目录）
std::string resolvePath(const std::string& raw) {
    std::string p = raw;
    if (!p.empty() && (p[0] == '~') && (p.size() == 1 || p[1] == '/' || p[1] == '\\')) {
        const char* home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
        if (home) p = std::string(home) + p.substr(1);
    }
    return p;
}

std::string fileExtLower(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = p.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return e;
}

struct Image {
    std::vector<unsigned char> px; // RGBA
    int w = 0, h = 0;
};

// 读图 → 统一 RGBA（jpg 的 CMYK、png 的 16bit 等由 stb 归一为 8bit）
bool loadImage(const std::string& path, Image& img, std::string& err) {
    int w, h, ch;
    unsigned char* data = stbi_load(resolvePath(path).c_str(), &w, &h, &ch, 4);
    if (!data) {
        err = stbi_failure_reason() ? stbi_failure_reason() : "无法解码";
        return false;
    }
    img.w = w; img.h = h;
    img.px.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

// 输出格式：按扩展名 png/jpg/bmp；其余一律 png
bool writeImage(const std::string& path, const Image& img, std::string& err) {
    std::string p = resolvePath(path);
    std::string e = fileExtLower(p);
    int ok = 0;
    if (e == "jpg" || e == "jpeg") ok = stbi_write_jpg(p.c_str(), img.w, img.h, 4, img.px.data(), 90);
    else if (e == "bmp") ok = stbi_write_bmp(p.c_str(), img.w, img.h, 4, img.px.data());
    else ok = stbi_write_png(p.c_str(), img.w, img.h, 4, img.px.data(), 0);
    if (!ok) { err = "写出失败（目录不存在/无写权限）: " + p; return false; }
    return true;
}

std::string humanBytes(long long b) {
    char buf[32];
    if (b < 1024) snprintf(buf, sizeof buf, "%lld B", b);
    else if (b < 1024 * 1024) snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
    else snprintf(buf, sizeof buf, "%.2f MB", b / 1048576.0);
    return buf;
}

// ---------------- 工具实现 ----------------

ToolResult toolInfo(const Json& args) {
    const Json* path = args.get("path");
    if (!path || !path->isStr() || path->str.empty())
        return ToolResult{ "缺少 path 参数", Json(), false };
    Image img;
    std::string err;
    if (!loadImage(path->str, img, err))
        return ToolResult{ "读取失败: " + err, Json(), false };
    // 直方图（RGBA → 亮度 256 桶）
    std::vector<int> hist(256, 0);
    for (size_t i = 0; i + 3 < img.px.size(); i += 4)
        hist[(unsigned char)(0.299 * img.px[i] + 0.587 * img.px[i + 1] + 0.114 * img.px[i + 2])]++;
    int modeBin = int(std::max_element(hist.begin(), hist.end()) - hist.begin());
    long long size = 0;
    if (FILE* f = fopen(resolvePath(path->str).c_str(), "rb")) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
    char dims[64];
    snprintf(dims, sizeof dims, "%dx%d (%d px)", img.w, img.h, img.w * img.h);
    Json data = Json::makeObj();
    data.set("path", Json::makeStr(path->str));
    data.set("width", Json::makeNum(img.w));
    data.set("height", Json::makeNum(img.h));
    data.set("pixels", Json::makeNum(double(img.w) * img.h));
    data.set("fileSize", Json::makeNum(double(size)));
    data.set("fileSizeHuman", Json::makeStr(humanBytes(size)));
    data.set("modeLuma", Json::makeNum(modeBin));
    std::string out = std::string("尺寸: ") + dims + "\n文件: " + humanBytes(size) +
        "\n亮度众数: " + std::to_string(modeBin) + "/255";
    return ToolResult{ out, data, true };
}

ToolResult toolGrayscale(const Json& args) {
    const Json* path = args.get("path");
    const Json* outP = args.get("output");
    if (!path || !path->isStr() || path->str.empty())
        return ToolResult{ "缺少 path 参数", Json(), false };
    Image img;
    std::string err;
    if (!loadImage(path->str, img, err))
        return ToolResult{ "读取失败: " + err, Json(), false };
    for (size_t i = 0; i + 3 < img.px.size(); i += 4) {
        unsigned char g = (unsigned char)(0.299 * img.px[i] + 0.587 * img.px[i + 1] + 0.114 * img.px[i + 2] + 0.5);
        img.px[i] = img.px[i + 1] = img.px[i + 2] = g; // alpha 保持
    }
    std::string outPath = outP && outP->isStr() && !outP->str.empty()
        ? outP->str : path->str + ".gray.png";
    if (!writeImage(outPath, img, err))
        return ToolResult{ err, Json(), false };
    Json data = Json::makeObj();
    data.set("output", Json::makeStr(outPath));
    data.set("width", Json::makeNum(img.w));
    data.set("height", Json::makeNum(img.h));
    char dims[64];
    snprintf(dims, sizeof dims, "%dx%d", img.w, img.h);
    return ToolResult{ std::string("灰度图已写出: ") + outPath + "（" + dims + "）", data, true };
}

ToolResult toolResize(const Json& args) {
    const Json* path = args.get("path");
    const Json* wJ = args.get("width");
    const Json* hJ = args.get("height");
    const Json* scaleJ = args.get("scale");
    const Json* outP = args.get("output");
    if (!path || !path->isStr() || path->str.empty())
        return ToolResult{ "缺少 path 参数", Json(), false };
    Image img;
    std::string err;
    if (!loadImage(path->str, img, err))
        return ToolResult{ "读取失败: " + err, Json(), false };
    int outW = 0, outH = 0;
    if (wJ && wJ->isNum()) outW = int(wJ->num);
    if (hJ && hJ->isNum()) outH = int(hJ->num);
    if ((outW <= 0 || outH <= 0) && scaleJ && scaleJ->isNum() && scaleJ->num > 0) {
        outW = std::max(1, int(img.w * scaleJ->num));
        outH = std::max(1, int(img.h * scaleJ->num));
    } else if (outW > 0 && outH <= 0) {
        outH = std::max(1, int(double(img.h) * outW / img.w)); // 等比
    } else if (outH > 0 && outW <= 0) {
        outW = std::max(1, int(double(img.w) * outH / img.h)); // 等比
    }
    if (outW <= 0 || outH <= 0 || outW > 20000 || outH > 20000)
        return ToolResult{ "目标尺寸非法（width/height/scale 至少给一项，上限 20000）", Json(), false };
    unsigned char* resized = stbir_resize_uint8_srgb(img.px.data(), img.w, img.h, 0, nullptr, outW, outH, 0, STBIR_4CHANNEL);
    if (!resized)
        return ToolResult{ "缩放失败（内存不足或尺寸非法）", Json(), false };
    Image out;
    out.w = outW; out.h = outH;
    out.px.assign(resized, resized + size_t(outW) * outH * 4);
    stbi_image_free(resized); // resize2 simple API 的返回 buffer 用 stbi 释放器（同为 malloc 族）
    std::string outPath = outP && outP->isStr() && !outP->str.empty()
        ? outP->str : path->str + ".resized.png";
    if (!writeImage(outPath, out, err))
        return ToolResult{ err, Json(), false };
    Json data = Json::makeObj();
    data.set("output", Json::makeStr(outPath));
    data.set("width", Json::makeNum(outW));
    data.set("height", Json::makeNum(outH));
    char dims[128];
    snprintf(dims, sizeof dims, "%dx%d -> %dx%d", img.w, img.h, outW, outH);
    return ToolResult{ std::string("缩放完成: ") + outPath + "（" + dims + "，sRGB 高质量重采样）", data, true };
}

ToolResult toolStats(const Json& args) {
    const Json* path = args.get("path");
    if (!path || !path->isStr() || path->str.empty())
        return ToolResult{ "缺少 path 参数", Json(), false };
    Image img;
    std::string err;
    if (!loadImage(path->str, img, err))
        return ToolResult{ "读取失败: " + err, Json(), false };
    // RGBA 通道统计 + Otsu 阈值（灰度二值化最佳分割点）
    double sums[3] = {0, 0, 0}, mins[3] = {255, 255, 255}, maxs[3] = {0, 0, 0};
    std::vector<int> hist(256, 0);
    long long opaque = 0;
    for (size_t i = 0; i + 3 < img.px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            unsigned char v = img.px[i + c];
            sums[c] += v; mins[c] = std::min(mins[c], double(v)); maxs[c] = std::max(maxs[c], double(v));
        }
        if (img.px[i + 3] >= 250) ++opaque;
        hist[(unsigned char)(0.299 * img.px[i] + 0.587 * img.px[i + 1] + 0.114 * img.px[i + 2])]++;
    }
    long long n = static_cast<long long>(img.w) * img.h;
    // Otsu
    double total = double(n), sum = 0;
    for (int t = 0; t < 256; ++t) sum += t * hist[t];
    double sumB = 0, wB = 0, best = -1; int thresh = 0;
    for (int t = 0; t < 256; ++t) {
        wB += hist[t];
        if (wB == 0) continue;
        double wF = total - wB;
        if (wF == 0) break;
        sumB += t * double(hist[t]);
        double mB = sumB / wB, mF = (sum - sumB) / wF;
        double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best) { best = between; thresh = t; }
    }
    char buf[256];
    snprintf(buf, sizeof buf,
             "R 均值 %.1f（%d-%d）\nG 均值 %.1f（%d-%d）\nB 均值 %.1f（%d-%d）",
             sums[0] / n, int(mins[0]), int(maxs[0]),
             sums[1] / n, int(mins[1]), int(maxs[1]),
             sums[2] / n, int(mins[2]), int(maxs[2]));
    Json data = Json::makeObj();
    Json ch = Json::makeObj();
    const char* names[3] = {"r", "g", "b"};
    for (int c = 0; c < 3; ++c) {
        Json o = Json::makeObj();
        o.set("mean", Json::makeNum(sums[c] / n));
        o.set("min", Json::makeNum(mins[c]));
        o.set("max", Json::makeNum(maxs[c]));
        ch.set(names[c], o);
    }
    data.set("channels", ch);
    data.set("otsuThreshold", Json::makeNum(thresh));
    data.set("opaqueRatio", Json::makeNum(double(opaque) / n));
    std::string out = std::string(buf) + "\nOtsu 阈值: " + std::to_string(thresh) +
        "\n不透明像素占比: " + std::to_string(int(double(opaque) / n * 1000) / 10.0) + "%";
    return ToolResult{ out, data, true };
}

} // namespace

// ---------------- 工具 schema ----------------

namespace {
const char* SCHEMA_INFO = R"json({
    "type": "object",
    "properties": {
        "path": {"type": "string", "description": "图片路径（png/jpg/jpeg/bmp/gif/tga/webp，相对 {agent_dir} 或绝对或 ~）"}
    },
    "required": ["path"]
})json";
const char* SCHEMA_GRAY = R"json({
    "type": "object",
    "properties": {
        "path": {"type": "string", "description": "输入图片路径"},
        "output": {"type": "string", "description": "输出路径（缺省 原名.gray.png）"}
    },
    "required": ["path"]
})json";
const char* SCHEMA_RESIZE = R"json({
    "type": "object",
    "properties": {
        "path": {"type": "string", "description": "输入图片路径"},
        "width": {"type": "number", "description": "目标宽（与 height 二选一可等比）"},
        "height": {"type": "number", "description": "目标高（与 width 二选一可等比）"},
        "scale": {"type": "number", "description": "缩放倍率（0.5 半幅 / 2 双幅；width/height 未给时生效）"},
        "output": {"type": "string", "description": "输出路径（缺省 原名.resized.png）"}
    },
    "required": ["path"]
})json";
} // namespace

int main() {
    gebai::registry().add({"info",
        "探测图片：尺寸/像素数/文件大小/亮度直方图众数——处理前确认图片可用与规模。常驻进程零启动开销。",
        gebai::Json::parse(SCHEMA_INFO), toolInfo});
    gebai::registry().add({"grayscale",
        "图片灰度化（Rec.601 亮度加权，保持 alpha）：输出 png/jpg/bmp（按扩展名，缺省 png）。",
        gebai::Json::parse(SCHEMA_GRAY), toolGrayscale});
    gebai::registry().add({"resize",
        "高质量缩放（stb_image_resize2 sRGB 重采样）：width/height 任给一项等比缩放，或 scale 倍率；"
        "上限 20000px。输出 png/jpg/bmp。",
        gebai::Json::parse(SCHEMA_RESIZE), toolResize});
    gebai::registry().add({"stats",
        "像素统计：RGB 各通道均值/最小/最大、亮度 Otsu 阈值（二值化最佳分割点）、不透明占比。",
        gebai::Json::parse(SCHEMA_INFO), toolStats});
    return gebai::main();
}
