/// 裁云脚本字段 schema 的实现（见 `schema.hpp`）。
///
/// ## 数据的来源纪律
///
/// 表里每一条都对应 `director.cpp` 里真实存在的一次读取。范围与默认值抄自那里的
/// `std::clamp(...)` 实参与 `Layer` / `Scene` 的字段默认值——**不是按文档猜的**，
/// 因为文档与代码已经漂移过一次（`script_design.md` 附录 B 仍写着"没有贴图/阴影"，
/// 而实现里 `model_texture` / `model_shadow` 早已可用）。
///
/// 改解析范围时**同一提交里改这张表**，否则校验会与真实行为分叉；
/// `tests/caiyun_schema_test.cpp` 有一条用例断言两处一致。

#include "schema.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "st/ext/json.hpp"

namespace caiyun {
namespace {

/// 别名列表构造（不足三个时补空串）。
[[nodiscard]] constexpr auto Aliases(std::string_view first, std::string_view second = {},
                                     std::string_view third = {})
    -> std::array<std::string_view, 3> {
  return {first, second, third};
}

// ————————————————————————————————————————————————————————————————————————————
// 字段表
// ————————————————————————————————————————————————————————————————————————————

/// 图层字段（`layers[]` 的每一项）。
///
/// 顺序刻意按"从通用到专项"排：身份 → 位置尺寸 → 外观 → 文字 → 逐字排版 → 素材 →
/// 特效 → 入场 → 弹跳 → 路径 → 关键帧 → 三维 → 相机 → 光照。这与 `layer.hpp`
/// 的字段声明顺序一致——表是给人读的，两处同序才不必来回翻。
constexpr auto kLayerFields = std::array{
    // —— 身份 ——
    FieldSpec{"kind", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "图层类型：rect/ellipse/line/text/image/path/glow/particles/model3d"},
    FieldSpec{"name", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "图层名（面板显示与轨迹诊断用，不影响渲染）"},
    // —— 位置与尺寸（数字=设计像素，字符串=画面比例）——
    FieldSpec{"x", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "左上角 X（设计像素，或 \"50%\" 这类画面比例）"},
    FieldSpec{"y", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "左上角 Y（设计像素，或画面比例）"},
    FieldSpec{"w", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "宽度（设计像素，或画面比例）"},
    FieldSpec{"h", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "高度（设计像素，或画面比例）"},

    // —— 外观 ——
    FieldSpec{"fill", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "填充色 #rrggbb / #rrggbbaa（矩形/椭圆/路径/光晕/粒子）"},
    FieldSpec{"fill_alt", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "渐变第二色（a=0 表示不用渐变）"},
    FieldSpec{"gradient_dir", SchemaScope::Layer, SchemaType::Integer, true, 0, 1, true,
              "渐变方向：0=竖向 1=横向"},
    FieldSpec{"stroke", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "描边色（a=0 表示无描边）"},
    FieldSpec{"stroke_width", SchemaScope::Layer, SchemaType::Number, true, 0.0, 80.0, true,
              "描边宽度（像素）"},
    FieldSpec{"radius", SchemaScope::Layer, SchemaType::Number, true, 0.0, 4000.0, true,
              "圆角半径（像素）"},
    FieldSpec{"opacity", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0, true,
              "整体不透明度 0~1"},

    // —— 文字 ——
    FieldSpec{"text", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "文字内容（\\n 换行）"},
    FieldSpec{"size", SchemaScope::Layer, SchemaType::Number, true, 4.0, 800.0, true,
              "字号（像素）"},
    FieldSpec{"tracking", SchemaScope::Layer, SchemaType::Number, true, 0.0, 200.0, true,
              "字距（像素）"},
    FieldSpec{"line_height", SchemaScope::Layer, SchemaType::Number, true, 1.0, 4.0, true,
              "行高倍数"},
    FieldSpec{"bold", SchemaScope::Layer, SchemaType::Bool, false, 0, 0, false, "是否粗体"},
    FieldSpec{"align", SchemaScope::Layer, SchemaType::Integer, true, 0, 3, true,
              "对齐：0=居中 1=左 2=右"},
    FieldSpec{"color", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "文字颜色（与 fill 独立）"},

    // —— 逐字排版（KinText）——
    FieldSpec{"kin_stagger", SchemaScope::Layer, SchemaType::Number, true, 0.0, 2.0, true,
              "逐字入场延迟（秒/字），建议 0.02~0.06"},
    FieldSpec{"kin_dur", SchemaScope::Layer, SchemaType::Number, true, 0.01, 10.0, true,
              "逐字入场时长（秒）"},
    FieldSpec{"kin_enter", SchemaScope::Layer, SchemaType::Integer, true, 0.0, 2.0, true,
              "逐字入场方式：0=上浮淡入 1=微缩放 2=模糊上浮"},
    FieldSpec{"kin_rise", SchemaScope::Layer, SchemaType::Number, true, 0.0, 400.0, true,
              "逐字上浮距离（像素）"},
    FieldSpec{"kin_float", SchemaScope::Layer, SchemaType::Number, true, 0.0, 80.0, true,
              "逐字持续起伏幅度（像素），0=不动"},
    FieldSpec{"kin_float_hz", SchemaScope::Layer, SchemaType::Number, true, 0.01, 8.0, true,
              "逐字起伏频率（Hz）"},
    FieldSpec{"kin_caret", SchemaScope::Layer, SchemaType::Integer, true, 0.0, 1.0, true,
              "光标：0=无 1=末尾闪烁竖线"},
    FieldSpec{"kin_caret_scale", SchemaScope::Layer, SchemaType::Number, true, 0.2, 2.0, true,
              "光标高度/字号比例"},
    FieldSpec{"kin_type_cps", SchemaScope::Layer, SchemaType::Number, true, 0.0, 200.0, true,
              "打字机每秒字数（0=不打字）"},

    // —— 素材与路径 ——
    FieldSpec{"src", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "图片素材引用（绝对路径/相对脚本/media 名）"},
    FieldSpec{"contain", SchemaScope::Layer, SchemaType::Bool, false, 0, 0, false,
              "图片：true=完整可见留边 false=cover 填满裁切"},
    FieldSpec{"dim", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0, true,
              "图片压暗 0~1"},
    FieldSpec{"points", SchemaScope::Layer, SchemaType::Array, false, 0, 0, false,
              "路径点列表 [x0,y0, x1,y1, ...]（设计像素）"},
    FieldSpec{"closed", SchemaScope::Layer, SchemaType::Bool, false, 0, 0, false,
              "路径是否闭合（闭合则填充）"},

    // —— 特效 ——
    FieldSpec{"intensity", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0, true,
              "光晕/粒子强度 0~1"},
    FieldSpec{"count", SchemaScope::Layer, SchemaType::Integer, true, 0.0, 600.0, true,
              "粒子数"},
    FieldSpec{"spread", SchemaScope::Layer, SchemaType::Number, true, 0.02, 2.0, true,
              "光晕半径 / 粒子扩散（相对画面短边）"},

    // —— 入场 ——
    FieldSpec{"enter", SchemaScope::Layer, SchemaType::Integer, true, 0.0, 3.0, true,
              "入场：0=无 1=淡入 2=上浮淡入 3=缩放"},
    FieldSpec{"enter_at", SchemaScope::Layer, SchemaType::Number, true, 0.0, 600.0, true,
              "入场起始（场景内秒）"},
    FieldSpec{"enter_dur", SchemaScope::Layer, SchemaType::Number, true, 0.05, 10.0, true,
              "入场时长（秒）"},

    // —— 阻尼弹跳 ——
    FieldSpec{"bounce", SchemaScope::Layer, SchemaType::Number, true, 0.0, 400.0, true,
              "弹跳幅度（像素），0=不弹"},
    FieldSpec{"bounce_hz", SchemaScope::Layer, SchemaType::Number, true, 0.05, 8.0, true,
              "弹跳频率（Hz），2~3 有活力"},
    FieldSpec{"bounce_damping", SchemaScope::Layer, SchemaType::Number, true, 0.0, 20.0, true,
              "弹跳衰减（1/秒），3~5 跳三下就定"},
    FieldSpec{"bounce_at", SchemaScope::Layer, SchemaType::Number, false, 0, 0, false,
              "弹跳起点（场景内秒）"},

    // —— 路径动画 ——
    FieldSpec{"path_from_x", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "路径起点 X（空=用图层自身 x）"},
    FieldSpec{"path_from_y", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "路径起点 Y"},
    FieldSpec{"path_to_x", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "路径终点 X"},
    FieldSpec{"path_to_y", SchemaScope::Layer, SchemaType::Dimension, false, 0, 0, false,
              "路径终点 Y"},
    FieldSpec{"path_arc", SchemaScope::Layer, SchemaType::Number, false, 0, 0, false,
              "弧高（像素，直线位移距离的 15~30%）"},
    FieldSpec{"path_dur", SchemaScope::Layer, SchemaType::Number, true, 0.02, 60.0, true,
              "飞行时长（秒）"},
    FieldSpec{"path_at", SchemaScope::Layer, SchemaType::Number, false, 0, 0, false,
              "飞行起始（场景内秒）"},
    FieldSpec{"path_ease", SchemaScope::Layer, SchemaType::Integer, true, 0.0, 3.0, true,
              "缓动：0=线性 1=ease-out 2=ease-in-out 3=落地过冲"},

    // —— 关键帧时间线 ——
    FieldSpec{"tracks", SchemaScope::Layer, SchemaType::Object, false, 0, 0, false,
              "关键帧轨道：{\"y\":[{\"t\":0,\"v\":400}], ...} 或 [{property,keys}]"},

    // —— 三维模型 ——
    FieldSpec{"model_shape", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "形状：box/sphere/torus/cylinder/plane/textured_box/textured_plane/"
              "textured_slab/obj:<路径>",
              true, Aliases("shape", "geometry")},
    FieldSpec{"model_color", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "模型主色", true, Aliases("color", "base_color")},
    FieldSpec{"model_color_alt", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "模型次色（非透明时按竖直方向双色渐变）"},
    FieldSpec{"model_segments", SchemaScope::Layer, SchemaType::Integer, true, 3.0, 96.0, true,
              "细分段数（24 在 720p 看不出棱，48 用于特写，上限 96）", true,
              Aliases("segments", "detail", "resolution")},
    FieldSpec{"model_scale", SchemaScope::Layer, SchemaType::Number, true, 0.05, 8.0, true,
              "模型缩放（组内=世界单位尺寸）", true, Aliases("scale", "size")},
    FieldSpec{"model_spin", SchemaScope::Layer, SchemaType::Number, true, -720.0, 720.0, true,
              "自转角速度（度/秒，绕 Y；负值=反向；8~15 是缓慢自转）", true,
              Aliases("rotation", "rotate", "model_rot")},
    FieldSpec{"model_yaw", SchemaScope::Layer, SchemaType::Number, true, -3600.0, 3600.0, true,
              "初始角度（度）", true, Aliases("yaw", "heading")},
    FieldSpec{"model_tilt", SchemaScope::Layer, SchemaType::Number, true, -89.0, 89.0, true,
              "绕 X 固定倾斜（度），10~20 显立体", true, Aliases("tilt", "pitch")},
    FieldSpec{"model_bob", SchemaScope::Layer, SchemaType::Number, true, 0.0, 0.5, true,
              "上下浮动幅度（0.03~0.08 舒适）", true, Aliases("float", "bob_y")},
    FieldSpec{"model_bob_hz", SchemaScope::Layer, SchemaType::Number, true, 0.01, 5.0, true,
              "浮动频率（Hz）"},
    FieldSpec{"model_backdrop", SchemaScope::Layer, SchemaType::Bool, false, 0, 0, false,
              "true=用主色铺满矩形；false=透明可叠"},
    FieldSpec{"model_texture", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "纹理源（PNG/SVG 路径；仅 textured_* 形状带 UV）", true,
              Aliases("texture", "map", "image")},
    FieldSpec{"model_group", SchemaScope::Layer, SchemaType::String, false, 0, 0, false,
              "场景组名：同名图层共用一个三维世界（同一相机/光照/正确遮挡）", true,
              Aliases("group", "world")},
    FieldSpec{"model_shadow", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0, true,
              "接触阴影强度 0~1（仅组内生效，0.45~0.65 建议）", true,
              Aliases("shadow", "contact_shadow")},
    FieldSpec{"model_shadow_color", SchemaScope::Layer, SchemaType::Color, false, 0, 0, false,
              "阴影颜色（带一点环境色比纯黑自然）"},
    FieldSpec{"model_bounce_squash", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0,
              true, "形变强度 0~1（体积守恒的 squash & stretch，0.10~0.20 建议）"},

    // —— 相机（球坐标）——
    FieldSpec{"cam_azimuth", SchemaScope::Layer, SchemaType::Number, true, -3600.0, 3600.0, true,
              "相机水平方位角（度）：0=正前方，正值向右", true,
              Aliases("camera_azimuth", "azimuth", "cam_angle")},
    FieldSpec{"cam_elevation", SchemaScope::Layer, SchemaType::Number, true, -85.0, 85.0, true,
              "相机仰角（度）：正值俯视", true,
              Aliases("camera_elevation", "elevation", "cam_pitch")},
    FieldSpec{"cam_distance", SchemaScope::Layer, SchemaType::Number, true, 0.6, 30.0, true,
              "相机距离（建议 2.2~4.0）", true,
              Aliases("camera_distance", "distance", "cam_dist")},
    FieldSpec{"cam_fov", SchemaScope::Layer, SchemaType::Number, true, 8.0, 110.0, true,
              "视野角（度）：小=长焦压缩，大=广角变形", true,
              Aliases("camera_fov", "fov", "cam_fov_y")},
    FieldSpec{"cam_dolly_from", SchemaScope::Layer, SchemaType::Number, true, 0.3, 4.0, true,
              "推拉起始倍率（1.0→1.06 即推 6%）"},
    FieldSpec{"cam_dolly_to", SchemaScope::Layer, SchemaType::Number, true, 0.3, 4.0, true,
              "推拉结束倍率"},

    // —— 光照 ——
    FieldSpec{"light_azimuth", SchemaScope::Layer, SchemaType::Number, true, -3600.0, 3600.0,
              true, "主光方位角（度）；侧逆光 120~150 才有体积感", true,
              Aliases("light_angle", "light_dir")},
    FieldSpec{"light_elevation", SchemaScope::Layer, SchemaType::Number, true, -89.0, 89.0, true,
              "主光仰角（度）"},
    FieldSpec{"light_ambient", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0, true,
              "环境光 0~1（0=背光面全黑，0.35=柔和底光）", true,
              Aliases("ambient", "light_amb")},
    FieldSpec{"light_rim", SchemaScope::Layer, SchemaType::Number, true, 0.0, 1.0, true,
              "边缘光 0~1（让模型从背景浮出，0.35~0.5 出彩）", true,
              Aliases("light_rimm", "rim", "light_edge")},
};

/// 场景字段（`scenes[]` 的每一项）。
constexpr auto kSceneFields = std::array{
    FieldSpec{"kind", SchemaScope::Scene, SchemaType::String, false, 0, 0, false,
              "版式：title/terminal/triptych/metrics/outro/custom"},
    FieldSpec{"duration", SchemaScope::Scene, SchemaType::Number, true, 0.2, 60.0, true,
              "该镜时长（秒）", true, Aliases("dur", "length", "time")},
    FieldSpec{"title", SchemaScope::Scene, SchemaType::String, false, 0, 0, false, "主文案"},
    FieldSpec{"subtitle", SchemaScope::Scene, SchemaType::String, false, 0, 0, false, "副文案"},
    FieldSpec{"lines", SchemaScope::Scene, SchemaType::Array, false, 0, 0, false,
              "逐行文案（终端=代码行，三联卡=标签，数字=说明）"},
    FieldSpec{"values", SchemaScope::Scene, SchemaType::Array, false, 0, 0, false,
              "metrics 版式的数值数组"},
    FieldSpec{"accent", SchemaScope::Scene, SchemaType::Color, false, 0, 0, false, "强调色"},
    FieldSpec{"background", SchemaScope::Scene, SchemaType::Array, false, 0, 0, false,
              "背景渐变 [上, 下]（两个 #rrggbb）", true, Aliases("bg", "background_color")},
    FieldSpec{"layers", SchemaScope::Scene, SchemaType::Array, false, 0, 0, false,
              "通用图层数组（kind=custom 时是主体）"},
    FieldSpec{"layout", SchemaScope::Scene, SchemaType::String, false, 0, 0, false,
              "JS 编排脚本（相对脚本目录的 .js 路径，或 {\"source\":...}）"},

    // 素材
    FieldSpec{"image", SchemaScope::Scene, SchemaType::String, false, 0, 0, false,
              "背景图（铺满，cover）"},
    FieldSpec{"image_card", SchemaScope::Scene, SchemaType::String, false, 0, 0, false,
              "卡片插图（嵌在卡片内，contain）"},
    FieldSpec{"image_dim", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "背景图压暗 0~1（有文字时必须压暗）"},
    FieldSpec{"image_ken_burns", SchemaScope::Scene, SchemaType::Number, true, 0.0, 0.25, true,
              "背景图缓慢推拉幅度（0.06~0.08 即可）"},

    // 影像特效
    FieldSpec{"dof", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "景深强度（0.2~0.5）"},
    FieldSpec{"particles", SchemaScope::Scene, SchemaType::Integer, true, 0.0, 600.0, true,
              "浮尘粒子数（40~90）"},
    FieldSpec{"particle_alpha", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "粒子不透明度"},
    FieldSpec{"grain", SchemaScope::Scene, SchemaType::Integer, true, 0.0, 12000.0, true,
              "背景颗粒数（让静止段安静而不冻结）。1px 亮点在 1080p 下需上万颗才"
              "能在 32×18 验收网格里显现——旧上限 2000 在本尺寸几乎测不出"},
    FieldSpec{"grain_alpha", SchemaScope::Scene, SchemaType::Number, true, 0.0, 0.6, true,
              "颗粒亮度（0.05~0.2）"},
    FieldSpec{"wrap", SchemaScope::Scene, SchemaType::Bool, false, 0, 0, false,
              "包装层（识标/四角标/页脚/进度）开关"},
    FieldSpec{"glow", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "径向光晕强度（0.4~0.9）"},
    FieldSpec{"glow_cx", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "光晕中心 X（画面比例）"},
    FieldSpec{"glow_cy", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "光晕中心 Y"},
    FieldSpec{"glow_radius", SchemaScope::Scene, SchemaType::Number, true, 0.05, 1.5, true,
              "光晕半径"},
    FieldSpec{"god_ray", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "体积光强度"},
    FieldSpec{"god_ray_angle", SchemaScope::Scene, SchemaType::Number, false, 0, 0, false,
              "体积光角度（弧度，-1.25 ≈ 右下 72°）"},
    FieldSpec{"light_sweep", SchemaScope::Scene, SchemaType::Number, true, 0.0, 1.0, true,
              "光扫强度（每段扫过一次）"},

    // 运镜
      FieldSpec{"cam_zoom_from", SchemaScope::Scene, SchemaType::Number, true, 0.02, 400.0, true,
            "整段缩放起始倍率。小幅度（0.95~1.08）= 镜头呼吸；"
            "大幅度（1→300，配 expo_in）= 钻穿尺度"},
  FieldSpec{"cam_zoom_to", SchemaScope::Scene, SchemaType::Number, true, 0.02, 400.0, true,
            "整段缩放结束倍率"},
  FieldSpec{"cam_anchor_x", SchemaScope::Scene, SchemaType::Number, true, -1.0, 1.0, true,
            "缩放销点 X（归一化 0~1；不填 = 画面中心）。销点是“从这一格钻出去”与"
            "“整幅放大”的全部区别：它让那个位置在屏幕上不动"},
  FieldSpec{"cam_anchor_y", SchemaScope::Scene, SchemaType::Number, true, -1.0, 1.0, true,
            "缩放销点 Y（归一化 0~1）"},
  FieldSpec{"cam_zoom_curve", SchemaScope::Scene, SchemaType::String, true, 0.0, 0.0, false,
            "缩放曲线：linear（默认）/ expo_in（吸进去）/ expo_out（弹出来）"},
  FieldSpec{"cam_window_start", SchemaScope::Scene, SchemaType::Number, true, 0.0, 600.0, false,
            "相机进度窗口起点（秒）。钻到位后要静止让观众读就得用它——"
            "没有窗口时相机只能一路走到幕尾"},
  FieldSpec{"cam_window_end", SchemaScope::Scene, SchemaType::Number, true, 0.0, 600.0, false,
            "相机进度窗口终点（秒）；≤ 起点 = 不启用"},
  FieldSpec{"cam_vector", SchemaScope::Scene, SchemaType::Bool, false, 0.0, 0.0, false,
            "矢量相机：绘制期缩放（图元含文字重新光栅化），钻穿不糊。"
            "只支持等比缩放 + 锚点（不支持旋转/震动）；粒子/三维/辉光不跟着缩放"},
    FieldSpec{"cam_rotate", SchemaScope::Scene, SchemaType::Number, true, -8.0, 8.0, true,
              "整段旋转总量（度，建议 ≤1）"},
    FieldSpec{"cam_pan_x", SchemaScope::Scene, SchemaType::Number, true, -0.5, 0.5, true,
              "整段水平平移"},
    FieldSpec{"cam_pan_y", SchemaScope::Scene, SchemaType::Number, true, -0.5, 0.5, true,
              "整段垂直平移"},
    FieldSpec{"cam_shake", SchemaScope::Scene, SchemaType::Number, true, 0.0, 0.08, true,
              "冲击震动幅度"},
    FieldSpec{"footer_note", SchemaScope::Scene, SchemaType::String, false, 0, 0, false,
              "本镜页脚左字（空则用脚本级）"},
    FieldSpec{"carry_out", SchemaScope::Scene, SchemaType::Object, false, 0, 0, false,
              "跨场承接图元：两块画面之间有一个东西活下来并变成下一个。"
              "字段 name（必填）/into/at/dur/from{x,y,w,h,radius}/to{...}/fill/stroke/"
              "stroke_width/text/text_color/text_size/curve/fade_out"},
};

/// 脚本顶层字段。
constexpr auto kScriptFields = std::array{
    FieldSpec{"name", SchemaScope::Script, SchemaType::String, false, 0, 0, false,
              "片子名（面板显示）"},
    FieldSpec{"fps", SchemaScope::Script, SchemaType::Number, true, 1.0, 120.0, true, "帧率",
              true, Aliases("frame_rate", "framerate")},
    FieldSpec{"size", SchemaScope::Script, SchemaType::Array, false, 0, 0, false,
              "画幅 [宽, 高]（如 [1920,1080]）", true, Aliases("resolution", "canvas")},
    FieldSpec{"transition", SchemaScope::Script, SchemaType::Number, true, 0.0, 2.0, true,
              "镜间交叉淡入时长（秒）"},
    FieldSpec{"fade_in", SchemaScope::Script, SchemaType::Number, true, 0.0, 5.0, true,
              "全片淡入（秒）"},
    FieldSpec{"fade_out", SchemaScope::Script, SchemaType::Number, true, 0.0, 5.0, true,
              "全片淡出（秒）"},
    FieldSpec{"brand_wordmark", SchemaScope::Script, SchemaType::String, false, 0, 0, false,
              "品牌字标（空=不画）"},
    FieldSpec{"brand_latin", SchemaScope::Script, SchemaType::String, false, 0, 0, false,
              "品牌拉丁副标"},
    FieldSpec{"footer_note", SchemaScope::Script, SchemaType::String, false, 0, 0, false,
              "页脚左字"},
    FieldSpec{"show_brand", SchemaScope::Script, SchemaType::Bool, false, 0, 0, false,
              "是否画包装层"},
    FieldSpec{"audio", SchemaScope::Script, SchemaType::Object, false, 0, 0, false,
              "配乐与配音规格"},
    FieldSpec{"scenes", SchemaScope::Script, SchemaType::Array, false, 0, 0, false,
              "分镜数组（必填，不能为空）", true, Aliases("shots", "clips")},
};

/// `audio` 对象字段。
constexpr auto kAudioFields = std::array{
    FieldSpec{"enabled", SchemaScope::Audio, SchemaType::Bool, false, 0, 0, false,
              "是否合成配乐"},
    FieldSpec{"style", SchemaScope::Audio, SchemaType::String, false, 0, 0, false,
              "曲风：uplift（明亮上行）/ calm（稀疏铺底）"},
    FieldSpec{"bpm", SchemaScope::Audio, SchemaType::Number, true, 40.0, 240.0, true,
              "节拍（强节奏 BGM 的切点要卡在拍上）"},
    FieldSpec{"gain", SchemaScope::Audio, SchemaType::Number, true, 0.0, 1.0, true,
              "配乐增益"},
    FieldSpec{"voice", SchemaScope::Audio, SchemaType::Array, false, 0, 0, false,
              "配音轨数组 [{path,start,gain}]", true, Aliases("voices", "narration")},
};

/// 关键帧字段（`tracks` 里每个 `{t, v, ease}`）。
constexpr auto kKeyFrameFields = std::array{
    FieldSpec{"t", SchemaScope::KeyFrame, SchemaType::Number, false, 0, 0, false,
              "该关键帧的时间（秒，场景内）", true, Aliases("time", "at")},
    FieldSpec{"v", SchemaScope::KeyFrame, SchemaType::Number, false, 0, 0, false,
              "该关键帧的属性值", true, Aliases("value", "val")},
    FieldSpec{"ease", SchemaScope::KeyFrame, SchemaType::Integer, true, 0.0, 3.0, true,
              "到下一帧的缓动：0=线性 1=ease-out 2=ease-in-out 3=过冲"},
};

/// `tracks` 数组写法里的每条轨道。
constexpr auto kObjEntryFields = std::array{
    FieldSpec{"property", SchemaScope::ObjEntry, SchemaType::String, false, 0, 0, false,
              "被驱动的属性名（x/y/w/h/scale/alpha/rotate）", true,
              Aliases("prop", "prop_name")},
    FieldSpec{"keys", SchemaScope::ObjEntry, SchemaType::Array, false, 0, 0, false,
              "关键帧数组 [{t,v,ease}]", true, Aliases("frames", "keyframes")},
};

constexpr std::array<SchemaScope, 6> kAllScopes = {SchemaScope::Script, SchemaScope::Audio,
                                                   SchemaScope::Scene,  SchemaScope::Layer,
                                                   SchemaScope::KeyFrame, SchemaScope::ObjEntry};

/// 取某作用域的字段表（起点 + 长度）。
[[nodiscard]] auto scope_table(SchemaScope scope) -> std::pair<const FieldSpec*, std::size_t> {
  switch (scope) {
    case SchemaScope::Script: return {kScriptFields.data(), kScriptFields.size()};
    case SchemaScope::Audio: return {kAudioFields.data(), kAudioFields.size()};
    case SchemaScope::Scene: return {kSceneFields.data(), kSceneFields.size()};
    case SchemaScope::Layer: return {kLayerFields.data(), kLayerFields.size()};
    case SchemaScope::KeyFrame: return {kKeyFrameFields.data(), kKeyFrameFields.size()};
    case SchemaScope::ObjEntry: return {kObjEntryFields.data(), kObjEntryFields.size()};
  }
  return {nullptr, 0};
}

}  // namespace

auto schema_type_name(SchemaType type) -> std::string_view {
  switch (type) {
    case SchemaType::Number: return "number";
    case SchemaType::Integer: return "integer";
    case SchemaType::Bool: return "bool";
    case SchemaType::String: return "string";
    case SchemaType::Color: return "color";
    case SchemaType::Dimension: return "dimension";
    case SchemaType::Array: return "array";
    case SchemaType::Object: return "object";
  }
  return "number";
}

auto schema_scope_name(SchemaScope scope) -> std::string_view {
  switch (scope) {
    case SchemaScope::Script: return "script";
    case SchemaScope::Audio: return "audio";
    case SchemaScope::Scene: return "scene";
    case SchemaScope::Layer: return "layer";
    case SchemaScope::KeyFrame: return "keyframe";
    case SchemaScope::ObjEntry: return "track";
  }
  return "layer";
}

auto all_field_specs() -> const std::vector<FieldSpec>& {
  // 函数内 static：首次使用时构造一次。"表与解析分叉"的风险由
  // `tests/caiyun_schema_test.cpp` 的覆盖度用例兜住（不是靠注释约定）。
  static const std::vector<FieldSpec> specs = [] {
    std::vector<FieldSpec> all;
    all.reserve(kScriptFields.size() + kAudioFields.size() + kSceneFields.size() +
                kLayerFields.size() + kKeyFrameFields.size() + kObjEntryFields.size());
    const std::pair<const FieldSpec*, std::size_t> tables[] = {
        {kScriptFields.data(), kScriptFields.size()},
        {kAudioFields.data(), kAudioFields.size()},
        {kSceneFields.data(), kSceneFields.size()},
        {kLayerFields.data(), kLayerFields.size()},
        {kKeyFrameFields.data(), kKeyFrameFields.size()},
        {kObjEntryFields.data(), kObjEntryFields.size()}};
    for (const auto& table : tables) {
      for (std::size_t i = 0; i < table.second; ++i) all.push_back(table.first[i]);
    }
    return all;
  }();
  return specs;
}

auto fields_in_scope(SchemaScope scope) -> std::vector<const FieldSpec*> {
  std::vector<const FieldSpec*> out;
  const auto [table, count] = scope_table(scope);
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) out.push_back(&table[i]);
  return out;
}

