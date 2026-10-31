/// 素材预览与自检（见 `model_preview.cpp` 的实现说明）。
#ifndef CAIYUN_MODEL_PREVIEW_HPP
#define CAIYUN_MODEL_PREVIEW_HPP

#include <string>

namespace caiyun {

/// 渲染一个素材场景文件并做自检。
///
/// `out_png` 为空则只统计不落盘（CI 用）。
/// 返回 0 = 通过；1 = 有检查项不通过；2 = 参数/IO/解析错误。
[[nodiscard]] auto preview_model(const std::string& spec_path, const std::string& out_png,
                                 int width, int height) -> int;

}  // namespace caiyun

#endif  // CAIYUN_MODEL_PREVIEW_HPP
