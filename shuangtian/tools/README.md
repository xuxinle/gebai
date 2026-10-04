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
| `grid_fit_report.cpp` | 网格拟合**收益量尺**：中间调占比（越低越锐）+ 墨量变化（形变护栏），逐字号逐文字类型 |
| `grid_fit_sampling_diag.cpp` | **拟合采样格量尺**：逐 supersample 档打印笔画数/生效数/**边缘命中物理像素网格的比例**（比 `grid=1` 与 `grid=supersample`）——用户反馈「拟合没生效/反而更糊」时先跑它 |
| `fit_apply_diag.cpp` | **逐字形生效性量尺**：打印每个字的 `applied`/`vertical_stems`/`mean_shift`/`drift`——护栏是否把 CJK 整体拒掉（`drift` 是判定量）一眼可见 |
| `text_quality_probe.cpp` | **文字质量量尺**：字形覆盖率矩阵（ASCII 直接看轮廓连续性）+ 笔画横截面（宽度一致性：沿长 sd/质心漂移/过渡像素）+ **覆盖率档位直方图**（50% 像素过多 = 拟合把边缘推进了像素正中间）；支持与浏览器截图对照 |
| `lcd_ink_probe.cpp` | **ink 口径分解探针**：逐字形打印「亚像素 vs 灰度」的逐像素平均偏差（滤波开/关两栏），用于定位 ink 口径超阈的构成（2026-10-02 用它确认超阈来自滤波摊墨而非字形走样） |
| `text_sharpness_probe.cpp` | **全组合锐度量尺**：同一段真实界面文本按 (亚像素 on/off) × (LCD 滤波 on/off) × (拟合 off/light/normal) × (画布 DPI) 全组合渲染，逐组合报 `dark/solid/mid/mid(solid)/ink`。**画布 DPI 口径必须与实际一致**——2026-10-04 的第一次测量就是口径错位才漏掉了真缺陷 |
| `text_ab_probe.cpp` + `text_ab_report.py` | **与浏览器逐像素 A/B**：同一对照串、同色、同字号，霜天侧落 PNG，浏览器侧 `edge --headless --force-device-scale-factor=1.5 --screenshot`；报告按覆盖率口径（相对背景与前景的投影）给出 `peak/solid/mid/half/mid(solid)/ink`，口径一致才可比 |
| `text_diff_report.py` | **差异分档定位**：把霜天与浏览器图按最优位移对齐后，按覆盖率分档（实心/深过渡/半覆盖/浅过渡/极浅）报差异——先对齐再比，否则 4 物理像素的基线差会把所有过渡带指标污染成噪声 |
| `text_transfer_report.py` | **覆盖率传递曲线拟合**：按霜天的覆盖率分箱看浏览器覆盖率的中位数与幂律/线性拟合——用来区分「几何差」与「色调映射差」，两者的修法完全不同 |
| `text_preblend_fit.py` | **预混合参数寻优**：把 Skia/ClearType 式 `a^gamma` + 对比度拉伸施加到现有覆盖率上，按 `mean|Δ|` 排序选参数。实测结论：在霜天当前的几何质量下预混合的收益 < 2%（identity 已是最优），**所以没有实现它**——数字摆在这里，避免下次又去追这个方向 |
| `text_phase_probe.cpp` + `text_phase_compare.py` | **位图网格相位量尺**：连续字号扫同一字形，报「相位抖动」（相邻字号 `mid/solid` 的平均变化）。奇偶交替大 = 位图原点未锚定采样格；带 `--` 对比两次输出即可验收修复 |
| `text_weight_probe.cpp` | **合成加粗幅度校核**：逐档报墨量增幅与**峰值覆盖率**（后者必须不变——变粗而不是变糊）+ 档位可分性 |
| `text_weight_cost.cpp` | **合成加粗代价量尺**：一屏中英混排（约 2000 字形）逐档报每帧耗时。注意：**同一渲染器连量多帧**才是真实稳态代价（反复新建画布会把缓存抖掉，量出 +264% 的假数） |
| `fit_shift_probe.cpp` | **拟合护栏预算量尺**：复刻位图管线、只扫 `max_shift` 取值，逐字号报 `applied / half / mid / ink`——用来判定「护栏是否在拦掉量化本身需要的位移」。2026-10-04 的「UI 字体差点意思」就是它定位的 |
| `text_role_compare.py` | **真实截图逐区域对照**：按行投影自动切文字带，每个带取「最暗像素众数」当作该带前景基准再归一。同时存在正文/次级/弱化色与语法高亮时必须这样做（统一按正文色会把「设计上就该浅」误判成「笔画没到满黑」） |
| `text_ink_conserve.cpp` | **笔画重量守恒**：与超采样 8 档的「真值」比墨量，用来把「变锐」与「被抽瘦」分开（只看到中间调下降是不够的，墨量跌了就是细了） |
| `text_fit_by_size.cpp` | **拟合按字号的收益/代价**：拟合当初是在 20~21px 验收的，但问题常在 12~14px；逐档报中间调占比与边缘陡度，避免“参数只在一档调过” |
| `st_visual_check.py` | 视觉树与截图核对 |
| `st_project_check.py` | 独立工程结构检查 |
| `st_win_check.py` | Windows 后端专项检查 |
| `title_bar_probe.py` | **窗框无头探针**：`#titlebar` 类型/属性面/无窗口时动作**如实拒绝**（自动退出的 `--frames` 形态，不依赖 quit 路径） |
| `title_bar_win_check.py` | **窗框真窗口检查**：去装饰建窗（无 `WS_CAPTION`）、保留 `WS_THICKFRAME`、**客户区 == 窗口矩形**、最大化 == 工作区、三动作生效、`--decorations` 对照——用 Win32 API 读真实窗口矩形/样式（无需 X 服务器） |
| `gallery_titlebar_shot.py` | 窗框巡检卡取证截图（切到组件页 + 滚轮滑到卡片再截） |
| `gallery_frame_shot.py` | **示例换壳取证**：gallery 的 `WindowFrame` 几何自检（标题栏/品牌/三控件/内容槽）+ 整窗截图（应用自行退出，不依赖 quit 路径） |
| `st_gdb_probe.py` | 崩溃现场信息提取 |
| `st_shot_region.py` | 指定区域截图 |
| `check_docs.py` | **文档引用一致性检查**：扫全部文档/源码里的 `DESIGN.md §X` / `CONVENTIONS.md §X` 引用，确认目标章节真的存在；并核对若干“旧值已清零”与“新内容已到位”。改了章节号或文档结构后跑一下 |
| `visual_assert_e2e.py` | **视觉断言原语端到端**：真实应用上验证 `capture.hash`（稳定性/区域敏感）与 `visual.diff`（写基线/同帧零差异/改动检出/tolerance/错误码） |
| `events_e2e.py` | **事件流端到端**：验证订阅后 `ui.changed` 携带 changed 清单（set/invoke/input.text 三路）且 version 递增 |
| `split_view_e2e.py` | **SplitView 端到端**：真实应用上验证初始比例/拖拽改比例/越界夹取/动作面/截图留证 |
| `input_action_e2e.py` | **Input 动作面端到端**：`invoke submit`（不经键盘 Enter）/ `clear` / 未知动作拒绝 |
| `gallery_declarative_e2e.py` | **gallery 声明式页端到端**（`mount_into` 子树挂载路径）：进页 → 声明式元素在树上 → 状态驱动回显 → 列表增项（key 对齐：既有项 id 不变）→ 条件内容裁剪与重新进场 → 截图 |
