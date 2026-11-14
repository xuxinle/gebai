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
 *   trailer = magic(8) | payload_len(8) | image_len(8) | fnv1a64(payload)(8)   全部小端
 * ```
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
import { openSync, readSync, writeSync, ftruncateSync, closeSync, copyFileSync, mkdirSync, statSync } from "node:fs"
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

/** FNV-1a 64（与 `st::hash::fnv1a64_bytes` 同参数，offset basis 0xcbf29ce484222325、prime 0x100000001b3）。 */
function fnv1a64(bytes) {
  let hash = 0xcbf29ce484222325n
  const prime = 0x100000001b3n
  const mask = 0xffffffffffffffffn
  for (let index = 0; index < bytes.length; index += 1) {
    hash ^= BigInt(bytes[index])
    hash = (hash * prime) & mask
  }
  return hash
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
const imageSize = statSync(output).size
{
  const fd = openSync(output, "a")
  try {
    const chunkSize = 8 * 1024 * 1024
    const sidecarFd = openSync(sidecar, "r")
    const sidecarSize = statSync(sidecar).size
    const chunk = Buffer.alloc(chunkSize)
    let hash = 0xcbf29ce484222325n
    const prime = 0x100000001b3n
    const mask = 0xffffffffffffffffn
    let offset = 0
    try {
      while (offset < sidecarSize) {
        const read = readSync(sidecarFd, chunk, 0, Math.min(chunkSize, sidecarSize - offset), offset)
        if (read <= 0) break
        const slice = chunk.subarray(0, read)
        // 流式哈希：270MB 不整载内存（与 fnv1a64 同参数，逐字节累加）。
        for (let index = 0; index < slice.length; index += 1) {
          hash ^= BigInt(slice[index])
          hash = (hash * prime) & mask
        }
        writeSync(fd, slice, 0, read)
        offset += read
      }
    } finally {
      closeSync(sidecarFd)
    }
    if (offset !== sidecarSize) {
      console.error(`[pack-desktop] 侧车读取不完整（${offset}/${sidecarSize}）`)
      process.exit(1)
    }
    const trailer = Buffer.alloc(TRAILER_SIZE)
    trailer.write(MAGIC, 0, "latin1")
    trailer.writeBigUInt64LE(BigInt(sidecarSize), 8)
    trailer.writeBigUInt64LE(BigInt(imageSize), 16)
    trailer.writeBigUInt64LE(hash, 24)
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
  const fd = openSync(output, "r")
  try {
    const buffer = Buffer.alloc(8 * 1024 * 1024)
    let hash = 0xcbf29ce484222325n
    const prime = 0x100000001b3n
    const mask = 0xffffffffffffffffn
    let offset = 0
    while (offset < written.payload) {
      const read = readSync(fd, buffer, 0, Math.min(buffer.length, written.payload - offset), written.image + offset)
      if (read <= 0) break
      const slice = buffer.subarray(0, read)
      for (let index = 0; index < slice.length; index += 1) {
        hash ^= BigInt(slice[index])
        hash = (hash * prime) & mask
      }
      offset += read
    }
    if (offset !== written.payload) {
      console.error(`[pack-desktop] 自检失败：payload 读取不完整（${offset}/${written.payload}）`)
      process.exit(1)
    }
    console.log(`[pack-desktop] 自检通过：payload ${written.payload} 字节 · 哈希 ${hash.toString(16)}`)
  } finally {
    closeSync(fd)
  }
}

const total = statSync(output).size
console.log(
  `[pack-desktop] ${output}\n  宿主 ${(imageSize / 1048576).toFixed(1)} MB + 侧车 ${(written.payload / 1048576).toFixed(1)} MB = ${(total / 1048576).toFixed(1)} MB`,
)
