// ⚠ 本文件由 tools/wayland_bindgen.py 生成——**请勿手改**。
//
// 输入：third_party/wayland/{wayland,xdg-shell,xdg-decoration-unstable-v1}.xml
//（官方协议 XML 的原样副本；来源与 SHA-256 见 third_party/wayland/SOURCES.md）
// 重新生成：python3 tools/wayland_bindgen.py --out src/shell/wayland_protocol.hpp
//
// 生成物**自包含**：不 include 任何 wayland 头，也不链接 libwayland-client
// （后端运行时 dlopen 它 —— 与 X11 后端同一姿态，见 CONVENTIONS.md §10 第 1 条）。
#pragma once

#include <cstdint>

namespace st::shell::wayland::proto {

// ————————————————————————————————————————————————————————————————————————————
// libwayland 的 ABI 结构
//
// 按 `wayland-util.h` 逐字段抄写。这些对象由**我们**构造（不是从库里取），
// 再传给 `wl_proxy_marshal_flags`——它用签名与 types 表做参数自证。
// ⚠ 字段顺序即 ABI：改了顺序，libwayland 会以错误的方式解释这些表。
// ————————————————————————————————————————————————————————————————————————————

struct Interface;

struct Message {
  const char* name{nullptr};
  /// 签名串：类型码序列（可空参数前缀 `?`；消息 since>1 时**首字符是版本号**）。
  const char* signature{nullptr};
  /// `Interface*` 表中的起始位置（只有 object/new_id 参数占非空槽位）。
  const Interface* const* types{nullptr};
};

struct Interface {
  const char* name{nullptr};
  int version{0};
  int request_count{0};
  const Message* requests{nullptr};
  int event_count{0};
  const Message* events{nullptr};
};

// ————————————————————————————————————————————————————————————————————————————
// 接口声明（互相引用，故先全部前置声明）
// ————————————————————————————————————————————————————————————————————————————

extern const Interface kwl_display;
extern const Interface kwl_registry;
extern const Interface kwl_callback;
extern const Interface kwl_compositor;
extern const Interface kwl_shm_pool;
extern const Interface kwl_shm;
extern const Interface kwl_buffer;
extern const Interface kwl_data_offer;
extern const Interface kwl_data_source;
extern const Interface kwl_data_device;
extern const Interface kwl_data_device_manager;
extern const Interface kwl_shell;
extern const Interface kwl_shell_surface;
extern const Interface kwl_surface;
extern const Interface kwl_seat;
extern const Interface kwl_pointer;
extern const Interface kwl_keyboard;
extern const Interface kwl_touch;
extern const Interface kwl_output;
extern const Interface kwl_region;
extern const Interface kwl_subcompositor;
extern const Interface kwl_subsurface;
extern const Interface kwl_fixes;
extern const Interface kxdg_wm_base;
extern const Interface kxdg_positioner;
extern const Interface kxdg_surface;
extern const Interface kxdg_toplevel;
extern const Interface kxdg_popup;
extern const Interface kzxdg_decoration_manager_v1;
extern const Interface kzxdg_toplevel_decoration_v1;

// ————————————————————————————————————————————————————————————————————————————
// 类型表（所有消息共用一张扁平表；表首是 8 个空槽 = 官方 `null_run_length`）
// ————————————————————————————————————————————————————————————————————————————

namespace detail {

inline const Interface* const kTypes[] = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &kwl_callback,
    &kwl_registry,
    &kwl_surface,
    &kwl_region,
    &kwl_buffer,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &kwl_shm_pool,
    nullptr,
    nullptr,
    &kwl_data_source,
    &kwl_surface,
    &kwl_surface,
    nullptr,
    &kwl_data_source,
    nullptr,
    &kwl_data_offer,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    &kwl_data_offer,
    &kwl_data_offer,
    &kwl_data_source,
    &kwl_data_device,
    &kwl_seat,
    &kwl_shell_surface,
    &kwl_surface,
    &kwl_seat,
    nullptr,
    &kwl_seat,
    nullptr,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &kwl_output,
    &kwl_seat,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    nullptr,
    &kwl_output,
    &kwl_buffer,
    nullptr,
    nullptr,
    &kwl_callback,
    &kwl_region,
    &kwl_region,
    &kwl_callback,
    &kwl_output,
    &kwl_output,
    &kwl_pointer,
    &kwl_keyboard,
    &kwl_touch,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    nullptr,
    &kwl_surface,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    &kwl_surface,
    nullptr,
    nullptr,
    nullptr,
    &kwl_subsurface,
    &kwl_surface,
    &kwl_surface,
    &kwl_surface,
    &kwl_surface,
    &kwl_registry,
    &kwl_registry,
    nullptr,
    &kxdg_positioner,
    &kxdg_surface,
    &kwl_surface,
    &kxdg_toplevel,
    &kxdg_popup,
    &kxdg_surface,
    &kxdg_positioner,
    &kxdg_toplevel,
    &kwl_seat,
    nullptr,
    nullptr,
    nullptr,
    &kwl_seat,
    nullptr,
    &kwl_seat,
    nullptr,
    nullptr,
    &kwl_output,
    &kwl_seat,
    nullptr,
    &kxdg_positioner,
    nullptr,
    &kzxdg_toplevel_decoration_v1,
    &kxdg_toplevel,
};

inline constexpr int kNullRun = 8;

}  // namespace detail

