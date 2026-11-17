/**
 * 桌面端打包：把 gebai 服务端二进制**挂到霜天宿主 exe 的末尾**，产出单文件 `dist/gebai-desktop.exe`。
 *
 * ## 为什么是「挂尾」而不是「编进去」
 *
 * 旧形态（Rust 启动器）用 `include_bytes!` 把整个服务端内嵌进启动器——那是 Rust 的常量池，
 * 只编一次。霜天的等价位是 `b::embed`，它把资源变成 **C++ 字节数组源码**：270MB 的二进制会
 * 生成约 1.1GB 的 `.cpp`，每次后端变化都要全量重编（实测十几分钟起，且中间产物巨大）。
 *
 * 挂尾把两件事分开：宿主（几十 MB C++ 工程）与侧车（bun --compile 产物）各自独立构建，
 * 最后由本脚本一次追加。**分发形态不变**：仍然是一个 exe，双击就跑。
 *
 * ## trailer 结构（必须与 `apps/gebai_desktop/main.cpp` 的 `read_payload_from_self` 一致）
 *
 * ```
 *   [宿主 exe 本体][侧车二进制][trailer 32 字节]
 *       trailer = magic(8) | payload_len(8) | image_len(8) | content_hash64(payload)(8)   全部小端
 * ```
 *
 * 哈希算法是 **`content_hash64`**（8 路独立通道），**不是 FNV-1a**：
 * 宿主启动时要哈希两次 259MB，而 FNV 的逐字节串行链实测 13.9 s/次（合计 28 s 启动开销）。
 * 两侧必须逐位一致（C++ 侧 `st::hash::content_hash64`），否则宿主会把每次启动都当成
 * “陈旧侧车”重写一遍。
 *
 * 固定 32 字节是为了运行时能 **seek 尾读**：扫描魔数要读完整 270MB，固定长度不用。
 * `image_len` 记的是宿主本体长度，三者之和必须等于文件长度——运行时据此判断「被截断」。
 *
 * ## 幂等
 *
 * 重复执行会把旧 payload **截掉再挂新的**：先按文件末尾的 trailer 校验结构，
 * 确认是上一次打包的产物才截断；不是就当普通宿主 exe，整个保留。
 * （不这么做的后果：反复打包会让文件无限增长，而每层 trailer 都“看起来合法”。）
 */
import { openSync, readSync, readFileSync, writeSync, ftruncateSync, closeSync, copyFileSync, mkdirSync, statSync } from "node:fs"
import { createHash } from "node:crypto"
import { dirname, join } from "node:path"

const TRAILER_SIZE = 32
const MAGIC = "GEBAIPAY"

const desktopDir = join(import.meta.dirname, "..")
/** 宿主 exe（霜天构建产物；`st build gebai_desktop` 的输出）。 */
const hostCandidates = [
  process.env.GEBAI_DESKTOP_HOST,
  join(desktopDir, "..", "..", "shuangtian", "build", "release", "bin", "gebai_desktop.exe"),
  join(desktopDir, "..", "..", "shuangtian", "build", "dev", "bin", "gebai_desktop.exe"),
].filter(Boolean)
/** 侧车（bun --compile 产物）：server:build 已产出。 */
const sidecar = process.env.GEBAI_DESKTOP_SIDECAR ?? join(desktopDir, "dist", "gebai.exe")
/** 产出（与原 Rust 启动器同名，下游脚本/文档不必改）。 */
const output = join(desktopDir, "dist", "gebai-desktop.exe")

const host = hostCandidates.find((candidate) => {
  try {
    return statSync(candidate).isFile()
  } catch {
    return false
  }
})
if (!host) {
  console.error(
    `[pack-desktop] 找不到宿主 exe。先构建它：\n  shuangtian\\build\\bin\\st.exe build gebai_desktop --profile release\n找过：\n  ${hostCandidates.join("\n  ")}`,
  )
  process.exit(1)
}
if (!exists(sidecar)) {
  console.error(`[pack-desktop] 找不到侧车：${sidecar}\n先构建：bun run --cwd packages/desktop server:build`)
  process.exit(1)
}

