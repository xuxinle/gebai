# smoke-infer-tools.ps1 — 本地推理「进程管理 + 批量提交 + 结构化输出」实机冒烟
#
# 用法:
#   pwsh -File infer/scripts/smoke-infer-tools.ps1                 # 用已就绪的服务（:8080）
#   pwsh -File infer/scripts/smoke-infer-tools.ps1 -Port 8080 -Items 3
#   pwsh -File infer/scripts/smoke-infer-tools.ps1 -Start          # 先按档位拉起服务再冒烟
#   pwsh -File infer/scripts/smoke-infer-tools.ps1 -Fixture        # 另写一份批量输入 JSONL 到 bench/runs/
#
# 检查项（全部为真实 HTTP 与真实文件，不依赖 GEBAI 服务）：
#   1. /health 与 /props（n_ctx、slot 数）——批量并发上限的依据
#   2. 服务状态文件 run/server-<Port>.json 的内容（PID/档位/模型/日志）与 PID 存活
#   3. 结构化输出：response_format=json_schema 约束→返回可解析且满足必填字段的 JSON；再测一次校验失败重试路径（-BadSchema）
#   4. 批量提交：并发 1 串行跑 N 条，逐条计时并汇总有效吞吐（服务端 timings 优先）
#   5. 结果落盘：bench/runs/<job>/results.jsonl 行数 = N 且全部 ok
#
# 退出码: 0 全通过 / 1 有失败项（可接入 CI 或开机自检）

