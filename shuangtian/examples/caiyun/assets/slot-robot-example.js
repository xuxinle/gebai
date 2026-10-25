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

// ═══════════════════════════════════════════════════════════
// 机器人骨架（世界单位；z=0 脚底，高约 320）
// ═══════════════════════════════════════════════════════════
const BODY_H = 320;

/// 每个部件：`seg` 决定它何时点亮
function buildRobot() {
  const parts = [];
  const add = (name, seg2, x, y, z, w, d, h, col, glow) => {
    parts.push({ name, seg: seg2, x, y, z, w, d, h, col: col || '#8A929E', glow: !!glow });
  };

  // 头
  add('head',  'head', -19, -17, 284, 38, 34, 32, '#9AA3B0');
  add('face',  'head', -13, -19, 292, 26, 4, 14, '#6E7784');
  add('eyeL',  'head',  -9, -20, 298, 7, 3, 5, '#E8452D', true);
  add('eyeR',  'head',   2, -20, 298, 7, 3, 5, '#E8452D', true);
  add('crest', 'head',  -3,  -7, 314, 6, 6, 8, '#E8452D', true);

  // 颈
  add('neck',  'neck',  -8,  -8, 262, 16, 16, 22, '#6E7784');

  // 胸（肩宽 96）
  add('chest', 'chest', -48, -25, 206, 96, 50, 56, '#9AA3B0');
  add('core',  'chest', -12, -27, 224, 24, 5, 24, '#E8452D', true);

  // 腰（收窄）
  add('waist', 'waist', -31, -20, 168, 62, 40, 38, '#7E8794');
  add('spine', 'waist',  -6, -22, 176, 12, 4, 24, '#E8912E', true);

  // 髋（比肩窄）
  add('hip',   'hip',   -40, -23, 148, 80, 46, 20, '#8A929E');

  // 腿（分节：大腿 / 膝 / 小腿 / 脚）
  for (const side of [-1, 1]) {
    const sx = side * 38;
    const tag = side < 0 ? 'L' : 'R';
    add('thigh' + tag, 'leg',  sx - 13, -13, 84, 26, 26, 64, '#9AA3B0');
    add('knee'  + tag, 'knee', sx - 15, -15, 68, 30, 30, 20, '#6E7784');
    add('shin'  + tag, 'shin', sx - 12, -12, 20, 24, 24, 48, '#9AA3B0');
    add('foot'  + tag, 'shin', sx - 15, -26,  0, 30, 46, 20, '#7E8794');
  }

  // 臂（分节：肩 / 上臂 / 肘 / 前臂 / 手）
  for (const side of [-1, 1]) {
    const sx = side * 72;
    const tag = side < 0 ? 'L' : 'R';
    add('shoulder' + tag, 'upper',    sx - 13, -13, 232, 26, 26, 26, '#6E7784');
    add('upperArm' + tag, 'upper',    sx -  9,  -9, 178, 18, 18, 54, '#9AA3B0');
    add('elbow'    + tag, 'upper',    sx - 11, -11, 162, 22, 22, 18, '#6E7784');
    add('foreArm'  + tag, 'fore',     sx -  8,  -8, 110, 16, 16, 52, '#9AA3B0');
    add('hand'     + tag, 'fore',     sx - 10, -10,  88, 20, 20, 22, '#7E8794');
  }

  return parts;
}

const SEG_ON = {
  head: 1.2, neck: 2.2, chest: 2.7, waist: 3.2, hip: 3.7,
  leg: 3.6, knee: 4.2, shin: 4.8,
  upper: 4.6, fore: 5.4,
};
const SEG_LIT = {
  head: '#A8B2C0', neck: '#A8B2C0', chest: '#B0BAC8', waist: '#A8B2C0',
  hip: '#A8B2C0', leg: '#B0BAC8', knee: '#8A94A4', shin: '#A8B2C0',
  upper: '#B0BAC8', fore: '#A8B2C0',
};

