// 生成可在浏览器里当普通 <script> 用的 three bundle。
//
// ## 为什么要自己拼，而不是 `bun build --format=iife`
//
// 实测了三种做法，都不行/不稳：
//
// | 做法 | 结果 |
// |---|---|
// | `<script>${bundle_iife}</script>` | `THREE.Scene is not a constructor`——bun 把 CJS 模块包成**惰性函数**（`var require_x = __commonJS(cb)`），只定义不执行 |
// | 在 bundle 末尾追 `require_x()` | 语法错（`lastIndexOf('})();')` 命中的是内部 `__commonJS` 的收尾） |
// | ESM + `<script type="module">` | 跑通了，但模块作用域的 `globalThis` 赋值时序不可靠 |
//
// 所以改成**最笨但确定**的做法：直接把 three.cjs 的源码包一层
// `(function(exports, module){ ... })(...)`，再在**同一段脚本**里把
// `module.exports` 挂到全局。没有任何惰性包装、没有模块作用域。
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname as pathDirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = pathDirname(fileURLToPath(import.meta.url));
const source = readFileSync(resolve(here, '../../third_party/three/three.cjs'), 'utf8');
const out = resolve(here, 'three_browser.js');

const wrapped = `/* 由 tools/parity/build_bundle.mjs 生成，请勿手改。
 * 源：third_party/three/three.cjs（r160）
 *
 * 这层包装的形状与 CJS 宿主完全一致（exports / module），所以 three.cjs
 * 顶层读写的 \`exports.xxx = ...\` 全部落在 \`__three_exports\` 上。
 * 最后一句把结果挂到 globalThis.THREE——**没有惰性包装**，加载即生效。
 */
(function () {
  var __three_exports = {};
  var __three_module = { exports: __three_exports };
  (function (exports, module, require) {
${source}
  })(__three_exports, __three_module, undefined);
  globalThis.THREE = __three_module.exports;
})();
`;

writeFileSync(out, wrapped);
console.log(`已生成 ${out}（${wrapped.length} 字节）`);