function exists(path) {
  try {
    return statSync(path).isFile()
  } catch {
    return false
  }
}

/**
 * 侧车内容指纹（十六进制）。**必须与宿主算法一致**。
 *
 * ## 为何不再自实现（实测代价）
 *
 * 之前脚本里自己写了一份 8 路乘积哈希，逐字对应 C++ 的 `content_hash64`。
 * 语义上没错，但 **JS 的 64 位整数只能靠 BigInt**，而 BigInt 每次运算都是堆分配的
 * 多精度对象：实测这份 259MB 算一次 **14.8 秒**，占据整个 `pack-desktop`
 * 46 秒里的一大半。而同一份字节用内置实现：
 *
 * | 实现 | 耗时 |
 * |---|---|
 * | BigInt 手写 | **14,788 ms** |
 * | `crypto.createHash("sha256")` | **150 ms**（硬件 SHA 指令集）|
 *
 * 差 **近 100 倍**。SHA-256 的 32 字节摘要里取前 8 字节作指纹，碰撞概率
 * （比“本 exe 尾部这一份 payload 是否与上一次打包一致”的判据需要）已经是天文学级别。
 *
 * ## 宿主侧跟着换成 SHA-256
 *
 * 这不是“脚本自己换个算法就行”：trailer 里的值必须两侧算得一样。所以宿主的
 * `st::hash::content_hash64` 也一并改为 SHA-256 前 8 字节（同样靠系统/库的
 * 优化实现，而不是手写循环）。
 */
function contentHash64Hex(bytes: Uint8Array): string {
  const digest = createHash("sha256").update(bytes).digest()
  // 取前 8 字节作 64 位指纹（小端读，与 C++ 侧 `read_u64_le(digest)` 同口径）。
  let value = 0n
  for (let i = 0; i < 8; i++) value |= BigInt(digest[i]) << BigInt(i * 8)
  return value.toString(16).padStart(16, "0")
}

/** 读文件末尾的 trailer；不是本工具写的就返回 null。 */
function readTrailer(path) {
  const size = statSync(path).size
  if (size < TRAILER_SIZE) return null
  const fd = openSync(path, "r")
  try {
    const buffer = Buffer.alloc(TRAILER_SIZE)
    readSync(fd, buffer, 0, TRAILER_SIZE, size - TRAILER_SIZE)
    if (buffer.subarray(0, MAGIC.length).toString("latin1") !== MAGIC) return null
    const payload = Number(buffer.readBigUInt64LE(8))
    const image = Number(buffer.readBigUInt64LE(16))
    if (payload <= 0 || image <= 0 || payload + image + TRAILER_SIZE !== size) return null
    return { image, payload }
  } finally {
    closeSync(fd)
  }
}

// ① 复制宿主到产出位置，并把上一次的 payload 截掉（幂等）。
//
// 目标被占用（上一个 gebai-desktop 还在跑）时 `copyFileSync` 只报 EBUSY，
// 报错里没有“该怎么办”。这里先探测再给可操作提示（实测：一个残留实例
// 会让整个 `launcher:build` 挂在一个与构建无关的地方）。
function isLocked(path) {
  if (!exists(path)) return false
  try {
    closeSync(openSync(path, "r+"))
    return false
  } catch {
    return true
  }
}
if (isLocked(output)) {
  console.error(
    `[pack-desktop] 产物被占用，无法写入：${output}\n结束那个实例（关掉 gebai-desktop 窗口，或任务管理器结束它）后重试。`,
  )
  process.exit(1)
}
mkdirSync(dirname(output), { recursive: true })
try {
  copyFileSync(host, output)
} catch (err) {
  console.error(`[pack-desktop] 复制宿主失败：${host} -> ${output}\n${err.message}`)
  process.exit(1)
}
const previous = readTrailer(output)
if (previous) {
  const fd = openSync(output, "r+")
  try {
    ftruncateSync(fd, previous.image)
  } finally {
    closeSync(fd)
  }
  console.log(`[pack-desktop] 已截掉旧 payload（${previous.payload} 字节）`)
}

