import { describe, expect, test } from "bun:test"
import { configCtxWindow, configMenuDetail, draftConfigToWrite, resolveConfigName } from "./model-picker-core"

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

  test("清单副标题给出模型标识、多模态能力与接口主机：不必切过去就能判断能干什么", () => {
    expect(configMenuDetail(LIST[0])).toBe("deepseek-flash · 支持图片 · api.deepseek.com")
    expect(configMenuDetail(LIST[1])).toBe("glm-5.3 · 仅文本 · open.bigmodel.cn")
    // 独立视觉模型：主模型标仅文本也能看图（副标题给出视觉模型这一事实）
    expect(configMenuDetail({ ...LIST[1], multimodal: true })).toBe("glm-5.3 · 图片+视觉模型 · open.bigmodel.cn")
    // 未声明接口地址：不补空段（不留多余的「 · 」）
    expect(configMenuDetail({ ...LIST[0], apiBase: "" })).toBe("deepseek-flash · 支持图片")
    // 非法/相对地址：去协议头后原样给出
    expect(configMenuDetail({ ...LIST[0], apiBase: "http://localhost:8000/v1" })).toBe("deepseek-flash · 支持图片 · localhost:8000")
    expect(configMenuDetail({ ...LIST[0], apiBase: "not a url" })).toBe("deepseek-flash · 支持图片 · not a url")
  })

  test("草稿页选择 → 新会话要写入的配置名：选默认项不写字段（默认跟着清单顺序走）", () => {
    expect(draftConfigToWrite(LIST, "glm")).toBe("glm")
    // 未选 / 选的正是默认（清单第一条）：不写名字
    expect(draftConfigToWrite(LIST, "")).toBeUndefined()
    expect(draftConfigToWrite(LIST, "deepseek")).toBeUndefined()
    // 选的名字已不在清单（清单改过）：回落默认，同样不写
    expect(draftConfigToWrite(LIST, "removed")).toBeUndefined()
    // 清单空：什么也不写
    expect(draftConfigToWrite([], "glm")).toBeUndefined()
  })
})
