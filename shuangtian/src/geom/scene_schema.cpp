#include "st/raster/scene_schema.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "st/core/json.hpp"

namespace st::raster {

namespace {

/// 全部字段表（**唯一真源**）。
///
/// 维护约定：**改解析代码时必须同步改这里**，否则新字段会被"未知字段检查"误报。
/// 这条约束是刻意的——误报会立刻在测试里爆出来，而漏报（表里有、代码不读）
/// 的代价是"字段被写进文档但不起作用"，那种事查起来极慢。
auto build_tables() -> std::vector<FieldTable> {
  std::vector<FieldTable> tables;

  // ── 场景顶层 ──
  tables.push_back(FieldTable{
      .scope = "scene",
      .fields =
          {
              {"camera", "object", false, "", "相机（eye/target/up/fov/near/far/projection）",
               R"j({"eye":[0,1,3],"target":[0,0,0]})j",
               {"写 camra 会被拒（不是静默忽略）"}},
              {"lighting", "object", false, "", "光照（light_from/ambient/diffuse/rim/…）",
               R"j({"ambient":0.3,"diffuse":0.8})j", {}},
                {"shadow", "object", false, "", "平面投影阴影（strength/ground_y/lift/color）",
  R"j({"strength":0.35,"ground_y":0})j", {}},
  {"shadow_map", "object", false, "",
  "**阴影贴图**（物体间互相遮挡）：strength/light_index/resolution/bias/soft_radius/extent。"
  "与 `shadow` 不互斥，可同时开",
  R"j({"strength":0.6,"resolution":512,"extent":5})j",
  {"只对 extra_lights 里的 directional/spot 生效；点光需立方体贴图，本实现不做"}},
  {"occlusion", "object", false, "",
  "**缝隙遮蔽**（AO，真 GI 的便宜近似）：strength/radius。**默认关**（0）",
  R"j({"strength":0.7,"radius":0.5})j",
  {"只乘间接光（环境光+IBL），不乘直接光；永远不会与 Cycles 逐像素相同"}},
  {"environment", "object", false, "",
  "**环境贴图**（PMREM 输入）：六张 cubemap 面 PNG（px/nx/py/ny/pz/nz）+ filter_angle/exposure/probes",
  R"j({"px":"px.png","nx":"nx.png","py":"py.png","ny":"ny.png","pz":"pz.png","nz":"nz.png"})j",
  {"六面必须齐全且尺寸一致；缺一即整体不启用（半张 cubemap 无法插值）"}},
              {"cull", "string", false, R"j("none"/"back"/"front")j", "背面剔除模式", R"j("back")j",
               {"拼成 culling 会被拒"}},
              {"objects", "array", false, "",
               "物体数组（写 `three_script` / `bpy_script` 时可省；三者都不写 = 空场景）",
               R"j([{"shape":"cube"}])j", {"拼成 objectsList 会被拒"}},
              {"three_script", "string", false, "",
               "three.js 源码（非空时**覆盖 objects**）：直接写 new THREE.Mesh(...)，"
               "最后一个表达式应是 Scene/Group/Mesh 或全局 scene",
               R"j("scene.add(new THREE.Mesh(new THREE.BoxGeometry(1,1,1), new THREE.MeshStandardMaterial({color:0x88aaff}))); scene;")j",
               {"与 objects 互斥：非空时 objects 被忽略",
                "几何/材质/相机/灯光全从脚本里抽；贴图与 glTF 仍走本框架字段",
                "与 bpy_script **同时写**时进入混合模式：bpy 供几何（THREE.__assets）、本脚本决定最终场景"}},
              {"three_script_name", "string", false, "",
               "three 脚本在报错里的文件名（从文件加载时填真实路径，报错才能指到行）",
               R"j("hero.js")j", {}},
              {"bpy_script", "string", false, "",
               "bpy 源码（Blender 的 Python 建模 API）："
               "bpy.ops.mesh.primitive_* / bmesh / mathutils 均可用，把要导出的赋给 result",
               R"j("import bpy\nbpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=1, depth=2)\nresult = bpy.context.active_object")j",
               {"单独写 = 完整场景（相机/灯光/物体全从脚本里抽）",
                "与 three_script 同时写 = 混合模式：本脚本只供几何，每个带 mesh 的物体成为 THREE.__assets.<物体名>",
                "`@` 运算符不可用（MicroPython 编译器限制），用 .matmul() / .dot()",
                "面三角化用扇形：凹多边形会出错"}},
              {"bpy_script_name", "string", false, "",
               "bpy 脚本在报错里的文件名（与 three_script_name 对称）",
               R"j("gears_asset.py")j", {}},
              {"animation", "object", false, "", "关键帧动画（traks/name/duration）",
               R"j({"tracks":[...]})j", {"拼成 animations 会被拒"}},
              {"skeleton", "object", false, "", "骨架（joints/inverse_bind）",
               R"j({"joints":["root","arm"]})j", {}},
              {"asset_basis", "string", false, "", "素材基准目录（obj:/gltf:/texture 的相对路径）",
               R"j("assets/models")j", {"设了它相对路径才按它解析"}},
              {"snap", "number", false, "> 0（0 = 关闭）",
               "网格吸附步长：手写 translate 先吸附到网格再解算约束（装配值不受影响）",
               "0.1", {"只吸 translate；rotation/scale 不动"}},
          },
      .open = false,
  });

