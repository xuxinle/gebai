// 验证 shapes 图元在不同分辨率下**位置与尺寸同比缩放**。
//
// ## 为什么必须单测这个（它曾经错了、且错得看不出来）
//
// `shape.x/y/w/h` 是设计像素（kDesignWidth=1920）。转物理像素有两条路：
//   · `p.circle(px, py, r)` 等收**物理像素**（内部不再乘 scale）；
//   · `p.point(x, y)` 收**设计像素**并过相机 + 乘 scale。
//
// 早先 `case 0/2/3` 写的是 `p.circle(shape.x * scale, …)`，而 `p.point` 内部
// **又乘了一次 scale** ⇒ 双重缩放。在 1920×1080 下 `scale == 1.0`，所以
// 13 条片全都没暴露——**换 1280×720（scale=0.667）就会整体缩小并偏移**。
// 这类缺陷没有别的发现途径：画面依旧"画出来了"，只是比例不对。
//
// ## 为什么不能拿 --export-dir 测
//
// `--export-dir` 写的是 **320×180 的预览帧**（尺寸与 --render-size 无关），
// 于是三种分辨率量出来一模一样——**假绿**。必须走 `--render` 出真尺寸，再抽帧量。
//
// 用法：node scripts/verify_shape_scaling.cjs
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync } = require("child_process");

const ROOT = path.dirname(__dirname);
const EXE = path.join(ROOT, "build", "dev", "bin", "caiyun.exe");
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), "shape-scale-"));

const SCRIPT = {
  name: "scale-probe", fps: 30, size: [1920, 1080], fade_in: 0, fade_out: 0,
  show_brand: false, brand_wordmark: "", brand_latin: "", footer_note: "",
  audio: { enabled: false },
  scenes: [{
    kind: "custom", duration: 1.0, wrap: false,
    background: ["#000000", "#000000"], accent: "#000000", grain: 0,
    layout: "scale.js",
    layers: [{ kind: "text", text: "", x: "0", y: "0", w: "1", h: "1", size: 10,
               color: "#00000000", enter: 0 }],
  }],
};

// 设计坐标正中 (960,540)、半径 200 设计像素的白圆。
// 三种分辨率下它都应落在**画面正中**，直径 = 400 设计像素 × scale。
const LAYOUT = `
function layout(t, duration, canvas, layers) {
  return { states: layers.map(() => ({})),
           shapes: [{ kind: 0, x: 960, y: 540, w: 400, color: "#FFFFFF", alpha: 1 }] };
}
`;

fs.writeFileSync(path.join(TMP, "scale.json"), JSON.stringify(SCRIPT, null, 2));
fs.writeFileSync(path.join(TMP, "scale.js"), LAYOUT);

const sizes = [[1920, 1080], [1280, 720], [960, 540]];
const rows = [];
for (const [w, h] of sizes) {
  const mp4 = path.join(TMP, `out_${w}x${h}.mp4`);
  try {
    execFileSync(EXE, ["--headless", "--script", path.join(TMP, "scale.json"),
                       "--render", mp4, "--render-size", `${w}x${h}`,
                       "--render-frames", "10"], { stdio: "ignore", timeout: 120000 });
  } catch (e) {
    rows.push({ w, h, err: `渲染失败: ${e.message}` });
    continue;
  }
  if (!fs.existsSync(mp4)) { rows.push({ w, h, err: "无产物" }); continue; }
  const png = path.join(TMP, `f_${w}x${h}.png`);
  try {
    execFileSync("ffmpeg", ["-y", "-v", "error", "-ss", "0.2", "-i", mp4,
                            "-frames:v", "1", png], { stdio: "ignore" });
  } catch (e) { rows.push({ w, h, err: "抽帧失败" }); continue; }
  rows.push({ w, h, png });
}
fs.writeFileSync(path.join(TMP, "rows.json"), JSON.stringify(rows, null, 2));
console.log(TMP);
