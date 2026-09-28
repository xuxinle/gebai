import { describe, expect, test } from "bun:test"
import {
  BOOT_KEY,
  CONFIG_KEY,
  DEFAULT_BOOT_TIMEOUT,
  MAX_BOOT_TIMEOUT,
  applyWebConfig,
  applyWebConfigStorage,
  awaitCustomBoot,
  bootPromises,
  configEnv,
  normalizeWebConfig,
  readWebConfig,
  urlPromptAllowed,
} from "./boot-config"

/** 内存版 localStorage（测试替身）。 */
function store(init: Record<string, string> = {}) {
  const m = new Map(Object.entries(init))
  return {
    getItem: (k: string) => (m.has(k) ? m.get(k)! : null),
    setItem: (k: string, v: string) => void m.set(k, v),
    dump: () => Object.fromEntries(m),
  }
}

describe("normalizeWebConfig（容错归一化）", () => {
  test("非对象/数组/undefined 回落默认（URL 提示词默认开启）", () => {
    const empty = { env: {}, envFromStorage: {}, storage: {}, allowUrlPrompt: true, bootTimeout: DEFAULT_BOOT_TIMEOUT }
    expect(normalizeWebConfig(undefined)).toEqual(empty)
    expect(normalizeWebConfig("x")).toEqual(empty)
    expect(normalizeWebConfig([1, 2])).toEqual(empty)
  })

  test("env 丢弃空值与非字符串；键值 trim", () => {
    const cfg = normalizeWebConfig({ env: { A: "1", " B ": " 2 ", C: "", D: 3, "": "x", E: null } })
    expect(cfg.env).toEqual({ A: "1", B: "2" })
  })

  test("storage 支持字符串（宿主键）与对象（from/value/force）两种写法", () => {
    const cfg = normalizeWebConfig({
      storage: {
        "gebai.ui.style": "myapp.theme",
        "gebai.ui.lowPower": { value: "on" },
        "gebai.ui.approvalSkip": { from: "myapp.skip", force: true },
        "gebai.ui.bad1": {},
        "gebai.ui.bad2": 42,
      },
    })
    expect(cfg.storage).toEqual({
      "gebai.ui.style": { from: "myapp.theme" },
      "gebai.ui.lowPower": { value: "on" },
      "gebai.ui.approvalSkip": { from: "myapp.skip", force: true },
    })
  })

  test("allowUrlPrompt 只有显式 false 才关闭", () => {
    expect(normalizeWebConfig({ allowUrlPrompt: false }).allowUrlPrompt).toBe(false)
    expect(normalizeWebConfig({ allowUrlPrompt: 0 }).allowUrlPrompt).toBe(true)
    expect(normalizeWebConfig({}).allowUrlPrompt).toBe(true)
  })

  test("bootTimeout 归一：0 保留、正数取整且封顶、其余回落默认", () => {
    expect(normalizeWebConfig({}).bootTimeout).toBe(DEFAULT_BOOT_TIMEOUT)
    expect(normalizeWebConfig({ bootTimeout: 0 }).bootTimeout).toBe(0)
    expect(normalizeWebConfig({ bootTimeout: 1200.7 }).bootTimeout).toBe(1200)
    expect(normalizeWebConfig({ bootTimeout: 999_999 }).bootTimeout).toBe(MAX_BOOT_TIMEOUT)
    for (const bad of [-5, "3000", Number.NaN, Number.POSITIVE_INFINITY, null]) {
      expect(normalizeWebConfig({ bootTimeout: bad }).bootTimeout).toBe(DEFAULT_BOOT_TIMEOUT)
    }
  })
})

describe("readWebConfig（从 window 读取）", () => {
  test("对象形态直读；缺失为空配置", () => {
    expect(readWebConfig({ [CONFIG_KEY]: { env: { A: "1" } } }).env).toEqual({ A: "1" })
    expect(readWebConfig({}).env).toEqual({})
  })

  test("函数形态求值；求值抛错回落空配置", () => {
    expect(readWebConfig({ [CONFIG_KEY]: () => ({ env: { A: "1" } }) }).env).toEqual({ A: "1" })
    const cfg = readWebConfig({
      [CONFIG_KEY]: () => {
        throw new Error("boom")
      },
    })
    expect(cfg.env).toEqual({})
    expect(cfg.allowUrlPrompt).toBe(true)
  })
})

describe("configEnv（配置文件预置 + 宿主存储取值）", () => {
  test("env 与 envFromStorage 合并，宿主键有值覆盖 env 同名项", () => {
    const s = store({ "myapp.key": "  host-key  " })
    const cfg = normalizeWebConfig({ env: { A: "1", B: "cfg" }, envFromStorage: { B: "myapp.key", C: "myapp.missing" } })
    expect(configEnv(cfg, s)).toEqual({ A: "1", B: "host-key" })
  })

  test("宿主存储不可用不影响静态 env", () => {
    const throwing = {
      getItem: () => {
        throw new Error("denied")
      },
      setItem: () => {},
    }
    const cfg = normalizeWebConfig({ env: { A: "1" }, envFromStorage: { B: "myapp.key" } })
    expect(configEnv(cfg, throwing)).toEqual({ A: "1" })
  })
})