  // ── objects[] ──
  tables.push_back(FieldTable{
      .scope = "objects[]",
      .fields =
          {
              {"name", "string", false, "", "节点名（同一场景内唯一；动画/骨架按名字引用）",
               R"j("arm")j", {"重名会报错（按名字寻址会歧义）"}},
              {"shape", "string", false, "图元名或 obj:<路径>/gltf:<路径>", "几何来源",
               R"j("capsule")j", {"拼错会列出全部可用名字"}},
              {"parent", "string", false, "", "父节点名（扁平写法；父可以写在后面）",
               R"j("shoulder")j", {"嵌套写法里不能再写 parent"}},
              {"children", "array", false, "", "子节点（嵌套写法）", R"j([{...}])j",
               {"与 parent 二选一"}},
              {"group", "bool", false, "", "是否纯分组（自己不画几何）", "true",
               {"分组节点不必有合法 shape"}},
              {"translate", "vec3", false, "", "平移（父坐标系）", R"j([0,1,0])j",
               {"拼成 translaet 会被拒"}},
              {"rotation", "vec3", false, "**度**；合成顺序 X→Y→Z", "欧拉角旋转（单位：度）",
               R"j([0, 0, 90])j", {"写成弧度会转过量（90 度应写 90，不是 1.5708）"}},
              {"scale", "vec3", false, "", "缩放", R"j([1,1,1])j", {}},
              {"color", "color", false, "", "固有色（sRGB）", R"j("#c8a07a")j",
               {"写 \"red\" 会被拒（要 #rrggbb）"}},
              {"size", "number/array", false, "> 0（**全长**）",
               "统一尺寸字段（首选写法）。标量或数组，含义按形状定："
               "cube/box 是 [X,Y,Z]、plane 是 [X,Z]、球类是直径、"
               "柱/锥/胶囊/棱柱是 [直径,高]、torus 是 [外径,管径]。"
               "**一律全长，没有半长**——这是替代 radius/width/height/depth 的写法",
               "R\"j([2, 1, 3])j\"",
               {"写成半径会得到一半大小（size 是全长）", "旧字段 radius 等仍可用但会告警"}},
              {"segments", "number", false, ">= 3", "周向分段", "24",
               {"小于 3 会退化成退化几何"}},
              {"rings", "number", false, ">= 2", "球的纬度环数（uv_sphere）", "16", {}},
              {"subdivisions", "number", false, ">= 0", "细分次数（ico_sphere/正多面体）", "1",
               {}},
              {"major_radius", "number", false, "> 0", "环的主半径（torus/knot）", "0.6",
               {"0 = 用 radius"}},
              {"minor_radius", "number", false, "> 0", "环的管半径", "0.2",
               {"0 = major_radius * 0.35"}},
              {"minor_segments", "number", false, ">= 3", "截面分段", "12", {}},
              {"top_radius", "number", false, ">= 0", "锥的顶面半径（0 = 尖锥）", "0.1", {}},
              {"columns", "number", false, ">= 1", "网格平面 X 向分段（grid）", "8", {}},
              {"rows", "number", false, ">= 1", "网格平面 Z 向分段（grid）", "8", {}},
              {"sides", "number", false, ">= 3", "棱柱边数（非 0 时覆盖 segments）", "6", {}},
              {"teeth", "number", false, ">= 3", "齿轮齿数", "12", {}},
              {"tooth_height", "number", false, "0..1", "齿高占比（gear）/环带宽度（circle）",
               "0.08", {}},
              {"tooth_width", "number", false, "0..1", "齿宽占比", "0.5", {}},
              {"twist", "number", false, "", "整段扭转角（弧度）", "0.5", {}},
              {"turns", "number", false, "", "螺旋圈数（helix/staircase）", "3.0", {}},
              {"taper", "number", false, "", "收缩量", "0.2", {}},
              {"thickness", "number", false, ">= 0",
               "壁厚（beam 型钢/fastener 紧固件；0 = 按比例取默认）", "0.04", {}},
              {"section", "string", false,
               "beam: i/channel/angle/box；ornament: star/cross；fastener: bolt/nut/washer",
               "截面/零件选型（留空 = 该图元默认型）", R"j("i")j", {}},
              {"knot_p", "number", false, ">= 1", "纽结 (p,q) 的 p", "2", {}},
              {"knot_q", "number", false, ">= 1", "纽结 (p,q) 的 q", "3", {}},
              {"face_camera", "bool", false, "", "平面/盘是否朝 +Z（立面）", "true", {}},
              {"metallic", "number", false, "0..1", "金属度（Blender Principled）", "0.8", {}},
              {"roughness", "number", false, "0..1", "粗糙度", "0.3", {}},
              {"specular", "number", false, "0..1", "高光强度", "0.5", {}},
              {"emission", "number", false, ">= 0", "自发光强度", "0.5", {}},
              {"opacity", "number", false, "0..1", "不透明度", "0.6",
               {"< 1 会进透明队列"}},
              {"emissive_color", "color", false, "", "自发光颜色", R"j("#ff8800")j",
               {"不设时用固有色"}},
              {"emission_unlit", "bool", false, "", "纯发光体（不受光照影响）", "true", {}},
              {"obj", "string", false, "", "**已废弃**：用 shape 的 `obj:<路径>` 前缀",
               R"j("chair.obj")j", {"写 obj 字段会被忽略——请改用 shape"}},
              {"gltf", "string", false, "", "外部 glTF（`gltf:<路径>` 或裸路径）",
               R"j("chair.glb")j", {"非空时覆盖 shape"}},
              {"gltf_use_material", "bool", false, "", "是否用 glTF 自带的材质", "true", {}},
              {"ground", "bool", false, "", "这是地面（阴影投射的目标；会强制水平）", "true",
               {"地面节点只保留绕 Y 的朝向"}},
              {"visible", "bool", false, "", "是否可见（父不可见时整棵子树都不可见）", "false",
               {}},
              {"skin", "bool", false, "", "是否参与蒙皮（需场景有 skeleton）", "true", {}},
              {"pivot", "vec3", false, "", "枢轴点（旋转/缩放绕它；局部坐标）", R"j([0,-0.5,0])j",
               {}},
              {"pivot_at", "string", false, R"j("origin"/"center"/"bottom"/"top")j",
               "枢轴预设（按几何包围盒算）", R"j("bottom")j", {"写 elbow 会被拒"}},
              {"attach", "string", false,
               "<anchor>[_of]:<名字>[+dx,dy,dz]（省略 anchor 默认 bottom）",
               "装配约束：我的锚点贴到目标的锚点（解算后写回 translate）",
               R"j("top_of:plinth")j",
               {"只能引用先出现的名字；改目标高度会自动跟随"}},
              {"op", "string", false,
               "union / subtract / intersect",
               "布尔运算：非空时 children 语义变为操作体列表（首个是基体），网格级 CSG",
               R"j("subtract")j",
               {"子元素各自 TRS 参与运算；操作体需闭合（Plane/Disc 不行）"}},
              {"profile", "string", false,
               R"j("x,y x,y …” 空格分点、逗号分坐标)j",
               "草图轮廓：extrude 沿 Y 拉伸（3+ 点）；revolve 绕 Y 车削（2+ 点，x=半径 y=高度）",
               R"j("0,0 1,0 1,0.8 0.5,1.4 0,1.4")j",
               {"顺逆时针都收；自交轮廓结果未定义"}},
              {"pattern", "string", false, R"j("ring"/"row")j",
               "阵列：非空时 children 首个子元素被实例化 pattern_count 次并 merge 成单网格",
               R"j("ring")j", {"ring 绕 Y 轴均分；row 沿 pattern_step 等距排"}},
              {"pattern_count", "number", false, ">= 1", "阵列份数", "8", {}},
              {"pattern_radius", "number", false, "> 0", "ring 阵列的环半径", "0.8", {}},
              {"pattern_step", "vec3", false, "[dx,dy,dz]",
               "row 阵列的间距向量", "[0.2, 0, 0]", {}},
              {"connect", "string", false,
               "<锚点A>:<锚点B>（两端各是 anchor_of:name 或省略 anchor 的 name）",
               "装配约束：两点定线——管/杆从 A 拉到 B（translate/rotation/size.Y 全自动）",
               R"j("col_cap:reactor_base")j",
               {"size 的 Y 项写 auto 则长度自动=两端距离"}},
              {"align", "string", false, R"j("axis:<名字>[+dx,dy,dz]")j",
               "装配约束：我的 Y 轴线对准目标的轴线（XY 对齐、Y 保留）",
               R"j("axis:main_shaft")j", {}},
              {"texture", "string", false, "", "贴图路径（PNG/JPEG；相对 asset_basis）",
               R"j("brick.png")j", {"文件缺失不中断渲染，但会 WARN"}},
              {"texture_tile", "number", false, "> 0", "便利字段：两个方向同时平铺",
               "4.0", {"等价于同时设 scale_u/scale_v"}},
              {"texture_scale_u", "number", false, "", "UV 缩放 U", "4.0", {}},
              {"texture_scale_v", "number", false, "", "UV 缩放 V", "4.0", {}},
              {"texture_offset_u", "number", false, "", "UV 偏移 U", "0.25", {}},
              {"texture_offset_v", "number", false, "", "UV 偏移 V", "0.25", {}},
              {"texture_clamp", "bool", false, "", "UV 越界取边缘色（默认 repeat 平铺）", "true",
               {}},
              {"texture_alpha", "bool", false, "", "用贴图 alpha 做透明", "true", {}},
          },
      .open = false,
  });

