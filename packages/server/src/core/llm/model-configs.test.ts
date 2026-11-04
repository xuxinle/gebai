import { describe, expect, test } from "bun:test"
import {
  modelConfigByName,
  modelEnvOverrides,
  parseModelConfigs,
  resolveVisionProvider,
  startupModelConfig,
} from "./llm"

const LIST = JSON.stringify([
  {
    name: "deepseek",
    model: "deepseek-flash",
    url: "https://api.deepseek.com",
    api_key: "sk-a",
    max_context: 512000,
    multimodal: true,
  },
  {
    name: "glm",
    model: "glm-5.3",
    url: "https://open.bigmodel.cn/api/paas/v4",
    api_key: "sk-b",
    api_kind: "openai",
    max_context: 256000,
    multimodal: "true",
    vision_model: "glm-5.3-flash",
    vision_api_base: "https://open.bigmodel.cn/api/paas/v4",
    vision_api_key: "sk-v",
  },
])

describe("模型配置清单（GEBAI_LLM_MODELS）", () => {
  test("解析：name 缺省用模型名、重名只留首项、缺 model 的项丢弃", () => {
    const raw = JSON.stringify([
      { model: "m1", url: "https://a" },
      { name: "dup", model: "d1" },
      { name: "dup", model: "d2" },
      { model: "" },
      "garbage",
      null,
    ])
    const list = parseModelConfigs({ GEBAI_LLM_MODELS: raw })
    expect(list.map((c) => c.name)).toEqual(["m1", "dup"])
    expect(list[1].model).toBe("d1") // 重名只留首项
    expect(list[0].apiKind).toBe("openai") // api_kind 缺省
    expect(list[0].multimodal).toBe(false) // 未声明即纯文本
  })

  test("解析：非法 JSON / 非数组 / 未配置一律空表（调用方据此回退旧散项）", () => {
    expect(parseModelConfigs({})).toEqual([])
    expect(parseModelConfigs({ GEBAI_LLM_MODELS: "not-json" })).toEqual([])
    expect(parseModelConfigs({ GEBAI_LLM_MODELS: '{"a":1}' })).toEqual([])
    expect(parseModelConfigs({ GEBAI_LLM_MODELS: "  " })).toEqual([])
  })

  test("解析：视觉模型键聚成 vision 配置，缺省项继承主模型", () => {
    const glm = modelConfigByName({ GEBAI_LLM_MODELS: LIST }, "glm")!
    expect(glm.vision).toEqual({ model: "glm-5.3-flash", apiBase: "https://open.bigmodel.cn/api/paas/v4", apiKey: "sk-v" })
    const deepseek = modelConfigByName({ GEBAI_LLM_MODELS: LIST }, "deepseek")!
    expect(deepseek.vision).toBeUndefined()
  })

  test("启动配置：清单存在即以第一条为基准（旧散项整体让位）", () => {
    const { main, vision, configs } = startupModelConfig({
      GEBAI_LLM_MODELS: LIST,
      // 旧写法同时在环境里：清单已配置时不得参与（否则会拼出谁也没写过的第三种配置）
      GEBAI_LLM_MODEL: "legacy-model",
      GEBAI_LLM_API_BASE: "https://legacy.test",
      GEBAI_LLM_API_KEY: "sk-legacy",
      GEBAI_LLM_MAX_CONTEXT: "1000",
    })
    expect(configs).toHaveLength(2)
    expect(main).toEqual({
      apiKind: "openai",
      apiBase: "https://api.deepseek.com",
      apiKey: "sk-a",
      model: "deepseek-flash",
      maxContextTokens: 512000,
      multimodal: true,
    })
    expect(vision).toBeNull() // 默认条目没有视觉模型 → 回落主模型（它声明了多模态）
  })

  test("启动配置：未配置清单时回退旧散项（含非法 EXTRA_PARAMS 启动即报错）", () => {
    const { main, vision, configs } = startupModelConfig({
      GEBAI_LLM_API_BASE: "https://legacy.test",
      GEBAI_LLM_API_KEY: "sk-legacy",
      GEBAI_LLM_MODEL: "legacy-model",
      GEBAI_LLM_MAX_CONTEXT: "64000",
      GEBAI_LLM_MULTIMODAL: "true",
      GEBAI_VISION_MODEL: "v-model",
    })
    expect(configs).toEqual([])
    expect(main.model).toBe("legacy-model")
    expect(main.maxContextTokens).toBe(64000)
    expect(main.multimodal).toBe(true)
    expect(vision?.model).toBe("v-model")
    expect(() => startupModelConfig({ GEBAI_LLM_EXTRA_PARAMS: "{bad" })).toThrow(/GEBAI_LLM_EXTRA_PARAMS/)
  })

  test("会话指名条目（GEBAI_LLM_CONFIG）：整份切换，同环境散项再覆盖其字段", () => {
    const o = modelEnvOverrides({ GEBAI_LLM_MODELS: LIST, GEBAI_LLM_CONFIG: "glm" })
    expect(o).toEqual({
      model: "glm-5.3",
      apiKind: "openai",
      apiBase: "https://open.bigmodel.cn/api/paas/v4",
      apiKey: "sk-b",
      maxContextTokens: 256000,
      multimodal: true,
    })
    // 子会话显式注入的旧键仍生效（优先级高于条目）——能把子会话单独指向另一个模型
    expect(modelEnvOverrides({ GEBAI_LLM_MODELS: LIST, GEBAI_LLM_CONFIG: "glm", GEBAI_LLM_MODEL: "x" })?.model).toBe("x")
  })

  test("会话指名条目：名字写错/清单未配置时返回 null（回落默认配置，不变成无模型）", () => {
    expect(modelEnvOverrides({ GEBAI_LLM_MODELS: LIST, GEBAI_LLM_CONFIG: "nope" })).toBeNull()
    expect(modelEnvOverrides({ GEBAI_LLM_CONFIG: "glm" })).toBeNull()
  })

  test("视觉：指名条目自带视觉模型时按该条目解析（不落到别的配置的视觉模型上）", () => {
    // 真实路径：启动基座就是清单第一条派生的配置（compose 的 mainConfig）
    const base = startupModelConfig({ GEBAI_LLM_MODELS: LIST }).main
    // 默认条目（deepseek）无视觉模型，但主模型声明多模态 → 回落主模型
    const fallback = resolveVisionProvider(base, null, { GEBAI_LLM_MODELS: LIST })
    expect(fallback?.capabilities().multimodal).toBe(true)
    // 指名 glm：视觉模型取 glm 条目自己的（不是启动基座那份）
    const p = resolveVisionProvider(base, null, { GEBAI_LLM_MODELS: LIST, GEBAI_LLM_CONFIG: "glm" })
    expect(p?.capabilities().multimodal).toBe(true)
    expect(p?.capabilities().model).toBe("glm-5.3-flash")
    // 任务级 GEBAI_VISION_MODEL 仍最高优先（子会话/任务显式指定）
    const override = resolveVisionProvider(base, null, { GEBAI_LLM_MODELS: LIST, GEBAI_LLM_CONFIG: "glm", GEBAI_VISION_MODEL: "task-vision" })
    expect(override?.capabilities().model).toBe("task-vision")
  })
})