// ————————————————————————————————————————————————————————————————————————————
// 消息表与接口定义（消息顺序 = 协议 opcode 顺序，不可重排）
// ————————————————————————————————————————————————————————————————————————————

inline const Message kwl_displayRequests[] = {
    {"sync", "n", detail::kTypes + 8},
    {"get_registry", "n", detail::kTypes + 9},
};

inline const Message kwl_displayEvents[] = {
    {"error", "ous", detail::kTypes + 0},
    {"delete_id", "u", detail::kTypes + 0},
};

inline const Interface kwl_display = {
    "wl_display", 1,
    2, kwl_displayRequests,
    2, kwl_displayEvents,
};

inline const Message kwl_registryRequests[] = {
    {"bind", "usun", detail::kTypes + 0},
};

inline const Message kwl_registryEvents[] = {
    {"global", "usu", detail::kTypes + 0},
    {"global_remove", "u", detail::kTypes + 0},
};

inline const Interface kwl_registry = {
    "wl_registry", 1,
    1, kwl_registryRequests,
    2, kwl_registryEvents,
};

inline const Message kwl_callbackEvents[] = {
    {"done", "u", detail::kTypes + 0},
};

inline const Interface kwl_callback = {
    "wl_callback", 1,
    0, nullptr,
    1, kwl_callbackEvents,
};

inline const Message kwl_compositorRequests[] = {
    {"create_surface", "n", detail::kTypes + 10},
    {"create_region", "n", detail::kTypes + 11},
    {"release", "7", detail::kTypes + 0},
};

inline const Interface kwl_compositor = {
    "wl_compositor", 7,
    3, kwl_compositorRequests,
    0, nullptr,
};

inline const Message kwl_shm_poolRequests[] = {
    {"create_buffer", "niiiiu", detail::kTypes + 12},
    {"destroy", "", detail::kTypes + 0},
    {"resize", "i", detail::kTypes + 0},
};

inline const Interface kwl_shm_pool = {
    "wl_shm_pool", 3,
    3, kwl_shm_poolRequests,
    0, nullptr,
};

inline const Message kwl_shmRequests[] = {
    {"create_pool", "nhi", detail::kTypes + 18},
    {"release", "2", detail::kTypes + 0},
};

inline const Message kwl_shmEvents[] = {
    {"format", "u", detail::kTypes + 0},
};

