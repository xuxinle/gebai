import { describe, expect, test } from "bun:test"
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs"
import { tmpdir } from "node:os"
import { join, resolve } from "node:path"
import { combinePromptExtra, loadDomainProfile, readExtraFile, resolveProfilePath } from "./profile"

function tmpHome(): string {
  return mkdtempSync(join(tmpdir(), "gebai-profile-"))
}

describe("领域档案路径解析（GEBAI_PROFILE）", () => {
  test("档案名 → {GEBAI_HOME}/profiles/{名}.json；路径与 .json 结尾按文件路径", () => {
    expect(resolveProfilePath("coding", "/home/x/.gebai")).toBe(join("/home/x/.gebai", "profiles", "coding.json"))
    expect(resolveProfilePath("coding.json", "/home/x/.gebai")).toBe(join(process.cwd(), "coding.json"))
    expect(resolveProfilePath("profiles/ops.json", "/home/x/.gebai")).toBe(join(process.cwd(), "profiles", "ops.json"))
    /* 绝对路径：「直接用」的**可判定含义**是不被当成档案名拼进 gebaiHome/profiles，
     * 而不是“字符串一字不改地返回”——实现走 `resolve(v)`，而 resolve 会按平台归一
     * （Windows 上 `/tmp/p.json` 是根相对路径，归一成 `C:\tmp\p.json`）。
     * 早期这里写死 `/tmp/p.json`，在 Windows 上必红；改成对 `resolve` 取齐后，
     * 断言的仍是同一件事（不经 profiles/），且两个平台都成立。 */
    const abs = resolve("/tmp/p.json")
    const got = resolveProfilePath("/tmp/p.json", "/home/x/.gebai")
    expect(got).toBe(abs)
    expect(got).not.toContain("profiles") // 没被当档案名
  })
})

describe("领域档案载入（缺失/非法/类型不符启动期抛错）", () => {
  test("合法档案：四组清单解析（未知字段无）", () => {
    const home = tmpHome()
    try {
      mkdirSync(join(home, "profiles"), { recursive: true })
      writeFileSync(
        join(home, "profiles", "coding.json"),
        JSON.stringify({
          name: "coding",
          description: "编码领域",
          prompt: { disable: ["artifact_naming"], extra: "只写代码。", extra_file: "coding.md" },
          tools: { enable: ["read", "write"] },
          sub_agents: { enable: ["code", "explore"], preload: ["code"] },
        }),
      )
      const { profile, path } = loadDomainProfile("coding", home)
      expect(path).toBe(join(home, "profiles", "coding.json"))
      expect(profile.name).toBe("coding")
      expect(profile.description).toBe("编码领域")
      expect(profile.prompt?.disable).toEqual(["artifact_naming"])
      expect(profile.prompt?.extra).toBe("只写代码。")
      expect(profile.prompt?.extra_file).toBe("coding.md")
      expect(profile.tools?.enable).toEqual(["read", "write"])
      expect(profile.sub_agents?.enable).toEqual(["code", "explore"])
      expect(profile.sub_agents?.preload).toEqual(["code"])
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })

  test("档案缺失 / JSON 非法 / 缺 name / 未知字段 / 类型不符：抛错而非静默降级", () => {
    const home = tmpHome()
    try {
      mkdirSync(join(home, "profiles"), { recursive: true })
      expect(() => loadDomainProfile("ghost", home)).toThrow(/档案不存在/)
      const w = (body: string) => {
        writeFileSync(join(home, "profiles", "x.json"), body)
        return () => loadDomainProfile("x", home)
      }
      expect(w("{ not json")).toThrow(/JSON 解析失败/)
      expect(w(JSON.stringify({ description: "无名字" }))).toThrow(/缺少 name/)
      expect(w(JSON.stringify({ name: "x", prompt_trim: [] }))).toThrow(/未知字段 "prompt_trim"/)
      expect(w(JSON.stringify({ name: "x", prompt: { disable: "artifact_naming" } }))).toThrow(/必须是字符串数组/)
      expect(w(JSON.stringify({ name: "x", sub_agents: { preload: [1] } }))).toThrow(/必须是字符串数组/)
    } finally {
      rmSync(home, { recursive: true, force: true })
    }
  })
})

describe("领域补充提示词（文件/内联）", () => {
  test("readExtraFile：相对路径相对 baseDir，缺失抛错", () => {
    const dir = tmpHome()
    try {
      writeFileSync(join(dir, "p.md"), "领域约束")
      expect(readExtraFile("p.md", dir)).toBe("领域约束")
      expect(() => readExtraFile("missing.md", dir)).toThrow(/提示词文件不存在/)
    } finally {
      rmSync(dir, { recursive: true, force: true })
    }
  })

  test("combinePromptExtra：文件内容在前、内联在后；皆空返回 undefined", () => {
    expect(combinePromptExtra("文件内容", "内联内容")).toBe("文件内容\n内联内容")
    expect(combinePromptExtra("文件内容", undefined)).toBe("文件内容")
    expect(combinePromptExtra("  ", "")).toBeUndefined()
  })
})
