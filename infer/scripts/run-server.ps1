# run-server.ps1 — 启动 llama-server（OpenAI 兼容端点）
# 用法: powershell -File infer/scripts/run-server.ps1 [-Profile balanced] [-NCpuMoe N] [-Ctx N] [-Port 8080] [-Background]
#
# 设计要点：
#   * 档位来自 config/profiles.json，命令行参数可覆盖
#   * 每次启动的完整命令行写入 bench/reports/launch-<时间戳>.json（可审计、可回放）
#   * 自动把系统 CUDA runtime 目录加入 PATH（预编译包需要 cudart64_12.dll）
#   * -Background 启动时**完全脱离当前进程树**（WMI 创建 + cmd 重定向日志）：若用 Start-Process，
#     服务会继承调用方的 stdio 句柄，使调用方（工具/脚本）一直挂在长驻子进程上无法返回
#   * -NoWait 在 -Background 基础上立即返回（不做就绪等待）——调用方自行轮询 /health

param(
    [string]$Profile   = '',
    [string]$Model     = '',
    [int]   $NCpuMoe   = -1,
    [int]   $Ctx       = 0,
    [int]   $Port      = 8080,
    [int]   $NGpuLayers = -1,
    [string]$SpecType  = '',
    [int]   $Threads   = 0,
    [switch]$NoSpec,
    [switch]$Background,
    [switch]$NoWait,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Repo = Split-Path -Parent $Root
$CfgPath = Join-Path $Root 'config\profiles.json'
$Cfg = Get-Content $CfgPath -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $Profile) { $Profile = $Cfg.default_profile }
$P = $Cfg.profiles.$Profile
if (-not $P) { throw "档位不存在: $Profile（可用: $($Cfg.profiles.PSObject.Properties.Name -join ', ')）" }

# ---------- 解析引擎与模型路径 ----------
$engineDir = Join-Path $Root $Cfg.engine_dir
$exe = Join-Path $engineDir 'llama-server.exe'
if (-not (Test-Path $exe)) { throw "找不到引擎: $exe（先把预编译包解压到 $($Cfg.engine_dir)）" }

$modelsDir = Join-Path $Repo 'resources\models\infer'
if (-not $Model) { $Model = if ($P.model) { $P.model } else { $Cfg.default_model } }
$modelPath = if ([System.IO.Path]::IsPathRooted($Model)) { $Model } else { Join-Path $modelsDir $Model }
if (-not (Test-Path $modelPath)) { throw "找不到模型: $modelPath" }

# ---------- CUDA runtime 进 PATH ----------
$cudaRoot = Join-Path $env:ProgramFiles 'NVIDIA GPU Computing Toolkit\CUDA'
if (Test-Path $cudaRoot) {
    $v = Get-ChildItem $cudaRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($v) { $env:PATH = "$(Join-Path $v.FullName 'bin');$env:PATH" }
}

# ---------- 组装参数 ----------
function Val($override, $profileVal, $default) {
    if ($override -ne $null -and $override -ne -1 -and $override -ne 0 -and $override -ne '') { return $override }
    if ($profileVal -ne $null) { return $profileVal }
    return $default
}

$threads = if ($Threads -gt 0) { $Threads } elseif ($P.threads) { $P.threads } else { [int](0.5 * (Get-CimInstance Win32_Processor | Select-Object -First 1).NumberOfLogicalProcessors) }

$argvList = [System.Collections.Generic.List[string]]::new()
$argvList.Add('-m');          $argvList.Add($modelPath)
$argvList.Add('--host');      $argvList.Add('127.0.0.1')
$argvList.Add('--port');      $argvList.Add("$Port")
$argvList.Add('-c');          $argvList.Add("$(Val $Ctx $P.ctx 32768)")
$argvList.Add('-ngl');        $argvList.Add("$(Val $NGpuLayers $P.n_gpu_layers 99)")
$argvList.Add('-fa');         $argvList.Add($(if ($P.flash_attn) { $P.flash_attn } else { 'auto' }))
$argvList.Add('-ctk');        $argvList.Add($(if ($P.cache_type_k) { $P.cache_type_k } else { 'f16' }))
$argvList.Add('-ctv');        $argvList.Add($(if ($P.cache_type_v) { $P.cache_type_v } else { 'f16' }))
$argvList.Add('-t');          $argvList.Add("$threads")
$argvList.Add('-b');          $argvList.Add("$(if ($P.batch) { $P.batch } else { 2048 })")
$argvList.Add('-ub');         $argvList.Add("$(if ($P.ubatch) { $P.ubatch } else { 512 })")
if ($P.parallel) { $argvList.Add('-np'); $argvList.Add("$($P.parallel)") }
$argvList.Add('-a');          $argvList.Add('agentworld-35b-a3b')

