/**
 * 静态加密（任务级环境变量的敏感值落盘保护）。
 *
 * 用途：`users/{user}/tasks.json` 是明文 JSON，用户把密钥类配置写进任务级 `env` 后会以明文长期驻留——
 * 文件工作台浏览、备份/同步、全盘 grep，乃至模型经 read/grep 读取任务定义时都会直接泄露。本模块把
 * **敏感键**的值封存后再落盘（非敏感值保持明文，便于运维直接查看任务配置）。
 *
 * 威胁边界（务必如实理解）：无人值守任务必须能自动解密，密钥不可能来自用户的交互输入，因此密钥与
 * 数据必然同机——本模块防的是「随手看到」（文件被浏览/被 grep/随备份外带/被模型读进上下文），
 * **不防**已经能读取整个数据目录与产物的攻击者（他能连密钥一起读）。这与「内置模型 Key 随二进制
 * 分发」是同一档强度：提高门槛，不作密码学保证。
 *
 * 格式：`enc:v1:<base64(iv)>:<base64(ciphertext||tag)>`——带版本前缀，便于日后轮换算法；
 * 以 `enc:v1:` 开头的字符串视为密文，解密失败（密钥不符/数据损坏）返回 null 由调用方降级为明文，
 * 绝不抛错打断调度。
 */
import { createCipheriv, createDecipheriv, createHash, randomBytes } from "node:crypto"

/** 封存值前缀（版本化：日后换算法可并行识别 v1/v2）。 */
export const ENC_PREFIX = "enc:v1:"

/**
 * 内置密钥（构建期随二进制分发）：无人值守任务要在没有任何交互的前提下自解密，故密钥只能是
 * 「服务端自己知道的常量」，而非用户输入的口令。
 *
 * 安全注意：常量随产物分发，**持有产物者可提取**——仅用于提高「随手暴露」的门槛。
 * 需要更高强度时由部署方在构建前替换本值（同一 GEBAI_HOME 下更换密钥后，旧密文解密失败，
 * 按明文降级原样返回——见 `unseal`，不会损坏任务）。
 */
/** 内置密钥种子：**它不是秘密，可以入库**——本值随产物分发、任何持有产物者都能提取，其作用是把
 *  数据文件从「明文可读」变为「需一步提取才能读」，而非提供密码学保密（真正的秘密：模型 API Key、
 *  admin 口令等一律零入库，见 AGENTS.md「绝不提交密钥」）。因此它不适用该铁律的约束对象。 */
const BUILTIN_KEY_SEED = "gebai-task-env-v1:9f2c7a41d8b35e60"

/** 由种子派生 32 字节 AES-256 密钥（域分隔串避免与其他用途的哈希语义混淆）。 */
const KEY = createHash("sha256").update(`gebai/task-env/kek/${BUILTIN_KEY_SEED}`).digest()

/** 判断字符串是否为封存值。 */
export function isSealed(value: string): boolean {
  return value.startsWith(ENC_PREFIX)
}

/** 封存明文（返回值可安全落盘）。 */
export function seal(plain: string): string {
  const iv = randomBytes(12)
  const cipher = createCipheriv("aes-256-gcm", KEY, iv)
  const body = Buffer.concat([cipher.update(plain, "utf8"), cipher.final()])
  const tag = cipher.getAuthTag()
  return `${ENC_PREFIX}${iv.toString("base64")}:${Buffer.concat([body, tag]).toString("base64")}`
}

/** 解封封存值；非封存值（明文/旧数据）原样返回；密文损坏或密钥不符返回 null（调用方按明文对待）。 */
export function unseal(value: string): string | null {
  if (!isSealed(value)) return value
  const rest = value.slice(ENC_PREFIX.length)
  const sep = rest.indexOf(":")
  if (sep <= 0) return null
  try {
    const iv = Buffer.from(rest.slice(0, sep), "base64")
    const blob = Buffer.from(rest.slice(sep + 1), "base64")
    if (iv.length !== 12 || blob.length < 16) return null
    const decipher = createDecipheriv("aes-256-gcm", KEY, iv)
    decipher.setAuthTag(blob.subarray(blob.length - 16))
    const out = Buffer.concat([decipher.update(blob.subarray(0, blob.length - 16)), decipher.final()])
    return out.toString("utf8")
  } catch {
    return null
  }
}