param(
    [int]   $Port    = 8080,
    [int]   $Items   = 3,
    [string]$Profile = '',
    [switch]$Start,
    [switch]$Fixture,
    [switch]$BadSchema
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$base = "http://127.0.0.1:$Port"
$statePath = Join-Path $Root "run/server-$Port.json"
$runsDir = Join-Path $Root 'bench/runs'
$jobId = "smoke-$((Get-Date).ToString('yyyyMMdd-HHmmss'))"
$jobDir = Join-Path $runsDir $jobId
$resultsPath = Join-Path $jobDir 'results.jsonl'

$script:failed = 0
function Check($name, $ok, $detail) {
    if ($ok) { Write-Host "  [OK]   $name" -ForegroundColor Green }
    else { Write-Host "  [FAIL] $name —— $detail" -ForegroundColor Red; $script:failed++ }
}
function Post-Json($url, $obj) {
    $body = $obj | ConvertTo-Json -Depth 12 -Compress
    return Invoke-RestMethod -Uri $url -Method Post -ContentType 'application/json; charset=utf-8' `
        -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) -TimeoutSec 900
}

Write-Host "== 本地推理工具链冒烟（端口 $Port）==" -ForegroundColor Cyan

# ── 0. 可选：先拉起服务 ──────────────────────────────────────────────────
if ($Start) {
    Write-Host "启动服务..." -ForegroundColor Cyan
    $argv = @('-Port', "$Port", '-Background', '-NoWait')
    if ($Profile) { $argv += @('-Profile', $Profile) }
    & (Join-Path $Root 'scripts/run-server.ps1') @argv
    for ($i = 0; $i -lt 90; $i++) {
        Start-Sleep -Seconds 2
        try { if ((Invoke-WebRequest "$base/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200) { break } } catch { }
    }
}

# ── 1. /health 与 /props ────────────────────────────────────────────────
Write-Host "`n[1] 服务探测" -ForegroundColor Cyan
try {
    $h = Invoke-RestMethod "$base/health" -TimeoutSec 5
    Check '/health 可用' ($null -ne $h) 'health 无响应'
} catch { Check '/health 可用' $false $_.Exception.Message }

$slots = 0; $nCtx = 0
try {
    $p = Invoke-RestMethod "$base/props" -TimeoutSec 5
    $slots = [int]$p.total_slots
    $nCtx = [int]$p.default_generation_settings.n_ctx
    Check "/props 报出 n_ctx/slot（n_ctx=$nCtx slots=$slots）" ($nCtx -gt 0 -and $slots -gt 0) '缺少 n_ctx/total_slots'
} catch { Check '/props 可用' $false $_.Exception.Message }

# ── 2. 服务状态文件 ─────────────────────────────────────────────────────
Write-Host "`n[2] 服务状态文件（进程管理依据）" -ForegroundColor Cyan
if (Test-Path $statePath) {
    $st = Get-Content $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $alive = $null -ne (Get-Process -Id $st.pid -ErrorAction SilentlyContinue)
    Check "run/server-$Port.json 存在且 PID $($st.pid) 存活" $alive '状态文件里的 PID 已不在'
    Check "状态文件含档位/模型/日志路径（$($st.profile) / $(Split-Path -Leaf $st.model)）" `
        ($st.profile -and $st.model -and $st.log) '字段缺失'
    if ($st.log -and (Test-Path $st.log)) { Write-Host "         日志: $($st.log)（$([math]::Round((Get-Item $st.log).Length/1KB,1)) KB）" }
    else { Write-Host "         （日志尚未落盘或路径未写：注意旧版启动脚本不写状态文件，工具会按端口补写）" -ForegroundColor Yellow }
} else {
    Check "run/server-$Port.json 存在" $false '无状态文件（服务可能由旧版脚本启动，local_infer_status 会按端口反查 PID）'
}

# ── 3. 结构化输出 ───────────────────────────────────────────────────────
Write-Host "`n[3] 结构化输出（json_schema 约束 + 校验）" -ForegroundColor Cyan
$schema = @{
    type       = 'object'
    required   = @('summary', 'tags', 'score')
    properties = @{
        summary = @{ type = 'string' }
        tags    = @{ type = 'array'; items = @{ type = 'string' } }
        score   = @{ type = 'integer'; minimum = 0; maximum = 10 }
    }
}
$reqBody = @{
    messages              = @(@{ role = 'user'; content = '用 JSON 概括：llama.cpp 在消费级 GPU 上跑 35B MoE 量化模型，解码约 143 token/s。' })
    temperature           = 0
    max_tokens            = 256
    chat_template_kwargs  = @{ enable_thinking = $false }
    response_format       = @{ type = 'json_schema'; json_schema = @{ name = 'summary'; strict = $true; schema = $schema } }
}
$sw = [Diagnostics.Stopwatch]::StartNew()
try {
    $r = Post-Json "$base/v1/chat/completions" $reqBody
    $sw.Stop()
    $content = $r.choices[0].message.content
    $head = if ($content) { $content.Substring(0, [Math]::Min(160, $content.Length)) } else { '(空)' }
    Write-Host "         返回 $head"
    try {
        $j = $content | ConvertFrom-Json
        $okFields = ($j.summary -is [string]) -and ($j.tags -is [array]) -and ($j.score -is [int] -or $j.score -is [long])
        Check '约束解码返回可解析 JSON 且必填字段齐备' $okFields "字段类型不符：$(($j | ConvertTo-Json -Compress))"
    } catch { Check '返回内容可解析为 JSON' $false $_.Exception.Message }
    $tps = if ($r.timings -and $r.timings.predicted_ms -gt 0) { [math]::Round($r.timings.predicted_n / ($r.timings.predicted_ms / 1000), 1) } else { 0 }
    Write-Host ("         服务端实测: 生成 {0} token / {1:N0} ms → {2} t/s（墙钟 {3:N1}s）" -f `
        $r.timings.predicted_n, $r.timings.predicted_ms, $tps, $sw.Elapsed.TotalSeconds)
} catch {
    $sw.Stop()
    Check '结构化请求成功' $false $_.Exception.Message
}

if ($BadSchema) {
    Write-Host "`n[3b] 校验失败路径（必填字段缺失时应回灌重试并如实报错）" -ForegroundColor Cyan
    $bad = @{
        messages        = @(@{ role = 'user'; content = '不要输出任何 JSON，用一句话说明本地推理的定位。' })
        temperature     = 0
        max_tokens      = 64
        response_format = @{ type = 'json_schema'; json_schema = @{ name = 's'; strict = $true; schema = $schema } }
    }
    try {
        $r2 = Post-Json "$base/v1/chat/completions" $bad
        $ok2 = $null -ne $r2.choices[0].message.content
        Check '异常输入下服务仍正常返回（不崩溃）' $ok2 '无 content'
        Write-Host "         返回片段: $($r2.choices[0].message.content.Substring(0,[Math]::Min(120,$r2.choices[0].message.content.Length)))"
    } catch { Check '异常输入下服务仍正常返回' $false $_.Exception.Message }
}

# ── 4/5. 批量提交（串行 + 逐条落盘） ────────────────────────────────────
Write-Host "`n[4] 批量提交（串行 $Items 条；服务 slot 上限 = $slots，批量工具按此夹取 concurrency）" -ForegroundColor Cyan
$conc = [Math]::Min([Math]::Max(1, $Items), [Math]::Max(1, $slots))
New-Item -ItemType Directory -Force -Path $jobDir | Out-Null
$results = [System.Collections.Generic.List[object]]::new()
$batchStart = Get-Date
for ($i = 1; $i -le $Items; $i++) {
    $body = @{
        messages             = @(@{ role = 'user'; content = "第 $i 条：用一句 JSON 描述本地推理的一个优势（字段 text）。" })
        temperature          = 0
        max_tokens           = 128
        chat_template_kwargs = @{ enable_thinking = $false }
        response_format      = @{ type = 'json_schema'; json_schema = @{ name = 'one'; strict = $true
            schema = @{ type = 'object'; required = @('text'); properties = @{ text = @{ type = 'string' } } } } }
    }
    $t0 = Get-Date
    $itemOk = $true; $err = $null; $content = $null; $predN = 0; $predMs = 0
    try {
        $rr = Post-Json "$base/v1/chat/completions" $body
        $content = $rr.choices[0].message.content
        $j = $content | ConvertFrom-Json
        if (-not ($j.text -is [string])) { throw "缺少 text 字段" }
        $predN = [int]$rr.timings.predicted_n; $predMs = [double]$rr.timings.predicted_ms
    } catch { $itemOk = $false; $err = $_.Exception.Message }
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    # 不用「hashtable 内联 if 表达式」（PS 5.1 解析不支持）——先取值再组装
    $jsonOut = $null
    if ($content) { try { $jsonOut = $content | ConvertFrom-Json } catch { $jsonOut = $null } }
    $rec = [ordered]@{
        id           = "item-$i"
        ok           = $itemOk
        elapsed_ms   = $ms
        predicted_n  = $predN
        predicted_ms = $predMs
        json         = $jsonOut
        error        = $err
    }
    $results.Add($rec)
    ($rec | ConvertTo-Json -Depth 8 -Compress) | Add-Content -Encoding UTF8 $resultsPath
    Write-Host ("         item-{0}: {1}  {2} ms{3}" -f $i, $(if ($itemOk) { 'OK' } else { 'FAIL' }), $ms, $(if ($err) { "  $err" } else { '' }))
}
$batchMs = [int]((Get-Date) - $batchStart).TotalMilliseconds
$okCount = ($results | Where-Object { $_.ok }).Count
$totGen = ($results | Measure-Object -Property predicted_n -Sum).Sum
$maxPred = ($results | Measure-Object -Property predicted_ms -Maximum).Maximum
Check "批量 $Items 条全部成功" ($okCount -eq $Items) "$okCount/$Items 成功"
if ($maxPred -gt 0) {
    Write-Host ("         聚合: 生成 {0} token / 最长单条 {1:N0} ms → {2} t/s（墙钟 {3:N1}s，并发上限 {4}）" -f `
        $totGen, $maxPred, [math]::Round($totGen / ($maxPred / 1000), 1), ($batchMs / 1000), $conc)
}

Write-Host "`n[5] 结果落盘" -ForegroundColor Cyan
Check "results.jsonl 存在且行数 = $Items" ((Test-Path $resultsPath) -and ((Get-Content $resultsPath).Count -eq $Items)) '文件缺失或行数不符'
Write-Host "         任务目录: $jobDir"
Write-Host "         续跑用法: local_infer_batch(job_id=`"$jobId`", resume=true)（同 id 重跑只处理未成功项）"

# ── 可选：另写一份输入 JSONL 供 items_file 用法 ────────────────────────
if ($Fixture) {
    $fx = Join-Path $runsDir 'smoke-questions.jsonl'
    New-Item -ItemType Directory -Force -Path $runsDir | Out-Null
    @(1..3) | ForEach-Object { (@{ id = "q$_"; prompt = "第 $_ 个问题：用一句 JSON（字段 text）说明本地推理的一个优势。" } | ConvertTo-Json -Compress) } |
        Set-Content -Encoding UTF8 $fx
    Write-Host "`n已写输入样本: $fx（用法 local_infer_batch(items_file=`"bench/runs/smoke-questions.jsonl`")）"
}

Write-Host ""
if ($script:failed -eq 0) { Write-Host "冒烟全部通过 ✓" -ForegroundColor Green; exit 0 }
Write-Host "冒烟有 $($script:failed) 项失败 ✗" -ForegroundColor Red; exit 1