  // ── camera ──
  tables.push_back(FieldTable{
      .scope = "camera",
      .fields =
          {
              {"eye", "vec3", false, "", "相机位置", R"j([0,1,3])j", {}},
              {"target", "vec3", false, "", "看向的点（与 eye 重合会报错）", R"j([0,0,0])j", {}},
              {"up", "vec3", false, "", "上方向", R"j([0,1,0])j", {}},
              {"fov", "number", false, "0 < fov < 180（度）", "竖直视场角", "42", {}},
              {"near", "number", false, "> 0", "近裁剪面（**小模型要调小**，默认 0.1）", "0.01",
               {"模型半径 < 0.1 时会被裁掉（实测 3cm 道具渲染出 0 像素）"}},
              {"far", "number", false, "> near", "远裁剪面", "100", {}},
              {"projection", "string", false, R"j("perspective"/"orthographic")j", "投影类型",
               R"j("perspective")j", {"正交下 fov 失效，用 ortho_scale"}},
              {"ortho_scale", "number", false, "> 0", "正交投影的垂直可视高度", "2.0", {}},
          },
      .open = false,
  });

  // ── lighting ──
  tables.push_back(FieldTable{
      .scope = "lighting",
      .fields =
          {
              {"light_from", "vec3", false, "", "主光方向（**指向光源**，不是照射方向）",
               R"j([0.4,0.8,0.6])j", {"写反会让阴影朝反方向"}},
              {"ambient", "number", false, ">= 0", "环境光强度", "0.3", {}},
              {"diffuse", "number", false, ">= 0", "漫反射强度", "0.8", {}},
              {"rim", "number", false, ">= 0", "边缘光强度", "0.2", {}},
              {"extra_lights", "array", false, "",
               "追加光列表 [{direction,color,intensity}]（裁云预设补光用）；写 null 清空",
               R"j([{"direction":[-0.7,0.25,0.5]}])j", {}},
              {"exposure", "number", false, "> 0", "曝光", "1.0", {}},

              {"env_sky", "color", false, "", "环境反射的天空色", R"j("#6b85c4")j",
               {"金属没有环境反射就是塑料"}},
              {"env_ground", "color", false, "", "环境反射的地面色", R"j("#1a1a21")j", {}},
              {"env_strength", "number", false, ">= 0", "环境反射强度（0=关闭）", "0", {}},
              {"env_rough_falloff", "number", false, "0..1", "粗糙度向均值弥散", "0.6", {}},
              {"tone_map", "string", false,
               R"j("none"/"reinhard"/"aces"/"filmic"/"agx")j",
               "色调映射算子。用于与外部工具对齐：three.js 默认 none、Blender 4.x 默认 agx。"
               "不写时按来源择默认（bpy 场景 → agx；其余 → aces）",
               R"j("aces")j",
               {"三条曲线的差异到了肉眼可辨（中灰相差 50/255）：见 tools/tone_map_measure",
                "想让 three_script 产物与浏览器一致，显式写 \"none\""}},
          },
      .open = false,
  });

  // ── shadow ──
  tables.push_back(FieldTable{
      .scope = "shadow",
      .fields =
          {
              {"strength", "number", false, "0..1", "阴影强度（0 = 关闭）", "0.35", {}},
              {"ground_y", "number", false, "", "投影地面的 Y 高度", "0", {}},
              {"lift", "number", false, ">= 0", "阴影抬起量（防 z-fighting）", "0.01", {}},
              {"color", "color", false, "", "阴影颜色", R"j("#000000")j", {}},
          },
      .open = false,
  });

  // ── environment（环境贴图 / PMREM 输入，2026-11-07）──
  //
  // 六张立方图面（PNG）。为何不是单一 .hdr/.exr：前者需要新解码依赖，
  // 而「六张 PNG」用已用的解码路径就行，且调用方能用任意工具生成。
  tables.push_back(FieldTable{
      .scope = "environment",
      .fields =
          {
              {"px", "string", false, "", "+X 面图路径", "px.png", {}},
              {"nx", "string", false, "", "-X 面图路径", "", {}},
              {"py", "string", false, "", "+Y 面（上方）图路径", "", {}},
              {"ny", "string", false, "", "-Y 面（下方）图路径", "", {}},
              {"pz", "string", false, "", "+Z 面图路径", "", {}},
              {"nz", "string", false, "", "-Z 面图路径", "", {}},
              {"filter_angle", "number", false, "0..1.5708",
               "预过滤锥角（弧度）：0 = 保留高频锐反射，越大越模糊", "0",
               {"它是**预过滤**而不是运行时模糊：值固定后不能按粗糙度变化"}},
              {"exposure", "number", false, "> 0", "贴图亮度倍数", "1", {}},
              {"probes", "number", false, "1..256", "预过滤成多少个方向探针", "64",
               {"越多越细腻也越慢；探针间是插值，不是连续反射"}},
          },
      .open = false,
  });

  // ── occlusion（缝隙遮蔽 / AO）──
  //
  // 真 GI 的便宜近似。默认 `strength = 0`（关）：所有像素级基准在关闭态下
  // 对账，开着会把那些基准整体变暗、全部变红。
  tables.push_back(FieldTable{
      .scope = "occlusion",
      .fields =
          {
              {"strength", "number", false, "0..1",
               "遮蔽强度（0 = 关闭，保持与像素级基准一致）", "0",
               {"0.5~0.8 是产品展示的常见档位"}},
              {"radius", "number", false, "> 0",
               "射线长度（世界单位）：多大范围的接触会被算成遮蔽", "0.5",
               {"它是场景尺度相关的：值太小只暗在棱角上，太大整面都变暗"}},
          },
      .open = false,
  });
  //
  // 为何它曾是个“就空的接口”：`ShadowMap` 结构早就写在 `scene3d.hpp`
  // 并写了完整的设计说明，但**脚本里声明不了、GPU 腿没实现、测试为零**。
  // 现在补上声明与软件腿路径。
  tables.push_back(FieldTable{
      .scope = "shadow_map",
      .fields =
          {
              {"strength", "number", false, "0..1", "阴影强度（0 = 关闭）", "0.6", {}},
              {"light_index", "number", false, "",
               "用 `extra_lights` 里哪一个光源做投影（仅 directional/spot）", "0",
               {"点光需要立方体贴图（6 张），本实现不做"}},
              {"resolution", "number", false, "64..2048",
               "阴影贴图边长（正方形）；越大阴影越锐、一次深度渲染越贵", "512", {}},
              {"bias", "number", false, "", "深度偏移（防自阴影条纹 shadow acne）",
               "0.0015", {"太小会出现条纹，太大阴影会脱离物体"}},
              {"soft_radius", "number", false, ">= 0",
               "PCF 柔化半径（贴图纹素为单位；0 = 硬阴影）", "1.0", {}},
              {"extent", "number", false, "> 0",
               "投影相机包络半径（世界单位）：覆盖场景所需的正交盒半径", "6",
               {"它是场景尺度相关的：太小阴影被裁掉，太大阴影发糊"}},
          },
      .open = false,
  });

  // ── animation ──
  tables.push_back(FieldTable{
      .scope = "animation",
      .fields =
          {
              {"name", "string", false, "", "动画名（默认 animation）", R"j("wave")j", {}},
              {"duration", "number", false, ">= 0", "时长（秒；0 = 由关键帧决定）", "2.0", {}},
              {"tracks", "array", false, "", "轨道数组", R"j([{...}])j", {}},
          },
      .open = false,
  });

  // ── animation.tracks[] ──
  tables.push_back(FieldTable{
      .scope = "animation.tracks[]",
      .fields =
          {
              {"node", "string", true, "", "目标节点名（必须存在于场景树）", R"j("shoulder")j",
               {"指向不存在的节点会在渲染时报错"}},
              {"kind", "string", true, R"j("translate"/"rotation"/"scale"/"visible")j",
               "属性种类", R"j("rotation")j", {"拼成 colour 会列出可用值"}},
              {"keys", "array", true, "", "关键帧数组（至少一个）", R"j([{...}])j", {}},
          },
      .open = false,
  });

  // ── animation.tracks[].keys[] ──
  tables.push_back(FieldTable{
      .scope = "animation.tracks[].keys[]",
      .fields =
          {
              {"t", "number", true, ">= 0（秒）", "时间（也接受 `time`）", "0.5",
               {"缺 t 会报错"}},
              {"time", "number", false, ">= 0（秒）", "`t` 的别名", "0.5", {}},
              {"value", "vec3", true, "", "值（rotate 是**欧拉角**；visible 取 x > 0.5）",
               R"j([0,0,1.57])j", {"rotate 写角度会得到很小的旋转"}},
              {"easing", "string", false,
               R"j("linear"/"constant"/"ease-in"/"ease-out"/"ease-in-out")j",
               "缓动（作用于本帧到下一帧）", R"j("ease-in-out")j",
               {"拼成 bounce 会列出可用值"}},
          },
      .open = false,
  });

  // ── skeleton ──
  tables.push_back(FieldTable{
      .scope = "skeleton",
      .fields =
          {
              {"joints", "array", true, "", "关节名数组（必须与节点名一致）", R"j(["root","arm"])j",
               {"找不到关节会报错并指出是哪个"}},
              {"inverse_bind", "array", false, "", "逆绑定矩阵（16 数 × 关节数，列主序）",
               "[[...16 个数...]]", {"省略时按单位阵处理"}},
          },
      .open = false,
  });

  return tables;
}

}  // namespace

auto FieldTable::find(std::string_view name) const -> const FieldSpec* {
  for (const FieldSpec& field : fields) {
    if (field.name == name) return &field;
  }
  return nullptr;
}

auto FieldTable::names() const -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(fields.size());
  for (const FieldSpec& field : fields) out.push_back(field.name);
  return out;
}