inline const Interface kwl_shm = {
    "wl_shm", 3,
    2, kwl_shmRequests,
    1, kwl_shmEvents,
};

inline const Message kwl_bufferRequests[] = {
    {"destroy", "", detail::kTypes + 0},
};

inline const Message kwl_bufferEvents[] = {
    {"release", "", detail::kTypes + 0},
};

inline const Interface kwl_buffer = {
    "wl_buffer", 1,
    1, kwl_bufferRequests,
    1, kwl_bufferEvents,
};

inline const Message kwl_data_offerRequests[] = {
    {"accept", "u?s", detail::kTypes + 0},
    {"receive", "sh", detail::kTypes + 0},
    {"destroy", "", detail::kTypes + 0},
    {"finish", "3", detail::kTypes + 0},
    {"set_actions", "3uu", detail::kTypes + 0},
};

inline const Message kwl_data_offerEvents[] = {
    {"offer", "s", detail::kTypes + 0},
    {"source_actions", "3u", detail::kTypes + 0},
    {"action", "3u", detail::kTypes + 0},
};

inline const Interface kwl_data_offer = {
    "wl_data_offer", 4,
    5, kwl_data_offerRequests,
    3, kwl_data_offerEvents,
};

inline const Message kwl_data_sourceRequests[] = {
    {"offer", "s", detail::kTypes + 0},
    {"destroy", "", detail::kTypes + 0},
    {"set_actions", "3u", detail::kTypes + 0},
};

inline const Message kwl_data_sourceEvents[] = {
    {"target", "?s", detail::kTypes + 0},
    {"send", "sh", detail::kTypes + 0},
    {"cancelled", "", detail::kTypes + 0},
    {"dnd_drop_performed", "3", detail::kTypes + 0},
    {"dnd_finished", "3", detail::kTypes + 0},
    {"action", "3u", detail::kTypes + 0},
};

inline const Interface kwl_data_source = {
    "wl_data_source", 4,
    3, kwl_data_sourceRequests,
    6, kwl_data_sourceEvents,
};

inline const Message kwl_data_deviceRequests[] = {
    {"start_drag", "?oo?ou", detail::kTypes + 21},
    {"set_selection", "?ou", detail::kTypes + 25},
    {"release", "2", detail::kTypes + 0},
};

inline const Message kwl_data_deviceEvents[] = {
    {"data_offer", "n", detail::kTypes + 27},
    {"enter", "uoff?o", detail::kTypes + 28},
    {"leave", "", detail::kTypes + 0},
    {"motion", "uff", detail::kTypes + 0},
    {"drop", "", detail::kTypes + 0},
    {"selection", "?o", detail::kTypes + 33},
};

inline const Interface kwl_data_device = {
    "wl_data_device", 4,
    3, kwl_data_deviceRequests,
    6, kwl_data_deviceEvents,
};

inline const Message kwl_data_device_managerRequests[] = {
    {"create_data_source", "n", detail::kTypes + 34},
    {"get_data_device", "no", detail::kTypes + 35},
    {"release", "4", detail::kTypes + 0},
};

inline const Interface kwl_data_device_manager = {
    "wl_data_device_manager", 4,
    3, kwl_data_device_managerRequests,
    0, nullptr,
};

inline const Message kwl_shellRequests[] = {
    {"get_shell_surface", "no", detail::kTypes + 37},
};

inline const Interface kwl_shell = {
    "wl_shell", 1,
    1, kwl_shellRequests,
    0, nullptr,
};

inline const Message kwl_shell_surfaceRequests[] = {
    {"pong", "u", detail::kTypes + 0},
    {"move", "ou", detail::kTypes + 39},
    {"resize", "ouu", detail::kTypes + 41},
    {"set_toplevel", "", detail::kTypes + 0},
    {"set_transient", "oiiu", detail::kTypes + 44},
    {"set_fullscreen", "uu?o", detail::kTypes + 48},
    {"set_popup", "ouoiiu", detail::kTypes + 51},
    {"set_maximized", "?o", detail::kTypes + 57},
    {"set_title", "s", detail::kTypes + 0},
    {"set_class", "s", detail::kTypes + 0},
};

