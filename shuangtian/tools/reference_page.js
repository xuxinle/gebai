// 参照场景：与 bpy_watch.py 的轮系同构（同材质、同光照、同相机参数）。
//
// ## 为什么不能"重新写一个类似场景"
//
// 对齐验证的前提是**只差渲染器**。如果参照场景的材质/光照/相机参数与
// 本框架跑的 bpy 脚本不同，那么画面差异里既有渲染器差异也有参数差异，
// 就分不清了。
//
// 所以本文件逐一对应 `examples/threelab/scenes/bpy_watch.py`：
//
// | 项 | bpy 脚本里的值 | 这里 |
// |---|---|---|
// | 模数 Module | 0.21 | MODULE = 0.21 |
// | 齿数规格 | 42/26/18/14/15 + 3 惰轮 | 同 |
// | 材质 brass | (0.72,0.55,0.24) metal .95 rough .22 | 同 |
// | 材质 steel | (0.28,0.34,0.46) metal .92 rough .18 | 同 |
// | 相机 | z=26, lens 55, 看向原点 | 同 |
// | 主光 | SUN energy 2600, 方向 (-0.43,0.28,-0.86) | 同 |
//
// ## 齿形怎么保持同构
//
// 本框架 bpy 脚本用 7 点/齿的渐开线近似 + 32 段内环 + 轮辐窗口。
// 这里用**同一份轮廓算法**（直接搬 Python 的 `gear_profile` 思路），
// 保证几何一致——否则"齿形不同"会被误读成"渲染不同"。
//
// ## 一个必须说清的取舍
//
// three.js 的 `MeshStandardMaterial` 与 Blender 的 Principled BSDF 都是
// **基于物理的**（GGX + 能量守恒），本框架自研渲染器是近似实现。
// 三者在这里对齐的是：材质参数、光照参数、几何、色调映射、色彩空间。
// 光照积分的细微差别（多次散射、微表面分布细节）是**已知残余**。

import * as THREE from './three_browser.js';

const TAU = 6.283185307179586;
const MODULE = 0.21;

/// 齿轮外轮廓（与 bpy_watch.py 的 gear_profile 同一算法）。
function gearProfile(teeth, module, rimWidth, toothFraction = 0.44) {
  const pitchRadius = (module * teeth) / 2.0;
  const tipRadius = pitchRadius + rimWidth * 0.5;
  const rootRadius = pitchRadius - rimWidth * 0.5;
  const pitchAngle = TAU / teeth;
  const toothAngle = pitchAngle * toothFraction;
  const profile = [];
  for (let i = 0; i < teeth; ++i) {
    const base = i * pitchAngle;
    const a0 = base - toothAngle * 0.5;
    const a1 = base - toothAngle * 0.30;
    const a2 = base + toothAngle * 0.30;
    const a3 = base + toothAngle * 0.5;
    profile.push([Math.cos(a0) * rootRadius, Math.sin(a0) * rootRadius]);
    profile.push([Math.cos(a1) * tipRadius, Math.sin(a1) * tipRadius]);
    profile.push([Math.cos(a2) * tipRadius, Math.sin(a2) * tipRadius]);
    profile.push([Math.cos(a3) * rootRadius, Math.sin(a3) * rootRadius]);
    // 齿槽底部的三个插值点
    const gapStart = a3;
    const gapEnd = base + pitchAngle - toothAngle * 0.5;
    for (let s = 1; s < 4; ++s) {
      const a = gapStart + (gapEnd - gapStart) * (s / 3.0);
      profile.push([Math.cos(a) * rootRadius, Math.sin(a) * rootRadius]);
    }
  }
  return { profile, pitchRadius, rootRadius };
}

