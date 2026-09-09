/**
 * 跨语言同名定义合并（core/agents/merge.ts mergeSubAgentDefs）单元测试：
 * 拼接/留空/全空兜底/工具冲突/并集/取或/函数字段取首个。
 */
import { describe, expect, test } from "bun:test"
import { mergeSubAgentDefs, fallbackDescription } from "./merge"
import type { Tool } from "../base/types"

const mkTool = (name: string, marker = name): Tool => ({
  name,
  description: `${marker} 工具`,
  parameters: { type: "object" as const, properties: {} },
  execute: async () => ({ output: marker }),
})

describe("mergeSubAgentDefs 合并语义", () => {
  test("description/systemPrompt 非空项依次拼接（分号/空行分隔）", () => {
    const merged = mergeSubAgentDefs("hsh", [
      { name: "hsh", description: "TS 描述", systemPrompt: "TS 提示词" },
      { name: "hsh", description: "native 描述", systemPrompt: "native 提示词" },
    ])
    expect(merged.description).toBe("TS 描述；native 描述")
    expect(merged.systemPrompt).toBe("TS 提示词\n\nnative 提示词")
  })

  test("留空侧跳过（只在一处定义、其他地方留空）", () => {
    const merged = mergeSubAgentDefs("hsh", [
      { name: "hsh", description: "", systemPrompt: "TS 提示词" },
      { name: "hsh", description: "native 描述", systemPrompt: "" },
    ])
    expect(merged.description).toBe("native 描述")
    expect(merged.systemPrompt).toBe("TS 提示词")
  })

  test("全空兜底：description 生成为「子代理 {name}」、systemPrompt 生成为引导句", () => {
    const merged = mergeSubAgentDefs("hsh", [
      { name: "hsh", description: "   ", systemPrompt: "" },
      { name: "hsh", description: "", systemPrompt: "" },
    ])
    expect(merged.description).toBe(fallbackDescription("hsh"))
    expect(merged.systemPrompt).toContain("hsh")
    expect(merged.systemPrompt).toContain(merged.description)
  })

  test("工具集合并：同名工具保留靠前贡献并告警、不同名并集", () => {
    const warnings: string[] = []
    const origWarn = console.warn
    console.warn = (msg: string) => warnings.push(msg)
    try {
      const merged = mergeSubAgentDefs("hsh", [
        { name: "hsh", description: "d1", systemPrompt: "p1", tools: { crc32: mkTool("crc32", "ts") } },
        { name: "hsh", description: "d2", systemPrompt: "p2", tools: { crc32: mkTool("crc32", "native"), sha256: mkTool("sha256") } },
      ])
      expect(Object.keys(merged.tools ?? {}).sort()).toEqual(["crc32", "sha256"])
      // 同名冲突：保留前者（TS 优先）
      expect(merged.tools?.crc32?.description).toBe("ts 工具")
      expect(warnings.some((w) => w.includes("crc32") && w.includes("多处定义"))).toBe(true)
      // 单侧独有工具原样保留
      expect(merged.tools?.sha256?.description).toBe("sha256 工具")
    } finally {
      console.warn = origWarn
    }
  })

  test("dependencies/envVars 并集去重、preload 取或、requiresApproval 对象合并", () => {
    const merged = mergeSubAgentDefs("x", [
      {
        name: "x",
        description: "d1",
        systemPrompt: "p1",
        dependencies: ["code", "vision"],
        envVars: [
          { name: "X_A", description: "a1" },
          { name: "X_B", description: "b1" },
        ],
        preload: true,
        requiresApproval: { t1: true },
      },
      {
        name: "x",
        description: "d2",
        systemPrompt: "p2",
        dependencies: ["code", "wps"],
        envVars: [
          { name: "X_B", description: "b2" },
          { name: "X_C", description: "c1" },
        ],
        requiresApproval: { t2: false },
      },
    ])
    expect(merged.dependencies?.sort()).toEqual(["code", "vision", "wps"])
    // envVars 同名取首个（声明不重复），不同名并集
    expect(merged.envVars?.map((v) => `${v.name}:${v.description}`).sort()).toEqual(["X_A:a1", "X_B:b1", "X_C:c1"])
    expect(merged.preload).toBe(true)
    expect(merged.requiresApproval).toEqual({ t1: true, t2: false })
  })

  test("函数字段取首个非空（projectRoot/writeGuard）", () => {
    const f1 = () => "/a"
    const f2 = () => "/b"
    const g1 = () => "deny1"
    const g2 = () => "deny2"
    const merged = mergeSubAgentDefs("x", [
      { name: "x", description: "d1", systemPrompt: "p1", projectRoot: f1, writeGuard: g1 },
      { name: "x", description: "d2", systemPrompt: "p2", projectRoot: f2, writeGuard: g2 },
    ])
    expect(merged.projectRoot).toBe(f1)
    expect(merged.writeGuard).toBe(g1)
  })

  test("输入校验：空贡献集/name 不一致抛错", () => {
    expect(() => mergeSubAgentDefs("a", [])).toThrow("贡献集为空")
    expect(() =>
      mergeSubAgentDefs("a", [
        { name: "a", description: "", systemPrompt: "" },
        { name: "b", description: "", systemPrompt: "" },
      ]),
    ).toThrow("name 不一致")
  })
})
