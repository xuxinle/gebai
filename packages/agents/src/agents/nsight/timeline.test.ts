/**
 * 时间线聚合的合成事件库测试：自建一张与 nsys 导出同构的 SQLite（已知数值），
 * 断言流式聚合结果（并集忙碌时间、并发、空闲缝、分组统计、类型标签、NVTX 两种存储形态）
 * 与诊断结论——验证「报告规模无关」的聚合正确性，且不依赖 GPU 与 nsys 安装。
 */
import { describe, expect, test, beforeAll, afterAll } from "bun:test"
import { rmSync } from "node:fs"
import { computeTimelineFacts, timelineFacts, apiFacts, syncFacts, nvtxFacts, deviceFacts, reportScale, gapNeighbours, timelineScale } from "./nsys-analysis"
import { _resetFactsCache } from "../../core/perf/agg"
import { makeSyntheticReport, type Fixture } from "./test-fixture"
import { diagnoseNsys, shortSymbol } from "./findings"


let fixture: Fixture

/* 夹具**只读**（用例不改库）、聚合缓存以「报告」为键而非每次新建的实例，故整文件共建一次即可。
   早先放在 beforeEach 里每个用例重建，在满载并行（bun test 多文件同跑）时会造成 hook 超时——
   聚合是按行扫过整张事件库的，单次建库 + 13 次重扫把测试窗口挤满。 */
beforeAll(async () => {
  _resetFactsCache()
  fixture = await makeSyntheticReport()
})

afterAll(() => {
  fixture.report.close()
  // Windows 上 SQLite 释放文件句柄**晚于** `close()` 返回：紧接着 `rmSync` 会撞 EBUSY
  // （`maxRetries` 也盖不住——重试窗口内句柄可能还没放）。
  // 临时目录删不掉不是测试要证明的东西（系统迟早会清），因此这里**容忍失败**：
  // 否则一个环境性的清理问题会把整个测试文件判为失败，掩盖真正的回归。
  try {
    rmSync(fixture.root, { recursive: true, force: true, maxRetries: 5, retryDelay: 50 })
  } catch {
    // 留给系统临时目录清理
  }
})

describe("computeTimelineFacts（合成事件库）", () => {
  test("并集忙碌时间与并发按区间重叠计算（不是简单求和）", () => {
    const facts = computeTimelineFacts(fixture.report)
    // [0,3e6]（窗口内三段落进同一并集段）+ [4.0e6,4.1e6] + [5.0e6,5.15e6] + [1.0e7,1.2e7] + tiny 3×5000
    // bigKernel [0.5e6,2.5e6] 完全落在 [0,3e6] 内 → 不增加忙碌时间
    expect(facts.busyNs).toBe(3_000_000 + 100_000 + 150_000 + 2_000_000 + 15_000)
    expect(facts.maxConcurrent).toBe(2)
    expect(facts.firstActivityNs).toBe(0)
    expect(facts.lastActivityNs).toBe(20_025_000)
  })

  test("空闲缝按阈值切分并累计总时长（边界含等于）", () => {
    const facts = computeTimelineFacts(fixture.report, { gapMinNs: 1_000_000 })
    // 3.0e6→4.0e6（= 阈值，计入）、4.1e6→5.0e6（0.9e6，不计）、5.15e6→1.0e7、1.2e7→2.0e7
    const gaps = facts.gaps.map((g) => g.durNs).sort((a, b) => a - b)
    expect(gaps).toEqual([1_000_000, 4_850_000, 8_000_000])
    expect(facts.gapTotalNs).toBe(1_000_000 + 4_850_000 + 8_000_000)
    expect(facts.utilization).toBeLessThan(0.5)
  })

  test("内核分组统计：次数/总耗时/极值/网格几何/寄存器，按总耗时降序", () => {
    const facts = computeTimelineFacts(fixture.report)
    expect(facts.kernelInstances).toBe(7)
    expect(facts.kernelDistinctGroups).toBe(2)
    const scale = facts.kernels.find((k) => k.name.includes("scaleKernel"))!
    expect(scale.instances).toBe(4) // 3 段 + 流 2 的 4096 网格那次（同名同 mangled）
    expect(scale.totalNs).toBe(1_000_000 + 2_000_000 + 2_000_000 + 2_000_000)
    expect(scale.minNs).toBe(1_000_000)
    expect(scale.maxNs).toBe(2_000_000)
    expect(scale.registersPerThread).toBe(200)
    expect(scale.streams).toEqual([1, 2])
    expect(scale.totalThreads).toBe(32 * 128)
  })

  test("小内核/小网格/占用压力分类与阈值一致", () => {
    const facts = computeTimelineFacts(fixture.report)
    expect(facts.smallKernelInstances).toBe(3)
    expect(facts.smallKernelGroups.map((g) => g.name)).toContain("void tinyKernel(int*)")
    expect(facts.undersizedGroups.length).toBeGreaterThan(0)
    expect(facts.pressuredGroups.map((g) => g.name).join(" ")).toContain("scaleKernel")
  })

  test("显存传输按枚举标签聚合，流统计与启动间隔齐备", () => {
    const facts = computeTimelineFacts(fixture.report)
    expect(facts.memcpyCount).toBe(2)
    expect(facts.memcpyBytes).toBe(4096 + 8192)
    expect(facts.memcpyKinds[0]!.kind).toBe("Device-to-Host")
    expect(facts.streams.map((s) => s.streamId).sort()).toEqual([1, 2])
    expect(facts.launchGaps.length).toBeGreaterThan(0)
    expect(facts.launchGaps[0]!.gapNs).toBeGreaterThan(0)
  })

  test("时间线序列有界（不随事件数增长）", () => {
    const facts = computeTimelineFacts(fixture.report)
    expect(facts.timeline.length).toBeGreaterThan(0)
    expect(facts.timeline.length).toBeLessThanOrEqual(240)
    const scale = timelineScale(facts)
    expect(scale.spanNs).toBe(facts.timelineSpanNs)
  })

  test("事实按报告指纹缓存：同一报告二次调用复用同一对象（秒回）", () => {
    const first = timelineFacts(fixture.report)
    const second = timelineFacts(fixture.report)
    expect(second).toBe(first)
  })
})

