#!/usr/bin/env pwsh
<#
.SYNOPSIS
  霜天构建/测试性能测量器：墙钟 + CPU 利用率 + 平均并行度。

.DESCRIPTION
  为什么需要它（而不是手敲 Measure-Command）：

  · **输出必须重定向到文件** —— 终端重绘会让每次 print 多付 ~37ms，
    对着终端量构建等于把渲染时间算进编译时间（`docs/BUILD_TEST_PERF.md` §0 第 1 条）。
    本脚本用 `Start-Process -RedirectStandardOutput` 落盘，天然满足。
  · **CPU 利用率要单独量** —— 只报墙钟看不出"核有没有空转"，
    而那正是"并行度不足"的唯一可观测证据。
  · **多轮取最小值** —— 本机有背景负载，单次测量会被污染（§0 第 3 条）。

  采集口径（写进 BUILD_TEST_PERF.md 时照抄）：

  · 墙钟：`Stopwatch` 包住整个子进程（含进程启动与退出）。
  · **CPU 秒**：每 100ms 枚举一次编译相关进程（st / g++ / cc1plus / cc1 / as / ld / collect2），
    把每个 PID 的 `TotalProcessorTime` 记进字典——**进程退出后保留最后一次值**，
    这样累加得到的是"本次运行真正消耗的 CPU 秒"，不会因为子进程先退出而漏计。
  · **平均并行度** = 总 CPU 秒 ÷ 墙钟秒（1.0 = 单核满载，28.0 = 28 核满载）。
  · **逻辑核利用率** = 平均并行度 ÷ 逻辑核数。
  · **峰值并行度** = 采样期间单次观测到的最大「同时活跃 CPU 秒增量 ÷ 采样间隔」。

.PARAMETER Command
  要跑的命令行（含参数，整体加引号）。

.PARAMETER Rounds
  轮数（默认 3）；报告**最小值**（口径纪律）。

.EXAMPLE
  ./tools/bench.ps1 -Label build-empty -Command "build st --profile dev --no-self-rebuild"

.EXAMPLE
  ./tools/bench.ps1 -Label test-full -Rounds 3 -Cmd "test"
#>
param(
  # 参数名用 `-Cmd` 而不是 `-Command`：后者是 PowerShell 自己的参数名，
  # `pwsh -File x.ps1 -Command "..."` 会被当成内联命令吃掉（脚本收不到值，
  # 报的错在高处：“无法将 Object[] 转换为 Int32”）。踩过，别改回去。
  [Parameter(Mandatory = $true)][string]$Cmd,
  [int]$Rounds = 3,
  [string]$Label = 'bench',
  [string]$St = '',
  [string]$LogDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $St) { $St = Join-Path $root 'build\bin\st.exe' }
if (-not (Test-Path $St)) { Write-Error "找不到 st：$St" }
if (-not $LogDir) { $LogDir = Join-Path $root 'tmp\logs' }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$cores = [int][Environment]::ProcessorCount
if ($cores -le 0) { $cores = 1 }
# 参与编译/链接/测试的进程名（小写比较）：st 自己 + GCC 全家 + 链接器 + 测试二进制。
#
# `st_tests` 必须列：`st test` 真正跑用例的是**它**，而不是 st 自己。
# 漏掉它的症状是“CPU 只采到 11s、并行度 0.44”——而实际墙钟 25s，
# 那不是“核在空转”，是**尺子没量到**（本仓 BUILD_TEST_PERF §0 第 2 条同一类错：
# 拿错的被测对象量出来的数字会导出相反结论）。
$targetNames = @('st', 'st_tests', 'g++', 'gcc', 'cc1plus', 'cc1', 'as', 'ld', 'collect2',
  'objcopy', 'clang++', 'clang', 'node', 'bun')