auto scene_field_tables() -> const std::vector<FieldTable>& {
  static const std::vector<FieldTable> tables = build_tables();
  return tables;
}

auto find_field_table(std::string_view scope) -> const FieldTable* {
  for (const FieldTable& table : scene_field_tables()) {
    if (table.scope == scope) return &table;
  }
  return nullptr;
}

auto edit_distance(std::string_view a, std::string_view b, int limit) -> int {
  // 标准 Levenshtein，带"提前放弃"：任一行最小值已超 limit 就返回 limit + 1。
  //
  // ## 为什么需要提前放弃
  //
  // 字段名都很短（< 20 字符），看不太出差别。但候选集合有上百个，
  // 逐个算完整距离是 O(n·m·k)。截断让"明显不相近"的候选在几行内出局。
  const std::size_t length_a = a.size();
  const std::size_t length_b = b.size();
  if (length_a == 0) return static_cast<int>(length_b);
  if (length_b == 0) return static_cast<int>(length_a);
  // 长度差已经超过 limit → 不可能更近
  if (static_cast<int>(length_a > length_b ? length_a - length_b : length_b - length_a) > limit) {
    return limit + 1;
  }

  std::vector<int> previous(length_b + 1U);
  std::vector<int> current(length_b + 1U);
  for (std::size_t j = 0; j <= length_b; ++j) previous[j] = static_cast<int>(j);

  for (std::size_t i = 1; i <= length_a; ++i) {
    current[0] = static_cast<int>(i);
    int row_minimum = current[0];
    for (std::size_t j = 1; j <= length_b; ++j) {
      const int cost = a[i - 1U] == b[j - 1U] ? 0 : 1;
      const int deletion = previous[j] + 1;
      const int insertion = current[j - 1U] + 1;
      const int substitution = previous[j - 1U] + cost;
      current[j] = std::min({deletion, insertion, substitution});
      row_minimum = std::min(row_minimum, current[j]);
    }
    if (row_minimum > limit) return limit + 1;
    std::swap(previous, current);
  }
  return previous[length_b];
}

