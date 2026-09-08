# imgproc —— 图像处理（C++ + stb 单头库）

本地图片批处理子代理：读取（png/jpg/jpeg/bmp/gif/tga/webp/hdr/pic/pnm）→ 灰度化 / 缩放 / 统计 → 写出（png/jpg/bmp）。

## 工具

- `imgproc_info(path)`：探测图片——尺寸/像素数/文件大小/亮度直方图众数。处理前确认可用与规模。
- `imgproc_grayscale(path, output?)`：Rec.601 加权灰度化（0.299R+0.587G+0.114B，保持 alpha），输出按扩展名定格式（缺省 `原名.gray.png`）。
- `imgproc_resize(path, width?|height?|scale?, output?)`：高质量缩放（stb_image_resize2 sRGB 感知重采样）；width/height 任给一项另一边等比；scale 为倍率；上限 20000px。
- `imgproc_stats(path)`：RGB 通道均值/最小/最大、亮度 Otsu 阈值（二值化最佳分割点）、不透明像素占比。

## 生态与构建

- stb 单头库（public domain）vendor 于语言目录 `cpp/stb/`：`stb_image.h`（解码）、`stb_image_write.h`（编码）、`stb_image_resize2.h`（重采样）——`#define *_IMPLEMENTATION` 后 include 即编译进二进制
- 构建：`cpp/build.bat imgproc`（MSVC）/ `cpp/build.sh imgproc`（g++）——构建引导自动执行（driver.exe 缺失时）
- 全部解码归一为 RGBA 8bit 再处理，写 png/jpg/bmp（按输出扩展名）

## 典型用法

1. 探测：`imgproc_info {"path": "assets/hero.png"}` → 尺寸与大小
2. 缩略图：`imgproc_resize {"path": "assets/hero.png", "width": 480, "output": "assets/hero.thumb.png"}`
3. 灰度：`imgproc_grayscale {"path": "assets/hero.png"}`
4. 二值化分割点：`imgproc_stats {"path": "scan.png"}` → Otsu 阈值
