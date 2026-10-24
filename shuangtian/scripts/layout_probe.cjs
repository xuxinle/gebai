// 直接跑 layout()，看关键帧上产出多少图元（不经过渲染器）。
// 用途：热场是"每格一个图元"，图元数会到几千——先确认不会把渲染拖垮。
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
function easeLocal(name, t) {
  t = Math.min(1, Math.max(0, t));
  if (name === "back") {
    const c1 = 1.70158, c3 = c1 + 1, i = t - 1;
    return 1 + c3 * i * i * i + c1 * i * i;
  }
  if (name === "out") { const i = 1 - t; return 1 - i * i * i; }
  if (name === "in") return t * t * t;
  if (name === "in_out") return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
  if (name === "expo_out") return t >= 1 ? 1 : 1 - Math.pow(2, -10 * t);
  if (name === "expo_in") return t <= 0 ? 0 : Math.pow(2, 10 * t - 10);
  if (name === "expo_in_out") {
    if (t <= 0) return 0; if (t >= 1) return 1;
    const f = (v) => (Math.pow(2, 10 * v) - 1) / 1023;
    return t < 0.5 ? f(2 * t) * 0.5 : 1 - f(2 - 2 * t) * 0.5;
  }
  if (name === "smoother") return t * t * t * (t * (t * 6 - 15) + 10);
  return t;
}
const fn = new Function("ease", "spring", "spring_vel", "settle", "lerp", "clamp", "snap",
                        "noise", src + "\nreturn layout;");
const layout = fn(easeLocal, () => 0, () => 0, () => 1, (a, b, t) => a + (b - a) * t,
                  (v, lo, hi) => Math.min(hi, Math.max(lo, v)), (t) => t, () => 0.5);

const W = 1920, H = 1080;
const layers = [
  { x: 360, y: 700, w: 1200, h: 76, alpha: 1, scale: 1, offset_y: 0 },
  { x: 360, y: 890, w: 1200, h: 30, alpha: 1, scale: 1, offset_y: 0 },
  { x: 560, y: 380, w: 800, h: 200, alpha: 1, scale: 1, offset_y: 0 },
  { x: 560, y: 600, w: 800, h: 34, alpha: 1, scale: 1, offset_y: 0 },
];
const duration = parseFloat(process.argv[3] || "25.5");
let worst = 0, worstT = 0;
for (let t = 0; t <= duration; t += 0.25) {
  const r = layout(t, duration, { width: W, height: H }, layers);
  const n = (r.shapes || []).length;
  if (n > worst) { worst = n; worstT = t; }
  if (Math.abs(t % 2) < 1e-6) {
    console.log(`t=${t.toFixed(2)}  shapes=${n}`);
  }
}
console.log(`\n峰值图元数 ${worst} @ t=${worstT.toFixed(2)}s`);
