# smoke-source-build.ps1 —— 内网源码编译冒烟（Windows 包装：真正的实现是 smoke-source-build.ts）
#
# 用法:
#   pwsh -File infer/scripts/smoke-source-build.ps1
#   pwsh -File infer/scripts/smoke-source-build.ps1 -SkipBuild            # 只验证下游链路（用已安装引擎）
#   pwsh -File infer/scripts/smoke-source-build.ps1 -Device cuda -Items 4
#   pwsh -File infer/scripts/smoke-source-build.ps1 -Keep -Port 19181
#
# 为什么包装成脚本而不是直接写 PowerShell：编译流程与产品共用同一套实现（local_infer 子Agent 的工具），
# 脚本里重复一遍 cmake 调用必然与产品漂移。本包装只负责找到 bun 并把参数原样透传。
#
# 退出码：0 全通过 / 1 有失败项（可接 CI）。

param(
    [ValidateSet('cpu', 'cuda', 'vulkan', 'metal', 'rocm')] [string]$Device = 'cpu',
    [string]$EngineId = '',
    [int]   $Jobs = 0,
    [int]   $Port = 19180,
    [string]$Model = '',
    [string]$Profile = 'cpu-small',
    [int]   $Items = 2,
    [int]   $Timeout = 1800,
    [switch]$SkipBuild,
    [switch]$Clean,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$ts = Join-Path $here 'smoke-source-build.ts'

$bun = Get-Command bun -ErrorAction SilentlyContinue
if (-not $bun) {
    Write-Host "找不到 bun —— 本脚本用 bun 运行 TypeScript 实现（$ts）。" -ForegroundColor Red
    Write-Host "安装 bun: https://bun.sh" -ForegroundColor Yellow
    exit 127
}

$argv = @('run', $ts, '--device', $Device, '--port', "$Port", '--profile', $Profile, '--items', "$Items", '--timeout', "$Timeout")
if ($EngineId) { $argv += @('--engine-id', $EngineId) }
if ($Jobs -gt 0) { $argv += @('--jobs', "$Jobs") }
if ($Model) { $argv += @('--model', $Model) }
if ($SkipBuild) { $argv += '--skip-build' }
if ($Clean) { $argv += '--clean' }
if ($Keep) { $argv += '--keep' }

& bun @argv
exit $LASTEXITCODE
