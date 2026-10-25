// 裁云 · 完整示例：机器人启动（clawd 风格）
//
// ## 这个文件示范什么
//
// 一个**完整的场景脚本**，覆盖了实际做片子时最常用的九件事：
//
//   1. 浅色底 + 竖向渐变（不用 background 字段，自己叠横带）
//   2. 弱透视 + 实体拼装（机器人 = 24 个分节立方体）
//   3. 关节式姿态（绕肩旋转，而不是整体平移）
//   4. 时间线驱动的"依次点亮"（每段一个时刻）
//   5. 弹簧/振铃用在"启动完成"的落地感上
//   6. 沿身体传导的能量脉冲（沿一条线跑一个亮环）
//   7. 扩散环（脚下启动圈）
//   8. 景深层次（前景/远景各一组虚化圆，不依赖 dof）
//   9. 手算居中的文字 + 阶段标签
//
// 依赖：`_kit.js` 的内容已内联在下方（`A. 运动学 / B. 等轴测 / C. 图元`）。
//
// ## 为什么机器人用弱透视而不是等轴测
//
// 等轴测的 30° 轴倾斜会把左右和前后都投到斜线上，人形会被压成一根柱子。
// 弱透视（x 直接向右、z 向上、水平垂直约 1:1）人形才立得住。

// ═══════════════════════════════════════════════════════════
// A. 运动学（节选自 _kit.js）
// ═══════════════════════════════════════════════════════════
function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
function seg(t, a, b) {
  if (b === a) return t >= b ? 1 : 0;
  return clamp01((t - a) / (b - a));
}
function spring(tau, zeta, omega) {
  const z = (zeta === undefined) ? 0.62 : zeta;
  const w = (omega === undefined) ? 20 : omega;
  if (tau <= 0) return 0;
  if (z < 1) {
    const wd = w * Math.sqrt(1 - z * z);
    return 1 - Math.exp(-z * w * tau) * (Math.cos(wd * tau) + (z * w / wd) * Math.sin(wd * tau));
  }
  return 1 - Math.exp(-w * tau) * (1 + w * tau);
}
function ring(tau, decay, omega) {
  const d = (decay === undefined) ? 9 : decay;
  const w = (omega === undefined) ? 26 : omega;
  return tau <= 0 ? 0 : Math.exp(-d * tau) * Math.sin(w * tau);
}
function hash01(a, b) {
  const x = Math.sin(a * 127.1 + (b || 0) * 311.7) * 43758.5453;
  return x - Math.floor(x);
}
function shadeHex(hex, k) {
  const r = parseInt(hex.slice(1, 3), 16), g = parseInt(hex.slice(3, 5), 16), b = parseInt(hex.slice(5, 7), 16);
  const f = (v) => Math.min(255, Math.max(0, Math.round(v * k)));
  return '#' + [f(r), f(g), f(b)].map(v => v.toString(16).padStart(2, '0')).join('');
}

// ═══════════════════════════════════════════════════════════
// B. 投影与实体
// ═══════════════════════════════════════════════════════════
function perspProj(ox, oy, scale, yaw, pers) {
  const y = yaw || 0;
  const p = (pers === undefined) ? 0.0016 : pers;
  const cy = Math.cos(y), sy = Math.sin(y);
  return (x, yy, z) => {
    const rx = x * cy - yy * sy;
    const ry = x * sy + yy * cy;
    const depth = 1 + ry * p * 0.5;
    return { x: ox + rx * scale * depth, y: oy + (ry * 0.42 - z) * scale * depth };
  };
}
function depthKeyOf(x, y, yaw) {
  const yw = yaw || 0;
  return (x * Math.sin(yw) + y * Math.cos(yw));
}
function isoBox(shapes, P, x, y, z, w, d, h, hex, alpha) {
  const v = [P(x, y, z), P(x + w, y, z), P(x + w, y + d, z), P(x, y + d, z),
             P(x, y, z + h), P(x + w, y, z + h), P(x + w, y + d, z + h), P(x, y + d, z + h)];
  const poly = (idx, c, a) => {
    const pts = [];
    for (const i of idx) { pts.push(v[i].x, v[i].y); }
    shapes.push({ kind: 4, points: pts, color: c, alpha: a });
  };
  poly([4, 5, 6, 7], shadeHex(hex, 1.00), alpha);
  poly([1, 5, 6, 2], shadeHex(hex, 0.68), alpha);
  poly([3, 2, 6, 7], shadeHex(hex, 0.45), alpha);
  shapes.push({ kind: 2, x: v[4].x, y: v[4].y, x2: v[5].x, y2: v[5].y,
                stroke: 1.6, color: shadeHex(hex, 1.38), alpha: alpha * 0.72 });
}
function isoShadow(shapes, P, x, y, z, w, d, alpha, tint) {
  const c = P(x + w / 2, y + d / 2, z);
  const r = Math.max(w, d);
  const col = tint || '#000000';
  for (let i = 3; i >= 1; i--) {
    shapes.push({ kind: 0, x: c.x + r * 0.40, y: c.y + r * 0.24,
                  w: r * (1.5 + i * 0.55), h: r * (0.50 + i * 0.16),
                  color: col, alpha: alpha * 0.26 / i });
  }
}

// ═══════════════════════════════════════════════════════════
// C. 图元
// ═══════════════════════════════════════════════════════════
function textWidth(txt, size) {
  let w = 0;
  for (const ch of txt) w += (ch.charCodeAt(0) > 127 ? size : size * 0.52);
  return w;
}
function textCentered(shapes, W, txt, y, size, color, alpha) {
  const tw = textWidth(txt, size);
  shapes.push({ kind: 3, text: txt, x: (W - tw) * 0.5, y,
                w: tw + 40, h: size, size: size, color: color, alpha: alpha, align: 0 });
}
function scanBand(shapes, W, H, lt, color, alphaPeak, period) {
  const p = (period === undefined) ? 2.8 : period;
  const phase = (lt % p) / p;
  const bandW = 340;
  const bx = -bandW + (W + bandW * 2) * phase;
  const col = color || '#FFFFFF';
  const peak = (alphaPeak === undefined) ? 0.018 : alphaPeak;
  for (let g = 0; g < 6; g++) {
    const f = g / 5;
    const edge = 1 - Math.abs(f - 0.5) * 2;
    shapes.push({ kind: 1, x: bx + (f - 0.5) * bandW, y: 0,
                  w: bandW / 6 + 2, h: H, color: col, alpha: peak * edge });
  }
}
function breatheLayer(shapes, W, H, lt, color, lo, hi, period) {
  const p = (period === undefined) ? 0.36 : period;
  const pulse = 0.5 + 0.5 * Math.sin((lt / p) * Math.PI * 2);
  const a0 = (lo === undefined) ? 0.004 : lo;
  const a1 = (hi === undefined) ? 0.018 : hi;
  shapes.push({ kind: 1, x: 0, y: 0, w: W, h: H, color: color || '#FFFFFF',
                alpha: a0 + (a1 - a0) * pulse });
}

