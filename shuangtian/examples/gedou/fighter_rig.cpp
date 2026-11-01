#include "fighter_rig.hpp"

#include <algorithm>
#include <cmath>

#include "st/core/log.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/skin.hpp"

namespace gedou {

namespace {

using math::Color;
using math::Mat4;
using math::Vec3;

/// 归一化（零向量返回 +Y，避免除零后 NaN 扩散到整个模型）。
[[nodiscard]] auto normalize(Vec3 v) -> Vec3 {
  const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (len < 1e-6f) return Vec3{0.0f, 1.0f, 0.0f};
  return Vec3{v.x / len, v.y / len, v.z / len};
}

[[nodiscard]] auto dot(Vec3 a, Vec3 b) -> float { return a.x * b.x + a.y * b.y + a.z * b.z; }

[[nodiscard]] auto cross(Vec3 a, Vec3 b) -> Vec3 {
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}






/// 把 `from` 方向转到 `to` 方向的最短旋转。
///
/// 退化情形必须显式处理（否则 NaN 会污染整条骨骼链，表现为"模型炸成碎片"）：
/// - 平行（dot > 1-ε）→ 单位阵；
/// - 反平行（dot < -1+ε）→ `cross` 是零向量，改取一个与 `from` 垂直的轴转 π。
[[nodiscard]] auto rotation_between(Vec3 from, Vec3 to) -> Mat4 {
  const Vec3 a = normalize(from);
  const Vec3 b = normalize(to);
  const float c = std::clamp(dot(a, b), -1.0f, 1.0f);
  if (c > 0.99995f) return Mat4::identity();
  if (c < -0.99995f) {
    // 取 a 中绝对值最小的分量来构造垂直向量（数值上最稳）。
    Vec3 axis;
    if (std::abs(a.x) <= std::abs(a.y) && std::abs(a.x) <= std::abs(a.z)) {
      axis = Vec3{0.0f, -a.z, a.y};
    } else if (std::abs(a.y) <= std::abs(a.z)) {
      axis = Vec3{-a.z, 0.0f, a.x};
    } else {
      axis = Vec3{-a.y, a.x, 0.0f};
    }
    return Mat4::rotation(normalize(axis), 3.14159265f);
  }
  return Mat4::rotation(normalize(cross(a, b)), std::acos(c));
}

/// 一根骨骼在绑定姿势下的静态信息（**从 CesiumMan 的实际节点表读出**）。
///
/// 这张表是本文件与模型的"契约"：换模型必须重读一遍这些位置与方向。
/// 写死而不是运行时推导，是因为"子关节是谁"在 glTF 里没有直接字段
/// （节点树里父子关系要从 `children` 推，而这个导入结果已经把变换合成过了）。
struct BoneBind {
  std::string_view name;
  /// 绑定姿势下的世界位置（米）。
  Vec3 pos{};
  /// 绑定姿势下**指向子关节**的方向（决定"这根骨骼一转，子关节往哪走"）。
  Vec3 to_child{};
};

/// CesiumMan 的骨骼表（顺序无所谓，`parent` 决定层级）。
///
/// 位置数据来自 `gltf_skin_check` 打印的节点 `transform` 平移。
/// 方向由"本关节位置 → 子关节位置"算出（见下面的 `kBind` 初始化）。
struct BoneSpec {
  std::string_view name;
  std::string_view parent;   ///< 空 = 根
  Vec3 pos{};
};

constexpr BoneSpec kBones[] = {
    // 躯干链
    {"Skeleton_torso_joint_1", "", Vec3{0.005f, 0.679f, 0.000f}},
    {"Skeleton_torso_joint_2", "Skeleton_torso_joint_1", Vec3{0.005f, 0.824f, 0.011f}},
    {"torso_joint_3", "Skeleton_torso_joint_2", Vec3{0.005f, 1.074f, -0.004f}},
    // 颈与头
    {"Skeleton_neck_joint_1", "torso_joint_3", Vec3{0.005f, 1.138f, 0.007f}},
    {"Skeleton_neck_joint_2", "Skeleton_neck_joint_1", Vec3{0.005f, 1.190f, 0.008f}},
    // 右臂（模型右侧 = x 负）
    {"Skeleton_arm_joint_R", "torso_joint_3", Vec3{-0.086f, 1.074f, -0.004f}},
    {"Skeleton_arm_joint_R__2_", "Skeleton_arm_joint_R", Vec3{-0.301f, 0.965f, -0.016f}},
    {"Skeleton_arm_joint_R__3_", "Skeleton_arm_joint_R__2_", Vec3{-0.445f, 0.875f, 0.067f}},
    // 左臂（模型左侧 = x 正）
    {"Skeleton_arm_joint_L__4_", "torso_joint_3", Vec3{0.096f, 1.074f, -0.004f}},
    {"Skeleton_arm_joint_L__3_", "Skeleton_arm_joint_L__4_", Vec3{0.312f, 0.965f, -0.016f}},
    {"Skeleton_arm_joint_L__2_", "Skeleton_arm_joint_L__3_", Vec3{0.455f, 0.875f, 0.066f}},
    // 右腿
    {"leg_joint_R_1", "Skeleton_torso_joint_1", Vec3{-0.063f, 0.614f, 0.024f}},
    {"leg_joint_R_2", "leg_joint_R_1", Vec3{-0.072f, 0.352f, 0.068f}},
    {"leg_joint_R_3", "leg_joint_R_2", Vec3{-0.073f, 0.086f, -0.005f}},
    {"leg_joint_R_5", "leg_joint_R_3", Vec3{-0.075f, 0.021f, 0.027f}},
    // 左腿
    {"leg_joint_L_1", "Skeleton_torso_joint_1", Vec3{0.073f, 0.614f, 0.024f}},
    {"leg_joint_L_2", "leg_joint_L_1", Vec3{0.082f, 0.352f, 0.068f}},
    {"leg_joint_L_3", "leg_joint_L_2", Vec3{0.083f, 0.086f, -0.005f}},
    {"leg_joint_L_5", "leg_joint_L_3", Vec3{0.085f, 0.021f, 0.027f}},
};

/// 按名字找骨骼规格；找不到返回 nullptr。
[[nodiscard]] auto find_bone(std::string_view name) -> const BoneSpec* {
  for (const auto& bone : kBones) {
    if (bone.name == name) return &bone;
  }
  return nullptr;
}

}  // namespace


auto fighter_look(std::size_t profile_index) -> const FighterLook& {
  // 与 `fighter_profile()` 的档案顺序一致（甜甜圈=0、户圣=1）。
  static const FighterLook kDonut{
      .skin = math::Color{0xE8, 0xB4, 0x92, 0xFF},
      .torso = math::Color{0xD9, 0x2B, 0x3A, 0xFF},          // 正红运动夹克
      .torso_accent = math::Color{0xFA, 0xFA, 0xFA, 0xFF},   // 白色跑动条
      .legs = math::Color{0x2E, 0x34, 0x42, 0xFF},
      .shoes = math::Color{0xF2, 0xF2, 0xF2, 0xFF},
      .hair = math::Color{0x2A, 0x20, 0x1A, 0xFF},
      .has_cap = true,
      .cap_base = math::Color{0x1F, 0x3A, 0x93, 0xFF},       // 国旗蓝
      .cap_stripe = math::Color{0xBF, 0x0A, 0x30, 0xFF},     // 国旗红
      .has_glasses = false,
      .long_sleeves = true,
  };
  static const FighterLook kHusheng{
      .skin = math::Color{0xF0, 0xC0, 0x9A, 0xFF},
      .torso = math::Color{0x9A, 0x9E, 0xA6, 0xFF},          // 中灰 polo
      .torso_accent = math::Color{0x7E, 0x82, 0x8A, 0xFF},   // 领/门襟深一号
      .legs = math::Color{0x3A, 0x3E, 0x46, 0xFF},
      .shoes = math::Color{0x1E, 0x1E, 0x22, 0xFF},
      .hair = math::Color{0x24, 0x1E, 0x1A, 0xFF},
      .has_cap = false,
      .has_glasses = true,
      .glasses_frame = math::Color{0xC8, 0xA8, 0x50, 0xFF},  // 金框
      .long_sleeves = false,
  };
  return profile_index == 0 ? kDonut : kHusheng;
}

auto FighterRig::create(std::string_view gltf_path, const FighterLook& look)
    -> std::shared_ptr<const FighterRig> {
  auto loaded = st::raster::gltf_read_file(gltf_path);
  if (!loaded.has_value()) {
    st::log::out("角色本体加载失败（{}）：{}\n", gltf_path, loaded.error().message);
    return nullptr;
  }
  if (!loaded->skeleton.has_value()) {
    st::log::out("角色本体没有骨架（{}），无法按姿态驱动\n", gltf_path);
    return nullptr;
  }
  // `make_shared` 不能直接用于私有构造 ⇒ 通过一个静态工厂结构体转一手。
  // （用 `new` 会被 lint 的 L1 规则拦下：裸 new 容易漏 delete / 多一次分配。）
  struct MakeSharedEnabler : FighterRig {};
  auto rig = std::make_shared<MakeSharedEnabler>();
  rig->body_ = std::move(*loaded);
  rig->skin_ = *rig->body_.skeleton;
  rig->look_ = look;
  rig->valid_ = true;
  return std::shared_ptr<const FighterRig>(std::move(rig));
}

void FighterRig::draw(st::raster::Scene3D& scene, const Pose& pose, float origin_x, float ground_y,
                      bool facing_right) const {
  if (!valid_) return;
  // `ground_y` 目前不参与换算（纵向基准由模型自身的髋高度决定，见 ⓪）。
  // 保留参数是因为调用方按"世界坐标"思考，去掉会让签名更难读。
  (void)ground_y;

  // ── ⓪ 尺度对齐（**这一步漏了会让角色被拉长 1.5 倍**）──
  //
  // 2D 姿态用的是游戏世界像素（`kBodyScale` 下角色约 300px 高），
  // 而 glTF 模型自有一套米制尺度（CesiumMan 髋 0.61、颈 1.19 ⇒ 约 1.24 米高）。
  // 直接 `像素 / kPixelPerMeter` 换过去必然尺度不符：实测表现为
  // **角色被纵向拉长、躺在地上**（骨骼被拉到两倍长，整条链子垮掉）。
  //
  // 正确做法：用**姿态自身的髋-颈高度**去除**模型的髋-颈高度**，得到本帧的
  // 局部位移缩放。这样无论模型多高，动作幅度都会自动适配。
  const float pose_hip_y = static_cast<float>(pose.hip.y);
  const float pose_head_y = static_cast<float>(pose.head.y);
  const float pose_span = std::max(1.0f, std::abs(pose_hip_y - pose_head_y));   // 像素
  // 模型的髋-颈跨度（米）：从绑定表读（0.679 → 1.138）。
  constexpr float kModelSpan = 1.138f - 0.679f;   // ≈ 0.459 米

  // ── ① 2D 姿态 → 模型空间的目标位置 ──
  //
  // `pose` 的 front/back 是"朝向前方/后方"，模型是固定左右。
  // 约定：角色面朝 +X 时，**前方臂 = 模型左臂（L）**、后方臂 = 右臂（R）。
  // 腿同理：前腿 = 左腿、后腿 = 右腿。
  // 本帧的姿态→模型映射：把 2D 里的"髋-颈跨度"映射到模型的同跨度上。
  //
  // `kScale` = 模型跨度 / 姿态跨度（米/像素）。
  // 有了它，"拳伸出去 60 像素"在模型上就是"伸出 60 × kScale 米"，
  // 动作幅度与模型比例自动一致。
  const float kScale = kModelSpan / pose_span;
  // 模型髋关节的世界 y（绑定值），用来把姿态的髋对齐到它。
  constexpr float kModelHipY = 0.679f;
  // ⚠ **Z-up → Y-up**：GLB 里有一个 `Z_UP` 节点，说明模型按 Z 轴朝上制作，
  // 而框架（与 glTF 规范默认）是 Y-up。不转换的话整个角色会**横躺**在地上
  // ——实测就是这个症状：人形横着摊平、脚朝向左边。
  // 转换：绕 X 轴 -90° 即 `(x, y, z) → (x, z, -y)`。
  const auto to_rig = [&](math::Point point) -> Vec3 {
    // x 用与 2D 同样的世界尺度（横向幅度靠 kPixelPerMeter 换算更自然，
    // 但纵向必须按骨骼跨度归一，否则肢体长短会不匹配）。
    const float relative = point.x - origin_x;
    const float mirrored = facing_right ? relative : -relative;
    const float x_meters = (origin_x + mirrored) / kPixelPerMeter;
    const float y_meters = kModelHipY + (pose_hip_y - point.y) * kScale;
    return Vec3{x_meters, y_meters, 0.0f};
  };

  const Vec3 hip = to_rig(pose.hip);
  const Vec3 chest = to_rig(pose.chest);
  const Vec3 head = to_rig(pose.head);
  const Vec3 knee_f = to_rig(pose.knee_front);
  const Vec3 foot_f = to_rig(pose.foot_front);
  const Vec3 knee_b = to_rig(pose.knee_back);
  const Vec3 foot_b = to_rig(pose.foot_back);
  const Vec3 sh_f = to_rig(pose.shoulder_front);
  const Vec3 el_f = to_rig(pose.elbow_front);
  const Vec3 hd_f = to_rig(pose.hand_front);
  const Vec3 sh_b = to_rig(pose.shoulder_back);
  const Vec3 el_b = to_rig(pose.elbow_back);
  const Vec3 hd_b = to_rig(pose.hand_back);

  // ── ①-b 绑定世界矩阵（关键！）──
  //
  // 每根骨骼在**绑定姿势**下的世界矩阵，直接取 glTF 节点的 `transform`
  //（该字段已合成父链，见 `GltfNode` 的注释）。
  //
  // ⚠ 为什么必须有它：绑定姿势下骨骼**本身带旋转**（比如手臂骨骼的局部轴
  // 并不与"指向子关节的方向"重合）。早先我把世界矩阵写成
  // `平移(目标位置) × 方向差旋转`，等于把那个固有旋转丢掉了——
  // 于是 `world × inverse_bind ≠ 单位阵`，网格被压扁成一团白布塌在地上。
  // 正确形式是 `T(目标) · R(方向差) · T(-绑定位置) · 绑定世界矩阵`，
  // 这样在绑定姿势（目标 = 绑定位置、方向差 = 单位阵）下**精确还原**原模型。
  std::map<std::string, Mat4> bind_world;
  for (const auto& node : body_.nodes) {
    if (!node.name.empty()) bind_world[node.name] = node.transform;
  }

  // 绑定位置从**节点 transform 的平移**读（它已合成父链，含 `Armature` 的
  // Z-up→Y-up 旋转）。硬编码的 `kBones` 位置是**关节局部坐标**，
  // 与这里的坐标系不是同一个——混用会让整条骨骼链歪掉。
  const auto bind_pos = [&bind_world](std::string_view name) -> Vec3 {
    const auto found = bind_world.find(std::string(name));
    if (found == bind_world.end()) return Vec3{};
    return Vec3{found->second.m[12], found->second.m[13], found->second.m[14]};
  };

  // 目标位置表：关节名 → 这一帧"它应该在哪"。
  //
  // 注意：**末端关节（腕/踝）的位置由上游骨骼长度决定**，不能任意指定。
  // 这里给的是"方向意图"：把目标点量出来只为算方向，实际位置仍由骨骼链推出。
  // 之所以可以这样，是因为模型骨骼长度是固定的——这正是"骨架动画"的本质。
  struct Target {
    std::string_view joint;
    Vec3 pos;
  };
  const Target targets[] = {
      {"Skeleton_torso_joint_1", hip},
      {"Skeleton_torso_joint_2", Vec3{hip.x + (chest.x - hip.x) * 0.62f,
                                      hip.y + (chest.y - hip.y) * 0.62f, 0.0f}},
      {"torso_joint_3", chest},
      {"Skeleton_neck_joint_1", Vec3{chest.x, chest.y + (head.y - chest.y) * 0.45f, 0.0f}},
      {"Skeleton_neck_joint_2", head},
      // 左臂 = 前方臂
      {"Skeleton_arm_joint_L__4_", sh_f},
      {"Skeleton_arm_joint_L__3_", el_f},
      {"Skeleton_arm_joint_L__2_", hd_f},
      // 右臂 = 后方臂
      {"Skeleton_arm_joint_R", sh_b},
      {"Skeleton_arm_joint_R__2_", el_b},
      {"Skeleton_arm_joint_R__3_", hd_b},
      // 左腿 = 前腿
      {"leg_joint_L_1", hip},
      {"leg_joint_L_2", knee_f},
      {"leg_joint_L_3", foot_f},
      {"leg_joint_L_5", foot_f},
      // 右腿 = 后腿
      {"leg_joint_R_1", hip},
      {"leg_joint_R_2", knee_b},
      {"leg_joint_R_3", foot_b},
      {"leg_joint_R_5", foot_b},
  };
  const auto target_of = [&targets, &bind_pos](std::string_view name) -> Vec3 {
    for (const auto& target : targets) {
      if (target.joint == name) return target.pos;
    }
    return bind_pos(name);
  };

  // ── ② 逐关节算世界矩阵 ──
  //
  // 顺序：先算父、后算子（`kBones` 已按层级排列，父总在子前面）。
  //
  // 每根骨骼的世界旋转 = **把"绑定方向"转到"这一帧的实际方向"**。
  // 实际方向 = 本关节目标位置 → 子关节目标位置。
  // 这样父骨骼一转，子树自然跟着走（父子关系由"子关节位置也在目标表里"隐含）。
  constexpr std::size_t kBoneCount = sizeof(kBones) / sizeof(kBones[0]);
  const auto index_of = [](std::string_view name) -> std::size_t {
    for (std::size_t i = 0; i < kBoneCount; ++i) {
      if (kBones[i].name == name) return i;
    }
    return kBoneCount;
  };

  std::vector<Mat4> world(kBoneCount, Mat4::identity());
  for (std::size_t i = 0; i < kBoneCount; ++i) {
    const BoneSpec& bone = kBones[i];
    const Vec3 here = target_of(bone.name);

    // 子关节目标位置：找第一个 parent == 本骨骼的骨骼。
    Vec3 child{};
    bool found_child = false;
    for (std::size_t j = 0; j < kBoneCount; ++j) {
      if (kBones[j].parent == bone.name) {
        child = target_of(kBones[j].name);
        found_child = true;
        break;
      }
    }
    const Vec3 bind_here = bind_pos(bone.name);
    if (!found_child) {
      // 末端关节（腕/踝/头）：没有子关节可指。方向沿用绑定方向，
      // 这样末端不会因为"没有目标"而乱转（它只影响自己那段网格）。
      const BoneSpec* self = find_bone(bone.name);
      const Vec3 fallback = self != nullptr ? Vec3{self->pos.x, self->pos.y - 0.08f, self->pos.z}
                                            : Vec3{0.0f, -0.08f, 0.0f};
      child = here + normalize(fallback - bind_here) * 0.08f;
    }

    const Vec3 want = child - here;
    // 绑定方向：从**绑定位置**到子关节的绑定位置（都在同一坐标系里）。
    const Vec3 bind_child = [&] {
      for (std::size_t j = 0; j < kBoneCount; ++j) {
        if (kBones[j].parent == bone.name) return bind_pos(kBones[j].name);
      }
      const BoneSpec* self = find_bone(bone.name);
      return self != nullptr ? Vec3{self->pos.x, self->pos.y - 0.08f, self->pos.z}
                             : bind_here + Vec3{0.0f, -0.08f, 0.0f};
    }();
    const Vec3 bind_dir = bind_child - bind_here;

    // **世界旋转** = 把绑定方向转到当前方向。
    //
    // ⚠ 不要再乘父骨骼的旋转：每根骨骼的位置都是从 2D 姿态**独立**给出的
    // （`target_of`），父子的相对关系已经体现在"父在 A、子在 B"这两个坐标里。
    // 再叠一层父旋转等于把父子链算了两次——实测后果是模型整个拧成一团、
    // 塌在地上（截图里像一块皱巴巴的白布）。
    const Mat4 delta = rotation_between(bind_dir, want);
    const Mat4 bind = [&] {
      const auto found = bind_world.find(std::string(bone.name));
      return found != bind_world.end() ? found->second : Mat4::identity();
    }();
    world[i] = Mat4::translation(here) * delta *
               Mat4::translation(Vec3{-bind_here.x, -bind_here.y, -bind_here.z}) * bind;
  }

  // ── ③ 蒙皮矩阵 = 世界矩阵 · 逆绑定 ──
  st::raster::SkinMatrices matrices;
  matrices.matrices.resize(skin_.joints.size(), Mat4::identity());
  matrices.joint_names = skin_.joints;
  for (std::size_t j = 0; j < skin_.joints.size(); ++j) {
    const std::size_t bone_index = index_of(skin_.joints[j]);
    const Mat4 world_matrix = bone_index < (sizeof(kBones) / sizeof(kBones[0])) ? world[bone_index] : Mat4::identity();
    matrices.matrices[j] = world_matrix * skin_.bind_matrix_at(j);
  }

  // ── ③-b 服装（程序化几何，挂到已驱动的骨骼上）──
  //
  // 为什么不做成"另一套蒙皮网格"：服装是**刚体**（夹克、帽子、眼镜），
  // 它们只需要跟随某根骨骼的整体变换，不需要逐顶点形变。
  // 用"骨骼世界矩阵 × 局部偏移"摆一件刚体几何，既准确又便宜得多。
  //
  // 骨骼世界矩阵直接从 `world[]` 取（它已经把 2D 姿态翻译成了模型空间），
  // 所以服装与本体**永远同源**——拳挥出去时帽子会跟着头动，不会脱节。
  const auto bone_world = [&](std::string_view name) -> Mat4 {
    const std::size_t index = index_of(name);
    return index < kBoneCount ? world[index] : Mat4::identity();
  };

  // 坐标约定：`world[]` 出来的已经是**模型空间**（Y-up），
  // 而 glTF 本体在 mesh 里是 Z-up。所以服装要按 Z-up 摆（与顶点同坐标系），
  // 蒙皮矩阵会把它们一起转过去——这里直接用 `Mat4::identity()` 之外的
  // `node.transform` 都不需要。
  //
  // 简化：服装几何按 **Y-up 直接建**，然后用 `bone_world × R(x,-90°)` 转进 Z-up 模型空间。
  const Mat4 yup_to_zup = Mat4::rotation(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963f);

  const Color torso_color = look_.torso;
  const Color accent = look_.torso_accent;

  // —— 夹克 / polo：挂在胸腔骨骼上的一圈箱体 ——
  {
    st::raster::Mesh jacket = st::raster::Mesh::colored_box(0.34f, 0.42f, 0.22f, torso_color);
    const Mat4 at_chest = bone_world("torso_joint_3");
    scene.draw_mesh(jacket, at_chest * yup_to_zup * Mat4::translation(Vec3{0.0f, 0.02f, 0.0f}));
    // 白色跑动条 / polo 门襟
    st::raster::Mesh stripe = st::raster::Mesh::colored_box(0.035f, 0.38f, 0.235f, accent);
    scene.draw_mesh(stripe, at_chest * yup_to_zup *
                                Mat4::translation(Vec3{-0.09f, 0.02f, 0.0f}));
    scene.draw_mesh(stripe, at_chest * yup_to_zup *
                                Mat4::translation(Vec3{0.09f, 0.02f, 0.0f}));

    // 袖子：挂到两根上臂骨骼上（夹克是长袖、polo 是短袖）。
    //
    // 为什么袖子必须单独做：躯干箱体只盖住胸背，手臂是**独立骨骼**驱动的，
    // 不做袖子就会看到"红夹克 + 光膀子"（截图里就是这样）。
    const float sleeve_len = look_.long_sleeves ? 0.20f : 0.10f;
    for (const std::string_view arm_bone : {"Skeleton_arm_joint_L__4_", "Skeleton_arm_joint_R"}) {
      st::raster::Mesh sleeve =
          st::raster::Mesh::cylinder(0.045f, sleeve_len, 12, torso_color);
      // 圆柱沿 Y 居中 ⇒ 需要把它移到骨骼下方半个长度处（骨骼朝下延伸）。
      scene.draw_mesh(sleeve, bone_world(arm_bone) * yup_to_zup *
                                  Mat4::translation(Vec3{0.0f, -sleeve_len * 0.5f, 0.0f}));
    }
  }

  // —— 头部特征件 ——
  const Mat4 at_head = bone_world("Skeleton_neck_joint_2");

  if (look_.has_cap) {
    // 帽体（深蓝球）+ 红白条 + 帽檐
    // 头部几何实测：颈关节 y=1.19，头顶约 1.30 ⇒ 头半径 ≈ 0.11 米。
    // 帽体必须**略大于头**、中心落在"头顶偏上"处，否则会盖住整张脸
    // （早先半径 0.13 + 偏移 0.125 ⇒ 帽子把脸包住了）。
    st::raster::Mesh crown = st::raster::Mesh::colored_sphere(0.118f, 18, look_.cap_base);
    scene.draw_mesh(crown, at_head * yup_to_zup * Mat4::translation(Vec3{0.0f, 0.075f, -0.005f}));
    st::raster::Mesh band = st::raster::Mesh::cylinder(0.121f, 0.020f, 20, look_.cap_stripe);
    scene.draw_mesh(band, at_head * yup_to_zup * Mat4::translation(Vec3{0.0f, 0.040f, -0.005f}));
    st::raster::Mesh band2 = st::raster::Mesh::cylinder(0.119f, 0.018f, 20, accent);
    scene.draw_mesh(band2, at_head * yup_to_zup * Mat4::translation(Vec3{0.0f, 0.062f, -0.005f}));
    // 帽檐：薄薄一片，贴在额头前方（**不要太大**——早先 0.26×0.19 直接糊住了整张脸）
    // 帽檐：薄片，从帽沿**向前**伸（模型前方向是 +Z 附近，取 0.13）
    // 帽檐：薄的短檐，紧贴帽沿**前上方**——太靠下会直接遮住眼睛
    st::raster::Mesh brim = st::raster::Mesh::colored_box(0.16f, 0.012f, 0.09f, look_.cap_base);
    scene.draw_mesh(brim, at_head * yup_to_zup *
                              Mat4::translation(Vec3{0.0f, 0.062f, 0.105f}));
    // 帽徽
    st::raster::Mesh star = st::raster::Mesh::colored_sphere(0.026f, 10, accent);
    scene.draw_mesh(star, at_head * yup_to_zup * Mat4::translation(Vec3{0.0f, 0.072f, 0.100f}));
  }

  if (look_.has_glasses) {
    const Color lens{0x2A, 0x24, 0x3C, 0xFF};
    // 两片镜片
    for (int i = 0; i < 2; ++i) {
      const float dx = (i == 0) ? -0.052f : 0.052f;
      st::raster::Mesh piece = st::raster::Mesh::colored_sphere(0.042f, 16, lens);
      scene.draw_mesh(piece, at_head * yup_to_zup *
                                 Mat4::translation(Vec3{dx, 0.010f, 0.100f}));
    }
    // 金框鼻梁
    st::raster::Mesh bridge = st::raster::Mesh::colored_box(0.035f, 0.010f, 0.014f,
                                                            look_.glasses_frame);
    scene.draw_mesh(bridge, at_head * yup_to_zup *
                                Mat4::translation(Vec3{0.0f, 0.010f, 0.106f}));
    // 亮片：绕两片镜片各一圈白点（"bling"的来源）
    for (int i = 0; i < 10; ++i) {
      const float angle = static_cast<float>(i) * 0.6283f;
      for (int side = 0; side < 2; ++side) {
        const float base_x = (side == 0) ? -0.052f : 0.052f;
        const float cx = base_x + std::cos(angle) * 0.034f;
        const float cy = 0.010f + std::sin(angle) * 0.034f;
        st::raster::Mesh sparkle = st::raster::Mesh::colored_sphere(0.010f, 8,
                                                                    Color{0xFF, 0xFF, 0xFF, 0xFF});
        scene.draw_mesh(sparkle, at_head * yup_to_zup *
                                     Mat4::translation(Vec3{cx, cy, 0.115f}));
      }
    }
  }

  // ── ④ 画本体（逐网格蒙皮后绘制，并带贴图/材质）──
  //
  // ⚠ 蒙皮是**每帧对全部顶点做 LBS**：CesiumMan 只有 3273 顶点，
  // 两个角色约 6.5k 次、每帧——这个量级可以接受。若换成几万顶点的模型，
  // 就得考虑缓存或移到 GPU（顶点着色器里做）。
  for (const auto& node : body_.nodes) {
    for (const std::size_t mesh_index : node.meshes) {
      if (mesh_index >= body_.meshes.size()) continue;
      const auto& mesh = body_.meshes[mesh_index];
      if (mesh.has_skins()) {
        // ⚠ 蒙皮网格用**单位阵**绘制，不能再乘 `node.transform`。
        //
        // 为什么：`skin[j] = world[j] · inverse_bind[j]` 已经把顶点
        // **从模型空间直接映射到世界空间**（`inverse_bind` 是 模型→关节绑定，
        // `world` 是 关节→世界）。再叠一次 `node.transform` 等于把
        // "模型→世界"那一段做两遍——而 `Cesium_Man` 节点的 transform 里
        // 正好带着 `Armature` 的 **Z-up→Y-up** 旋转，于是整个角色被多转 90°，
        // 表现为**横躺在地上**（这正是反复调试时的症状）。
        scene.draw_mesh(st::raster::skin_mesh(mesh, matrices), Mat4::identity());
      } else {
        // 静态网格没有蒙皮信息，必须靠节点变换摆位。
        scene.draw_mesh(mesh, node.transform);
      }
      // 贴图（glTF 自带）
      if (node.material >= 0 &&
          static_cast<std::size_t>(node.material) < body_.materials.size()) {
        const auto& material = body_.materials[static_cast<std::size_t>(node.material)];
        if (material.base_color_texture >= 0 &&
            static_cast<std::size_t>(material.base_color_texture) < body_.textures.size()) {
          const auto& texture =
              body_.textures[static_cast<std::size_t>(material.base_color_texture)];
          if (texture.is_valid()) scene.set_texture(texture.rgba, texture.width, texture.height);
        }
      }
    }
  }
}

}  // namespace gedou
