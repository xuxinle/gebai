// 浏览器真值通道：把同一份 three.js 场景在无头 Chrome（SwiftShader）里渲出来，
// 取 gl.readPixels 的原始 RGBA，落成 PNG。
//
// 为什么不截图：截图经过窗口合成、色彩管理、DPI 缩放，拿到的不是渲染器的输出。
// 这里让页面自己 POST 回读像素，服务端写 PNG——只经过 three 自己的输出链
// （toneMapping → outputColorSpace → 8bit）。
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { dirname as pathDirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const __dirname = pathDirname(fileURLToPath(import.meta.url));

const CHROME = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';

// ── 最小 PNG 编码（RGBA8，无滤波）──
function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function encodePng(width, height, rgba) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;
    rgba.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// ── 页面：加载 three bundle + 用户的场景源码 + 显式 renderer 配置 ──
function buildPage({ bundle, sceneSource, width, height, toneMapping, exposure }) {
  return `<!DOCTYPE html><html><head><meta charset="utf-8"><style>
html,body{margin:0;padding:0;background:#000}canvas{display:block}
</style></head><body>
<script>
${bundle}
</script>
<script>
(async function () {
  const THREE = globalThis.THREE;
  if (!THREE || !THREE.Scene) throw new Error('three bundle 未就绪（globalThis.THREE 为空）');
  const W = ${width}, H = ${height};
  const log = [];
  function post(path, body) {
    return fetch(path, { method: 'POST', body });
  }
  try {
    // 用户的场景源码：末表达式是场景根（霜天的同一条约定）
    const build = new Function('THREE', ${JSON.stringify(sceneSource)} + '\\n;return (typeof scene!=="undefined")?scene:null;');
    const scene = build(THREE);
    if (!scene) throw new Error('scene 为空');

    let camera = null;
    scene.traverse((o) => { if (!camera && o.isCamera) camera = o; });
    if (!camera) throw new Error('场景里没有相机');

    const renderer = new THREE.WebGLRenderer({ antialias: false, preserveDrawingBuffer: true, alpha: false });
    renderer.setPixelRatio(1);
    renderer.setSize(W, H, false);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.toneMapping = ${toneMapping};
    renderer.toneMappingExposure = ${exposure};
    renderer.shadowMap.enabled = false;
    document.body.appendChild(renderer.domElement);

    renderer.render(scene, camera);
    const gl = renderer.getContext();
    const raw = new Uint8Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, raw);
    // WebGL 左下原点 → PNG 左上原点
    const flipped = new Uint8Array(W * H * 4);
    const stride = W * 4;
    for (let y = 0; y < H; y++) flipped.set(raw.subarray(y * stride, (y + 1) * stride), (H - 1 - y) * stride);
    const meta = {
      version: THREE.REVISION,
      gl: gl.getParameter(gl.VERSION),
      renderer: gl.getParameter(gl.RENDERER),
      toneMapping: renderer.toneMapping,
      exposure: renderer.toneMappingExposure,
      outputColorSpace: renderer.outputColorSpace,
      triangles: (() => { let n = 0; scene.traverse(o => { if (o.isMesh) n += (o.geometry.index ? o.geometry.index.count : o.geometry.attributes.position.count) / 3; }); return Math.round(n); })(),
    };
    await post('/meta?w=' + W + '&h=' + H, JSON.stringify(meta));
    await post('/pixels?w=' + W + '&h=' + H, flipped);
    document.title = 'PARITY_DONE';
  } catch (e) {
    await post('/error', String(e && e.stack ? e.stack : e));
    document.title = 'PARITY_ERROR';
  }
})();
</script></body></html>`;
}

// ── 主流程 ──
const args = process.argv.slice(2);
function arg(name, def) {
  const i = args.indexOf('--' + name);
  return i >= 0 ? args[i + 1] : def;
}

const scenePath = resolve(arg('scene', ''));
const outPath = resolve(arg('out', 'browser.png'));
const width = Number(arg('width', 960));
const height = Number(arg('height', 540));
const toneArg = arg('tone', 'NoToneMapping');
const exposure = Number(arg('exposure', 1.0));
// three 的浏览器 bundle：由 `build_bundle.mjs` 生成（直接包一层 CJS 宿主，加载即生效）。
//
// ## 为什么不用 `bun build --format=iife`
//
// 实测过三种写发，只有这个确定：
//
// | 写发 | 结果 |
// |---|---|
// | `<script>${iife_bundle}</script>` | `THREE.Scene is not a constructor`（bun 把 CJS 模块包成**惰性函数**，只定义不执行） |
// | 在 bundle 末尾追 `require_x()` | 语法错（`lastIndexOf('})();')` 命中的是内部 `__commonJS` 的收尾） |
// | ESM + `<script type="module">` | 能跑，但模块作用域的赋值时序不稳 |
const bundlePath = resolve(arg('bundle', resolve(__dirname, 'three_browser.js')));
if (!existsSync(bundlePath)) {
  const { execFileSync } = await import('node:child_process');
  execFileSync('bun', [resolve(__dirname, 'build_bundle.mjs')], { stdio: 'inherit' });
}
const bundle = readFileSync(bundlePath, 'utf8');

const toneConst = 'THREE.' + toneArg;
const sceneSource = readFileSync(scenePath, 'utf8');
const html = buildPage({ bundle, sceneSource, width, height, toneMapping: toneConst, exposure });

mkdirSync(pathDirname(outPath), { recursive: true });
let pixels = null, meta = null, errors = [];

// Bun.serve 收像素
const server = Bun.serve({
  port: 0,
  async fetch(req) {
    const url = new URL(req.url);
    if (req.method === 'POST') {
      if (url.pathname === '/pixels') {
        pixels = Buffer.from(await req.arrayBuffer());
        return new Response('ok');
      }
      if (url.pathname === '/meta') { meta = await req.text(); return new Response('ok'); }
      if (url.pathname === '/error') { errors.push(await req.text()); return new Response('ok'); }
    }
    if (url.pathname === '/page.html') return new Response(html, { headers: { 'content-type': 'text/html' } });
    if (url.pathname === '/done') return new Response(pixels ? 'done' : 'wait');
    return new Response('not found', { status: 404 });
  },
});
const port = server.port;

const profileDir = resolve('profile');
const child = spawn(CHROME, [
  '--headless=new',
  '--disable-gpu', // 强制 SwiftShader（deterministic CPU 光栅）
  `--user-data-dir=${profileDir}`,
  '--no-first-run',
  '--no-default-browser-check',
  '--disable-extensions',
  '--disable-background-networking',
  '--force-color-profile=srgb',
  '--hide-scrollbars',
  '--allow-running-insecure-content',
  '--enable-unsafe-swiftshader',
  `--window-size=${width},${height}`,
  `http://127.0.0.1:${port}/page.html`,
], { stdio: ['ignore', 'pipe', 'pipe'] });

let stderr = '';
child.stderr.on('data', (d) => { stderr += d.toString(); });

const deadline = Date.now() + 60000;
let ok = false;
while (Date.now() < deadline) {
  await new Promise((r) => setTimeout(r, 300));
  if (pixels && pixels.length === width * height * 4) { ok = true; break; }
  if (errors.length) break;
}
try { child.kill(); } catch {}
server.stop(true);

if (!ok) {
  console.error('捕获失败');
  if (errors.length) console.error(errors.join('\n'));
  console.error(stderr.slice(-2000));
  process.exit(1);
}
writeFileSync(outPath, encodePng(width, height, pixels));
writeFileSync(outPath.replace(/\.png$/, '.json'), meta || '{}');
console.log(`OK ${outPath} ${width}x${height}`);
console.log(meta || '{}');










