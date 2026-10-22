import { describe, expect, test } from "bun:test"
import * as imagegen from "../imagegen/imagegen"
import * as louyue from "../louyue/louyue"
import * as caiyun from "../caiyun/caiyun"

/**
 * 三个子代理（imagegen / caiyun / louyue）的结构约定。
 *
 * 为什么值得钉住：
 * - 裁云与镂月是**零自有工具**的组合式子代理：霜天、图像生成、语音音效全部经 `dependencies`
 *   连带装载。依赖名单一旦被误删，装载「成功」但一个能力都没有——失败现象是"工具不存在"
 *   而不是"依赖丢了"，极难从现象反推。
 * - 依赖能否成立取决于**命名前缀**：工具注册表禁子Agent 名互为前缀，与依赖名撞前缀就装不下来。
 *   本测试钉住这条约束（依赖名与自身名无前缀关系），防止有人把名字改回带前缀的形态。
 */
describe("caiyun / louyue", () => {
  const combos = [
    { name: "caiyun", mod: caiyun },
    { name: "louyue", mod: louyue },
  ] as const

  test("二者都导出完整的 SubAgentDef 形状", () => {
    for (const { name, mod } of combos) {
      const def = mod as unknown as Record<string, unknown>
      expect(def.name).toBe(name)
      expect(typeof def.description).toBe("string")
      expect((def.description as string).length).toBeGreaterThan(80)
      expect(typeof def.systemPrompt).toBe("string")
      expect(def.preload).toBe(false)
      expect(def.def).toBeTruthy()
    }
  })

  test("依赖面不被内联（自有工具可以有，但别家的工具不许摊进自己命名空间）", () => {
    // 「内联」= 把**别家的**工具对象摊进自己的 tools：会以本代理命名空间再注册一份，
    // 于是同一个能力有两个名字（提示词该写哪个会分叉）。
    //
    // 判据不能写成「tools 必须为空」——那会把**自有能力**一并禁掉，而有些能力
    // 只能由本代理提供（裁云的 `verify` 读的是本应用渲染器在渲染循环里量好的
    // 逐镜数据，旁挂子代理拿不到）。正确的判据是：**自有工具的键不得与依赖冲突**。
    expect((caiyun as unknown as { def: { dependencies?: string[] } }).def.dependencies).toEqual([
      "shuangtian",
      "imagegen",
      "tts",
    ])
    expect((louyue as unknown as { def: { dependencies?: string[] } }).def.dependencies).toEqual([
      "shuangtian",
      "imagegen",
    ])
    for (const { name, mod } of combos) {
      const deps = (mod as unknown as { def: { dependencies: string[] } }).def.dependencies
      const own = Object.keys((mod as unknown as { tools: Record<string, unknown> }).tools)
      for (const key of own) {
        // 自有工具名不得是 `{dep}_xxx` 形态（那是依赖方的工具被内联进来的样子），
        // 也不得恰好等于依赖名（同样会与 `{dep}_{tool}` 命名冲突）。
        for (const dep of deps) {
          expect(key.startsWith(`${dep}_`)).toBe(false)
          expect(key).not.toBe(dep)
        }
      }
      // 现存的自有工具只有裁云的 verify（成片质检）——写死一条，改动时逼人回看这里
      if (name === "caiyun") {
        expect(own).toEqual(["verify"])
      } else {
        expect(own).toEqual([])
      }
    }
  })

  test("名字与依赖不构成前缀关系（否则注册期抛 prefix collision）", () => {
    // 工具注册表按 `{agent}_` 前缀解析，名字互为前缀会产生歧义，register 直接抛错
    // （registry.ts 的 sub-agent name prefix collision）。这正是 caiyun/louyue 这两个名字的由来。
    for (const { name, mod } of combos) {
      for (const dep of (mod as unknown as { def: { dependencies: string[] } }).def.dependencies) {
        expect(dep.startsWith(`${name}_`)).toBe(false)
        expect(name.startsWith(`${dep}_`)).toBe(false)
      }
    }
  })

  test("提示词里的工具名与实际注册名一致（依赖方命名空间，带全前缀）", () => {
    // 提示词写 `shuangtian_run`，实际注册名就是 `shuangtian_run`（依赖路径不会加装载方前缀）；
    // 若写成本代理前缀（`caiyun_run`）则会撞上前缀规则而调不通。
    for (const { name, mod } of combos) {
      const prompt = (mod as unknown as { systemPrompt: string }).systemPrompt
      expect(prompt).toContain("`shuangtian_")
      expect(prompt).not.toContain(`\`${name}_run`)
      expect(prompt).not.toContain(`\`${name}_capture`)
    }
    // 图像生成是两条共有的依赖，提示词必须告知来源与名字
    for (const { mod } of combos) {
      const prompt = (mod as unknown as { systemPrompt: string }).systemPrompt
      expect(prompt).toContain("imagegen_assets")
      expect(prompt).toContain("imagegen")
    }
    expect((caiyun as unknown as { systemPrompt: string }).systemPrompt).toContain("tts_sfx")
  })

  test("imagegen 是真子代理（有 name/description/systemPrompt/def，可单独 agent_load）", () => {
    const mod = imagegen as unknown as Record<string, unknown>
    expect(mod.name).toBe("imagegen")
    expect(typeof mod.description).toBe("string")
    expect((mod.description as string).length).toBeGreaterThan(80)
    expect(typeof mod.systemPrompt).toBe("string")
    expect((mod.systemPrompt as string).length).toBeGreaterThan(500)
    expect(mod.preload).toBe(false)
    const def = mod.def as { tools?: Record<string, unknown>; requiresApproval?: Record<string, boolean> }
    expect(def).toBeTruthy()
    for (const required of ["imagegen", "imagegen_check", "imagegen_assets"]) {
      expect(def.tools).toHaveProperty(required)
      expect(def.requiresApproval?.[required]).toBe(false)
    }
  })

  test("音效默认零（模板不得再鼓励 riser→impact→sparkle 三件套）", () => {
    // 踩过的坑：模板原本写着「收尾固定 riser→impact→sparkle 三件套」，子代理于是
    // 照模板填满音效，而裁云只对**配乐**做 ducking、对音效不做——实测音效比旁白还响，
    // 听感是「叮一下把说话盖掉」。根因在规则本身，所以规则要反过来。
    const prompt = caiyun.systemPrompt
    expect(prompt).toContain("音效默认不加")
    expect(prompt).not.toContain("收尾固定")
    // 加音效的三条硬约束必须在场：gain 上限、响度比旁白低 6dB、只在旁白空隙
    expect(prompt).toContain("0.25")
    expect(prompt).toContain("6dB")
  })

  test("提示词里写清了关键事实（不是空壳）", () => {
    const video = caiyun
    // 裁云必须知道：素材要压暗、渲染完要抽帧验片
    expect(video.systemPrompt).toContain("image_dim")
    expect(video.systemPrompt).toContain("抽帧")

    // ── 创作方法论锚点：每一条都对应一类返工代价 ──
    // 逻辑不复杂，但少了这些锚点，子代理会退回"排好版就出片"的旧行为。
    for (const anchor of [
      "C1", // 三处确认点（概要/每节主帧/成片关键帧）
      "主帧", // 每节送审而不是一次渲完整片
      "确定性", // 禁随机：两次出片必须同帧同像素
      "呼吸", // 落定后留静止（节奏单向偏好：只会嫌快，不会嫌慢）
      "推镜不超过 1.08", // 安全边距（实测踩过硬裁切）
      "实测时长", // 配音定稿后重排场景窗口
    ]) {
      expect(video.systemPrompt).toContain(anchor)
    }

    // ── 「剧本先行」与「不写 C++」是工具化的两个支柱，单独钉 ──
    //
    // 剧本先行：跳过它 = 改一句话要重渲全片（返工代价最大的一步）。
    expect(video.systemPrompt).toContain("先写剧本")
    expect(video.systemPrompt).toContain("script_design.md")
    expect(video.systemPrompt).toContain("逐字写出来")

    // 不写 C++：内置版式不够时必须想到 custom + layers，而不是改源码。
    // 缺了这条，子代理会退化成"只能从五种模板里挑"。
    expect(video.systemPrompt).toContain("custom")
    expect(video.systemPrompt).toContain("layers")
    expect(video.systemPrompt).toContain("不写 C++")

    const poster = louyue
    // 镂月必须知道：图层顺序会盖住文字（实测踩过的坑）
    expect(poster.systemPrompt).toContain("盖住")
    expect(poster.systemPrompt).toContain("blocks")
    // 图元能力：内置块不够时不能退回"改 C++"
    for (const anchor of ["line", "path", "rect", "paragraph"]) {
      expect(poster.systemPrompt).toContain(anchor)
    }
    expect(poster.systemPrompt).toContain("不要想着改 C++")
  })
})