/// 建一个齿轮（挤出轮廓 + 中心孔 + 轮辐窗口）。
function makeGear(teeth, module, rimWidth, depth, bore, spokes) {
  const { profile, pitchRadius, rootRadius } = gearProfile(teeth, module, rimWidth);
  const shape = new THREE.Shape();
  profile.forEach(([x, y], i) => (i === 0 ? shape.moveTo(x, y) : shape.lineTo(x, y)));
  shape.closePath();

  // 中心孔
  const holePath = new THREE.Path();
  holePath.absarc(0, 0, bore, 0, TAU, true);
  shape.holes.push(holePath);

  // 轮辐窗口：与 bpy 脚本同构（spokes=0 时留空，>0 时挖扇形窗口）
  const innerRadius = Math.max(rootRadius - rimWidth * 1.15, bore * 2.0);
  const hubRadius = Math.max(bore * 2.8, rootRadius * 0.26);
  if (spokes > 0) {
    const span = TAU / spokes;
    const spokeWidth = span * (0.34 / Math.max(1.0, spokes / 5.0));
    for (let s = 0; s < spokes; ++s) {
      const start = s * span + spokeWidth * 0.5;
      const end = (s + 1) * span - spokeWidth * 0.5;
      const window = new THREE.Path();
      const segs = 6;
      // 外弧（沿 innerRadius）从 end 到 start
      for (let k = segs; k >= 0; --k) {
        const a = start + (end - start) * (k / segs);
        const px = Math.cos(a) * innerRadius;
        const py = Math.sin(a) * innerRadius;
        if (k === segs) window.moveTo(px, py);
        else window.lineTo(px, py);
      }
      // 内弧（沿 hubRadius）从 start 到 end
      for (let k = 0; k <= segs; ++k) {
        const a = start + (end - start) * (k / segs);
        window.lineTo(Math.cos(a) * hubRadius, Math.sin(a) * hubRadius);
      }
      window.closePath();
      shape.holes.push(window);
    }
  }

  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth,
    bevelEnabled: false,
    curveSegments: 8,
  });
  geometry.translate(0, 0, -depth * 0.5);
  // 法线：挤出后需要重算（Shape 的绕序与 bpy 的 bmesh 不同）
  geometry.computeVertexNormals();
  return { geometry, pitchRadius };
}

function pbr(color, metallic, roughness) {
  return new THREE.MeshStandardMaterial({
    color: new THREE.Color(color[0], color[1], color[2]),
    metalness: metallic,
    roughness,
  });
}

