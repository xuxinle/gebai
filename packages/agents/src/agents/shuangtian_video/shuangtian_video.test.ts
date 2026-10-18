import { describe, expect, test } from "bun:test"
import * as imagegen from "../imagegen/imagegen"
import * as shuangtianPoster from "../shuangtian_poster/shuangtian_poster"
import * as shuangtianVideo from "../shuangtian_video/shuangtian_video"

/**
 * 两个歌白子代理（shuangtian_video / shuangtian_poster）的结构约定。
 *
 * 为什么值得钉住：
 * - 它们**依赖霜天子代理的工具面**（构建/启动/截图/点击）。这条依赖是 import 进来的
 *   对象展开，一旦霜天改了导出形状（比如 tools 从 Record 变成函数），这里会静默变空——
 *   而变空的后果是子代理"看着装载成功、实际没有工具"，极难从现象反推。
 * - 它们还依赖 imagegen 的 3 个工具。同上。
 */
describe("shuangtian_video / shuangtian_poster", () => {
  test("二者都导出完整的 SubAgentDef 形状", async () => {
    for (const mod of ["shuangtian_video", "shuangtian_poster"] as const) {
      const def = (mod === "shuangtian_video" ? shuangtianVideo : shuangtianPoster) as unknown as Record<string, unknown>
      expect(typeof def.name).toBe("string")
      expect(def.name).toBe(mod)
      expect(typeof def.description).toBe("string")
      expect((def.description as string).length).toBeGreaterThan(80)
      expect(typeof def.systemPrompt).toBe("string")
      expect(def.preload).toBe(false)
      const sub = def.def as { tools?: Record<string, unknown> }
      expect(sub).toBeTruthy()
      expect(Object.keys(sub.tools ?? {}).length).toBeGreaterThan(10)
    }
  })

  test("都继承了霜天子代理的工具面（构建/启动/截图/点击）", async () => {
    for (const mod of ["shuangtian_video", "shuangtian_poster"] as const) {
      const mod0 = (mod === "shuangtian_video" ? shuangtianVideo : shuangtianPoster) as unknown as {
        tools: Record<string, unknown>
      }
      const { tools } = mod0
      // 这几个是"能驱动应用"的最小集：缺任何一个都做不成事
      for (const required of ["run", "tree", "invoke", "capture", "click", "wait"]) {
        expect(tools).toHaveProperty(required)
      }
    }
  })

  test("都带上了图像生成工具（素材来源）", async () => {
    for (const mod of ["shuangtian_video", "shuangtian_poster"] as const) {
      const mod1 = (mod === "shuangtian_video" ? shuangtianVideo : shuangtianPoster) as unknown as {
        tools: Record<string, unknown>
      }
      const { tools } = mod1
      for (const required of ["imagegen", "imagegen_check", "imagegen_assets"]) {
        expect(tools).toHaveProperty(required)
      }
    }
  })

  test("两个来源的工具合并后数量正确（无同名覆盖）", async () => {
    // 直接 import 霜天模块会牵动它的运行时依赖（client/工具实现），测试环境加载不了；
    // 改从**子代理已合并的 tools** 反推：合并后的键数 = 两个来源之和（说明无覆盖）。
    const imagegenCount = Object.keys(imagegen.tools).length
    for (const mod of ["shuangtian_video", "shuangtian_poster"] as const) {
      const mod2 = (mod === "shuangtian_video" ? shuangtianVideo : shuangtianPoster) as unknown as {
        tools: Record<string, unknown>
      }
      const { tools } = mod2
      const total = Object.keys(tools).length
      // 未合并前只有 imagegen 的 3 个；合并后必须多出霜天那一批（十多个）
      expect(total).toBeGreaterThan(imagegenCount + 8)
      // 图像工具的键必须还在（没被同名项覆盖掉）
      for (const key of Object.keys(imagegen.tools)) {
        expect(tools).toHaveProperty(key)
      }
    }
  })

  test("提示词里写清了关键事实（不是空壳）", async () => {
    const video = shuangtianVideo
    // 视频子代理必须知道：素材要压暗、渲染完要抽帧验片
    expect(video.systemPrompt).toContain("image_dim")
    expect(video.systemPrompt).toContain("抽帧")
    const poster = shuangtianPoster
    // 海报子代理必须知道：图层顺序会盖住文字（实测踩过的坑）
    expect(poster.systemPrompt).toContain("盖住")
    expect(poster.systemPrompt).toContain("blocks")
  })
})
