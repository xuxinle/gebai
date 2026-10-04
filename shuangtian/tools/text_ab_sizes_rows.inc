// 由 `tools/gen_text_ab_sizes.py` 生成——**不要手改**。
// 同一段字、只变字号；行距 56 逻辑 px（> 最大字号 32 × 1.5）。
constexpr int kWidth = 560;
constexpr int kHeight = 692;
constexpr char kSample[] = "组件画廊 Overview 24";

struct Line {
  float top;
  float size;
};

constexpr std::array<Line, 12> kLines{{
    {4.0f, 10.0f},
    {60.0f, 11.0f},
    {116.0f, 12.0f},
    {172.0f, 13.0f},
    {228.0f, 14.0f},
    {284.0f, 15.0f},
    {340.0f, 16.0f},
    {396.0f, 18.0f},
    {452.0f, 20.0f},
    {508.0f, 24.0f},
    {564.0f, 28.0f},
    {620.0f, 32.0f},
}};