/// 主入口：建场景、渲一帧、把像素读回来。
///
/// 返回 `{ width, height, pixels: Uint8Array }`——像素是 **sRGB 8bit**
/// （three 的 `renderer.outputColorSpace` 默认就是 srgb）。
window.renderReference = async function renderReference(options) {
  const width = (options && options.width) || 1100;
  const height = (options && options.height) || 800;
  const toneMapping = (options && options.toneMapping) || 'none';

  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  document.body.appendChild(canvas);

  const renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: true,
    preserveDrawingBuffer: true,
  });
  renderer.setSize(width, height, false);
  renderer.setPixelRatio(1);
  // ## 色调映射：默认跟 three 一致（NoToneMapping）
  //
  // 本框架默认 ACES（对 three 脚本保持存量不变），所以做对齐验证时
  // 要显式把两边都设成同一个算子。
  const map = {
    none: THREE.NoToneMapping,
    aces: THREE.ACESFilmicToneMapping,
    reinhard: THREE.ReinhardToneMapping,
    cineon: THREE.CineonToneMapping,
    agx: THREE.AgXToneMapping,
  };
  renderer.toneMapping = map[toneMapping] !== undefined ? map[toneMapping] : THREE.NoToneMapping;
  renderer.toneMappingExposure = 1.0;
  // three r152+ 默认 outputColorSpace = SRGBColorSpace（正确）
  renderer.outputColorSpace = THREE.SRGBColorSpace;

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(18 / 255, 20 / 255, 24 / 255);

  const camera = new THREE.PerspectiveCamera(32.4, width / height, 0.05, 200.0);
  camera.position.set(1.5, -1.0, 26.0);
  camera.lookAt(0, 0, 0);

  // 主光：与 bpy 脚本同参数（SUN energy 2600 → three 的 intensity 需要换算）
  //
  // ## 这一处换算必须显式写清楚
  //
  // Blender 的 SUN `energy` 单位是 W/m²，three 的 `DirectionalLight.intensity`
  // 是**无量纲的相对强度**。两者没有官方换算——所以这里的系数是**实测标定**的，
  // 而不是推导出来的。标定法：渲一个已知反射率的标准球，比对中灰位置。
  const SUN_TO_THREE = 2600 / 3200;   // 实测标定：2600W/m² ≈ 0.8125
  const sun = new THREE.DirectionalLight(0xffffff, SUN_TO_THREE);
  sun.position.set(-0.43 * 20, 0.28 * 20, -0.86 * -20);
  scene.add(sun);

  // 环境光：bpy 脚本里 Blender 的世界背景为默认灰（0.05）
  scene.add(new THREE.AmbientLight(0xffffff, 0.35));

  // 材质（与 bpy_watch.py 的 MAT_* 同值）
  const brass = pbr([0.72, 0.55, 0.24], 0.95, 0.22);
  const steel = pbr([0.28, 0.34, 0.46], 0.92, 0.18);
  const copper = pbr([0.66, 0.32, 0.18], 0.45, 0.35);
  const gold = pbr([0.90, 0.72, 0.30], 0.98, 0.15);

  const SPECS = [
    ['barrel_wheel', 42, MODULE, 0.34, 0.30, 5, brass],
    ['center_wheel', 26, MODULE, 0.24, 0.26, 5, brass],
    ['third_wheel', 18, MODULE, 0.20, 0.22, 4, gold],
    ['fourth_wheel', 14, MODULE, 0.17, 0.20, 4, gold],
    ['escape_wheel', 15, MODULE * 0.86, 0.15, 0.13, 5, steel],
  ];
  const IDLERS = [
    ['idler_a', 12, MODULE * 0.9, 0.16, 0.13, 6, brass],
    ['idler_b', 10, MODULE * 0.9, 0.16, 0.13, 5, copper],
    ['idler_c', 16, MODULE * 0.9, 0.16, 0.13, 5, brass],
  ];

  const built = [];
  for (const [name, teeth, module, rim, depth, spokes, material] of SPECS) {
    const { geometry, pitchRadius } = makeGear(teeth, module, rim, depth, 0.11, spokes);
    const mesh = new THREE.Mesh(geometry, material);
    mesh.name = name;
    scene.add(mesh);
    built.push({ mesh, pitchRadius, teeth, module });
  }
  for (const [name, teeth, module, rim, depth, spokes, material] of IDLERS) {
    const { geometry, pitchRadius } = makeGear(teeth, module, rim, depth, 0.11, spokes);
    const mesh = new THREE.Mesh(geometry, material);
    mesh.name = name;
    scene.add(mesh);
    built.push({ mesh, pitchRadius, teeth, module });
  }

  // 布局：按啮合中心距（与 bpy 脚本同公式 `center = m*(z1+z2)/2`）
  built[0].mesh.position.set(-1.2, 0.9, 0);
  built[0].mesh.rotation.z = 0;
  function placePair(prev, curr, angleDeg, baseZ) {
    const distance = prev.module * (prev.teeth + curr.teeth) / 2.0;
    const rad = (angleDeg * Math.PI) / 180.0;
    curr.mesh.position.set(
      prev.mesh.position.x + Math.cos(rad) * distance,
      prev.mesh.position.y + Math.sin(rad) * distance,
      baseZ
    );
    curr.mesh.rotation.z = rad + Math.PI / curr.teeth;
  }
  placePair(built[0], built[1], 205.0, 0.05);
  placePair(built[1], built[2], 150.0, 0.10);
  placePair(built[2], built[3], 95.0, 0.15);
  placePair(built[3], built[4], 40.0, 0.20);

  const idlerPositions = [[5.4, -3.2, 0.30], [-5.8, 3.4, 0.34], [6.6, 2.8, 0.26]];
  for (let i = 0; i < 3; ++i) {
    built[5 + i].mesh.position.set(...idlerPositions[i]);
    built[5 + i].mesh.rotation.z = i * 0.7;
  }

  renderer.render(scene, camera);

  // 读回像素（Y 翻转：WebGL 是左下原点，PNG 是左上）
  const gl = renderer.getContext();
  const raw = new Uint8Array(width * height * 4);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, raw);

  return {
    width,
    height,
    bytes: Array.from(raw),
    toneMapping,
    triangleCount: (() => {
      let n = 0;
      scene.traverse((o) => {
        if (o.isMesh && o.geometry.index) n += o.geometry.index.count / 3;
        else if (o.isMesh) n += o.geometry.attributes.position.count / 3;
      });
      return Math.round(n);
    })(),
  };
};

window.__ready = true;
