/**
 * `HF_ENDPOINT` 下载源的回归测试。
 *
 * ## 这条用例防的是什么
 *
 * 下载地址曾写死 `https://hf-mirror.com`。它在公网可用，于是**测试全绿**；
 * 但合规/内网环境里该地址不可达，`tts_install` 只会报「连接超时」——
 * 从错误信息完全看不出「是地址问题」，于是会去查模型名、查网络权限，白绕。
 *
 * 判据必须落在**可观察的 URL 上**（而不是"环境变量读到了"）：
 * 环境变量改了就真的改地址，没设就回落到公网镜像。
 */
import { afterEach, describe, expect, test } from "bun:test"
import { hfEndpoint } from "./tts"

const original = process.env.HF_ENDPOINT

afterEach(() => {
  if (original === undefined) delete process.env.HF_ENDPOINT
  else process.env.HF_ENDPOINT = original
})

describe("HF_ENDPOINT 下载源", () => {
  test("未设置时回落到公网镜像（保持原有行为）", () => {
    delete process.env.HF_ENDPOINT
    expect(hfEndpoint()).toBe("https://hf-mirror.com")
  })

  test("设置后真的改用该地址（内网镜像可用的前提）", () => {
    process.env.HF_ENDPOINT = "https://aidf-repo.byd.auto/hf"
    expect(hfEndpoint()).toBe("https://aidf-repo.byd.auto/hf")
  })

  test("末尾斜杠归一（两种写法拼出的 URL 都要合法，不能出现 //resolve）", () => {
    process.env.HF_ENDPOINT = "https://aidf-repo.byd.auto/hf/"
    expect(hfEndpoint()).toBe("https://aidf-repo.byd.auto/hf")
    process.env.HF_ENDPOINT = "https://aidf-repo.byd.auto/hf///"
    expect(hfEndpoint()).toBe("https://aidf-repo.byd.auto/hf")
    // 拼出来的下载 URL 不能带双斜杠（部分网关会因此 404）
    expect(`${hfEndpoint()}/repo/resolve/main/x.onnx`).toBe(
      "https://aidf-repo.byd.auto/hf/repo/resolve/main/x.onnx",
    )
  })

  test("空串/纯空白视为未设置（环境变量被置空是常见形态）", () => {
    process.env.HF_ENDPOINT = ""
    expect(hfEndpoint()).toBe("https://hf-mirror.com")
    process.env.HF_ENDPOINT = "   "
    expect(hfEndpoint()).toBe("https://hf-mirror.com")
  })
})
