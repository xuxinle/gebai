/**
 * 仓库自包含闸门（scripts/check-repo-self-contained.ts）的**判定核**单测。
 *
 * 为什么这条闸门必须有单测：它失败时是红、通过时是绿——**豁免写歪一点把真源码盖住，
 * 闸门依然退出码 0**，这是最安静的那种失效。所以两类用例都必须钉住：
 * ① 该报的必须报（尤其「加豁免之后」仍要报）；
 * ② 该放过的必须放过（否则闸门变成噪音，早晚被人整体关掉）。
 */
import { describe, expect, test } from "bun:test"
import { EXEMPT_PREFIXES, findOffenders, isExempt, needsTracking } from "./check-repo-core"

describe("needsTracking：源码要入库", () => {
  test("普通源文件必须入库（各语言）", () => {
    for (const p of [
      "packages/web/src/main.ts",
      "packages/server/src/index.ts",
      "shuangtian/src/ui/xml_ui.cpp",
      "shuangtian/include/st/ui/element.hpp",
      "tools/build.py",
      "infer/scripts/fetch.ps1".replace(".ps1", ".py"),
    ]) {
      expect(needsTracking(p)).toBe(true)
    }
  })

  test("事故原型：过宽 .gitignore 模式盖住 src/core/ 下的源码，必须报", () => {
    // 仓里曾有过「为崩溃转储写裸 core」的先例——这条正是这道闸门存在的理由
    expect(needsTracking("packages/server/src/core/engine/engine.ts")).toBe(true)
    expect(needsTracking("shuangtian/include/st/core/entry.cpp")).toBe(true)
  })
})

describe("needsTracking：产物与生成物放过", () => {
  test("产物目录段（build/dist/node_modules…）跳过", () => {
    for (const p of [
      "packages/web/dist/assets/main.js",
      "shuangtian/build/bin/x.cpp",
      "node_modules/foo/index.js",
      "packages/x/.turbo/a.ts",
      "coverage/lcov-report/x.js",
    ]) {
      expect(needsTracking(p)).toBe(false)
    }
  })

  test(".generated. 约定与非常规扩展名跳过", () => {
    expect(needsTracking("packages/server/src/core/web.bundle.generated.ts")).toBe(false)
    expect(needsTracking("docs/design.md")).toBe(false)
    expect(needsTracking("resources/model.gguf")).toBe(false)
    expect(needsTracking("README")).toBe(false)
  })
})

describe("显式豁免：精确命中，不误杀相邻路径", () => {
  test("豁免表每条都必须写理由（空理由 = 闸门被静默削弱）", () => {
    expect(EXEMPT_PREFIXES.length).toBeGreaterThan(0)
    for (const e of EXEMPT_PREFIXES) {
      expect(e.prefix.length).toBeGreaterThan(0)
      // 理由要能说清「谁生成/谁托管 + 重建入口」——只允许有实质长度的一句话
      expect(e.reason.trim().length).toBeGreaterThan(20)
    }
  })

  test("托管的外部源码树命中豁免（llama.cpp 基线）", () => {
    expect(isExempt("infer/engine/llama.cpp-b11100/src/llama.cpp")).toBe(true)
    expect(isExempt("infer/engine/llama.cpp-b11100/ggml/include/ggml.h")).toBe(true)
    expect(needsTracking("infer/engine/llama.cpp-b11100/src/llama.cpp")).toBe(false)
    // 换基线 tag（b12000）同样是托管资产——`*` 语义就是为這種版本号漂移准备
    expect(needsTracking("infer/engine/llama.cpp-b12000/src/llama.cpp")).toBe(false)
  })

  test("**不误杀**：前缀是精确语义，不是模糊包含", () => {
    // 「llama.cpp-」是前缀：别的引擎目录不该被盖住
    expect(isExempt("infer/engine/other-engine/src/a.cpp")).toBe(false)
    expect(needsTracking("infer/engine/other-engine/src/a.cpp")).toBe(true)
    // 「data/」是目录语义：data 开头的兄弟目录不该被盖住
    expect(isExempt("data/cefu/x.js")).toBe(true)
    expect(needsTracking("data2/real-source.ts")).toBe(true)
    expect(isExempt("database/schema.ts")).toBe(false)
    expect(needsTracking("database/schema.ts")).toBe(true)
    // 无结尾斜杠的前缀按「相等或后接 /」匹配：three_browser.js 的后缀兄弟不命中
    expect(isExempt("shuangtian/tools/parity/three_browser.js")).toBe(true)
    expect(isExempt("shuangtian/tools/parity/three_browser.js.bak.ts")).toBe(false)
    expect(needsTracking("shuangtian/tools/parity/other.ts")).toBe(true)
  })

  test("新的漏项位置不会被老豁免顺手放过（回归：别把豁免写成段名）", () => {
    // 这些都是「长得像豁免目录、但确实是该入库源码」的路径
    for (const p of [
      "packages/web/src/todo-status.ts",
      "shuangtian/apps/caiyun/kit/templates/tpl-multi-element.js",
      "infer/config/assets.manifest.json".replace(".json", ".ts"),
      "scripts/check-repo-core.ts",
    ]) {
      expect(needsTracking(p)).toBe(true)
    }
  })
})

describe("findOffenders：整批筛选", () => {
  test("大小写健壮与空项过滤", () => {
    const out = findOffenders(["", "a/b/C.PP", "a/b/c.txt", "infer/engine/llama.cpp-b1/src/x.c"])
    // `.PP` 不在扩展名表里（表内为 .cpp/.h 等），故不报；托管源码树被豁免；空项被过滤
    expect(out).toEqual([])
    expect(findOffenders(["src/REAL.H"])).toEqual(["src/REAL.H"]) // .H → .h 在表里
  })

  test("混合输入：只留违规项且保持原顺序", () => {
    const input = [
      "a/src/real.ts",
      "b/build/gen.ts",
      "c/x.generated.ts",
      "data/cefu/x.js",
      "infer/engine/llama.cpp-b11100/src/y.cpp",
      "d/real2.py",
    ]
    expect(findOffenders(input)).toEqual(["a/src/real.ts", "d/real2.py"])
  })
})
