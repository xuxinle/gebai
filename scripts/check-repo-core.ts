/**
 * 仓库自包含自检的**判定核**（纯逻辑，无文件系统）：从「被 gitignore 忽略的文件清单」里挑出
 * 那些**本该入库、却因忽略规则而从未入库的源文件**。
 *
 * 为什么单独拆出来：判定表（源码扩展名 / 产物目录段 / 豁免路径）是这个闸门的全部内容，
 * 而闸门本身**失败时是红、通过时是绿**——只有单测能在它「悄悄放过该报的」时立刻发现。
 * 实测教训：加了豁免之后如果没单测，「豁免写歪一点、把真源码一并盖住」不会有任何反馈
 * （闸门照样退出码 0，最安静的那种失效）。
 *
 * 判定顺序（先豁免、后产物段，与直觉相反但有必要）：
 *  1. `.generated.` 生成物约定 → 跳过
 *  2. 非源码扩展名 → 跳过（生成物不走这些扩展名）
 *  3. **显式豁免**（精确路径前缀 + 必须写理由）→ 跳过
 *  4. 路径含产物目录段（build/dist/node_modules…）→ 跳过
 *  5. 其余 = 违规
 * 第 3 步放在第 4 步之前，是为了让豁免能覆盖「目录段判定够不到」的形态
 * （如 `infer/engine/llama.cpp-b11100/src`——段名里没有 build，但它确实是托管资产）。
 */

/** 源码类扩展名：这些文件**必须**入库（生成物不走这些扩展名，故不会误报）。 */
export const SOURCE_EXTENSIONS: ReadonlySet<string> = new Set([
  ".ts",
  ".tsx",
  ".mts",
  ".cts",
  ".js",
  ".mjs",
  ".cjs",
  ".jsx",
  ".py",
  ".c",
  ".cc",
  ".cpp",
  ".cxx",
  ".h",
  ".hh",
  ".hpp",
  ".rs",
  ".go",
  ".java",
  ".kt",
  ".swift",
  ".rb",
  ".lua",
])

/**
 * 路径中出现这些**目录段**即跳过：构建产物、依赖缓存、外部依赖源码（由各自的下载/固化流程管理）。
 * 采用「段名」判定而非具体路径，是因为这些目录会出现在各处的层级里（`<pkg>/build`、`<pkg>/dist`…）。
 */
export const IGNORED_SEGMENTS: ReadonlySet<string> = new Set([
  "node_modules",
  "dist",
  "build",
  "out",
  "target",
  ".turbo",
  ".git",
  ".cache",
  "cache",
  "coverage",
  ".venv",
  "venv",
  "__pycache__",
  "vendor",
  ".next",
  ".pnpm-store",
  "tmp",
  "temp",
])

/** 构建期生成物的仓库内约定（`.gitignore` 里逐条列出，由 `scripts/build-*.ts` 重建）。 */
export const GENERATED_MARKER = ".generated."

/**
 * 显式豁免：**精确路径前缀** + **必须写理由**。
 *
 * 与 `IGNORED_SEGMENTS` 的分工：段名规则是「这类目录一律不算源码」的粗筛，语义清晰但覆盖面
 * 取决于目录名；豁免表用来收口「形态不像产物目录、却确实是托管/生成资产」的具体位置。
 *
 * **纪律**（这张表是闸门唯一能被削弱的地方，写错就是静默失效）：
 *  - 只写**具体路径**，不写宽泛的段名或单字母通配；
 *  - 每条必须给出 `reason`：**谁生成/谁托管、重建入口在哪**——说不清重建方式的不许进表；
 *  - 三种前缀形式必须**显式表达意图**（不允许“看起来像前缀就算”）：
 *    · 以 `*` 结尾 = **段内部分前缀**（`infer/engine/llama.cpp-*` 命中 `llama.cpp-b11100/…`，
 *      用于版本号会变、但属于同一类托管资产的目录）；
 *    · 以 `/` 结尾 = **目录前缀**（命中其下全部）；
 *    · 其余 = **精确路径**（相等，或作为目录被其后 `/` 跟随）——故
 *      `…/three_browser.js` 不会连 `three_browser.js.bak.ts` 一起盖住。
 */
