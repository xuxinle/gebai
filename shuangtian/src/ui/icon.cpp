#include "st/ui/icon.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace st::ui {
namespace {

/// 图标表（24×24 视图框；描边式为主，`filled` 为实心）。
///
/// ## 坐标不必“画满”视框——绘制时按**各自墨迹**做光学归一化
///
/// 每个图标只需画成它自己的比例正确；`Icon::path` 会把它的墨迹
/// 等比缩放到统一的可视尺寸并居中（参见其注释），所以这张表里
/// “这个字形占 24 里的多少”**只影响起始相位**，不影响最终光感大小。
/// 好处是修正一个图标不会牵动另一个，也不必手算 73 组缩放。
inline constexpr float kOpticalTargetPx = 17.0f;
/// 光学放大的上界：小字形（`chevron`/`minus`/`more-*`）需要明显放大，
/// 但不把它们放到超过视框（否则笔画会顶到邻居）。
inline constexpr float kOpticalMaxScale = 1.9f;
/// 光学缩小的下界：大字形要收一点才能与其余对齐；下界只防病态数据，
/// 正常数据离它很远（实测最大那枚只需 0.78×）。
inline constexpr float kOpticalMinScale = 0.7f;

inline constexpr auto kIcons = std::to_array<IconGlyph>({
    {"check", "M4 12.5 L9.5 18 L20 6", 2.2f, false},
    {"close", "M6 6 L18 18 M18 6 L6 18", 2.2f, false},
    {"plus", "M12 5 L12 19 M5 12 L19 12", 2.2f, false},
    {"minus", "M5 12 L19 12", 2.2f, false},
    {"chevron-down", "M6 9.5 L12 15.5 L18 9.5", 2.2f, false},
    {"chevron-up", "M6 14.5 L12 8.5 L18 14.5", 2.2f, false},
    {"chevron-left", "M14.5 6 L8.5 12 L14.5 18", 2.2f, false},
    {"chevron-right", "M9.5 6 L15.5 12 L9.5 18", 2.2f, false},
    {"arrow-right", "M4 12 L19 12 M13 6 L19 12 L13 18", 2.0f, false},
    {"arrow-left", "M20 12 L5 12 M11 6 L5 12 L11 18", 2.0f, false},
    {"arrow-up", "M12 20 L12 5 M6 11 L12 5 L18 11", 2.0f, false},
    {"arrow-down", "M12 4 L12 19 M6 13 L12 19 L18 13", 2.0f, false},
    {"search", "M17.5 17.5 L21 21 M10.5 3.5 C14.4 3.5 17.5 6.6 17.5 10.5 C17.5 14.4 14.4 17.5 "
               "10.5 17.5 C6.6 17.5 3.5 14.4 3.5 10.5 C3.5 6.6 6.6 3.5 10.5 3.5 Z",
     2.0f, false},
    {"settings", "M12 8.6 C13.9 8.6 15.4 10.1 15.4 12 C15.4 13.9 13.9 15.4 12 15.4 C10.1 15.4 "
                 "8.6 13.9 8.6 12 C8.6 10.1 10.1 8.6 12 8.6 Z M12 2.5 L13.6 4.3 L16 3.7 L16.8 6.1 "
                 "L19.2 6.5 L19 9 L21 10.5 L19.6 12.6 L21 14.7 L19 16.2 L19.2 18.7 L16.8 19.1 "
                 "L16 21.5 L13.6 20.9 L12 22.7 L10.4 20.9 L8 21.5 L7.2 19.1 L4.8 18.7 L5 16.2 "
                 "L3 14.7 L4.4 12.6 L3 10.5 L5 9 L4.8 6.5 L7.2 6.1 L8 3.7 L10.4 4.3 Z",
     1.7f, false},
    {"user", "M12 11.5 C14.5 11.5 16.5 9.5 16.5 7 C16.5 4.5 14.5 2.5 12 2.5 C9.5 2.5 7.5 4.5 7.5 "
             "7 C7.5 9.5 9.5 11.5 12 11.5 Z M4 21.5 C4 16.9 7.6 14 12 14 C16.4 14 20 16.9 20 21.5",
     1.9f, false},
    {"users", "M9 11 C11.2 11 13 9.2 13 7 C13 4.8 11.2 3 9 3 C6.8 3 5 4.8 5 7 C5 9.2 6.8 11 9 11 "
              "Z M2 20.5 C2 16.6 5.1 14 9 14 C12.9 14 16 16.6 16 20.5 M16 4.2 C17.7 4.7 19 6.2 19 "
              "8 C19 9.8 17.7 11.3 16 11.8 M17.5 14.4 C20 15.2 22 17.4 22 20.5",
     1.8f, false},
    {"home", "M3.5 10.5 L12 3.5 L20.5 10.5 M6 9.5 L6 20 L18 20 L18 9.5 M10 20 L10 14 L14 14 L14 "
             "20",
     1.9f, false},
    {"folder", "M3 6.5 C3 5.7 3.7 5 4.5 5 L9.5 5 L11.5 7.5 L19.5 7.5 C20.3 7.5 21 8.2 21 9 L21 "
               "17.5 C21 18.3 20.3 19 19.5 19 L4.5 19 C3.7 19 3 18.3 3 17.5 Z",
     1.9f, false},
    {"file", "M13.5 3 L6.5 3 C5.7 3 5 3.7 5 4.5 L5 19.5 C5 20.3 5.7 21 6.5 21 L17.5 21 C18.3 21 "
             "19 20.3 19 19.5 L19 8.5 Z M13.5 3 L13.5 8.5 L19 8.5",
     1.8f, false},
    {"code", "M8.5 7 L3.5 12 L8.5 17 M15.5 7 L20.5 12 L15.5 17 M13.5 4.5 L10.5 19.5", 2.0f, false},
    {"terminal", "M3.5 4.5 L20.5 4.5 C20.9 4.5 21 4.7 21 5 L21 19 C21 19.3 20.9 19.5 20.5 19.5 L3.5 "
                 "19.5 C3.1 19.5 3 19.3 3 19 L3 5 C3 4.7 3.1 4.5 3.5 4.5 Z M6.5 9 L10 12.5 L6.5 16 "
                 "M12.5 16 L17 16",
     1.8f, false},
    {"play", "M7 4.5 L19 12 L7 19.5 Z", 1.8f, false},
    {"pause", "M8.5 4.5 L8.5 19.5 M15.5 4.5 L15.5 19.5", 2.4f, false},
    {"refresh", "M20 12 C20 16.4 16.4 20 12 20 C7.6 20 4 16.4 4 12 C4 7.6 7.6 4 12 4 C15.4 4 "
               "18.3 6.1 19.5 9 M19.5 3.5 L19.5 9 L14 9",
     1.9f, false},
    {"download", "M12 3.5 L12 15 M6.5 9.5 L12 15 L17.5 9.5 M4.5 19.5 L19.5 19.5", 1.9f, false},
    {"upload", "M12 20.5 L12 9 M6.5 14.5 L12 9 L17.5 14.5 M4.5 4.5 L19.5 4.5", 1.9f, false},
    {"trash", "M4.5 6.5 L19.5 6.5 M9.5 6.5 L9.5 4.5 C9.5 4.1 9.8 3.8 10.2 3.8 L13.8 3.8 C14.2 "
              "3.8 14.5 4.1 14.5 4.5 L14.5 6.5 M6.5 6.5 L7.5 20 C7.5 20.4 7.8 20.7 8.2 20.7 "
              "L15.8 20.7 C16.2 20.7 16.5 20.4 16.5 20 L17.5 6.5",
     1.8f, false},
    {"edit", "M4 20 L4 16 L15.5 4.5 L19.5 8.5 L8 20 Z M13.5 6.5 L17.5 10.5", 1.8f, false},
    {"copy", "M9 9 L9 4.5 C9 4.1 9.3 3.8 9.7 3.8 L19.3 3.8 C19.7 3.8 20 4.1 20 4.5 L20 14.5 C20 "
             "14.9 19.7 15.2 19.3 15.2 L15 15.2 M4.5 9.5 L4.5 19.5 C4.5 19.9 4.8 20.2 5.2 20.2 "
             "L14.8 20.2 C15.2 20.2 15.5 19.9 15.5 19.5 L15.5 9.5 C15.5 9.1 15.2 8.8 14.8 8.8 "
             "L5.2 8.8 C4.8 8.8 4.5 9.1 4.5 9.5 Z",
     1.7f, false},
    {"link", "M10 13.5 C11.4 14.9 13.6 14.9 15 13.5 L18.5 10 C19.9 8.6 19.9 6.4 18.5 5 C17.1 "
             "3.6 14.9 3.6 13.5 5 L12.5 6 M14 10.5 C12.6 9.1 10.4 9.1 9 10.5 L5.5 14 C4.1 15.4 "
             "4.1 17.6 5.5 19 C6.9 20.4 9.1 20.4 10.5 19 L11.5 18",
     1.8f, false},
    {"lock", "M7 10.5 L7 7.5 C7 4.7 9.2 2.5 12 2.5 C14.8 2.5 17 4.7 17 7.5 L17 10.5 M5.5 10.5 "
             "L18.5 10.5 C19 10.5 19.4 10.9 19.4 11.4 L19.4 20 C19.4 20.5 19 20.9 18.5 20.9 L5.5 "
             "20.9 C5 20.9 4.6 20.5 4.6 20 L4.6 11.4 C4.6 10.9 5 10.5 5.5 10.5 Z",
     1.8f, false},
    {"bell", "M12 3 C8.7 3 6 5.7 6 9 L6 14 L4 17 L20 17 L18 14 L18 9 C18 5.7 15.3 3 12 3 Z M9.5 "
             "20 C10 21 11 21.5 12 21.5 C13 21.5 14 21 14.5 20",
     1.8f, false},
    {"calendar", "M4.5 6.5 L19.5 6.5 C19.9 6.5 20.2 6.8 20.2 7.2 L20.2 19.5 C20.2 19.9 19.9 "
                 "20.2 19.5 20.2 L4.5 20.2 C4.1 20.2 3.8 19.9 3.8 19.5 L3.8 7.2 C3.8 6.8 4.1 6.5 "
                 "4.5 6.5 Z M8 3.5 L8 8 M16 3.5 L16 8 M3.8 11 L20.2 11",
     1.8f, false},
    {"clock", "M12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 C7.3 20.5 3.5 16.7 "
              "3.5 12 C3.5 7.3 7.3 3.5 12 3.5 Z M12 7.5 L12 12 L15.5 14",
     1.8f, false},
    {"star", "M12 3.5 L14.6 9 L20.5 9.8 L16.2 14 L17.3 20 L12 17.1 L6.7 20 L7.8 14 L3.5 9.8 "
             "L9.4 9 Z",
     1.7f, false},
    {"heart", "M12 20.5 C12 20.5 3.5 15.5 3.5 9.5 C3.5 6.7 5.7 4.5 8.3 4.5 C10 4.5 11.4 5.4 12 "
              "6.8 C12.6 5.4 14 4.5 15.7 4.5 C18.3 4.5 20.5 6.7 20.5 9.5 C20.5 15.5 12 20.5 12 "
              "20.5 Z",
     1.7f, false},
    {"info", "M12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 C7.3 20.5 3.5 16.7 "
             "3.5 12 C3.5 7.3 7.3 3.5 12 3.5 Z M12 11 L12 16.5 M12 7.6 L12 8",
     1.8f, false},
    {"warning", "M12 3.5 L21.5 20 L2.5 20 Z M12 9 L12 14 M12 16.4 L12 17", 1.8f, false},
    {"error", "M12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 C7.3 20.5 3.5 16.7 "
              "3.5 12 C3.5 7.3 7.3 3.5 12 3.5 Z M8.5 8.5 L15.5 15.5 M15.5 8.5 L8.5 15.5",
     1.8f, false},
    {"success", "M12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 C7.3 20.5 3.5 "
                "16.7 3.5 12 C3.5 7.3 7.3 3.5 12 3.5 Z M8 12.3 L11 15.3 L16.3 9",
     1.8f, false},
    {"moon", "M20 14.5 C18.7 15.2 17.2 15.6 15.6 15.6 C10.7 15.6 6.7 11.7 6.7 6.8 C6.7 5.3 7.1 "
             "3.9 7.8 2.7 C4.6 4 2.4 7.2 2.4 10.9 C2.4 16 6.6 20.2 11.7 20.2 C15.5 20.2 18.8 "
             "17.9 20 14.5 Z",
     1.7f, false},
    {"sun", "M12 7.5 C14.5 7.5 16.5 9.5 16.5 12 C16.5 14.5 14.5 16.5 12 16.5 C9.5 16.5 7.5 14.5 "
            "7.5 12 C7.5 9.5 9.5 7.5 12 7.5 Z M12 2 L12 4 M12 20 L12 22 M4.2 4.2 L5.6 5.6 M18.4 "
            "18.4 L19.8 19.8 M2 12 L4 12 M20 12 L22 12 M4.2 19.8 L5.6 18.4 M18.4 5.6 L19.8 4.2",
     1.8f, false},
    {"menu", "M4 7 L20 7 M4 12 L20 12 M4 17 L20 17", 2.0f, false},
    {"grid", "M4 4 L10.5 4 L10.5 10.5 L4 10.5 Z M13.5 4 L20 4 L20 10.5 L13.5 10.5 Z M4 13.5 "
             "L10.5 13.5 L10.5 20 L4 20 Z M13.5 13.5 L20 13.5 L20 20 L13.5 20 Z",
     1.7f, false},
    {"list", "M4 6.5 L4.02 6.5 M8 6.5 L20 6.5 M4 12 L4.02 12 M8 12 L20 12 M4 17.5 L4.02 17.5 M8 "
             "17.5 L20 17.5",
     2.0f, false},
    {"filter", "M3.5 5 L20.5 5 L14 12.5 L14 19 L10 21 L10 12.5 Z", 1.7f, false},
    {"external", "M14 4 L20 4 L20 10 M20 4 L12.5 11.5 M17.5 14.5 L17.5 19.5 C17.5 19.8 17.3 20 "
                 "17 20 L4.5 20 C4.2 20 4 19.8 4 19.5 L4 7 C4 6.7 4.2 6.5 4.5 6.5 L9.5 6.5",
     1.8f, false},
    {"more-horizontal", "M6 12 L6.02 12 M12 12 L12.02 12 M18 12 L18.02 12", 2.6f, false},
    {"more-vertical", "M12 6 L12.02 6 M12 12 L12.02 12 M12 18 L12.02 18", 2.6f, false},
    {"sparkles", "M12 3 L13.6 8.4 L19 10 L13.6 11.6 L12 17 L10.4 11.6 L5 10 L10.4 8.4 Z M18.5 "
                 "15.5 L19.2 17.8 L21.5 18.5 L19.2 19.2 L18.5 21.5 L17.8 19.2 L15.5 18.5 L17.8 "
                 "17.8 Z",
     1.5f, false},
    {"cpu", "M8.5 8.5 L15.5 8.5 L15.5 15.5 L8.5 15.5 Z M5.5 5.5 L18.5 5.5 C18.9 5.5 19.2 5.8 "
            "19.2 6.2 L19.2 18.8 C19.2 19.2 18.9 19.5 18.5 19.5 L5.5 19.5 C5.1 19.5 4.8 19.2 "
            "4.8 18.8 L4.8 6.2 C4.8 5.8 5.1 5.5 5.5 5.5 Z M9 2.5 L9 5.5 M15 2.5 L15 5.5 M9 19.5 "
            "L9 22.5 M15 19.5 L15 22.5 M2.5 9 L5.5 9 M2.5 15 L5.5 15 M19.5 9 L22.5 9 M19.5 15 "
            "L22.5 15",
     1.6f, false},
    {"database", "M12 3 C16.4 3 20 4.3 20 6 C20 7.7 16.4 9 12 9 C7.6 9 4 7.7 4 6 C4 4.3 7.6 3 12 "
                 "3 Z M4 6 L4 12 C4 13.7 7.6 15 12 15 C16.4 15 20 13.7 20 12 L20 6 M4 12 L4 18 C4 "
                 "19.7 7.6 21 12 21 C16.4 21 20 19.7 20 18 L20 12",
     1.7f, false},
    {"cloud", "M7.5 19 C4.7 19 2.5 16.8 2.5 14 C2.5 11.4 4.4 9.3 6.9 9 C7.7 6.1 10.2 4 13.2 4 "
              "C16.8 4 19.8 6.9 19.8 10.5 C19.8 10.7 19.8 10.9 19.8 11.1 C21.1 11.8 22 13.1 22 "
              "14.6 C22 16.8 20.2 18.6 18 18.6 L7.5 19 Z",
     1.7f, false},
    {"bolt", "M13.5 2.5 L5.5 14 L11.5 14 L10.5 21.5 L18.5 10 L12.5 10 Z", 1.6f, false},
    {"shield", "M12 2.5 L20 5.5 L20 11.5 C20 16.4 16.6 20.4 12 21.5 C7.4 20.4 4 16.4 4 11.5 L4 "
               "5.5 Z",
     1.7f, false},
    {"key", "M15.5 3.5 C18.8 3.5 21.5 6.2 21.5 9.5 C21.5 12.8 18.8 15.5 15.5 15.5 C14.9 15.5 "
            "14.4 15.4 13.9 15.3 L11.5 17.7 L9 17.7 L9 20.2 L5.5 20.2 L5.5 16.7 L12.7 9.5 C12.6 "
            "9 12.5 8.5 12.5 7.9 M16.8 7.5 L16.82 7.5",
     1.7f, false},
    {"tag", "M11 3.5 L20.5 13 L13 20.5 L3.5 11 L3.5 3.5 Z M7.5 7.5 L7.52 7.5", 1.7f, false},
    {"image", "M3.5 5 L20.5 5 C20.9 5 21 5.2 21 5.5 L21 18.5 C21 18.8 20.9 19 20.5 19 L3.5 19 "
              "C3.1 19 3 18.8 3 18.5 L3 5.5 C3 5.2 3.1 5 3.5 5 Z M8.5 10 C9.3 10 10 9.3 10 8.5 "
              "C10 7.7 9.3 7 8.5 7 C7.7 7 7 7.7 7 8.5 C7 9.3 7.7 10 8.5 10 Z M3.5 16 L9 11.5 "
              "L13.5 15.5 L16.5 13 L20.5 16.5",
     1.6f, false},
    {"mail", "M3.5 6.5 L20.5 6.5 C20.9 6.5 21 6.7 21 7 L21 17 C21 17.3 20.9 17.5 20.5 17.5 L3.5 "
             "17.5 C3.1 17.5 3 17.3 3 17 L3 7 C3 6.7 3.1 6.5 3.5 6.5 Z M3.5 7.5 L12 13.5 L20.5 "
             "7.5",
     1.7f, false},
    {"message", "M4 5 L20 5 C20.6 5 21 5.4 21 6 L21 15 C21 15.6 20.6 16 20 16 L9 16 L4.5 19.5 L5 "
                "16 L4 16 C3.4 16 3 15.6 3 15 L3 6 C3 5.4 3.4 5 4 5 Z",
     1.7f, false},
    {"send", "M21 3.5 L3 10.5 L10 12.5 L12.5 20 Z M10 12.5 L21 3.5", 1.7f, false},
    {"eye", "M2.5 12 C5 7.5 8.4 5.5 12 5.5 C15.6 5.5 19 7.5 21.5 12 C19 16.5 15.6 18.5 12 18.5 "
            "C8.4 18.5 5 16.5 2.5 12 Z M12 9 C13.7 9 15 10.3 15 12 C15 13.7 13.7 15 12 15 C10.3 "
            "15 9 13.7 9 12 C9 10.3 10.3 9 12 9 Z",
     1.7f, false},
    {"eye-off", "M4 4 L20 20 M9.5 9.7 C9.2 10.4 9 11.1 9 12 C9 13.7 10.3 15 12 15 C12.9 15 13.6 "
                "14.8 14.3 14.5 M6.5 7.3 C4.7 8.5 3.3 10.1 2.5 12 C5 16.5 8.4 18.5 12 18.5 C13.3 "
                "18.5 14.6 18.2 15.8 17.6 M18.6 15.4 C20 14.4 21 13.3 21.5 12 C19 7.5 15.6 5.5 12 "
                "5.5 C11.5 5.5 10.9 5.6 10.4 5.7",
     1.7f, false},
    {"layers", "M12 3 L21 8 L12 13 L3 8 Z M3 12 L12 17 L21 12 M3 16 L12 21 L21 16", 1.7f, false},
    {"package", "M12 3 L20.5 7.5 L20.5 16.5 L12 21 L3.5 16.5 L3.5 7.5 Z M3.5 7.5 L12 12 L20.5 7.5 "
                "M12 12 L12 21",
     1.7f, false},
    {"git-branch", "M6.5 6 C7.9 6 9 4.9 9 3.5 C9 2.1 7.9 1 6.5 1 C5.1 1 4 2.1 4 3.5 C4 4.9 5.1 6 "
                   "6.5 6 Z M6.5 23 C7.9 23 9 21.9 9 20.5 C9 19.1 7.9 18 6.5 18 C5.1 18 4 19.1 4 "
                   "20.5 C4 21.9 5.1 23 6.5 23 Z M17.5 9.5 C18.9 9.5 20 8.4 20 7 C20 5.6 18.9 4.5 "
                   "17.5 4.5 C16.1 4.5 15 5.6 15 7 C15 8.4 16.1 9.5 17.5 9.5 Z M6.5 6 L6.5 18 "
                   "M17.5 9.5 C17.5 14 14 14 12 14.5 C10 15 6.5 15 6.5 18",
     1.7f, false},
    // 「变更」视图图标：**两侧横线 + 中间空心圆环**（提交节点的通用形态）。
    //
    // 为什么是这个形态（三轮返工的结论）：图标在 Small 档按钮里只渲染 ~16px，
    //  - 第一版 git-compare（两个圆 + 两条曲线）在 `viewBox` 里糊成一团；
    //  - 第二版 加号+减号 清晰，但语义偏"编辑操作"；
    //  - 第三版（线**断开** + 实心圆）形态仍不对，见下面的坑。
    //
    // ⚠ **用实心（filled）+ 两条反向子路径会画出错误的形态**（实测踩到）：
    // 横线穿过圆时，圆环与线**重叠**——按 nonzero winding 抵消后，圆只剩下
    // **上下两个外围缺块**，看起来像"实心圆中间挖了个洞"，而不是一圈细环。
    // 同理也不能拆成"描边式线 + 描边式圆"：两段的描边在交点会叠成一团。
    //
    // ✅ **正确做法：一整条子路径 + 一次性描边**（`stroke_path` 会正确处理自相交）。
    // 路径 = 左线 → 右线 → 圆环；线**不穿过圆内部**（左右各停在圆的边缘处，
    // 中间一段留空），于是描边后是干净的"线—环—线"，圆内无墨。
    //
    // 几何取自参考图（Lucide `git-commit-horizontal`）的**实测比例**：
    // 参考墨迹 21×8px（长宽比 2.62），线径 2px。换算到 24 视框近似为
    // 线横跨 4..20（宽 16）、圆半径 3.4（直径 6.8）、描边 2.2 —— 长宽比 2.35，
    // 与参考同级（参考的圆相对更大，视觉重心更靠中间）。
    //
    // ⚠ **图标必须登记在这张内置表里**：`Icon` 是 C++ 的 `inline constexpr` 表，
    // 与 `examples/*/assets/icons.svg` 那份是**两套东西**（后者由 gallery 自己
    // embed 来演示 SVG 渲染器）。实测踩到：只在 svg 里加 `diff` 而没登记这里，
    // 活动栏按钮渲染成空白——看不出按钮存在（现在有 `warn_unknown_icon_once` 提示）。
    //
    // ⚠ 手算 kappa：圆用四段三次贝塞尔逼近，控制点偏移 = 半径 × 0.5523。
    // 半径 3.4 → 偏移 1.878（= 3.4 × 0.5523），本条目按此写死。
    // 圆的左右端点取 (8.6,12) 与 (15.4,12)；横线各停在这两点外侧
    // （左线 4→8.4、右线 15.6→20），留 0.2 单位让描边的圆头端帽自然接上。
    {"diff", "M4 12 L8.4 12 M15.6 12 L20 12 "
             "M8.6 12 C8.6 13.878 10.122 15.4 12 15.4 C13.878 15.4 15.4 13.878 15.4 12 "
             "C15.4 10.122 13.878 8.6 12 8.6 C10.122 8.6 8.6 10.122 8.6 12 Z",
     2.2f, false},
    {"rocket", "M14.5 3.5 C17.5 4.5 20 7 20.5 10.5 L14 17 L7.5 17 L7.5 10.5 Z M7.5 10.5 C7.5 7 "
               "10 4 14.5 3.5 M14 17 C14 17 15.5 19 15.5 21 L8.5 21 C8.5 19 7.5 17 7.5 17 M11 12.5 "
               "C11 13.6 11.9 14.5 13 14.5 C14.1 14.5 15 13.6 15 12.5 C15 11.4 14.1 10.5 13 10.5 "
               "C11.9 10.5 11 11.4 11 12.5 Z",
     1.6f, false},
    {"bookmark", "M6.5 3.5 L17.5 3.5 C17.9 3.5 18 3.7 18 4 L18 20.5 L12 16.5 L6 20.5 L6 4 C6 3.7 "
                 "6.1 3.5 6.5 3.5 Z",
     1.7f, false},
    {"dot", "M12 6 C15.3 6 18 8.7 18 12 C18 15.3 15.3 18 12 18 C8.7 18 6 15.3 6 12 C6 8.7 8.7 "
            "6 12 6 Z",
     1.6f, true},
    {"circle", "M12 3.5 C16.7 3.5 20.5 7.3 20.5 12 C20.5 16.7 16.7 20.5 12 20.5 C7.3 20.5 3.5 "
               "16.7 3.5 12 C3.5 7.3 7.3 3.5 12 3.5 Z",
     1.6f, false},
    {"square", "M5 5 L19 5 L19 19 L5 19 Z", 1.6f, false},
    // 还原（窗口已最大化时"最大化"按钮的形态）：**两个叠放的方框**。
    // 之前它复用 `square`，于是"最大化"与"还原"两种形态在像素上**完全一样**——
    // 用户看不出这个按钮此刻是哪个动作。图形与名字并排的图标墙（gallery）才能看出这类问题。
    //
    // ⚠ 后窗（`4,4`–`16,16`）被前窗（`8,8`–`20,20`）遮住的部分**不能画**：
    // 旧写法 `M4 4 L16 4 L16 16` 把后窗右边缘一路画到 `y=16`——那已经在前窗**内部**，
    // 于是多出一截“插进窗里”的竖线（用户截图报的就是它）。
    // 正确收尾点是后窗右边缘与前窗上缘的交点 `y=8`（再往下就进了前窗内部）。
    {"restore", "M16 8 L16 4 L4 4 L4 16 L8 16 M8 8 L20 8 L20 20 L8 20 Z", 1.6f, false},
    {"triangle", "M12 4.5 L20.5 19.5 L3.5 19.5 Z", 1.6f, false},
});

struct Cursor {
  std::string_view text;
  std::size_t index{0};
};

void skip_spaces(Cursor& cursor) {
  while (cursor.index < cursor.text.size() && cursor.text[cursor.index] == ' ') ++cursor.index;
}

[[nodiscard]] auto next_number(Cursor& cursor) -> float {
  skip_spaces(cursor);
  std::size_t end = cursor.index;
  while (end < cursor.text.size() && cursor.text[end] != ' ') ++end;
  const std::string_view token = cursor.text.substr(cursor.index, end - cursor.index);
  cursor.index = end;
  float value = 0.0f;
  bool negative = false;
  std::size_t position = 0;
  if (!token.empty() && (token[0] == '-' || token[0] == '+')) {
    negative = token[0] == '-';
    position = 1;
  }
  bool fraction = false;
  float divisor = 1.0f;
  for (; position < token.size(); ++position) {
    const char raw = token[position];
    if (raw == '.') {
      fraction = true;
      continue;
    }
    if (raw < '0' || raw > '9') break;
    const float digit = static_cast<float>(raw - '0');
    if (fraction) {
      divisor *= 10.0f;
      value += digit / divisor;
    } else {
      value = value * 10.0f + digit;
    }
  }
  return negative ? -value : value;
}

/// 解析 `M/L/C/Z` 指令序列（绝对坐标）；返回视图框内的点集合供包围盒计算。
[[nodiscard]] auto parse_path(std::string_view data, std::vector<math::Point>& out_points) -> bool {
  Cursor cursor{data, 0};
  bool has_current = false;
  math::Point current{};
  bool closed = false;
  while (cursor.index < data.size()) {
    skip_spaces(cursor);
    if (cursor.index >= data.size()) break;
    const char command = data[cursor.index];
    ++cursor.index;
    switch (command) {
      case 'M':
      case 'L': {
        const float x = next_number(cursor);
        const float y = next_number(cursor);
        current = math::Point{x, y};
        out_points.push_back(current);
        has_current = true;
        closed = false;
        break;
      }
      case 'C': {
        const float x1 = next_number(cursor);
        const float y1 = next_number(cursor);
        const float x2 = next_number(cursor);
        const float y2 = next_number(cursor);
        const float x = next_number(cursor);
        const float y = next_number(cursor);
        out_points.push_back(math::Point{x1, y1});
        out_points.push_back(math::Point{x2, y2});
        out_points.push_back(math::Point{x, y});
        current = math::Point{x, y};
        has_current = true;
        closed = false;
        break;
      }
      case 'Z': {
        closed = true;
        break;
      }
      default:
        return false;
    }
  }
  (void)has_current;
  (void)closed;
  return !out_points.empty();
}

}  // namespace

