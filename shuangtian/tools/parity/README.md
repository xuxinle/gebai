# 像素级对标：工具与用法

> 配套文档：`docs/render_parity.md`（实测与结论）。
> 这里只讲**怎么跑**。

## 三步复算

```bash
cd <repo>

# ① 浏览器真值：无头 Chrome + SwiftShader + gl.readPixels
#    （首次会 bun build 出 three 的 IIFE bundle）
bun tools/parity/browser_truth.mjs \
    --scene tools/parity/quad_parity.js \
    --out   out/quad-browser.png \
    --width 1100 --height 800 --tone NoToneMapping

# ② 霜天侧（同一份脚本；脚本里已声明 compat:'three'）
st build align_render
ST_SCENE3D_BACKEND=software ./build/dev/bin/align_render \
    tools/parity/quad_parity.js out/quad-fw.png 1100 800 0

# ③ 逐像素对比（分位数 / 按亮度分桶归因 / 空间热点）
bun tools/parity/analyze.mjs out/quad-fw.png out/quad-browser.png
```

## `align_render` 的参数

```
align_render <scene.js> <out.png> [W] [H] [toneMap] [bgR bgG bgB] [ambient diffuse rim env]
```

| 参数 | 说明 |
|---|---|
| `toneMap` | **与 `ToneMap` 枚举一致**：0=None 1=Reinhard 2=Aces 3=Filmic 4=Agx **5=AcesThree** |
| `bgR/bgG/bgB` | 清屏色（**线性值 × 255**，见 `begin_frame` 契约） |
| `ambient/diffuse/rim/env` | 光照覆盖（-1 = 不覆盖）。用于**消融实验**：把差异归因到具体某一项 |

`ST_SCENE3D_BACKEND=software|gpu` 切腿；不设时按探测结果自动选（通常是 gpu）。

## `browser_truth.mjs` 为什么要自己 bundle

`third_party/three/three.cjs` 是 CJS。直接 `<script>` 引入会 `exports is not defined`。
所以先 `bun build --format=iife`，并在页面里挂 `globalThis.THREE`：

```bash
cat > /tmp/entry.js <<'EOF'
globalThis.THREE = require("<repo>/third_party/three/three.cjs");
EOF
bun build /tmp/entry.js --format=iife --outfile=three_browser_iife.js
```

**为什么不用截图**：截图经过窗口合成、色彩管理、DPI 缩放，拿到的不是渲染器的输出。
这里让页面自己 `POST` 回读像素，只经过 three 自己的输出链
（`toneMapping → outputColorSpace → 8bit`）。

## 三个硬数字（回归基线）

| 场景 | 期望 | 来源 |
|---|---|---|
| 0.5 灰平面 + 单平行光、`compat:'three'`、NoToneMapping | **76** | 浏览器 `readPixels` |
| 同上，光强 2.0 | **106** | 同上 |
| ACESFilmic(线性 0.2159) | **141** | 同上（Narkowicz 会给 154，差 13 级） |

已钉进 `tests/raster_three_parity_test.cpp`（`st test three_parity`）。

> **教训**：这些数字必须来自真值通道。手推过两个（153、120），两次都错——
> 一次忘了高光项，一次不知道 `0x808080` 会被 three 的色彩管理**解码**成线性 0.2159。