export interface SelfContainedExemption {
  /** 路径匹配式（三形式见上：`*` 结尾 / `/` 结尾 / 精确路径）。 */
  prefix: string
  /** 为什么这里可以不放源码：谁生成 / 谁托管 / 怎么重建。 */
  reason: string
}

export const EXEMPT_PREFIXES: readonly SelfContainedExemption[] = [
  {
    prefix: "infer/engine/llama.cpp-*",
    reason:
      "llama.cpp 上游源码树（自持 fork 的基线），不入库——由 infer/config/assets.manifest.json 的 " +
      "engine-source 条目托管，`pwsh infer/scripts/restore.ps1 -Only engine-source` 按 sha256 校验后解压重建；" +
      "入库的只有 infer/engine/README.md（fork 说明与补丁清单）与构建 .bat",
  },
  {
    prefix: "infer/engine/dl/",
    reason: "上游源码/二进制的下载缓存，由 restore.ps1 重新拉取",
  },
  {
    prefix: "shuangtian/.st/",
    reason:
      "霜天的运行时 dot 目录：CDP/VSCode 调试用的浏览器 profile、探测产物、临时脚本。" +
      "内容随时可丢弃重建（`st` 每次运行重写），不是源码",
  },
  {
    prefix: "shuangtian/apps/caiyun/_build/",
    reason:
      "裁云模板的打包产物：`shuangtian/apps/caiyun/kit/build.py` 从 kit/templates/*.js 生成（文件头自带「请勿手改」）；" +
      "源模板已在库中",
  },
  {
    prefix: "shuangtian/apps/caiyun/kit/_build/",
    reason: "同上：kit/build.py 的产物目录（与 templates 一一对应，源已入库）",
  },
  {
    prefix: "shuangtian/tools/parity/three_browser.js",
    reason: "由 `shuangtian/tools/parity/build_bundle.mjs` 从 third_party/three/three.cjs 生成的浏览器侧 bundle（1.2MB，文件头自带「请勿手改」）",
  },
  {
    prefix: "data/",
    reason:
      "GEBAI_HOME 的运行时数据根（`/data/` 已整体 gitignore）：册府/知识图谱等生成物落在这里" +
      "（文件头自带「由 xxx 生成，不要手改」），属用户数据而非仓库源码",
  },
  {
    prefix: "users/",
    reason: "用户数据根（会话记录、工作区、附件）——运行态数据，天然不入库",
  },
]

/**
 * 路径是否命中显式豁免。三种前缀形式（语义见 `EXEMPT_PREFIXES` 的注释）：
 *  - `*` 结尾 → 段内部分前缀（去掉 `*` 后做字符串前缀匹配）
 *  - `/` 结尾 → 目录前缀
 *  - 其余 → 精确路径（相等，或作为目录被其后 `/` 跟随）
 * 导出供单测直接验证「豁免命中」与「不误杀相似路径」两件事。
 */
export function isExempt(relative: string): boolean {
  return EXEMPT_PREFIXES.some(({ prefix }) => {
    if (prefix.endsWith("*")) return relative.startsWith(prefix.slice(0, -1))
    if (prefix.endsWith("/")) return relative.startsWith(prefix)
    return relative === prefix || relative.startsWith(`${prefix}/`)
  })
}

/** 单条路径是否需要入库（`false` = 可忽略）。违规判定即 `needsTracking(rel) === true`。 */
export function needsTracking(relative: string): boolean {
  const segments = relative.split("/")
  const fileName = segments.at(-1) ?? ""
  if (fileName.includes(GENERATED_MARKER)) return false
  const dot = fileName.lastIndexOf(".")
  if (dot < 0 || !SOURCE_EXTENSIONS.has(fileName.slice(dot).toLowerCase())) return false
  if (isExempt(relative)) return false
  // 目录段判定：末段（文件名）不参与，只看它所在的目录链
  if (segments.slice(0, -1).some((segment) => IGNORED_SEGMENTS.has(segment))) return false
  return true
}

/** 从忽略清单里挑出违规项（保持输入顺序，便于报错时按路径稳定排序）。 */
export function findOffenders(ignored: readonly string[]): string[] {
  return ignored.filter((relative) => relative.length > 0 && needsTracking(relative))
}