auto Icon::names() -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  out.reserve(kIcons.size());
  for (const auto& glyph : kIcons) out.push_back(glyph.name);
  return out;
}

auto Icon::find(std::string_view name) noexcept -> const IconGlyph* {
  for (const auto& glyph : kIcons) {
    if (glyph.name == name) return &glyph;
  }
  return nullptr;
}

auto Icon::view_bounds(std::string_view name) -> math::Rect {
  const IconGlyph* glyph = find(name);
  if (glyph == nullptr) return math::Rect{0.0f, 0.0f, 24.0f, 24.0f};
  std::vector<math::Point> points;
  if (!parse_path(glyph->data, points)) return math::Rect{0.0f, 0.0f, 24.0f, 24.0f};
  float min_x = points.front().x;
  float min_y = points.front().y;
  float max_x = min_x;
  float max_y = min_y;
  for (const auto& point : points) {
    min_x = std::min(min_x, point.x);
    min_y = std::min(min_y, point.y);
    max_x = std::max(max_x, point.x);
    max_y = std::max(max_y, point.y);
  }
  return math::Rect::from_ltrb(min_x, min_y, max_x, max_y);
}

namespace {

/// 光学归一化的**缩放因子**（把图标墨迹最长边归一到 `kOpticalTargetPx`）。
///
/// 为何抽成一个函数：`Icon::path`（实际绘制）与 `Icon::ink_size`（排版占位）
/// 必须用**同一个因子**。两处各算一份的话，“占多宽”与“画多宽”会静默脱钩——
/// 而这类脱钩正是“内容看着不居中”那类缺陷的来源。
///
/// 描边式图标用 `stroke_path` 画，线宽**跨在路径上**（两侧各外扩半个线宽），
/// 所以实际墨迹 = 几何 × fit + 描边；实心图标无这层外扩。
[[nodiscard]] auto optical_fit(const IconGlyph& glyph) -> float {
  const math::Rect ink = Icon::view_bounds(glyph.name);
  const float stroke_units = glyph.filled ? 0.0f : glyph.stroke;
  const float geometry = std::max(ink.width, ink.height);
  if (geometry <= 0.0f) return 1.0f;
  // 归一化后的实际墨迹 = 几何 × fit + 描边，要它等于目标，fit 就该是
  // `(target - stroke) / geometry`——**描边从目标里扣掉**，而不是加进分母
  //（两者只差 1~2%，但后者会让描边粗的图标系统性偏大，方向是反的）。
  //
  // 上下夹取只是防极端值（远离目标的病态数据）把图标拉爆。
  return std::clamp((kOpticalTargetPx - stroke_units) / geometry, kOpticalMinScale,
                    kOpticalMaxScale);
}

}  // namespace

