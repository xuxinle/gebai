# tools — 构建与资源工具

## 依赖放哪：代码进仓库，二进制走资源管理

| 类别 | 去处 | 例子 |
|---|---|---|
| **代码**（含生成的代码） | 直接进仓库 | `third_party/quickjs`、`nlohmann`、`battery`、`opengl/gl.h` |
| **二进制 / 大源码库** | 资源管理（不进仓库） | 模型权重走 `resources/`（见 `resources/README.md`）、13–17 GB 的 GGUF 走主仓库 `infer/config/assets.manifest.json` |

界是"它是不是给人读/给编译器读的代码"。`third_party/opengl/gl.h` 是 glad 生成的
加载器单头（33 万字节）——看着像"生成物"，但它与 quickjs 同量级、同性质，
按上面这条界就该内置：克隆下来直接能编。

> 这里曾走过弯路：一开始把 glad 单头划成"生成物、不进仓库"，配了拉取脚本 +
> sha256 清单 + 校验。结果是**凭空多出一条"克隆后还得跑脚本"的路径**，
> 且多了一套要维护的清单。按上面的口径直接内置，这些都不需要了。

需要更大功能集（更高级别 GL、更多扩展）时，用 glad 在线生成器按需勾选、
替换 `third_party/opengl/gl.h` 即可——替换文件就是全部操作，没有清单要同步。

## 构建工具

| 文件 | 用途 |
|---|---|
| `stpm/` | `st` 工具链本体（构建/测试/lint/清单解析） |
| `st_probe.py` | 控制通道最小示例客户端 |
| `st_ctl.py` | 控制通道批量操作脚本 |
| `ft_compare.py` | 用 FreeType 对照自研 CFF 解释器（仅测试用，不进框架构建） |
| `lcd_compare.cpp` | 文字抗锯齿对照：同一段文字按 灰度/亚像素(滤波)/亚像素(原始) 各渲一张 PNG + 扫描行边缘剖面（仅验证用；手工编译命令见文件头） |
| `stem_phase_probe.cpp` | **小字锐度量尺**：统计竖笔画的边缘相位与过渡带像素数（“边缘落在整数网格”占比、“最糊相位”占比），用于量化网格拟合/hinting 的收益与验收（仅验证用，编译同 `lcd_compare.cpp`） |
| `hinting_gain_probe.cpp` | **hinting 收益测量台**：用 FreeType 把「无 hinting / TARGET_LIGHT / 完整 TT 指令 / auto-hinter / MONO」各档对竖笔画边缘网格对齐率的改善量出来，用来**选实现方向**而不是拍脑袋选最大的那个（需 `-I/usr/include/freetype2 -lfreetype`） |
| `st_visual_check.py` | 视觉树与截图核对 |
| `st_project_check.py` | 独立工程结构检查 |
| `st_win_check.py` | Windows 后端专项检查 |
| `st_gdb_probe.py` | 崩溃现场信息提取 |
| `st_shot_region.py` | 指定区域截图 |
| `check_docs.py` | **文档引用一致性检查**：扫全部文档/源码里的 `DESIGN.md §X` / `CONVENTIONS.md §X` 引用，确认目标章节真的存在；并核对若干“旧值已清零”与“新内容已到位”。改了章节号或文档结构后跑一下 |
