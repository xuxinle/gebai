/**
 * 镜像构建裁剪计划器测试：档案解析与结构校验、CLI 覆盖合并、环境文件与报告输出。
 * 计划器是镜像裁剪的唯一真相源，字段拼错/静默忽略都会让镜像能力面与预期不符，故逐字段覆盖。
 */
import { describe, expect, test } from "bun:test"
import { applyOverride, emitBuildArgs, finalizeProvisioning, parseProfile, planToEnv, renderReport, validateProvisioning, type BuildPlan } from "./build-image-plan"

const ALL_ASSETS_ON = { web_ui: true, cv: true, d2: true, analyzer: true, browser: true, ripgrep: true } as const

/** 取 env 文件里某键的值（去掉 shell 引号：清单用双引号、路径用单引号）。 */
function envValue(env: string, key: string): string | undefined {
  const line = env.split("\n").find((l) => l.startsWith(`${key}=`))
  if (!line) return undefined
  return line.slice(key.length + 1).replace(/^["']|["']$/g, "")
}

describe("裁剪档案解析", () => {
  test("缺省全开：资产、vendor 全量、系统组默认开（chromium 默认关）", () => {
    const plan = parseProfile({ name: "full" }, "测试")
    expect(plan.name).toBe("full")
    expect(plan.assets).toEqual(ALL_ASSETS_ON)
    expect(plan.vendor).toEqual(["monaco", "plantuml", "mermaid", "echarts", "d2js", "xterm", "tree_sitter"])
    expect(plan.system).toEqual({ git: true, python: true, bubblewrap: true, fonts: true, tzdata: true, procps: true, chromium: false })
    expect(plan.subAgents).toEqual({})
    expect(plan.tools).toEqual({})
  })

  test("档案逐段覆盖：子Agent / 工具 / 资产 / vendor / 系统", () => {
    const plan = parseProfile(
      {
        name: "code",
        description: "编码场景",
        sub_agents: { enable: ["code", "explore"], preload: ["code"] },
        tools: { disable: ["show"] },
        assets: { cv: false, d2: false },
        web: { vendor: ["monaco", "xterm"] },
        system: { python: false, chromium: true },
        prompt: { disable: ["batching"] },
      },
      "测试",
    )
    expect(plan.description).toBe("编码场景")
    expect(plan.subAgents).toEqual({ enable: ["code", "explore"], disable: undefined, preload: ["code"] })
    expect(plan.tools).toEqual({ disable: ["show"] })
    expect(plan.assets.cv).toBe(false)
    expect(plan.assets.d2).toBe(false)
    expect(plan.assets.web_ui).toBe(true)
    expect(plan.vendor).toEqual(["monaco", "xterm"])
    expect(plan.system.python).toBe(false)
    expect(plan.system.chromium).toBe(true)
  })

  test("未知字段 / 类型不符 / 未知组 / 白名单黑名单并存：一律报错（不静默降级）", () => {
    expect(() => parseProfile({ name: "x", unknown_section: {} }, "测试")).toThrow(/未知字段/)
    expect(() => parseProfile({ description: "无名字" }, "测试")).toThrow(/缺少 name/)
    expect(() => parseProfile({ name: "x", assets: { d2: "no" } }, "测试")).toThrow(/assets\.d2 必须是布尔值/)
    expect(() => parseProfile({ name: "x", assets: { chalk: false } }, "测试")).toThrow(/未知字段/)
    expect(() => parseProfile({ name: "x", web: { vendor: ["monaco", "chalk"] } }, "测试")).toThrow(/未知组/)
    expect(() => parseProfile({ name: "x", system: { gitt: true } }, "测试")).toThrow(/未知字段/)
    expect(() => parseProfile({ name: "x", sub_agents: { enable: ["code"], disable: ["wps"] } }, "测试")).toThrow(/互斥/)
    expect(() => parseProfile({ name: "x", sub_agents: 1 }, "测试")).toThrow(/必须是对象/)
  })

  test("tools.enable 属运行期语义：构建期忽略但不报错（同一份档案可两用）", () => {
    const plan = parseProfile({ name: "x", tools: { enable: ["read"], disable: ["show"] } }, "测试")
    expect(plan.tools.disable).toEqual(["show"])
  })
})

describe("镜像本体（image 段）", () => {
  test("缺省即交付口径：Ubuntu 24.04、非 root 的 uid/gid 1000 gebai、数据根 /data、端口 3000", () => {
    const img = parseProfile({ name: "x" }, "测试").image
    expect(img.base).toBe("ubuntu:24.04")
    expect(img.bunImage).toBe("oven/bun:1.4.2")
    expect([img.user, img.uid, img.gid, img.home]).toEqual(["gebai", 1000, 1000, "/home/gebai"])
    expect(img.runAsRoot).toBe(false)
    expect(img.dataDir).toBe("/data")
    expect([img.mode, img.host, img.port]).toEqual(["server", "0.0.0.0", 3000])
    expect(img.tz).toBeUndefined()
    expect(img.extraPackages).toEqual([])
    expect(img.labels).toEqual({})
    expect(img.healthcheck).toEqual({ interval: "30s", timeout: "5s", startPeriod: "20s", retries: 3, path: "/api/health" })
  })

  test("逐字段覆盖（基础镜像/用户/目录/端口/时区/包/标签/健康检查）", () => {
    const img = parseProfile(
      {
        name: "x",
        image: {
          base: "registry.internal/ubuntu:24.04",
          bun_image: "registry.internal/bun:1.4.2",
          apt_mirror: "http://mirror.internal/ubuntu",
          npm_registry: "http://npm.internal/",
          proxy: "http://proxy.internal:3128",
          user: "acme",
          uid: 2001,
          gid: 2002,
          home: "/srv/home/acme",
          shell: "/bin/sh",
          run_as_root: true,
          data_dir: "/srv/gebai",
          mode: "local",
          host: "127.0.0.1",
          port: 8080,
          tz: "Asia/Shanghai",
          extra_packages: ["vim", "less"],
          labels: { owner: "Acme Inc", "com.example.env": "prod" },
          healthcheck: { path: "/healthz", interval: "10s" },
        },
      },
      "测试",
    ).image
    expect(img.base).toBe("registry.internal/ubuntu:24.04")
    expect(img.bunImage).toBe("registry.internal/bun:1.4.2")
    expect(img.aptMirror).toBe("http://mirror.internal/ubuntu")
    expect(img.npmRegistry).toBe("http://npm.internal/")
    expect(img.proxy).toBe("http://proxy.internal:3128")
    expect([img.user, img.uid, img.gid, img.home, img.shell]).toEqual(["acme", 2001, 2002, "/srv/home/acme", "/bin/sh"])
    expect(img.runAsRoot).toBe(true)
    expect(img.dataDir).toBe("/srv/gebai")
    expect([img.mode, img.host, img.port, img.tz]).toEqual(["local", "127.0.0.1", 8080, "Asia/Shanghai"])
    expect(img.extraPackages).toEqual(["vim", "less"])
    expect(img.labels).toEqual({ owner: "Acme Inc", "com.example.env": "prod" })
    expect(img.healthcheck).toEqual({ interval: "10s", timeout: "5s", startPeriod: "20s", retries: 3, path: "/healthz" })
  })

  test("浏览器供给：缺省为下载通道，字段逐项可覆盖", () => {
    const def = parseProfile({ name: "x" }, "测试").image
    expect([def.browserSource, def.browserDir, def.browserDeps]).toEqual(["download", "docker/browsers", "auto"])
    expect(def.browserDownloadHost).toBeUndefined()

    const local = parseProfile(
      { name: "x", image: { browser_source: "local", browser_dir: "docker/pw", browser_deps: "docker/pw/deps.txt" } },
      "测试",
    ).image
    expect([local.browserSource, local.browserDir, local.browserDeps]).toEqual(["local", "docker/pw", "docker/pw/deps.txt"])

    const dl = parseProfile({ name: "x", image: { browser_download_host: "https://mirror.internal/pw", browser_deps: false } }, "测试").image
    expect(dl.browserDownloadHost).toBe("https://mirror.internal/pw")
    expect(dl.browserDeps).toBe("off")
  })

  test("浏览器供给：非法来源、上下文外的预置目录一律报错", () => {
    const bad = (image: unknown, re: RegExp) => expect(() => parseProfile({ name: "x", image }, "测试")).toThrow(re)
    bad({ browser_source: "mirror" }, /browser_source 须为/)
    bad({ browser_source: "local", browser_dir: "/srv/pw" }, /须为构建上下文内的相对路径/)
    bad({ browser_deps: 42 }, /browser_deps 必须是非空字符串/)
  })

  test("浏览器供给：预置目录缺失/开关不一致在计划阶段就报错（不滞到 docker build）", () => {
    const withBrowser = (extra: Record<string, unknown>) => {
      const p = parseProfile({ name: "x", system: { chromium: true }, image: extra }, "测试")
      finalizeProvisioning(p)
      return p
    }
    // 目录不存在 → 提示导出来源
    const missing = validateProvisioning(withBrowser({ browser_source: "local" }), () => false)
    expect(missing).toMatch(/预置目录不存在/)
    expect(missing).toMatch(/--export-browsers/)
    // 目录存在 → 无问题；下载通道不检查目录
    expect(validateProvisioning(withBrowser({ browser_source: "local" }), () => true)).toBeNull()
    expect(validateProvisioning(withBrowser({ browser_source: "download" }), () => false)).toBeNull()
    // 未启用浏览器却声明 local 来源 → 不一致报错；未启用 + 下载缺省 → 无问题
    const off = parseProfile({ name: "x", image: { browser_source: "local" } }, "测试")
    finalizeProvisioning(off)
    expect(validateProvisioning(off, () => true)).toMatch(/两者需一致/)
    const idle = parseProfile({ name: "x" }, "测试")
    finalizeProvisioning(idle)
    expect(validateProvisioning(idle, () => false)).toBeNull()
  })

  test("运行时供给：bun/node 缺省与离线通道，node 跟随浏览器需求", () => {
    const idle = parseProfile({ name: "x" }, "测试")
    finalizeProvisioning(idle)
    expect([idle.image.bunSource, idle.image.bunDir]).toEqual(["image", "docker/bun"])
    expect(idle.image.nodeSource).toBe("off") // 不用浏览器就不装 node
    expect([idle.image.nodeImage, idle.image.nodeDir]).toEqual(["node:22-slim", "docker/node"])

    // 启用浏览器 → auto 自动要 node（缺省从 node 镜像取）
    const withBrowser = parseProfile({ name: "x", system: { chromium: true } }, "测试")
    finalizeProvisioning(withBrowser)
    expect(withBrowser.image.nodeSource).toBe("image")

    // 离线：bun/node 都预置
    const offline = parseProfile(
      { name: "x", system: { chromium: true }, image: { bun_source: "local", node_source: "local" } },
      "测试",
    )
    finalizeProvisioning(offline)
    expect([offline.image.bunSource, offline.image.nodeSource]).toEqual(["local", "local"])

    // apt 通道：nodejs 自动并入装包清单
    const apt = parseProfile({ name: "x", system: { chromium: true }, image: { node_source: "apt" } }, "测试")
    finalizeProvisioning(apt)
    expect(apt.image.extraPackages).toContain("nodejs")
  })

  test("运行时供给：启用了浏览器却把 node 关掉 → 报矛盾（装浏览器也没用）", () => {
    const p = parseProfile({ name: "x", system: { chromium: true }, image: { node_source: "off" } }, "测试")
    finalizeProvisioning(p)
    expect(validateProvisioning(p, () => true)).toMatch(/node_source=off/)
    expect(validateProvisioning(p, () => true)).toMatch(/Bun\.spawn/)
  })

  test("运行时供给：预置目录缺文件时报错并给出导出途径；非法来源报错", () => {
    const bunLocal = parseProfile({ name: "x", image: { bun_source: "local" } }, "测试")
    finalizeProvisioning(bunLocal)
    expect(validateProvisioning(bunLocal, () => false)).toMatch(/docker\/bun\/bun/)
    expect(validateProvisioning(bunLocal, () => true)).toBeNull()

    const nodeLocal = parseProfile({ name: "x", system: { chromium: true }, image: { node_source: "local" } }, "测试")
    finalizeProvisioning(nodeLocal)
    expect(validateProvisioning(nodeLocal, () => false)).toMatch(/docker\/node\/node/)

    const bad = (image: unknown, re: RegExp) => expect(() => parseProfile({ name: "x", image }, "测试")).toThrow(re)
    bad({ bun_source: "apt" }, /bun_source 须为/)
    bad({ node_source: "bundle" }, /node_source 须为/)
    bad({ bun_dir: "/opt/bun" }, /须为构建上下文内的相对路径/)
  })

  test("非法值一律报错（uid/port 越界、data_dir 非绝对路径或根、用户名、标签名、包名、时长、未知字段）", () => {
    const bad = (image: unknown, re: RegExp) => expect(() => parseProfile({ name: "x", image }, "测试")).toThrow(re)
    bad({ uid: 0 }, /uid 须在/)
    bad({ port: 70000 }, /port 须在/)
    bad({ port: "3000" }, /port 必须是整数/)
    bad({ data_dir: "data" }, /必须是绝对路径/)
    bad({ data_dir: "/" }, /不能是根目录/)
    bad({ user: "Acme" }, /user 须匹配/)
    bad({ labels: { "bad key": "v" } }, /非法标签名/)
    bad({ extra_packages: ["vim; rm -rf /"] }, /非法包名/)
    bad({ healthcheck: { interval: "10" } }, /须为时长/)
    bad({ healthcheck: { retries: 0 } }, /retries 须在/)
    bad({ healthcheck: { nope: 1 } }, /未知字段/)
    bad({ unknown_field: 1 }, /未知字段/)
  })

  test("healthcheck: false = 不写 HEALTHCHECK（改由 --target runtime-nohealthcheck 选阶段）", () => {
    expect(parseProfile({ name: "x", image: { healthcheck: false } }, "测试").image.healthcheck).toBe(false)
  })

  test("时区联动：设了 image.tz 而 system.tzdata=false 时自动启用 tzdata（否则 TZ 静默失效）", () => {
    expect(parseProfile({ name: "x", system: { tzdata: false }, image: { tz: "Asia/Shanghai" } }, "测试").system.tzdata).toBe(true)
    // 未设 tz 时不干预用户的裁剪决定
    expect(parseProfile({ name: "x", system: { tzdata: false } }, "测试").system.tzdata).toBe(false)
  })
})

describe("CLI 覆盖（优先于档案）", () => {
  const plan = (): BuildPlan => parseProfile({ name: "x", assets: { d2: false }, web: { vendor: ["monaco"] } }, "测试")

  test("资产 / vendor / 系统组字段级覆盖", () => {
    const p = plan()
    applyOverride(p, "assets.d2=1")
    applyOverride(p, "web.vendor=monaco,mermaid")
    applyOverride(p, "system.python=off")
    expect(p.assets.d2).toBe(true)
    expect(p.vendor).toEqual(["monaco", "mermaid"])
    expect(p.system.python).toBe(false)
  })

  test("子Agent 名单覆盖：设置 enable 会清掉 disable（互斥在覆盖后仍成立）", () => {
    const p = parseProfile({ name: "x", sub_agents: { disable: ["wps"] } }, "测试")
    applyOverride(p, "sub_agents.enable=code")
    expect(p.subAgents.enable).toEqual(["code"])
    expect(p.subAgents.disable).toBeUndefined()
  })

  test("image.* 覆盖：字段级生效、非法值仍报错、标签逗号拆分", () => {
    const p = plan()
    applyOverride(p, "image.base=registry.internal/ubuntu:24.04")
    applyOverride(p, "image.user=acme")
    applyOverride(p, "image.uid=2001")
    applyOverride(p, "image.data_dir=/srv/gebai")
    applyOverride(p, "image.port=8080")
    applyOverride(p, "image.run_as_root=1")
    applyOverride(p, "image.extra_packages=vim,less")
    applyOverride(p, "image.labels=owner=Acme Inc,tier=prod")
    applyOverride(p, "image.healthcheck=false")
    expect(p.image.base).toBe("registry.internal/ubuntu:24.04")
    expect([p.image.user, p.image.uid, p.image.dataDir, p.image.port]).toEqual(["acme", 2001, "/srv/gebai", 8080])
    // 家目录跟随用户名（未显式指定时）
    expect(p.image.home).toBe("/home/acme")
    expect(p.image.runAsRoot).toBe(true)
    expect(p.image.extraPackages).toEqual(["vim", "less"])
    expect(p.image.labels).toEqual({ owner: "Acme Inc", tier: "prod" })
    expect(p.image.healthcheck).toBe(false)

    const q = plan()
    applyOverride(q, "image.healthcheck.path=/healthz")
    applyOverride(q, "image.healthcheck.interval=10s")
    expect(q.image.healthcheck).toEqual({ interval: "10s", timeout: "5s", startPeriod: "20s", retries: 3, path: "/healthz" })

    expect(() => applyOverride(plan(), "image.uid=abc")).toThrow(/需要整数/)
    expect(() => applyOverride(plan(), "image.nope=1")).toThrow(/未知镜像字段/)
    expect(() => applyOverride(plan(), "image.healthcheck.nope=1")).toThrow(/未知健康检查字段/)
    expect(() => applyOverride(plan(), "image.labels=owner")).toThrow(/需 "键=值"/)
  })

  test("未知键 / 未知资产 / 非法布尔一律报错", () => {
    const p = plan()
    expect(() => applyOverride(p, "assets.chalk=0")).toThrow(/未知资产/)
    expect(() => applyOverride(p, "unknown=1")).toThrow(/未知键/)
    expect(() => applyOverride(p, "assets.d2=maybe")).toThrow(/需要布尔值/)
    expect(() => applyOverride(p, "assets.d2")).toThrow(/键=值/)
  })
})

describe("构建计划输出", () => {
  test("env：资产开关、清单、系统包按裁剪结果落成 shell 变量", () => {
    const plan = parseProfile(
      {
        name: "minimal",
        sub_agents: { enable: ["task"] },
        tools: { disable: ["show", "fetch_url"] },
        assets: { web_ui: false, cv: false, d2: false, analyzer: false, browser: false, ripgrep: false },
        web: { vendor: [] },
        system: { git: false, python: false, bubblewrap: false, fonts: false, procps: false, tzdata: true },
      },
      "测试",
    )
    const env = planToEnv(plan, "docker/profiles/minimal.json")
    expect(env).toContain("GEBAI_BUILD_WEB_UI=0")
    expect(env).toContain("GEBAI_BUILD_CV=0")
    expect(env).toContain("GEBAI_BUILD_RG=0")
    expect(env).toContain("GEBAI_BUILD_SUBAGENTS=task")
    expect(env).toContain("GEBAI_BUILD_EXCLUDE_SUBAGENTS=")
    expect(env).toContain("GEBAI_BUILD_EXCLUDE_TOOLS=show,fetch_url")
    expect(env).toContain("GEBAI_WEB_VENDOR=")
    expect(envValue(env, "PLAN_SYSTEM_PACKAGES")).toBe("ca-certificates curl tini tzdata")
    expect(envValue(env, "PLAN_WITH_BROWSER")).toBe("0")
  })

  test("env：系统组全开 + chromium 时列出全部包并置位浏览器开关", () => {
    const plan = parseProfile({ name: "full", system: { chromium: true } }, "测试")
    const env = planToEnv(plan, "")
    expect(envValue(env, "PLAN_SYSTEM_PACKAGES")).toBe(
      "ca-certificates curl tini git python3 python3-venv python3-pip bubblewrap fonts-noto-cjk fontconfig tzdata procps",
    )
    expect(envValue(env, "PLAN_WITH_BROWSER")).toBe("1")
    expect(env).toContain("GEBAI_BUILD_WEB_UI=1")
    expect(envValue(env, "GEBAI_WEB_VENDOR")).toBe("monaco,plantuml,mermaid,echarts,d2js,xterm,tree_sitter")
  })

  test("报告：如实列出被裁掉的能力面", () => {
    const plan = parseProfile({ name: "minimal", description: "最小面", assets: { web_ui: false }, web: { vendor: [] } }, "测试")
    const report = renderReport(plan, "docker/profiles/minimal.json")
    expect(report).toContain("镜像构建裁剪计划：minimal（最小面）")
    expect(report).toContain("web_ui=off")
    expect(report).toContain("无（跳过全部前端引擎）")
    expect(report).toContain("chromium: off")
  })

  test("env：镜像本体项落成 shell 变量（数据根/用户/uid 供构建期 chown 与镜像内自查）", () => {
    const plan = parseProfile(
      { name: "x", image: { data_dir: "/srv/gebai", user: "acme", uid: 2001, apt_mirror: "http://m/ubuntu", npm_registry: "http://n/" } },
      "测试",
    )
    const env = planToEnv(plan, "")
    expect(envValue(env, "PLAN_DATA_DIR")).toBe("/srv/gebai")
    expect(envValue(env, "PLAN_USER")).toBe("acme")
    expect(envValue(env, "PLAN_UID")).toBe("2001")
    expect(envValue(env, "PLAN_APT_MIRROR")).toBe("http://m/ubuntu")
    expect(envValue(env, "PLAN_NPM_REGISTRY")).toBe("http://n/")
    expect(envValue(env, "PLAN_IMAGE_BASE")).toBe("ubuntu:24.04")
  })

  test("报告：列出镜像本体定制项（含健康检查的运行期可覆盖口径）", () => {
    const plan = parseProfile(
      {
        name: "x",
        image: { base: "registry.internal/ubuntu:24.04", user: "acme", uid: 2001, data_dir: "/srv/gebai", port: 8080, tz: "Asia/Shanghai", labels: { a: "1" }, apt_mirror: "http://m/ubuntu" },
      },
      "测试",
    )
    const report = renderReport(plan, "")
    expect(report).toContain("registry.internal/ubuntu:24.04｜用户 acme(2001:1000)｜数据根 /srv/gebai｜端口 8080｜时区 Asia/Shanghai")
    expect(report).toContain("标签 1 个")
    expect(report).toContain("探针 /api/health")
    expect(report).toContain("apt 源 http://m/ubuntu")
  })

  test("emitBuildArgs：指令级 build-arg（标签逐行、root 时运行用户为 root、空值留空交给 Dockerfile 默认）", () => {
    const plan = parseProfile(
      {
        name: "x",
        image: { user: "acme", uid: 2001, home: "/srv/home/acme", data_dir: "/srv/gebai", port: 8080, labels: { owner: "Acme Inc", tier: "prod" }, healthcheck: false },
      },
      "测试",
    )
    const args = emitBuildArgs(plan)
    for (const expected of ["IMAGE_USER=acme", "IMAGE_UID=2001", "IMAGE_HOME=/srv/home/acme", "IMAGE_DATA_DIR=/srv/gebai", "IMAGE_PORT=8080", "IMAGE_RUNTIME_USER=acme", "IMAGE_LABEL=owner=Acme Inc", "IMAGE_LABEL=tier=prod", "HEALTHCHECK_ENABLED=0", "IMAGE_TZ="]) {
      expect(args).toContain(expected)
    }
    // 不带 PLAN_ 前缀（那是镜像内自查的元信息，不作为 build-arg）
    expect(args.some((a) => a.startsWith("PLAN_"))).toBe(false)

    const asRoot = emitBuildArgs(parseProfile({ name: "x", image: { run_as_root: true } }, "测试"))
    expect(asRoot).toContain("IMAGE_RUN_AS_ROOT=1")
    expect(asRoot).toContain("IMAGE_RUNTIME_USER=root")
  })
})