auto Icon::ink_size(std::string_view name, math::Size box) -> math::Size {
  const IconGlyph* glyph = find(name);
  if (glyph == nullptr || !(box.width > 0.0f) || !(box.height > 0.0f)) return math::Size{};
  const math::Rect ink = view_bounds(name);
  const float fit = optical_fit(*glyph);
  const float scale = std::min(box.width, box.height) / 24.0f;
  // 与 `path` 同一算式：几何 × scale × fit，再加上描边外扩。
  //
  // ⚠ 描边项**不乘 fit**：线宽是画布像素量，`Icon::draw` 按
  // `stroke/24 × 盒径` 算它（不经过光学归一化的 `fit`）。乘上 fit 会让
  // 描边图标的声明值比实际小一档（实测 64px 盒里差 ~2px）。
  const float stroke_extent = glyph->filled ? 0.0f : glyph->stroke * scale;
  return math::Size{ink.width * scale * fit + stroke_extent,
                    ink.height * scale * fit + stroke_extent};
}

auto Icon::path(std::string_view name, math::Rect box, float stroke_width) -> raster::Path {
  raster::Path path;
  const IconGlyph* glyph = find(name);
  if (glyph == nullptr || box.is_empty()) return path;
  (void)stroke_width;

  // **光学归一化**：把每个图标的墨迹等比缩放并居中到统一的视觉尺寸。
  //
  // 为何要做：一套图标里各图标的**光感大小**必须一致——“看着一样大”是图标库的基本要求。
  // 而真实图标不可能用同一个 24×24 盒：方形、三角形、圆形按包围盒等高对齐时
  // （“几何居中”）**视觉大小不同**（圆形/三角形看起来明显偏小）。实测这套表：
  // 最大/最小光感尺寸相差 **2.2 倍**（`list` 0.42 对 `git-branch` 0.78），
  // 同一排里有的像实心有的像发丝，是“不精致”的首要来源。
  //
  // 归一化按**各自墨迹范围**做（`view_bounds`，即排版对齐用的同一份几何），
  // 于是“画多大”与“占多宽”永远一致。
  //
  // ⚠ 描边不跟着缩放：`Icon::draw` 传的是**绝对线宽**，一缩放它就会让
  // 小字形（`chevron` 等）的笔画变粗、破坏笔画粗细的一致性——那是另一个正交量。
  const math::Rect ink = view_bounds(name);
  // 归一化的缩放因子走 `optical_fit`（与 `Icon::ink_size` **同一个入口**）：
  // 两处各算一份的话，“占多宽”（排版）与“画多宽”（绘制）会静默脱钩。
  //
  // ⚠ 用**较大边**而不是要求两边都为正：`minus`/`more-horizontal` 这类
  // **零高度**字形（一条横线）的 `view_bounds.height` 恰为 0，
  // 要求两边都 > 0 会让它们**完全跳过归一化**（实测踩到：其余图标都归到 0.604，
  // 只有它们停在 0.500/0.438）——该判据在 `optical_fit` 里。
  const float fit = optical_fit(*glyph);
  // 先按 24 视框映射，再在**同一坐标系**里围绕墨迹中心做光学缩放。
  const float scale = std::min(box.width, box.height) / 24.0f;
  const float offset_x = box.x + (box.width - 24.0f * scale) * 0.5f;
  const float offset_y = box.y + (box.height - 24.0f * scale) * 0.5f;
  // 归一化后的落点：把**墨迹中心**对齐到**视框中心**。
  //
  // 为何要居中而不是保持原位置：① 光感大小一致之后，位置也得一致——
  // 否则同一排里有的图标偏上、有的偏下；② 它同时是一个**护栏**：
  // 目标尺寸 17px 居中到 24 视框后，墨迹必然落在 `[3.5, 20.5]`，
  // 永远不会被放大推到视框之外（否则会与邻居视觉重叠）。
  const float box_cx = offset_x + 12.0f * scale;
  const float box_cy = offset_y + 12.0f * scale;
  const float ink_cx = (ink.x + ink.width * 0.5f) * scale;
  const float ink_cy = (ink.y + ink.height * 0.5f) * scale;
  const auto map = [scale, box_cx, box_cy, fit, ink_cx, ink_cy](float x, float y) -> math::Point {
    return math::Point{box_cx + (x * scale - ink_cx) * fit, box_cy + (y * scale - ink_cy) * fit};
  };

  Cursor cursor{glyph->data, 0};
  bool has_subpath = false;
  math::Point subpath_start{};

  // ⚠️ **每个 `next_number` 必须单独成句**——不要写成
  // `map(next_number(cursor), next_number(cursor))`。C++ **没有规定函数实参的求值顺序**，
  // 而 MSVC 是从右往左求值：那样写会把两个数颠倒着取，于是 x/y 被交换。
  //
  // 这个缺陷的后果很隐蔽：所有图标都画得出来，只是“转了个方向”——
  // `check` 交换后仍像对钩、`search` 仍像放大镜，只有把**图形与名字并排看**
  // （图标全集）才发现 `chevron-down` 指向右、`home` 变成 `<E`；
  // 而且 GCC/Clang 通常从左往右求值，**Linux 上完全正常**（只在 Windows 上现形）。
  const auto read_point = [&cursor, &map]() -> math::Point {
    const float x = next_number(cursor);
    const float y = next_number(cursor);
    return map(x, y);
  };

  while (cursor.index < glyph->data.size()) {
    skip_spaces(cursor);
    if (cursor.index >= glyph->data.size()) break;
    const char command = glyph->data[cursor.index];
    ++cursor.index;
    switch (command) {
      case 'M': {
        const math::Point point = read_point();
        path.move_to(point);
        subpath_start = point;
        has_subpath = true;
        break;
      }
      case 'L': {
        const math::Point point = read_point();
        if (!has_subpath) {
          path.move_to(point);
          has_subpath = true;
        } else {
          path.line_to(point);
        }
        break;
      }
      case 'C': {
        const math::Point control1 = read_point();
        const math::Point control2 = read_point();
        const math::Point end = read_point();
        if (!has_subpath) {
          path.move_to(control1);
          has_subpath = true;
        }
        path.cubic_to(control1, control2, end);
        break;
      }
      case 'Z': {
        if (has_subpath) path.close();
        (void)subpath_start;
        break;
      }
      default:
        break;
    }
  }
  return path;
}

