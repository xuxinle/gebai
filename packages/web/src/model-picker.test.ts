import { describe, expect, test } from "bun:test"
import { configCtxWindow, configTipLines, resolveConfigName } from "./model-picker-core"

const LIST = [
  { name: "deepseek", model: "deepseek-flash", apiKind: "openai", apiBase: "https://api.deepseek.com", maxContextTokens: 512000, multimodal: true },
  { name: "glm", model: "glm-5.3", apiKind: "openai", apiBase: "https://open.bigmodel.cn/api/paas/v4", maxContextTokens: 256000, multimodal: false, visionModel: "glm-5.3-flash" },
]

describe("模型配置选择器（纯逻辑）", () => {
  test("会话未选择 → 用清单第一条（默认配置）", () => {
    expect(resolveConfigName(LIST, undefined)).toBe("deepseek")
    expect(resolveConfigName(LIST, "")).toBe("deepseek")
  })

  test("会话已选择且清单里仍在 → 用它（切换会话各自记住）", () => {
    expect(resolveConfigName(LIST, "glm")).toBe("glm")
  })

  test("会话选的名字已不在清单（清单改过/别处导入）→ 回落默认，界面与实际一致", () => {
    expect(resolveConfigName(LIST, "removed")).toBe("deepseek")
    expect(resolveConfigName([], "glm")).toBe("")
  })

  test("上下文窗口按当前配置算（未声明 → 0，不覆盖服务端默认）", () => {
    expect(configCtxWindow(LIST, "deepseek")).toBe(512000)
    expect(configCtxWindow(LIST, "glm")).toBe(256000)
    expect(configCtxWindow([{ name: "x", model: "m", apiKind: "openai", apiBase: "", multimodal: false }], "x")).toBe(0)
    expect(configCtxWindow(LIST, "missing")).toBe(0)
  })

  test("悬浮提示含模型/接口/多模态/视觉模型，一眼确认选的是哪一份", () => {
    expect(configTipLines(LIST[1])).toEqual([
      "模型配置：glm",
      "模型 glm-5.3（openai）",
      "接口 https://open.bigmodel.cn/api/paas/v4",
      "多模态：仅文本",
      "视觉模型 glm-5.3-flash",
    ])
    expect(configTipLines({ ...LIST[0], apiBase: "" })).toEqual(["模型配置：deepseek", "模型 deepseek-flash（openai）", "多模态：支持图片输入"])
  })
})
