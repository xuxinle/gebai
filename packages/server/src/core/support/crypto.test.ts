import { describe, expect, test } from "bun:test"
import { ENC_PREFIX, isSealed, seal, unseal } from "./crypto"

/** 静态加密（任务级环境变量敏感值落盘保护）单测：往返、随机 IV、篡改/异钥降级。 */
describe("静态加密（任务级 env 敏感值）", () => {
  test("封存/解封往返（含中文、空串、长值）", () => {
    for (const plain of ["secret-value", "", "中文密钥-含符号 !@#$%^&*()", "x".repeat(4096)]) {
      const sealed = seal(plain)
      expect(isSealed(sealed)).toBe(true)
      expect(sealed.startsWith(ENC_PREFIX)).toBe(true)
      expect(sealed).not.toContain(plain || "\u0000")
      expect(unseal(sealed)).toBe(plain)
    }
  })

  test("同一明文两次封存结果不同（随机 IV），但都能解回同一明文", () => {
    const a = seal("same")
    const b = seal("same")
    expect(a).not.toBe(b)
    expect(unseal(a)).toBe("same")
    expect(unseal(b)).toBe("same")
  })

  test("非封存值原样返回（明文旧数据兼容）", () => {
    expect(unseal("plain-text")).toBe("plain-text")
    expect(unseal("")).toBe("")
    expect(isSealed("plain-text")).toBe(false)
  })

  test("篡改/结构损坏返回 null（调用方按明文降级，不抛错中断调度）", () => {
    const sealed = seal("tamper-me")
    // 翻转密文末字节（GCM 认证标签校验失败）
    const tampered = sealed.slice(0, -4) + (sealed.endsWith("A==") ? "B==" : "A==")
    expect(unseal(tampered)).toBeNull()
    // 结构损坏（缺分隔/长度不足）
    for (const bad of [ENC_PREFIX, `${ENC_PREFIX}only-iv`, `${ENC_PREFIX}a:b`, `${ENC_PREFIX}:`, `${ENC_PREFIX}AAAA:AAAA`, `${ENC_PREFIX}AAAA:AAAABBBBCCCC`]) {
      expect(unseal(bad)).toBeNull()
    }
  })

  test("跨密钥不可解（换构建密钥后旧密文降级为不可读，而非误还原）", async () => {
    const sealed = seal("rotate-me")
    // 独立进程内以不同种子派生密钥解封：GCM 认证失败 → null
    const proc = Bun.spawn(
      [
        "bun",
        "-e",
        `import { createDecipheriv, createHash } from "node:crypto";
const key = createHash("sha256").update("gebai/task-env/kek/another-seed").digest();
const rest = ${JSON.stringify(sealed.slice(ENC_PREFIX.length))};
const sep = rest.indexOf(":");
const iv = Buffer.from(rest.slice(0, sep), "base64");
const blob = Buffer.from(rest.slice(sep + 1), "base64");
try {
  const d = createDecipheriv("aes-256-gcm", key, iv);
  d.setAuthTag(blob.subarray(blob.length - 16));
  console.log(Buffer.concat([d.update(blob.subarray(0, blob.length - 16)), d.final()]).toString("utf8"));
} catch { console.log("FAILED"); }`,
      ],
      { stdout: "pipe", stderr: "ignore" },
    )
    const out = await new Response(proc.stdout).text()
    expect(out.trim()).toBe("FAILED")
  })
})
