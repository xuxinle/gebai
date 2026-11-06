#pragma once

/// 裁云 · **逐字排版**（kinetic type）。
///
/// 见 `kin_text.cpp` 顶部的完整说明（为何单独立一个图层类型、
/// 排版怎么算、为什么不做换行）。
///
/// 这里只声明入口：把一行字按"每字独立驱动"的方式画出去。

#include "layer.hpp"
#include "st/ui/text_port.hpp"

namespace caiyun {

/// 绘制一个 `KinText` 图层。
///
/// ## 为何 `Painter` 用模板参数而不是具名类型
///
/// `Painter` 是 `motion.cpp` 内部**匿名命名空间**里的类（只服务于画面绘制，
/// 不对外暴露）。头文件里无法前向声明一个匿名命名空间的类型，
/// 其结果是两个不同的 `Painter` 类型冲突。
///
/// 用模板把它变成“任何有 `text()` 与 `fill()` 的东西”既解决了链接问题，
/// 也让这个模块不依赖调用方的内部结构（也就不会被它拖下水）。
///
/// @param painter    绘制器（需提供 `text(content,size,x,top,color)` 与
///                   `fill(x,y,w,h,color,radius)`）
/// @param layer      图层（含 `kin_*` 逐字参数）
/// @param state      该帧的位置/尺寸/透明度（关键帧与 JS 编排已算完）
/// @param scale      设计坐标 → 画布坐标的缩放
/// @param port       字体通道（量字宽、取基线）
/// @param local_time 场景内时间（秒）——逐字入场与打字机都靠它推进
template <typename PainterT>
void paint_kin_text(PainterT& painter, const Layer& layer, const LayerState& state, float scale,
                    const st::ui::TextPort* port, float local_time);

}  // namespace caiyun

// 实现随头文件走（模板必须可见才能真正实例化）
#include "kin_text_impl.hpp"