# 显存规划：-ncmoe 优先，其次档位值
$effNcmoe = if ($NCpuMoe -ge 0) { $NCpuMoe } elseif ($P.n_cpu_moe -ne $null) { [int]$P.n_cpu_moe } else { -1 }
if ($effNcmoe -ge 0) { $argvList.Add('-ncmoe'); $argvList.Add("$effNcmoe") }

# 投机解码
$effSpec = if ($NoSpec) { $null } elseif ($SpecType) { $SpecType } elseif ($P.spec_type) { $P.spec_type } else { $null }
if ($effSpec) { $argvList.Add('--spec-type'); $argvList.Add($effSpec) }

# 附加参数
if ($P.extra_args) { foreach ($a in $P.extra_args) { $argvList.Add($a) } }

# ---------- 记录与启动 ----------
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$repDir = Join-Path $Root 'bench\reports'
if (-not (Test-Path $repDir)) { New-Item -ItemType Directory -Force -Path $repDir | Out-Null }
$log = Join-Path $repDir "server-$stamp.log"

$record = [ordered]@{
    started_at   = (Get-Date).ToString('s')
    profile      = $Profile
    model        = (Split-Path -Leaf $modelPath)
    model_bytes  = (Get-Item $modelPath).Length
    n_cpu_moe    = $effNcmoe
    spec_type    = $effSpec
    threads      = $threads
    port         = $Port
    exe          = $exe
    argv         = $argvList
    log          = $log
}
($record | ConvertTo-Json -Depth 5) | Set-Content -Encoding UTF8 (Join-Path $repDir "launch-$stamp.json")

Write-Host "档位: $Profile" -ForegroundColor Cyan
Write-Host "模型: $(Split-Path -Leaf $modelPath)  ($([math]::Round((Get-Item $modelPath).Length/1GB,2)) GB)"
Write-Host "显存规划: n_cpu_moe=$effNcmoe  线程=$threads  投机=$effSpec"
Write-Host "命令:" -ForegroundColor Cyan
Write-Host "  `"$exe`" $($argvList -join ' ')"
Write-Host "日志: $log"

if ($DryRun) { Write-Host '(DryRun，不启动)' -ForegroundColor Yellow; exit 0 }

if ($Background) {
    $quoted = ($argvList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $cmdline = "cmd /c `"`"$exe`" $quoted > `"$log`" 2> `"$($log).err`"`""
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmdline }
    if ($created.ReturnValue -ne 0) {
        Write-Host "启动失败（WMI Win32_Process.Create 返回 $($created.ReturnValue)）" -ForegroundColor Red
        exit 1
    }
    $pid2 = $created.ProcessId
    Write-Host "已启动 PID=$pid2（已脱离当前进程树）" -ForegroundColor Green
    Write-Host "日志: $log"

    # 服务状态文件：进程管理的唯一事实来源（status/stop/restart/logs 据此精确定位实例）。
    # 无 BOM 写出（Windows PowerShell 5.1 的 Set-Content -Encoding UTF8 会带 BOM，
    # 使读取侧 JSON.parse 直接失败）；失败只警告，不影响服务运行。
    try {
        $stateDir = Join-Path $Root 'run'
        if (-not (Test-Path $stateDir)) { New-Item -ItemType Directory -Force -Path $stateDir | Out-Null }
        $statePath = Join-Path $stateDir "server-$Port.json"
        $stateRec = [ordered]@{
            pid        = $pid2
            port       = $Port
            profile    = $Profile
            model      = (Split-Path -Leaf $modelPath)
            host       = '127.0.0.1'
            exe        = $exe
            log        = $log
            err_log    = "$($log).err"
            started_at = (Get-Date).ToString('s')
            argv       = @($argvList)
            engine     = (Split-Path -Leaf (Split-Path -Parent $exe))
        }
        [System.IO.File]::WriteAllText($statePath, (($stateRec | ConvertTo-Json -Depth 5) + "`n"), [System.Text.UTF8Encoding]::new($false))
        Write-Host "状态文件: $statePath"
    } catch {
        Write-Host "警告：服务状态文件写入失败（$($_.Exception.Message)）——服务本身不受影响" -ForegroundColor Yellow
    }

    if ($NoWait) { exit 0 }

    Write-Host "等待就绪..."
    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Seconds 2
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 2 -UseBasicParsing
            if ($r.StatusCode -eq 200) { Write-Host "服务就绪: http://127.0.0.1:$Port (耗时 $($i*2)s)" -ForegroundColor Green; exit 0 }
        } catch { }
        if (-not (Get-Process -Id $pid2 -ErrorAction SilentlyContinue)) {
            # 不删状态文件：留下的陈旧记录正是「启动即崩溃」的现场证据（status 会提示并附日志尾部），
            # 由 stop 显式清理。
            Write-Host "进程已退出（见日志 $log）" -ForegroundColor Red
            exit 1
        }
    }
    Write-Host '等待超时，服务可能仍在加载模型（大模型首次加载较慢）' -ForegroundColor Yellow
} else {
    & $exe @argvList
}