inline const Message kwl_shell_surfaceEvents[] = {
    {"ping", "u", detail::kTypes + 0},
    {"configure", "uii", detail::kTypes + 0},
    {"popup_done", "", detail::kTypes + 0},
};

inline const Interface kwl_shell_surface = {
    "wl_shell_surface", 1,
    10, kwl_shell_surfaceRequests,
    3, kwl_shell_surfaceEvents,
};

inline const Message kwl_surfaceRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"attach", "?oii", detail::kTypes + 58},
    {"damage", "iiii", detail::kTypes + 0},
    {"frame", "n", detail::kTypes + 61},
    {"set_opaque_region", "?o", detail::kTypes + 62},
    {"set_input_region", "?o", detail::kTypes + 63},
    {"commit", "", detail::kTypes + 0},
    {"set_buffer_transform", "2i", detail::kTypes + 0},
    {"set_buffer_scale", "3i", detail::kTypes + 0},
    {"damage_buffer", "4iiii", detail::kTypes + 0},
    {"offset", "5ii", detail::kTypes + 0},
    {"get_release", "7n", detail::kTypes + 64},
};

inline const Message kwl_surfaceEvents[] = {
    {"enter", "o", detail::kTypes + 65},
    {"leave", "o", detail::kTypes + 66},
    {"preferred_buffer_scale", "6i", detail::kTypes + 0},
    {"preferred_buffer_transform", "6u", detail::kTypes + 0},
};

inline const Interface kwl_surface = {
    "wl_surface", 7,
    12, kwl_surfaceRequests,
    4, kwl_surfaceEvents,
};

inline const Message kwl_seatRequests[] = {
    {"get_pointer", "n", detail::kTypes + 67},
    {"get_keyboard", "n", detail::kTypes + 68},
    {"get_touch", "n", detail::kTypes + 69},
    {"release", "5", detail::kTypes + 0},
};

inline const Message kwl_seatEvents[] = {
    {"capabilities", "u", detail::kTypes + 0},
    {"name", "2s", detail::kTypes + 0},
};

inline const Interface kwl_seat = {
    "wl_seat", 11,
    4, kwl_seatRequests,
    2, kwl_seatEvents,
};

inline const Message kwl_pointerRequests[] = {
    {"set_cursor", "u?oii", detail::kTypes + 70},
    {"release", "3", detail::kTypes + 0},
};

inline const Message kwl_pointerEvents[] = {
    {"enter", "uoff", detail::kTypes + 74},
    {"leave", "uo", detail::kTypes + 78},
    {"motion", "uff", detail::kTypes + 0},
    {"button", "uuuu", detail::kTypes + 0},
    {"axis", "uuf", detail::kTypes + 0},
    {"frame", "5", detail::kTypes + 0},
    {"axis_source", "5u", detail::kTypes + 0},
    {"axis_stop", "5uu", detail::kTypes + 0},
    {"axis_discrete", "5ui", detail::kTypes + 0},
    {"axis_value120", "8ui", detail::kTypes + 0},
    {"axis_relative_direction", "9uu", detail::kTypes + 0},
    {"warp", "11ff", detail::kTypes + 0},
};

inline const Interface kwl_pointer = {
    "wl_pointer", 11,
    2, kwl_pointerRequests,
    12, kwl_pointerEvents,
};

inline const Message kwl_keyboardRequests[] = {
    {"release", "3", detail::kTypes + 0},
};