auto suggest_fields(std::string_view typed, const std::vector<std::string>& candidates,
                    int max_distance, std::size_t max_suggestions) -> std::vector<std::string> {
  struct Scored {
    std::string name;
    int distance{0};
  };
  std::vector<Scored> scored;
  for (const std::string& candidate : candidates) {
    // 忽略大小写差异（`Radius` 与 `radius` 应当算 0 距离的候选）
    std::string lowered_typed{typed};
    std::string lowered_candidate{candidate};
    for (char& ch : lowered_typed) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    for (char& ch : lowered_candidate) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    const int distance = edit_distance(lowered_typed, lowered_candidate, max_distance);
    if (distance <= max_distance) scored.push_back({candidate, distance});
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored& lhs, const Scored& rhs) { return lhs.distance < rhs.distance; });
  std::vector<std::string> out;
  for (const Scored& entry : scored) {
    if (out.size() >= max_suggestions) break;
    out.push_back(entry.name);
  }
  return out;
}

auto check_unknown_fields(const Json& value, const FieldTable& table, std::string_view path)
    -> Status {
  if (!value.is_object()) return st::ok();   // 类型错由各字段自己报
  if (table.open) return st::ok();           // 开放字典（如 extensions）

  const std::vector<std::string> known = table.names();
  for (auto it = value.begin(); it != value.end(); ++it) {
    const std::string key = it.key();
    if (table.find(key) != nullptr) continue;

    // ## 未知字段一律报错，不静默忽略
    //
    // 理由见头文件：拼错的字段被静默忽略时，调用方看到"解析成功"却看不到效果，
    // 只能反复试错。而给出候选之后，一次就能改对。
    const auto suggestions = suggest_fields(key, known);
    std::string advice;
    if (!suggestions.empty()) {
      advice = "；你是不是想写 ";
      for (std::size_t i = 0; i < suggestions.size(); ++i) {
        if (i > 0) advice += " 或 ";
        advice += suggestions[i];
      }
      advice += "？";
    } else {
      // 没有相近候选时列出该层的全部合法字段（数量通常可控，且这是最后的线索）
      advice = "；该处可用字段：";
      for (std::size_t i = 0; i < known.size(); ++i) {
        if (i > 0) advice += ", ";
        advice += known[i];
      }
    }
    return st::unexpected(
        ErrorCode::Parse,
        std::format("{}.{}：未知字段（{} 这一层不认识 \"{}\"）{}", path, key, table.scope, key,
                    advice));
  }
  return st::ok();
}

}  // namespace st::raster
