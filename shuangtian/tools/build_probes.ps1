# 一次性编译文字量尺（仅验证用；不进 st.pkg）。
# 复用 build/dev/obj 的框架对象，剔除 platform_*（GPU/窗口后端，量尺不需要且要额外系统库）与 _test。
# 用法： pwsh -File tools/_build_probes.ps1 [-Only name] [-Profile dev]
param([string]$Only = '', [string]$Profile = 'dev')
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root
$outDir = Join-Path $Root 'build/probe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$objs = Get-ChildItem (Join-Path $Root "build/$Profile/obj") -Filter *.o |
  Where-Object { $_.Name -match "_shuangtian_src_(core|math|codec|raster|text)_" -and
                 # 只排真·后端文件：`core/platform_time.cpp` 这类是**跨模块被依赖**的实现，排掉就缺符号
                 # （实测：排掉 core_platform_* 后 core/time.cpp 的 local_fields 直接 undefined）。
                 $_.Name -notmatch "raster_platform_|shell_platform_|platform_gl|_test\.|test_runner|zz_probe" } |
  Select-Object -ExpandProperty FullName
if ($objs.Count -lt 10) { throw "框架对象不足（$($objs.Count)）：先跑 st build gallery --profile=$Profile" }

$probes = @(
  'text_sharpness_probe', 'stem_phase_probe', 'text_quality_probe', 'text_ink_conserve',
  'text_fit_by_size', 'fit_shift_probe', 'text_weight_probe', 'text_phase_probe',
  'lcd_compare', 'grid_fit_report', 'text_weight_cost', 'text_ab_probe',
  'grid_fit_sampling_diag', 'fit_apply_diag', 'glyph_phase_probe', 'stroke_uniformity_probe', 'fit_reject_probe', 'fit_recall_probe'
)
if ($Only) { $probes = @($Only) }

foreach ($name in $probes) {
  $source = Join-Path $Root "tools/$name.cpp"
  if (-not (Test-Path $source)) { Write-Host "-- 跳过（无源文件）: $name"; continue }
  $exe = Join-Path $outDir "$name.exe"
  if (Test-Path $exe) { Remove-Item $exe -Force -ErrorAction SilentlyContinue }
  $flags = @('-std=c++20', '-O1', '-Iinclude', '-Ithird_party', '-Ithird_party/sqlite')
  $log = & g++ @flags $source @objs '-o' $exe '-lws2_32' '-lwinpthread' 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Host "== 失败: $name" -ForegroundColor Red
    Write-Host ($log | Out-String)
  } else {
    Write-Host "== OK: $name"
  }
}