inline const Message kwl_keyboardEvents[] = {
    {"keymap", "uhu", detail::kTypes + 0},
    {"enter", "uoa", detail::kTypes + 80},
    {"leave", "uo", detail::kTypes + 83},
    {"key", "uuuu", detail::kTypes + 0},
    {"modifiers", "uuuuu", detail::kTypes + 0},
    {"repeat_info", "4ii", detail::kTypes + 0},
};

inline const Interface kwl_keyboard = {
    "wl_keyboard", 11,
    1, kwl_keyboardRequests,
    6, kwl_keyboardEvents,
};

inline const Message kwl_touchRequests[] = {
    {"release", "3", detail::kTypes + 0},
};

inline const Message kwl_touchEvents[] = {
    {"down", "uuoiff", detail::kTypes + 85},
    {"up", "uui", detail::kTypes + 0},
    {"motion", "uiff", detail::kTypes + 0},
    {"frame", "", detail::kTypes + 0},
    {"cancel", "", detail::kTypes + 0},
    {"shape", "6iff", detail::kTypes + 0},
    {"orientation", "6if", detail::kTypes + 0},
};

inline const Interface kwl_touch = {
    "wl_touch", 11,
    1, kwl_touchRequests,
    7, kwl_touchEvents,
};

inline const Message kwl_outputRequests[] = {
    {"release", "3", detail::kTypes + 0},
};

inline const Message kwl_outputEvents[] = {
    {"geometry", "iiiiissi", detail::kTypes + 0},
    {"mode", "uiii", detail::kTypes + 0},
    {"done", "2", detail::kTypes + 0},
    {"scale", "2i", detail::kTypes + 0},
    {"name", "4s", detail::kTypes + 0},
    {"description", "4s", detail::kTypes + 0},
};

inline const Interface kwl_output = {
    "wl_output", 4,
    1, kwl_outputRequests,
    6, kwl_outputEvents,
};

inline const Message kwl_regionRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"add", "iiii", detail::kTypes + 0},
    {"subtract", "iiii", detail::kTypes + 0},
};

inline const Interface kwl_region = {
    "wl_region", 7,
    3, kwl_regionRequests,
    0, nullptr,
};

inline const Message kwl_subcompositorRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"get_subsurface", "noo", detail::kTypes + 91},
};

inline const Interface kwl_subcompositor = {
    "wl_subcompositor", 1,
    2, kwl_subcompositorRequests,
    0, nullptr,
};

inline const Message kwl_subsurfaceRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"set_position", "ii", detail::kTypes + 0},
    {"place_above", "o", detail::kTypes + 94},
    {"place_below", "o", detail::kTypes + 95},
    {"set_sync", "", detail::kTypes + 0},
    {"set_desync", "", detail::kTypes + 0},
};

inline const Interface kwl_subsurface = {
    "wl_subsurface", 1,
    6, kwl_subsurfaceRequests,
    0, nullptr,
};

inline const Message kwl_fixesRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"destroy_registry", "o", detail::kTypes + 96},
    {"ack_global_remove", "2ou", detail::kTypes + 97},
};

inline const Interface kwl_fixes = {
    "wl_fixes", 2,
    3, kwl_fixesRequests,
    0, nullptr,
};

inline const Message kxdg_wm_baseRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"create_positioner", "n", detail::kTypes + 99},
    {"get_xdg_surface", "no", detail::kTypes + 100},
    {"pong", "u", detail::kTypes + 0},
};

inline const Message kxdg_wm_baseEvents[] = {
    {"ping", "u", detail::kTypes + 0},
};

inline const Interface kxdg_wm_base = {
    "xdg_wm_base", 7,
    4, kxdg_wm_baseRequests,
    1, kxdg_wm_baseEvents,
};

inline const Message kxdg_positionerRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"set_size", "ii", detail::kTypes + 0},
    {"set_anchor_rect", "iiii", detail::kTypes + 0},
    {"set_anchor", "u", detail::kTypes + 0},
    {"set_gravity", "u", detail::kTypes + 0},
    {"set_constraint_adjustment", "u", detail::kTypes + 0},
    {"set_offset", "ii", detail::kTypes + 0},
    {"set_reactive", "3", detail::kTypes + 0},
    {"set_parent_size", "3ii", detail::kTypes + 0},
    {"set_parent_configure", "3u", detail::kTypes + 0},
};