auto find_field_schema(SchemaScope scope, std::string_view name) -> const FieldSpec* {
  const auto [table, count] = scope_table(scope);
  for (std::size_t i = 0; i < count; ++i) {
    if (table[i].name == name) return &table[i];
  }
  return nullptr;
}

auto edit_distance(std::string_view a, std::string_view b) -> std::size_t {
  // 经典 Levenshtein（两行滚动 DP）。字段名都很短（<40 字符），无需优化。
  const std::size_t n = b.size();
  std::vector<std::size_t> prev(n + 1);
  std::vector<std::size_t> cur(n + 1);
  for (std::size_t j = 0; j <= n; ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    for (std::size_t j = 1; j <= n; ++j) {
      const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[n];
}

auto suggestion_distance_limit(std::size_t key_length) -> std::size_t {
  // 短键（`x`/`w`/`t`/`v`）打错一个字符就是另一个键，允许 1；
  // 长键（`model_bounce_squash`）允许 3。上限 4 防止"完全无关也被硬配上"。
  if (key_length <= 4) return 1;
  if (key_length <= 10) return 2;
  if (key_length <= 20) return 3;
  return 4;
}

auto suggest_field(SchemaScope scope, std::string_view unknown) -> std::string_view {
  if (unknown.empty()) return {};

  // —— 第一优先：别名命中 ——
  //
  // 别名表达的是"同一个概念的另一种叫法"，这是**确定**的语义信息，
  // 不能被距离算法推翻（`model_rot` 与 `model_bob` 的编辑距离更近，
  // 但 `model_rot` 明确是"旋转"= `model_spin`）。
  for (const FieldSpec& spec : all_field_specs()) {
    if (spec.scope != scope || !spec.suggestable) continue;
    for (const std::string_view alias : spec.aliases) {
      if (!alias.empty() && alias == unknown) return spec.name;
    }
  }

  // —— 第二优先：编辑距离（含受控的子串启发）——
  const std::size_t limit = suggestion_distance_limit(unknown.size());
  const FieldSpec* best = nullptr;
  std::size_t best_distance = limit + 1;
  std::size_t best_second = limit + 2;

  for (const FieldSpec& spec : all_field_specs()) {
    if (spec.scope != scope || !spec.suggestable) continue;
    if (spec.name.size() > unknown.size() * 2 + 6) continue;  // 长度差太远，不必算
    std::size_t distance = edit_distance(unknown, spec.name);
    // 子串启发：只在**双方都够长**时参与。
    //
    // 为何要限长度：短字段名（`h`/`x`/`w`）几乎必然是长名的子串——
    // 早先无条件折算成"距离 1"，于是 `camera_azimuth` 会命中单字母 `h`，
    // 建议出一个完全无关的字段（实测）。长名之间的包含关系才是真信号
    // （`camera_azimuth` 含 `azimuth`、`model_rot` 前缀 `model_`）。
    constexpr std::size_t kSubstringMinLength = 6;
    if (unknown.size() >= kSubstringMinLength && spec.name.size() >= kSubstringMinLength) {
      if (unknown.find(spec.name) != std::string_view::npos ||
          spec.name.find(unknown) != std::string_view::npos) {
        distance = std::min(distance, std::size_t{2});
      }
    }
    if (distance < best_distance) {
      best_second = best_distance;
      best_distance = distance;
      best = &spec;
    } else if (distance < best_second) {
      best_second = distance;
    }
  }
  if (best == nullptr || best_distance > limit) return {};
  // 两个候选同样好 → 不猜（猜错会让智能体照着错的改，比不给建议更糟）
  if (best_second == best_distance) return {};
  return best->name;
}

auto Diagnostic::to_string() const -> std::string {
  const std::string body = path.empty() ? message : std::format("{} · {}", path, message);
  return is_error ? std::format("错误：{}", body) : std::format("警告：{}", body);
}

auto unknown_key_diagnostic(SchemaScope scope, std::string_view key, std::string_view path)
    -> Diagnostic {
  Diagnostic diag{};
  diag.is_error = false;  // 宽容：未知键不阻断加载（老脚本仍要能出片）
  diag.path = std::string(path);
  diag.key = std::string(key);
  const std::string_view suggestion = suggest_field(scope, key);
  if (!suggestion.empty()) {
    diag.suggestion = std::string(suggestion);
    diag.message = std::format("未知字段 '{}'（已忽略）——是不是想写 '{}'？", key, suggestion);
  } else {
    diag.message = std::format("未知字段 '{}'（已忽略）——该层没有这个字段，"
                               "可用字段见 schema（get director schema_json）",
                               key);
  }
  return diag;
}

auto out_of_range_diagnostic(const FieldSpec& spec, double written, double applied,
                             std::string_view path) -> Diagnostic {
  Diagnostic diag{};
  diag.is_error = false;
  diag.path = std::string(path);
  diag.key = std::string(spec.name);
  diag.message = std::format("字段 '{}' 的值 {} 超出范围 [{}, {}]，已按 {} 生效", spec.name,
                             written, spec.min_value, spec.max_value, applied);
  return diag;
}

auto wrong_type_diagnostic(const FieldSpec& spec, std::string_view actual_type,
                           std::string_view path) -> Diagnostic {
  Diagnostic diag{};
  diag.is_error = false;
  diag.path = std::string(path);
  diag.key = std::string(spec.name);
  diag.message = std::format("字段 '{}' 需要 {}，收到 {}（已用默认值）", spec.name,
                             schema_type_name(spec.type), actual_type);
  return diag;
}

auto schema_to_json() -> std::string {
  st::Json root = st::Json::object();
  root["version"] = 1;
  root["note"] = "裁云脚本字段自省表；键名/类型/范围/说明。写脚本时以此为准。";
  st::Json scopes = st::Json::object();
  for (const SchemaScope scope : kAllScopes) {
    st::Json list = st::Json::array();
    for (const FieldSpec* spec : fields_in_scope(scope)) {
      st::Json item = st::Json::object();
      item["name"] = std::string(spec->name);
      item["type"] = std::string(schema_type_name(spec->type));
      if (spec->has_range) {
        item["min"] = spec->min_value;
        item["max"] = spec->max_value;
        item["clamped"] = spec->clamped;
      }
      item["summary"] = std::string(spec->summary);
      st::Json aliases = st::Json::array();
      for (const std::string_view alias : spec->aliases) {
        if (!alias.empty()) aliases.push_back(std::string(alias));
      }
      if (!aliases.empty()) item["aliases"] = std::move(aliases);
      list.push_back(std::move(item));
    }
    scopes[std::string(schema_scope_name(scope))] = std::move(list);
  }
  root["scopes"] = std::move(scopes);
  return st::json_dump(root, true, 2);
}

}  // namespace caiyun