describe("SQL 下推聚合（api/sync/nvtx/devices/scale）", () => {
  test("API 聚合与阻塞型 API 识别", () => {
    const api = apiFacts(fixture.report)
    expect(api.count).toBe(3)
    expect(api.totalNs).toBe(100 + 100 + 900_000)
    expect(api.top[0]!.name).toBe("cudaDeviceSynchronize_v3020")
    expect(api.blocking.map((b) => b.name)).toContain("cudaDeviceSynchronize_v3020")
  })

  test("同步聚合按类型标签", () => {
    const sync = syncFacts(fixture.report)
    expect(sync.count).toBe(1)
    expect(sync.byKind[0]!.kind).toBe("Stream sync")
    expect(sync.totalNs).toBe(2_000_000)
  })

  test("NVTX 两种存储形态都解析出名称（textId 与 text 列）", () => {
    const nvtx = nvtxFacts(fixture.report)
    expect(nvtx.available).toBe(true)
    expect(nvtx.count).toBe(2)
    const names = nvtx.top.map((t) => t.text).sort()
    expect(names).toEqual(["phase_compute", "phase_tiny"])
  })

  test("设备信息与规模统计可用（老版本缺列时不抛错）", () => {
    const dev = deviceFacts(fixture.report)
    expect(dev.devices[0]!.name).toBe("Synthetic GPU")
    const scale = reportScale(fixture.report)
    expect(scale.kernels).toBe(7)
    expect(scale.memcpys).toBe(2)
    expect(scale.totalEvents).toBe(7 + 2 + 3 + 1)
  })

  test("空闲缝邻接活动用索引定位边界", () => {
    const facts = computeTimelineFacts(fixture.report, { gapMinNs: 1_000_000 })
    const neighbours = gapNeighbours(fixture.report, facts.gaps.slice(0, 2))
    expect(neighbours.length).toBe(2)
    expect(neighbours.every((n) => n.before !== undefined || n.after !== undefined)).toBe(true)
  })
})

describe("诊断（合成数据端到端）", () => {
  test("问题清单反映合成报告的真实问题，且证据带实测值", () => {
    const facts = computeTimelineFacts(fixture.report, { gapMinNs: 1_000_000 })
    const d = diagnoseNsys({
      facts,
      api: apiFacts(fixture.report),
      sync: syncFacts(fixture.report),
      nvtx: nvtxFacts(fixture.report),
      devices: deviceFacts(fixture.report).devices,
      gapNeighbours: gapNeighbours(fixture.report, facts.gaps.slice(0, 5)),
    })
    const ids = d.findings.map((f) => f.id)
    expect(ids).toContain("gpu-idle") // 利用率低
    expect(ids).toContain("sync-stall") // 单次同步 2ms 超过 1ms 阈值
    expect(ids).not.toContain("launch-bound") // 小内核仅 3 次，未达 100 次阈值
    expect(ids).toContain("grid-undersized")
    expect(ids).toContain("occupancy-pressure")
    expect(ids).not.toContain("no-concurrency") // 两个流且并发 >1
    expect(d.metrics["NVTX区间数"]).toBe(2)
    const idle = d.findings.find((f) => f.id === "gpu-idle")!
    expect(idle.evidence.join(" ")).toContain(shortSymbol("void scaleKernel<float>(float*, float, int)"))
  })
})