void Icon::draw_filled(raster::Surface& canvas, std::string_view name, math::Rect box,
                       math::Color color) {
  if (box.is_empty() || color.a == 0U) return;
  raster::Path path = Icon::path(name, box, 0.0f);
  if (path.is_empty()) return;
  canvas.fill_path(path, raster::Paint::solid(color));
}

namespace {

/// 图标名解包：`svg:name` → (true, name)；裸名 → (false, name)。
auto unpack_svg_name(std::string_view name) -> std::pair<bool, std::string_view> {
  if (name.size() > 4 && name.substr(0, 4) == "svg:") {
    return {true, name.substr(4)};
  }
  return {false, name};
}

}  // namespace

// —— SVG 图标注册表 ——

bool Icon::has(std::string_view name) noexcept {
  const auto [is_svg, id] = unpack_svg_name(name);
  if (is_svg) return svg_registry().has(id);
  if (find(name) != nullptr) return true;
  return svg_registry().has(name);
}

auto svg_registry() -> SvgIconRegistry& {
  // 图标 sprite 表按设计全局唯一（应用启动时装载一次，所有 IconView 共享）。
  // 只持有不可变源文本与位图缓存，退出期无跨 TU 引用。
  // lint-allow: L8 进程级图标 sprite 表（按设计跨实例共享），持有不可变数据
  static SvgIconRegistry instance;  // NOLINT(cppcoreguidelines-avoid-non-const-global-vars)
  return instance;
}

