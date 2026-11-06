#include "fighter_rig.hpp"

#include "procedural_texture.hpp"

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

  // 生成程序化贴图（一次性；生成是逐像素的，每帧做会掉帧）。
  const SkinPalette palette{look.skin, look.torso, look.torso_accent};
  rig->skin_texture_ = make_skin_texture(palette, 512);
  rig->cloth_texture_ = make_cloth_texture(look.torso, look.torso_accent, 512);
  if (look.has_cap) {
    rig->cap_texture_ = make_cap_texture(look.cap_base, look.cap_stripe, look.torso_accent, 256);
  }
  if (look.has_glasses) {
    rig->lens_texture_ = make_lens_texture(128);
  }
  return std::shared_ptr<const FighterRig>(std::move(rig));
}

void FighterRig::draw(st::raster::Scene3D& scene, const Pose& pose, float origin_x, float ground_y,
                      bool facing_right, float pixels_per_meter) const {
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
  // 用全局唯一的换算比例（与相机同源），不再按姿态各算一套。
  const float kScale = kPoseToModelScale;
  (void)pose_span;
  (void)kModelSpan;
  // 模型髋关节的世界 y（绑定值），用来把姿态的髋对齐到它。
  constexpr float kModelHipY = 0.679f;
  // ⚠ **Z-up → Y-up**：GLB 里有一个 `Z_UP` 节点，说明模型按 Z 轴朝上制作，
  // 而框架（与 glTF 规范默认）是 Y-up。不转换的话整个角色会**横躺**在地上
  // ——实测就是这个症状：人形横着摊平、脚朝向左边。
  // 转换：绕 X 轴 -90° 即 `(x, y, z) → (x, z, -y)`。
  // 姿态点（世界像素）→ 模型空间（米）。
  //
  // **横竖用同一把尺**（`kScale`，米/姿态像素）。骨骼是刚体，
  // 任何非等比缩放都会让它变形——实测踩过：x 用相机尺度、y 用模型尺度，
  // 结果腿像面条一样垂到地上、手臂甩得老长。
  //
  // 世界位置：模型空间的原点定在**场地中心的地面**（x=场地中心、y=0 为脚底），
  // 这样相机对准场地中心即可，角色按自己的世界 x 排开。
  const auto to_rig = [&](math::Point point) -> Vec3 {
    // 相对角色脚下的局部横向位移（先镜像再缩放，保证镜像绕角色自己）
    const float relative = point.x - origin_x;
    const float mirrored = facing_right ? relative : -relative;
    // 角色在世界里的位置：**用相机尺度**换算（`pixels_per_meter`）。
    //
    // 为什么这里不能也用 `kScale`：`kScale` 是"姿态像素→模型米"的**骨骼尺度**
    // （把 300px 的姿态映射到 1.24m 的模型上）；而"角色站在舞台哪个位置"
    // 是**世界坐标**，必须与相机取景用同一把尺，否则人不会出现在该在的地方。
    //
    // 两者职责不同，混用任一个都会出错：
    // - 位置用骨骼尺度 ⇒ 角色横向被压缩/拉伸（跑到场地外或两人叠在一起）；
    // - 骨骼用相机尺度 ⇒ 骨骼非等比缩放（腿拉成面条）。
    const float world_x = (origin_x + mirrored) / pixels_per_meter;
    // 纵向：以模型髋关节为基准对齐
    const float y_meters = kModelHipY + (pose_hip_y - point.y) * kScale;
    return Vec3{world_x, y_meters, 0.0f};
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

  // ── ③-b 服装：**按顶点位置直接染色**（不是外挂几何）──
  //
  // ## 为什么换成这个做法
  //
  // 之前是"在骨骼位置挂独立的球/方块"当衣服，结果衣服与身体**错位**
  // （截图里人物光秃秃的，衣服零件飘在旁边）——两套坐标系没对齐，
  // 而对着骨骼调位置既脆又难验证。
  //
  // 现在改成：身体网格每个顶点本来就是"皮肤"，按它在**模型空间的位置**
  // 判断这一块该是什么颜色（躯干→夹克、腿→裤子、头→脸），直接写顶点色。
  // 衣服于是**天生贴合身体**——它就是同一张皮上的颜色。
  //
  // ## 分区依据（实测的顶点分布，见 tools/vertex_region.cpp）
  //
  // 网格是归一化的（y ∈ [-1, 1]，模型缩放后对应身高）：
  //
  // | y 区间 | 部位 | 依据 |
  // |---|---|---|
  // | [-1.00, -0.62] | 脚/鞋 | 最窄，x 在 ±0.26 内 |
  // | [-0.62, -0.20] | 腿 | 两条分开的柱（中线附近顶点少） |
  // | [-0.20, +0.32] | 躯干 | 顶点密度最高（765+337+210） |
  // | [+0.32, +0.58] | 手臂 | x 明显外扩 |
  // | [+0.58, +1.00] | 头/颈 | 最上，x 收窄到 ±0.24 |
  //
  // 边界值都在 `kRegion*` 常量里，调衣服长短只改这里。
  // 分区边界：**用 Z 轴判高度**（这个模型是 Z-up，不是 Y-up）。
  //
  // ⚠ 这是本文件里最难发现的一个坑：`Cesium_Man` 节点带着一个
  // `Armature` 的 **Z-up → Y-up** 旋转，但那个旋转在**节点变换**里，
  // 蒙皮网格的**顶点坐标仍然是原来的 Z-up**（顶点数据没被转过）。
  //
  // 我一开始按 `y` 判"头/躯干/腿"，结果分区完全错乱——
  // 诊断色截图里**脸是躯干的红色、头顶是腿的绿色**，因为
  // 那个模型的 `y` 是**前后深度**，`z` 才是上下高度。
  //
  // 实测顶点范围：x∈[-0.131,0.181]（左右）、y∈[-0.57,0.57]（**前後**）、
  // z 才是身高方向。所以下面的"高度"一律取 `z`。
  // **实测的 z 范围**（从 dress 的顶点日志读出：z ∈ [0.974, 1.449]）。
  //
  // 之前写成 ±0.569，那是一次错误采样的结果 —— 实际所有顶点的 z 都在
  // 0.97 以上，于是"z >= kHeadBottom(0.35)"永远成立，整个模型被当成头染成肤色
  //（截图里全身米白）。这台模型是 Z-up 且原点在地面上方约 1 米处。
  // **实测的 z 直方图**（每 0.075 一格，见 dress 里的诊断输出）：
  //
  //     z:  0.04  0.11  0.19  0.26  0.34  0.41  0.49  0.57  0.64  0.72
  //     n:   264    74    18     0    72    18    18    36    80    26
  //     z:  0.79  0.87  0.94  1.02  1.09  1.17  1.24  1.32  1.39  1.47
  //     n:    53   127   111    97    74   233   538   459   566   409
  //
  // 读法（从下往上）：
  //   0.00~0.26 脚（顶点集中，脚掌扁）
  //   0.26~0.55 腿（稀疏，两条细柱）
  //   0.55~0.95 躯干（中等密度）
  //   0.95~1.15 肩/臂（密度下降）
  //   1.15~1.51 头（密度骤增，是个球）
  //
  // 早先按 ±0.569 写边界，而实际最低点是 0（脚底）、最高 1.507，
  // 所以整条判断链全错（全身变米白）。**分区边界必须用实测值**。
  // 边界（第二轮微调：躯干下探到 0.48、上探到 1.05，头从 1.26 起）
  //
  // 第一轮用 0.55/0.95/1.15 时，粉红夹克出现在**上臂**、头是个大肉球——
  // 说明躯干区间偏下、头区间偏大。这轮的取值让
  // 躯干覆盖"腰到胸"、头部只含颅骨（脖子以下算躯干）。
  constexpr float kFootTop = 0.24f;      // 脚踝
  constexpr float kLegTop = 0.48f;       // 腰
  constexpr float kTorsoTop = 1.05f;     // 胸/肩线
  constexpr float kHeadBottom = 1.26f;   // 颈以上
  // 躯干半宽：用**左右方向**的绝对值，Z-up 下左右是 x
  const float kTorsoHalfWidth = 0.085f;

  const auto dress_vertex = [this, kFootTop, kLegTop, kTorsoTop, kHeadBottom,
                             kTorsoHalfWidth](float x, float y, float z) -> std::array<float, 3> {
    // Z-up：`z` = 高度、`x` = 左右、`y` = 前后深度。
    const float height = z;
    const float abs_x = std::abs(x);
    // 正面判定：模型面朝 +Y 一侧（从截图可见脸在 y>0 那面）。
    // 保守起见用 `|y|` 判"离中面多远"，只在明显偏正面时才画条纹。
    const bool front = y > 0.02f;

    math::Color color = look_.skin;

    if (height >= kHeadBottom) {
      // 头：皮肤色（帽子/头发是另外的几何）
      color = look_.skin;
    } else if (height >= kTorsoTop) {
      // 肩/臂带：手臂看袖长，肩部算上衣
      // 肩/臂区：**整个都算袖子**（长袖=上衣色；短袖=上臂以下露肤）
      //
      // 早先按 |x| 区分"躯干/手臂"导致整条胳膊判成皮肤——
      // 因为这个高度区间（1.05~1.26）本来就全是肩膀与上臂，
      // 而 |x| 阈值 0.085 太小，几乎全被判成"躯干"。
      color = look_.long_sleeves ? look_.torso : look_.skin;
      // 肩部点缀（夹克的肩线）
      if (look_.long_sleeves && abs_x <= kTorsoHalfWidth && height > kTorsoTop + 0.02f) {
        color = look_.torso_accent;
      }
    } else if (height >= kLegTop) {
      // 躯干：上衣主色
      color = look_.torso;
      if (front) {
        if (look_.long_sleeves) {
          // 夹克：两条竖向白色跑动条
          if (std::abs(abs_x - 0.052f) < 0.014f) color = look_.torso_accent;
        } else {
          // polo：中间一道门襟
          if (abs_x < 0.011f) color = look_.torso_accent;
        }
      }
    } else if (height >= kFootTop) {
      color = look_.legs;    // 裤子
    } else {
      color = look_.shoes;   // 鞋
    }
    return {static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
            static_cast<float>(color.b) / 255.0f};
  };

  // 给蒙皮后的网格逐顶点上色。
  //
  // ⚠ 用**蒙皮前**的顶点位置判定（那是标准的 T-pose 模型空间，分区稳定）；
  // 蒙皮后的位置会随动作变化，拿它分区会让衣服"跟着动作乱跳"。
  const auto dress = [&dress_vertex](const st::raster::Mesh& mesh) -> st::raster::Mesh {
    st::raster::Mesh out = mesh;
    const std::size_t count = out.vertex_count();
    for (std::size_t i = 0; i < count; ++i) {
      const float x = out.vertices[i * 9 + 0];
      const float y = out.vertices[i * 9 + 1];
      const float z = out.vertices[i * 9 + 2];
      const auto rgb = dress_vertex(x, y, z);
      out.vertices[i * 9 + 6] = rgb[0];
      out.vertices[i * 9 + 7] = rgb[1];
      out.vertices[i * 9 + 8] = rgb[2];
    }
    return out;
  };

  // ── ③-c 头部特征件（帽子 / 头发 / 五官）──
  //
  // ## 坐标系（这一层最容易搞错）
  //
  // 蒙皮网格是用 `Mat4::identity()` 画的（蒙皮矩阵已把顶点送到世界），
  // 所以头部特征件也必须**直接放在世界坐标**里——它们的坐标要跟
  // "头顶在世界里的位置"一致，而不是模型空间。
  //
  // 头顶的世界坐标：由 `Skeleton_neck_joint_2` 的骨骼世界矩阵给出
  //（`world[]` 里的矩阵，已经是"模型空间"的米制坐标）。
  {
    const std::size_t head_index = index_of("Skeleton_neck_joint_2");
    const Vec3 head_pos = head_index < kBoneCount
                              ? Vec3{world[head_index].m[12], world[head_index].m[13],
                                     world[head_index].m[14]}
                              : Vec3{};
    // 实测：颈关节在绑定姿势下 z=1.19（模型空间），头中心约在它上方 0.09 米。
    // 但世界矩阵的平移已经是"该关节在世界的位置"，直接用。
    // `world[head]` 的平移是**颈关节的骨骼原点**（不是头中心）。
    // 实测：颈部骨骼在 z≈1.14，而头中心约在 z≈1.30（头顶 1.51）。
    // 所以头中心 = 骨骼原点 + 约 0.16 米。
    //
    // 早先直接用骨骼原点当"头中心"，帽子和眼镜全部贴在**下半脸/下巴**上
    //（截图里蓝帽子压在鼻子上、黑发贴着嘴）。
    const Vec3 head_center = Vec3{head_pos.x, head_pos.y, head_pos.z + 0.16f};

    // 头半径：模型总高 1.507 米、头占最后 0.35 米 ⇒ 半径约 0.11
    // 头的实际半径：模型头顶 1.507、颈 1.15 ⇒ 高 0.357，半径约 0.18。
    // 早先按 0.105 建帽子，结果帽子比头小、被肤色球包住看不见。
    // 最终取值：0.125。0.175 时帽子把整张脸包住（看着像大头娃娃），
    // 0.105 时帽子又被肤色球埋掉看不见。0.125 让帽子露在头顶、脸留出来。
    constexpr float kHeadRadius = 0.125f;

    if (look_.has_cap) {
      // 帽体：比头略大的球，中心上移 0.055（只盖颅顶，帽檐以下露脸）
      st::raster::Mesh crown = st::raster::Mesh::colored_sphere(kHeadRadius * 1.02f, 16,
                                                                look_.cap_base);
      scene.draw_mesh(crown, Mat4::translation(Vec3{head_center.x, head_center.y,
                                                    head_center.z + 0.085f}));
      // 帽沿红白条
      st::raster::Mesh band = st::raster::Mesh::cylinder(kHeadRadius * 1.04f, 0.016f, 18,
                                                         look_.cap_stripe);
      scene.draw_mesh(band, Mat4::translation(Vec3{head_center.x, head_center.y,
                                                   head_center.z + 0.060f}));
      // 帽檐：薄片前伸（模型正面朝 +Y）
      st::raster::Mesh brim = st::raster::Mesh::colored_box(kHeadRadius * 1.5f, 0.011f, 0.075f,
                                                            look_.cap_base);
      scene.draw_mesh(brim, Mat4::translation(Vec3{head_center.x, head_center.y + 0.075f,
                                                   head_center.z + 0.058f}));
    } else {
      // 头发：略大于头的球，后上方
      st::raster::Mesh hair = st::raster::Mesh::colored_sphere(kHeadRadius * 1.03f, 14,
                                                               look_.hair);
      scene.draw_mesh(hair, Mat4::translation(Vec3{head_center.x, head_center.y - 0.012f,
                                                   head_center.z + 0.062f}));
    }

    // 五官（正面 = +Y 方向）
    {
      const Color eye_white{0xF8, 0xF8, 0xF8, 0xFF};
      const Color pupil{0x1A, 0x1A, 0x22, 0xFF};
      for (int i = 0; i < 2; ++i) {
        const float dx = (i == 0) ? -0.038f : 0.038f;
        st::raster::Mesh white = st::raster::Mesh::colored_sphere(0.026f, 10, eye_white);
        scene.draw_mesh(white, Mat4::translation(Vec3{head_center.x + dx,
                                                      head_center.y + 0.082f,
                                                      head_center.z + 0.012f}));
        st::raster::Mesh dark = st::raster::Mesh::colored_sphere(0.014f, 8, pupil);
        scene.draw_mesh(dark, Mat4::translation(Vec3{head_center.x + dx,
                                                     head_center.y + 0.100f,
                                                     head_center.z + 0.012f}));
      }
      // 嘴
      st::raster::Mesh mouth = st::raster::Mesh::colored_sphere(0.5f, 8,
                                                                Color{0xB0, 0x6A, 0x60, 0xFF});
      scene.draw_mesh(mouth, Mat4::translation(Vec3{head_center.x, head_center.y + 0.085f,
                                                    head_center.z - 0.042f}) *
                                Mat4::scaling(Vec3{0.026f, 0.010f, 0.008f}));
    }

    if (look_.has_glasses) {
      // 亮片眼镜：两片深色镜片 + 金框 + 白点
      for (int i = 0; i < 2; ++i) {
        const float dx = (i == 0) ? -0.038f : 0.038f;
        st::raster::Mesh lens = st::raster::Mesh::colored_sphere(0.032f, 12,
                                                                 Color{0x2A, 0x24, 0x3C, 0xFF});
        scene.draw_mesh(lens, Mat4::translation(Vec3{head_center.x + dx,
                                                     head_center.y + 0.090f,
                                                     head_center.z + 0.012f}));
        for (int k = 0; k < 6; ++k) {
          const float angle = static_cast<float>(k) * 1.047f;
          st::raster::Mesh sparkle = st::raster::Mesh::colored_sphere(
              0.006f, 6, Color{0xFF, 0xFF, 0xFF, 0xFF});
          scene.draw_mesh(sparkle,
                          Mat4::translation(Vec3{head_center.x + dx + std::cos(angle) * 0.026f,
                                                 head_center.y + 0.104f,
                                                 head_center.z + 0.012f +
                                                     std::sin(angle) * 0.026f}));
        }
      }
      // 金框鼻梁
      st::raster::Mesh bridge = st::raster::Mesh::colored_box(0.018f, 0.008f, 0.010f,
                                                              look_.glasses_frame);
      scene.draw_mesh(bridge, Mat4::translation(Vec3{head_center.x, head_center.y + 0.092f,
                                                     head_center.z + 0.012f}));
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
      // 材质：皮肤是**哑光绝缘体**（metallic=0），粗糙度 0.62 让它有一点柔和高光。
      // 默认 roughness=1 会显得像粉笔；0.3 以下又像塑料。
      // 材质：**只调粗糙度，不动固有色**。
      //
      // `base_color_set` 必须保持 false —— 为 true 时材质颜色会**覆盖顶点色**，
      // 逐顶点染好的衣服就全被抹成同一个颜色（实测：全身变白）。
      {
        st::raster::Material material{};
        material.base_color_set = false;   // 关键：用顶点色
        material.metallic = 0.0f;
        material.roughness = 0.72f;
        material.specular = 0.18f;
        scene.set_material(material);
      }
      if (mesh.has_skins()) {
        // 上色 → 蒙皮 → 绘制。
        //
        // 顺序要紧：**先上色再蒙皮**。上色用的是 T-pose 的模型空间坐标
        //（分区稳定）；若反过来（蒙皮后再上色），顶点已被姿态挪走，
        // "y > 0.58 是头"这类规则会在举手时把手臂当成头——衣服跟着乱跳。
        //
        // 蒙皮网格用**单位阵**绘制，不能再乘 `node.transform`：
        // `skin[j] = world[j] · inverse_bind[j]` 已经把顶点从模型空间直接映射到
        // 世界空间；再叠 `node.transform` 会把 "模型→世界" 做两遍
        //（而那个节点带着 Armature 的 Z-up→Y-up 旋转），角色会横躺在地上。
        const st::raster::Mesh dressed = dress(mesh);
        scene.draw_mesh(st::raster::skin_mesh(dressed, matrices), Mat4::identity());
      } else {
        // 静态网格没有蒙皮信息，必须靠节点变换摆位。
        scene.draw_mesh(mesh, node.transform);
      }
    }
  }
}

}  // namespace gedou
