// 取 PNG 中心像素（含邻域），用于把「手算的期望值」与「实际渲出的值」对上。
//
// 用法: bun pixel.mjs <png> [x] [y]
import { readFileSync } from 'node:fs';
import { inflateSync } from 'node:zlib';

function decodePng(buf) {
  let pos = 8;
  let width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9];
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    pos += 12 + len;
  }
  if (bitDepth !== 8) throw new Error('只支持 8bit');
  const channels = colorType === 6 ? 4 : colorType === 2 ? 3 : colorType === 0 ? 1 : 0;
  if (!channels) throw new Error('不支持 colorType ' + colorType);
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(width * height * channels);
  const bpp = channels;
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? out[y * stride + x - bpp] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= bpp && y > 0 ? out[(y - 1) * stride + x - bpp] : 0;
      let v = line[x];
      switch (filter) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: {
          const p = a + b - c;
          const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: throw new Error('bad filter ' + filter);
      }
      out[y * stride + x] = v & 0xff;
    }
  }
  return { width, height, channels, data: out };
}

const path = process.argv[2];
const img = decodePng(readFileSync(path));
const cx = process.argv[3] ? Number(process.argv[3]) : (img.width >> 1);
const cy = process.argv[4] ? Number(process.argv[4]) : (img.height >> 1);
const px = (x, y) => {
  const i = (y * img.width + x) * img.channels;
  return [img.data[i], img.data[i + 1], img.data[i + 2]];
};
console.log(`${path} ${img.width}x${img.height} ch=${img.channels}`);
console.log(`  center(${cx},${cy}) = rgb(${px(cx, cy).join(',')})`);
// 取 5x5 邻域的范围，判断是否纯色（平面场景应该完全纯色）
const vals = [];
for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) vals.push(px(cx + dx, cy + dy)[0]);
console.log(`  R 5x5 邻域 min=${Math.min(...vals)} max=${Math.max(...vals)}`);
const corners = [[2, 2], [img.width - 3, 2], [2, img.height - 3], [img.width - 3, img.height - 3]];
console.log('  corners: ' + corners.map(([x, y]) => px(x, y).join(',')).join(' | '));