auto SvgIconRegistry::load_sprite(std::string_view source) -> bool {
  return set_->load(source);
}

auto SvgIconRegistry::add_single(std::string_view id, std::string_view source) -> bool {
  return set_->load_single(id, source);
}

auto SvgIconRegistry::has(std::string_view id) const -> bool {
  return set_->has(id);
}

auto SvgIconRegistry::ids() const -> std::vector<std::string> {
  return set_->ids();
}

auto SvgIconRegistry::draw(raster::Surface& canvas, std::string_view id, math::Rect box,
                           math::Color color) const -> bool {
  return painter_.draw(canvas, id, box, color), true;
}

void SvgIconRegistry::clear_cache() const { painter_.clear_cache(); }

auto SvgIconRegistry::cache_entries() const -> std::size_t {
  return painter_.cache_stats().entries;
}


void Icon::draw(raster::Surface& canvas, std::string_view name, math::Rect box, math::Color color,
                float stroke_width) {
  const IconGlyph* glyph = find(name);
  if (glyph == nullptr) {
    // 回退：SVG 注册表（裸名无 `svg:` 前缀也查——内置表优先，SVG 补位）
    const auto [is_svg, id] = unpack_svg_name(name);
    if (is_svg || !id.empty()) {
      if (svg_registry().draw(canvas, id, box, color)) return;
    }
  }
  if (glyph == nullptr || box.is_empty() || color.a == 0U) return;
  const float scale = std::min(box.width, box.height) / 24.0f;
  const float effective_stroke = glyph->stroke * scale;
  const float width = stroke_width > 0.0f ? stroke_width : effective_stroke;
  raster::Path path = Icon::path(name, box, width);
  if (path.is_empty()) return;
  if (glyph->filled) {
    canvas.fill_path(path, raster::Paint::solid(color));
    return;
  }
  // 描边样式：**圆角连接 + 圆头端帽**（不用 raster 缺省的 miter/butt）。
  //
  // 为何必须在这里指定：缺省 miter/butt 下，**折线转角是尖角、线段两端是平切**，
  // 观感偏硬；更实际的是，两端平切让**零长度风格的圆点完全画不出来**——
  // `more-horizontal`/`more-vertical` 用 `M6 12 L6.02 12` 这类 0.02 长的线段当圆点，
  // 截面虽被补圆覆盖，但带的**矩形两端仍与端点平齐**，外侧那半个圆被切掉，
  // 再经抗锯齿平摊到几乎不可见（接触印相实测：两格墨迹 **完全为空**）。
  // 圆头端帽把带外扩到端点之外，那半个圆才真的存在，圆点自然显现。
  //
  // 顶点的转角补圆是**几何必需**（见 `stroke_to_path` 的缺口深度判据），
  // 与这里选圆角/尖角是两件事：`join` 只决定补出来的形状。
  static constexpr raster::StrokeStyle kIconStroke{raster::LineCap::Round, raster::LineJoin::Round};
  canvas.stroke_path(path, raster::Paint::solid(color), width, kIconStroke);
}

}  // namespace st::ui