describe("applyWebConfigStorage（写歌白设置键）", () => {
  test("只写未设置的键，已有值不被覆盖", () => {
    const s = store({ "gebai.ui.style": "ink", "myapp.theme": "cny" })
    const cfg = normalizeWebConfig({ storage: { "gebai.ui.style": "myapp.theme", "gebai.ui.lowPower": { value: "on" } } })
    expect(applyWebConfigStorage(cfg, s)).toEqual(["gebai.ui.lowPower"])
    expect(s.dump()["gebai.ui.style"]).toBe("ink")
    expect(s.dump()["gebai.ui.lowPower"]).toBe("on")
  })

  test("force 覆盖已有值；宿主键无值时跳过", () => {
    const s = store({ "gebai.ui.style": "ink", "myapp.theme": "cny" })
    const cfg = normalizeWebConfig({
      storage: { "gebai.ui.style": { from: "myapp.theme", force: true }, "gebai.ui.cnyScheme": "myapp.missing" },
    })
    expect(applyWebConfigStorage(cfg, s)).toEqual(["gebai.ui.style"])
    expect(s.dump()["gebai.ui.style"]).toBe("cny")
    expect("gebai.ui.cnyScheme" in s.dump()).toBe(false)
  })
})

describe("applyWebConfig + urlPromptAllowed（URL 提示词开关）", () => {
  test("应用配置并据 allowUrlPrompt 收放开入口", () => {
    const s = store({ "myapp.theme": "matrix" })
    const off = applyWebConfig({ host: { [CONFIG_KEY]: { allowUrlPrompt: false, storage: { "gebai.ui.style": "myapp.theme" } } }, store: s })
    expect(off).toEqual({ written: ["gebai.ui.style"], allowUrlPrompt: false })
    expect(s.dump()["gebai.ui.style"]).toBe("matrix")
    expect(urlPromptAllowed()).toBe(false)
    applyWebConfig({ host: {}, store: s })
    expect(urlPromptAllowed()).toBe(true)
  })

  test("无配置文件时开关保持开启且不写任何键", () => {
    const s = store()
    expect(applyWebConfig({ host: {}, store: s })).toEqual({ written: [], allowUrlPrompt: true })
    expect(s.dump()).toEqual({})
  })
})

/** 抑制引导路径的 console.warn（提示类日志，测试只验证行为）。 */
async function quiet<T>(fn: () => T | Promise<T>): Promise<T> {
  const orig = console.warn
  console.warn = () => {}
  try {
    return await fn()
  } finally {
    console.warn = orig
  }
}

describe("bootPromises（二开初始化脚本引导值收集）", () => {
  test("Promise / 返回 Promise 的函数 / 嵌套数组均收集", async () => {
    const p1 = Promise.resolve("a")
    const p2 = Promise.resolve("b")
    const host = { [BOOT_KEY]: [p1, () => p2, [() => Promise.resolve("c")]] }
    const got = await quiet(() => bootPromises(host))
    expect(got.length).toBe(3)
    expect(got[0]).toBe(p1)
    expect(got[1]).toBe(p2)
    await expect(got[2]!).resolves.toBe("c")
  })

  test("非 Promise 项与缺省值忽略（不抛错）", async () => {
    await quiet(async () => {
      expect(bootPromises({})).toEqual([])
      expect(bootPromises({ [BOOT_KEY]: "not-promise" })).toEqual([])
      expect(bootPromises({ [BOOT_KEY]: 42 })).toEqual([])
    })
  })

  test("引导函数抛错只忽略该值，不向外抛", async () => {
    const host = {
      [BOOT_KEY]: (): unknown => {
        throw new Error("boom")
      },
    }
    expect(await quiet(() => bootPromises(host))).toEqual([])
  })

  test("宿主取值抛错回落空数组", async () => {
    const host = {
      get [BOOT_KEY](): unknown {
        throw new Error("denied")
      },
    }
    expect(await quiet(() => bootPromises(host))).toEqual([])
  })
})

describe("awaitCustomBoot（init 最早期等待二开异步引导）", () => {
  test("等待引导完成后再继续", async () => {
    const order: string[] = []
    const host = {
      [BOOT_KEY]: (async () => {
        await Promise.resolve()
        order.push("boot")
      })(),
    }
    await quiet(() => awaitCustomBoot({ host }))
    order.push("after")
    expect(order).toEqual(["boot", "after"])
  })

  test("无引导值时立即返回", async () => {
    await awaitCustomBoot({ host: {} })
  })

  test("引导失败（reject）被吞掉，不向调用方抛出", async () => {
    const host = { [BOOT_KEY]: Promise.reject(new Error("boom")) }
    await quiet(() => awaitCustomBoot({ host }))
  })

  test("超时（bootTimeout）到点即继续，后台引导仍可落定", async () => {
    let release: () => void = () => {}
    const pending = new Promise<void>((r) => {
      release = r
    })
    const host = { [CONFIG_KEY]: { bootTimeout: 20 }, [BOOT_KEY]: pending }
    await quiet(() => awaitCustomBoot({ host }))
    release()
    await pending
  })

  test("bootTimeout: 0 不等待（永不落定的引导不阻塞）", async () => {
    const host = { [CONFIG_KEY]: { bootTimeout: 0 }, [BOOT_KEY]: new Promise<void>(() => {}) }
    await quiet(() => awaitCustomBoot({ host }))
  })

  test("同一宿主重复调用复用同一次等待（引导函数只求值一次）", async () => {
    let calls = 0
    const host = {
      [BOOT_KEY]: (): Promise<void> => {
        calls++
        return Promise.resolve()
      },
    }
    await quiet(async () => {
      await awaitCustomBoot({ host })
      await awaitCustomBoot({ host })
    })
    expect(calls).toBe(1)
  })
})