inline const Interface kxdg_positioner = {
    "xdg_positioner", 7,
    10, kxdg_positionerRequests,
    0, nullptr,
};

inline const Message kxdg_surfaceRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"get_toplevel", "n", detail::kTypes + 102},
    {"get_popup", "n?oo", detail::kTypes + 103},
    {"set_window_geometry", "iiii", detail::kTypes + 0},
    {"ack_configure", "u", detail::kTypes + 0},
};

inline const Message kxdg_surfaceEvents[] = {
    {"configure", "u", detail::kTypes + 0},
};

inline const Interface kxdg_surface = {
    "xdg_surface", 7,
    5, kxdg_surfaceRequests,
    1, kxdg_surfaceEvents,
};

inline const Message kxdg_toplevelRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"set_parent", "?o", detail::kTypes + 106},
    {"set_title", "s", detail::kTypes + 0},
    {"set_app_id", "s", detail::kTypes + 0},
    {"show_window_menu", "ouii", detail::kTypes + 107},
    {"move", "ou", detail::kTypes + 111},
    {"resize", "ouu", detail::kTypes + 113},
    {"set_max_size", "ii", detail::kTypes + 0},
    {"set_min_size", "ii", detail::kTypes + 0},
    {"set_maximized", "", detail::kTypes + 0},
    {"unset_maximized", "", detail::kTypes + 0},
    {"set_fullscreen", "?o", detail::kTypes + 116},
    {"unset_fullscreen", "", detail::kTypes + 0},
    {"set_minimized", "", detail::kTypes + 0},
};

inline const Message kxdg_toplevelEvents[] = {
    {"configure", "iia", detail::kTypes + 0},
    {"close", "", detail::kTypes + 0},
    {"configure_bounds", "4ii", detail::kTypes + 0},
    {"wm_capabilities", "5a", detail::kTypes + 0},
};

inline const Interface kxdg_toplevel = {
    "xdg_toplevel", 7,
    14, kxdg_toplevelRequests,
    4, kxdg_toplevelEvents,
};

inline const Message kxdg_popupRequests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"grab", "ou", detail::kTypes + 117},
    {"reposition", "3ou", detail::kTypes + 119},
};

inline const Message kxdg_popupEvents[] = {
    {"configure", "iiii", detail::kTypes + 0},
    {"popup_done", "", detail::kTypes + 0},
    {"repositioned", "3u", detail::kTypes + 0},
};

inline const Interface kxdg_popup = {
    "xdg_popup", 7,
    3, kxdg_popupRequests,
    3, kxdg_popupEvents,
};

inline const Message kzxdg_decoration_manager_v1Requests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"get_toplevel_decoration", "no", detail::kTypes + 121},
};

inline const Interface kzxdg_decoration_manager_v1 = {
    "zxdg_decoration_manager_v1", 2,
    2, kzxdg_decoration_manager_v1Requests,
    0, nullptr,
};

inline const Message kzxdg_toplevel_decoration_v1Requests[] = {
    {"destroy", "", detail::kTypes + 0},
    {"set_mode", "u", detail::kTypes + 0},
    {"unset_mode", "", detail::kTypes + 0},
};

inline const Message kzxdg_toplevel_decoration_v1Events[] = {
    {"configure", "u", detail::kTypes + 0},
};

inline const Interface kzxdg_toplevel_decoration_v1 = {
    "zxdg_toplevel_decoration_v1", 2,
    3, kzxdg_toplevel_decoration_v1Requests,
    1, kzxdg_toplevel_decoration_v1Events,
};

}  // namespace st::shell::wayland::proto