function layout(tLocal, duration, canvas, layers) {
  const shapes = [];
  const W = canvas.width || 1920;
  const H = canvas.height || 1080;
  const lt = tLocal;

  // ── 1. 浅底：竖向渐变（横带叠）──
  {
    const bands = 56;
    for (let i = 0; i < bands; i++) {
      const f = i / (bands - 1);
      const r = Math.round(0xFF - 0x0A * f);
      const g = Math.round(0xFF - 0x19 * f);
      const b = Math.round(0xFF - 0x1F * f);
      shapes.push({ kind: 1, x: 0, y: (H / bands) * i, w: W, h: H / bands + 2,
                    color: '#' + [r, g, b].map(v => v.toString(16).padStart(2, '0')).join(''), alpha: 1 });
    }
  }

  const groundY = H * 0.885;
  {
    const u = Math.min(1, Math.max(0, lt / 0.5));
    shapes.push({ kind: 1, x: 0, y: groundY, w: W, h: 1.2, color: '#E0CEC6', alpha: 0.5 * u });
  }

  // yaw 必须接近正面（等轴测/弱透视都一样：侧视会让人形挤成柱子）
  const yaw = 0.10 + Math.sin(lt * 0.30) * 0.04;
  const S = 1.92;
  const P = perspProj(W * 0.5, groundY - 8, S, yaw);

  const parts = buildRobot();

  // ── 3. 姿态：抬头 / 绕肩抬手 ──
  const headLift = Math.min(1, Math.max(0, (lt - 1.2) / 1.0));
  const armEase = 1 - Math.pow(1 - Math.min(1, Math.max(0, (lt - 4.6) / 2.0)), 3);

  const items = [];
  for (const p of parts) {
    let x = p.x, y = p.y, z = p.z;

    // 抬头：绕颈根（z=278）微前倾
    if (p.seg === 'head') {
      const pivot = 278;
      const tilt = (1 - headLift) * 0.22;
      z += -(z - pivot) * tilt;
      y += Math.sin(tilt) * (z - pivot) * 0.9;
    }

    // 抬手：**绕肩关节**旋转（不是整体上移 —— 那样肩膀会脱离躯干）
    if (p.seg === 'upper' || p.seg === 'fore') {
      const PIVOT_Z = 226;
      const side = p.name.endsWith('L') ? -1 : 1;
      const theta = armEase * 0.30;
      const cz = z + p.h / 2;
      const dist = PIVOT_Z - cz;
      if (dist > 0) {
        z += (PIVOT_Z - dist * Math.cos(theta)) - cz;
        x += dist * Math.sin(theta) * side;
      }
    }

    // ── 4. 依次点亮 ──
    const onAt = (SEG_ON[p.seg] !== undefined) ? SEG_ON[p.seg] : 99;
    const lu = Math.min(1, Math.max(0, (lt - onAt) / 0.45));
    const lit = 1 - Math.pow(1 - lu, 3);
    const baseCol = p.glow ? p.col : (SEG_LIT[p.seg] || p.col);
    const col = shadeHex(baseCol, 0.52 + 0.48 * lit);

    items.push({ ...p, x, y, z, col, lit, key: depthKeyOf(x + p.w / 2, y + p.d / 2, yaw) });
  }

  // ── 2/6. 投影先画 ──
  for (const it of items) {
    if (it.name === 'head' || it.name === 'chest' || it.name === 'waist' ||
        it.name === 'hip' || it.name.startsWith('foot') || it.name.startsWith('thigh')) {
      isoShadow(shapes, P, it.x, it.y, 0, it.w, it.d, 0.85, '#8A7060');
    }
  }
  isoShadow(shapes, P, -58, -30, 0, 116, 60, 0.55, '#8A7060');

  // ── 5. 实体（远 → 近）──
  items.sort((a, b) => a.key - b.key);
  for (const it of items) {
    isoBox(shapes, P, it.x, it.y, it.z, it.w, it.d, it.h, it.col, 0.96);
    if (it.glow && it.lit > 0.05) {
      const c = P(it.x + it.w / 2, it.y + it.d / 2, it.z + it.h / 2);
      // 弹簧用在发光件的强度上（"咔"一下亮起来）
      const k = spring(lt - ((SEG_ON[it.seg] !== undefined) ? SEG_ON[it.seg] : 0), 0.5, 16);
      const inten = clamp01(k) * it.lit;
      for (let g = 3; g >= 1; g--) {
        shapes.push({ kind: 0, x: c.x, y: c.y,
                      w: 46 * g * 0.55, h: 46 * g * 0.55,
                      color: it.col, alpha: 0.10 * inten / g });
      }
    }
  }

  // ── 6. 能量沿脊椎下行（2.2–4.2s）──
  {
    const t0 = 2.2, t1 = 4.2;
    if (lt > t0 && lt < t1 + 0.3) {
      const pu = Math.min(1, (lt - t0) / (t1 - t0));
      const pz = 268 - (268 - 156) * pu;
      const c = P(0, 0, pz);
      const fade = lt < t1 ? 1 : Math.max(0, 1 - (lt - t1) / 0.3);
      for (let g = 3; g >= 1; g--) {
        shapes.push({ kind: 0, x: c.x, y: c.y,
                      w: 54 * g * 0.55, h: 30 * g * 0.55,
                      color: '#FFD98A', alpha: 0.34 * fade / g });
      }
      shapes.push({ kind: 0, x: c.x, y: c.y, w: 13, h: 13, color: '#FFFFFF', alpha: 0.95 * fade });
    }
  }

  // ── 7. 启动完成：脚下扩散环（用振铃做衰减）──
  {
    const t0 = 6.0;
    if (lt > t0 && lt < t0 + 1.2) {
      const tau = lt - t0;
      const amp = Math.abs(ring(tau, 2.6, 5.0));
      const c = P(0, 0, 2);
      const r = 90 + tau * 320;
      shapes.push({ kind: 0, x: c.x, y: c.y, w: r * 2, h: r * 2 * 0.55,
                    color: '#E8452D', alpha: 0.30 * amp });
      shapes.push({ kind: 0, x: c.x, y: c.y, w: r * 1.4, h: r * 1.4 * 0.55,
                    color: '#FFD98A', alpha: 0.22 * amp });
    }
  }

  // 关节亮点
  {
    const joints = items.filter(it => it.name.startsWith('knee') || it.name.startsWith('elbow') || it.name === 'neck');
    for (const j of joints) {
      if (j.lit < 0.1) continue;
      const c = P(j.x + j.w / 2, j.y + j.d / 2, j.z + j.h / 2);
      const pulse = 0.5 + 0.5 * Math.sin(lt * 2.6 + j.z * 0.03);
      shapes.push({ kind: 0, x: c.x, y: c.y, w: 16, h: 16,
                    color: '#FFD98A', alpha: 0.55 * j.lit * pulse });
    }
  }

  // ── 8. 景深层次 ──
  {
    const u = Math.min(1, Math.max(0, (lt - 0.4) / 0.8));
    if (u > 0.01) {
      for (let i = 0; i < 3; i++) {
        const x = 80 + i * 120 + Math.sin(lt * 0.45 + i) * 10;
        const y = H - 48 + Math.cos(lt * 0.38 + i * 1.4) * 7;
        for (let b = 3; b >= 1; b--) {
          shapes.push({ kind: 0, x, y, w: 130 * b * 0.5, h: 130 * b * 0.5,
                        color: '#FFFFFF', alpha: 0.080 * u / b });
        }
      }
      for (let i = 0; i < 3; i++) {
        const x = W - 140 - i * 76 + Math.sin(lt * 0.3 + i * 2) * 7;
        const y = 92 + Math.cos(lt * 0.28 + i) * 7;
        for (let b = 3; b >= 1; b--) {
          shapes.push({ kind: 0, x, y, w: 70 * b * 0.5, h: 70 * b * 0.5,
                        color: '#E8452D', alpha: 0.026 * u / b });
        }
      }
    }
  }

  // ── 9. 文字 ──
  {
    const u = Math.min(1, Math.max(0, (lt - 0.4) / 0.7));
    if (u > 0.01) {
      const e = 1 - Math.pow(1 - u, 3);
      shapes.push({ kind: 0, x: 108, y: H * 0.078, w: 15, h: 15, color: '#E8452D', alpha: 0.9 * e });
      shapes.push({ kind: 3, text: '打造新世界智能的躯体', x: 130, y: H * 0.078 - 24,
                    w: 1100, h: 46, size: 46, color: '#2A2320', alpha: 0.96 * e, align: 0 });
      shapes.push({ kind: 3, text: 'A BODY FOR NEW-WORLD INTELLIGENCE', x: 132, y: H * 0.078 + 36,
                    w: 1000, h: 17, size: 17, color: '#9B8B82', alpha: 0.85 * e, align: 0 });
    }
  }

  // 阶段标签（随进度切换）
  {
    const phases = [
      [0.0, '待机'], [1.2, '视觉上线'], [2.2, '脊椎供能'],
      [3.6, '下肢通电'], [4.6, '手臂展开'], [6.0, '启动完成'],
    ];
    let cur = phases[0][1];
    for (const ph of phases) if (lt >= ph[0]) cur = ph[1];
    const u = Math.min(1, Math.max(0, (lt - 0.8) / 0.5));
    if (u > 0.01) {
      shapes.push({ kind: 3, text: '· ' + cur, x: 132, y: H * 0.078 + 62,
                    w: 400, h: 19, size: 19, color: '#C08A78', alpha: 0.9 * u, align: 0 });
    }
  }

  // ── 扫描带 + 呼吸（浅底上用暗色）──
  scanBand(shapes, W, H, lt, '#A98A78', 0.012, 3.2);
  breatheLayer(shapes, W, H, lt, '#8A6A5E', 0.004, 0.017, 0.40);

  return { states: [], shapes };
}
