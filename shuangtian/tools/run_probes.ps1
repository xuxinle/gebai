# 跑全部文字量尺并把输出归档到 build/probe/out/（纯取证，不改框架）。
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root
$outDir = Join-Path $Root 'build/probe/out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$list = @('text_phase_probe', 'stem_phase_probe', 'text_ink_conserve', 'text_fit_by_size',
          'grid_fit_report', 'fit_apply_diag', 'grid_fit_sampling_diag', 'text_weight_probe',
          'text_sharpness_probe')
foreach ($name in $list) {
  $exe = Join-Path $Root "build/probe/$name.exe"
  if (-not (Test-Path $exe)) { Write-Host "SKIP $name"; continue }
  $target = Join-Path $outDir "$name.txt"
  if ($name -eq 'text_sharpness_probe') {
    & $exe (Join-Path $outDir 'sharp') 1.5 > $target 2>&1
  } else {
    & $exe > $target 2>&1
  }
  $lines = (Get-Content $target -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
  Write-Host ("DONE {0} lines={1}" -f $name, $lines)
}
