import { describe, expect, test } from "bun:test"
import * as jianmu from "./jianmu"

/**
 * 建木子代理的结构约定。
 *
 * 与裁云/镂月那组测试同一个理由：依赖名单被误删时，装载会"成功"但一个能力都没有
 * （现象是"工具不存在"，很难从现象反推到"依赖丢了"）；而自有工具与依赖命名前缀
 * 撞车时，注册表会装不下来。
 */
describe("jianmu", () => {
  test("导出完整的 SubAgentDef 形状", () => {
    const def = jianmu as unknown as Record<string, unknown>
    expect(def.name).toBe("jianmu")
    expect(typeof def.description).toBe("string")
    // 描述会被宿主写进"可选子Agent"清单——太短就等于没写，模型不会选它
    expect((def.description as string).length).toBeGreaterThan(80)
    expect(typeof def.systemPrompt).toBe("string")
    expect(def.preload).toBe(false)
    expect(def.def).toBeTruthy()
  })

  test("系统提示词写清了三条最贵的事", () => {
    // 这三条是这个子代理存在的理由（缺任一条，它就跟"通用霜天代理 + 三维文件"没区别）：
    //   ① 两种表达层各自擅长什么（选错表达层会白写一整轮）
    //   ② 三维失败几乎不报错 → 必须看截图 + 读状态面
    //   ③ bpy 与 three.js 的混合工作流（bpy 产资产 → js 组装）
    const prompt = jianmu.systemPrompt
    expect(prompt).toContain("bpy")
    expect(prompt).toContain("three.js")
    expect(prompt).toMatch(/几乎从不报错|几乎都不报错|不报错/)
    expect(prompt).toContain("_asset.py")
  })

  test("依赖只有霜天；自有工具不撞依赖前缀", () => {
    const deps = (jianmu as unknown as { def: { dependencies?: string[] } }).def.dependencies
    expect(deps).toEqual(["shuangtian"])
    const own = Object.keys(jianmu.tools)
    for (const key of own) {
      for (const dep of deps ?? []) {
        expect(key.startsWith(`${dep}_`)).toBe(false)
        expect(key).not.toBe(dep)
      }
    }
    // 自有工具清单写死：逼人回看"为什么这三个只能由本代理提供"。
    //   · `scenes`    —— 建木场景目录的约定（哪些是资产端/组装端/能力证据）
    //   · `render_check` —— 知道出图二进制分两种（three_check / align_bench_probe）
    //                    且按扩展名自动选，还知道它们各自的参数与退出码语义
    //   · `capability` —— 两条表达层的能力边界表（从三份文档摘出来的判据）
    expect(own.sort()).toEqual(["capability", "render_check", "scenes"])
  })
})