$results = @()
foreach ($roundIndex in 1..$Rounds) {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $logPath = Join-Path $LogDir ("{0}-r{1}-{2}.log" -f $Label, $roundIndex, $stamp)
  $errLog = "$logPath.err"
  $argv = @($Cmd -split '\s+' | Where-Object { $_ -ne '' })

  # pid → CPU 秒：进程退出后**保留**（总 CPU 累计不能漏计已跑完的单元）
  $cpuByPid = @{}
  # 上一轮的 pid → CPU 秒快照：**只用于算瞬时并行度**（两端都在的才算增量）
  $prevSnapshot = @{}
  $maxInstant = 0.0
  $prevTick = [Diagnostics.Stopwatch]::GetTimestamp()

  $stopwatch = [Diagnostics.Stopwatch]::StartNew()
  $process = Start-Process -FilePath $St -ArgumentList $argv `
    -RedirectStandardOutput $logPath -RedirectStandardError $errLog `
    -NoNewWindow -PassThru

  while (-not $process.HasExited) {
    $snapshot = @{}
    foreach ($p in (Get-Process -ErrorAction SilentlyContinue | Where-Object { $targetNames -contains $_.ProcessName.ToLower() })) {
      try {
        $seconds = $p.TotalProcessorTime.TotalSeconds
        $cpuByPid[$p.Id] = $seconds
        $snapshot[$p.Id] = $seconds
      } catch { }
    }
    # 瞬时并行度：**只算两轮都在的进程**的增量 ÷ 时间。
    #
    # 不能算“全部见过的 pid”：一个 cc1plus 可能在两次采样间启动**并跑完**，
    # 首次观测到时已带 2 秒 CPU——把这 2 秒当成本轮增量，会算出 5492 这种
    # 不可能的并行度（实测踩到两次）。只算两端都在的，才是“这一刻几核在跑”。
    $liveDelta = 0.0
    foreach ($procId in $snapshot.Keys) {
      if ($prevSnapshot.ContainsKey($procId)) { $liveDelta += ($snapshot[$procId] - $prevSnapshot[$procId]) }
    }
    $nowTs = [Diagnostics.Stopwatch]::GetTimestamp()
    $dt = ($nowTs - $prevTick) / [double][Diagnostics.Stopwatch]::Frequency
    if ($dt -ge 0.05 -and $liveDelta -ge 0) {
      $instant = $liveDelta / $dt   # 这一刻有几核在跑
      if ($instant -gt $maxInstant) { $maxInstant = $instant }
    }
    $prevSnapshot = $snapshot
    $prevTick = $nowTs
    Start-Sleep -Milliseconds 100
  }
  $stopwatch.Stop()
  $exitCode = $process.ExitCode
  # 收尾再采一次（可能还有未入库的短命进程的最终值）
  foreach ($p in (Get-Process -ErrorAction SilentlyContinue | Where-Object { $targetNames -contains $_.ProcessName.ToLower() })) {
    try { $cpuByPid[$p.Id] = $p.TotalProcessorTime.TotalSeconds } catch { }
  }
  $totalCpu = 0.0
  foreach ($v in $cpuByPid.Values) { $totalCpu += $v }

  $wall = [Math]::Max(0.001, $stopwatch.Elapsed.TotalSeconds)
  $parallel = $totalCpu / $wall
  $utilization = 100.0 * $parallel / $cores

  $results += [pscustomobject]@{
    round     = $roundIndex
    wall_s    = [Math]::Round($wall, 2)
    exit      = $exitCode
    cpu_s     = [Math]::Round($totalCpu, 1)
    parallel  = [Math]::Round($parallel, 2)
    peak      = [Math]::Round($maxInstant, 1)
    util_pct  = [Math]::Round($utilization, 1)
    log       = (Split-Path -Leaf $logPath)
  }
  Write-Host ("  r{0}: wall {1,7:N2}s  exit {2}  CPU {3,7:N1}s  并行度 {4,5:N2}  峰值 {5,4:N1}  利用率 {6,5:N1}%" -f `
      $roundIndex, $wall, $exitCode, $totalCpu, $parallel, $maxInstant, $utilization)
}

$minWall = (@($results | measure -Property wall_s -Minimum)[0]).Minimum
$bestRow = $results | Where-Object { $_.wall_s -eq $minWall } | Select-Object -First 1
Write-Host ''
Write-Host ("[{0}] 最小墙钟 {1:N2}s（{2} 轮）· 该轮 CPU {3:N1}s · 并行度 {4:N2}/{5} 核（{6:N1}%）· 峰值 {7:N1}" -f `
    $Label, $minWall, $Rounds, $bestRow.cpu_s, $bestRow.parallel, $cores, $bestRow.util_pct, $bestRow.peak)
$results | Format-Table -AutoSize