// ② 追加侧车 + trailer。
//
// ## 为何整载内存而不是分块流式（与旧版的关键差异）
//
// 旧版分块哈希 FNV-1a，是因为 FNV 可以跨块续算（状态只有一个）。
// 而 `content_hash64` 是**位置相关**的（8 路轮转 + 长度参与），分块算会得到与
// 一次性算不同的值——而宿主那边是**一次性算整块**（它本来就把 payload 整载内存）。
// 两侧必须同口径，所以这里也整载。
//
// 259MB 整载完全可接受（宿主侧也是这么做的，实测读取只要 166ms），
// 换来的是一致性与一个数量级的速度。
const imageSize = statSync(output).size
{
  const fd = openSync(output, "a")
  try {
    const payloadBytes = readFileSync(sidecar)
    const sidecarSize = payloadBytes.length
    // 一次性算（SHA-256 前 8 字节，与 C++ `content_hash64` 同口径）。
    const hashHex = contentHash64Hex(payloadBytes)
    writeSync(fd, payloadBytes, 0, sidecarSize)
    const trailer = Buffer.alloc(TRAILER_SIZE)
    trailer.write(MAGIC, 0, "latin1")
    trailer.writeBigUInt64LE(BigInt(sidecarSize), 8)
    trailer.writeBigUInt64LE(BigInt(imageSize), 16)
    trailer.writeBigUInt64LE(BigInt("0x" + hashHex), 24)
    writeSync(fd, trailer)
  } finally {
    closeSync(fd)
  }
}

// ③ 自检：按运行时的判据回读一遍（长度自洽 + 哈希一致），不一致就不交付。
const written = readTrailer(output)
if (!written || written.image !== imageSize) {
  console.error("[pack-desktop] 自检失败：trailer 结构不符")
  process.exit(1)
}
{
  // 自检也整载（与 ② 同一算法：`content_hash64` 是位置相关的，分块算不等价）
  const whole = readFileSync(output)
  const payloadView = whole.subarray(written.image, written.image + written.payload)
  if (payloadView.length !== written.payload) {
    console.error(`[pack-desktop] 自检失败：payload 读取不完整（${payloadView.length}/${written.payload}）`)
    process.exit(1)
  }
  const hashHex = contentHash64Hex(payloadView)
  // 与 trailer 里记的值对一遍：这是“打包脚本自己算出来的值能不能被宿主复现”的唯一证据。
  const trailerHash = (() => {
    const buffer = Buffer.alloc(TRAILER_SIZE)
    const fd = openSync(output, "r")
    try {
      readSync(fd, buffer, 0, TRAILER_SIZE, statSync(output).size - TRAILER_SIZE)
    } finally {
      closeSync(fd)
    }
    return buffer.readBigUInt64LE(24)
  })()
  if (BigInt("0x" + hashHex) !== trailerHash) {
    console.error(`[pack-desktop] 自检失败：重算哈希与 trailer 不符（${hashHex} ≠ ${trailerHash.toString(16)}）`)
    process.exit(1)
  }
  console.log(`[pack-desktop] 自检通过：payload ${written.payload} 字节 · 哈希 ${hashHex}`)
}

const total = statSync(output).size
console.log(
  `[pack-desktop] ${output}\n  宿主 ${(imageSize / 1048576).toFixed(1)} MB + 侧车 ${(written.payload / 1048576).toFixed(1)} MB = ${(total / 1048576).toFixed(1)} MB`,
)
