# 霜天自举脚本（Windows / MSVC）：用 cl.exe 直接编出 stpm 自身（`build/bin/st.exe`）。
#
# 与 `bootstrap.sh` 的关系：两者是**同一件事的两个平台入口**，都只做"编出 st"这一件事，
# 之后一切构建/测试/依赖管理都走 `st`。现有 `.sh` 需要 bash + g++/clang++；
# 本脚本面向"只有 Visual Studio、没有 MinGW/LLVM"的 Windows 机器（这是最常见的形态）：
# Windows 上 `cl.exe` 不在 PATH 里，必须先用 `vcvars64.bat` 准备 INCLUDE/LIB/PATH，
# 而它在哪里要看 VS 的安装位置——本脚本负责把这套探测做完。
#
# 用法（PowerShell 7+）：pwsh -File bootstrap.ps1 [-Jobs 14] [-Profile release]
param(
  [int]$Jobs = 0,
  [string]$Profile = 'dev',
  [string]$Root = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $MyInvocation.MyCommand.Path }
Set-Location $Root
if ($PSVersionTable.PSVersion.Major -lt 7) {
  Write-Error "需要 PowerShell 7+（本脚本用 ForEach-Object -Parallel 并行编译）；当前 $($PSVersionTable.PSVersion)"
}
if ($Jobs -le 0) { $Jobs = [Math]::Max(2, [int]((Get-CimInstance Win32_Processor).NumberOfLogicalProcessors * 0.6)) }

# —— 定位 MSVC 工具集 ——
# 顺序与框架侧一致（pkg/compiler.cpp）：ST_VCVARS → vswhere（VS 官方查询器）→ 常见目录扫描。
function Find-VcVars {
  if ($env:ST_VCVARS -and (Test-Path $env:ST_VCVARS)) { return $env:ST_VCVARS }
  foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if (-not $root) { continue }
    $vswhere = Join-Path $root 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
      $install = (& $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1)
      if ($install) {
        $candidate = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path $candidate) { return $candidate }
      }
    }
  }
  foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if (-not $root) { continue }
    $found = Get-ChildItem -Path (Join-Path $root 'Microsoft Visual Studio') -Recurse -Depth 4 `
                           -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
  }
  return $null
}

$vcvars = Find-VcVars
if (-not $vcvars) {
  Write-Error "未找到 vcvars64.bat：请安装 Visual Studio 的「使用 C++ 的桌面开发」工作负载，或用 ST_VCVARS 指定"
}
Write-Host "[bootstrap] MSVC 环境: $vcvars"

# 把 vcvars 的环境导入本进程（子进程继承；并行 runspace 共享进程环境）
$envLines = & cmd.exe /d /c "chcp 65001 >nul & call `"$vcvars`" >nul & set"
foreach ($line in $envLines) {
  if ($line -match '^([^=]+)=(.*)$' -and $matches[1] -notmatch '^=') {
    Set-Item -Path ("env:" + $matches[1]) -Value $matches[2]
  }
}

$binDir = Join-Path $Root 'build\bin'
$objDir = Join-Path $Root 'build\obj\bootstrap'
New-Item -ItemType Directory -Force -Path $binDir, $objDir | Out-Null

# 源集合与框架清单一致：core + ext + pkg + stpm 自身（另加第三方 C 源：st 也要链接脚本引擎）
$sources = @()
foreach ($dir in 'src\core', 'src\ext', 'src\pkg', 'tools\stpm') {
  $sources += Get-ChildItem -Path (Join-Path $Root $dir) -Recurse -Filter *.cpp |
              Select-Object -ExpandProperty FullName
}
$cSources = Get-ChildItem -Path (Join-Path $Root 'third_party') -Recurse -Filter *.c -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
# defines 从 st.pkg 读取（消除双写：版本号改清单忘改这里 → 自举产物版本漂移，实测发生过）。
# 兼容缺defines/缺字段的清单：回退到默认值。
$pkg = Get-Content (Join-Path $Root 'st.pkg') -Raw | ConvertFrom-Json
$pkgVersion = if ($pkg.version) { $pkg.version } else { '0.1.0' }
$defines = @(("/DST_VERSION=`"$pkgVersion`""), '/DST_ENABLE_LINT=1')
foreach ($item in @($pkg.defines)) {
  if ($item -is [string] -and $item -match 'ST_') { $defines += "/D$item" }
}
$profileFlags = switch ($Profile) {
  'release' { @('/O2', '/DNDEBUG') }
  'quick'   { @('/Od') }
  'debug'   { @('/Od', '/Z7') }
  # `/Z7` 而非 `/Zi`：并行编译时多个 cl 写同一个 PDB 会报 C1041
  default   { @('/O2', '/Z7') }
}
$common = @('/nologo', '/utf-8', '/I', (Join-Path $Root 'include'), '/I', (Join-Path $Root 'third_party')) + $defines
$cxxFlags = $common + @('/std:c++20', '/EHsc', '/permissive-', '/Zc:__cplusplus', '/Zc:preprocessor', '/bigobj',
                        '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE', '/W4') + $profileFlags
$cFlags   = $common + @('/std:c11', '/bigobj', '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE', '/w') + $profileFlags

Write-Host "[bootstrap] 编译 $($sources.Count) 个 C++ 单元 + $($cSources.Count) 个 C 单元（并行 $Jobs，档位 $Profile）"
$started = Get-Date
$results = @($sources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $src = $_; $objDir = $using:objDir; $flags = $using:cxxFlags
    $name = ($src -replace '[:\\/]', '_')
    $log = Join-Path $objDir ($name + '.log')
    $output = & cl.exe @flags '/c' $src ('/Fo:' + (Join-Path $objDir ($name + '.obj'))) 2>&1
    if ($LASTEXITCODE -ne 0) { Set-Content -Path $log -Value ($output | Out-String) -Encoding utf8 }
    [pscustomobject]@{ source = $src; code = $LASTEXITCODE; output = ($output | Out-String) }
} ) + @($cSources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $src = $_; $objDir = $using:objDir; $flags = $using:cFlags
    $name = ($src -replace '[:\\/]', '_')
    $log = Join-Path $objDir ($name + '.log')
    $output = & cl.exe @flags '/c' $src ('/Fo:' + (Join-Path $objDir ($name + '.obj'))) 2>&1
    if ($LASTEXITCODE -ne 0) { Set-Content -Path $log -Value ($output | Out-String) -Encoding utf8 }
    [pscustomobject]@{ source = $src; code = $LASTEXITCODE; output = ($output | Out-String) }
} )

$failed = @($results | Where-Object { $_.code -ne 0 })
if ($failed.Count -gt 0) {
  foreach ($item in $failed | Select-Object -First 5) {
    Write-Host "--- 失败: $($item.source)" -ForegroundColor Red
    Write-Host $item.output
  }
  Write-Error "$($failed.Count) 个翻译单元编译失败"
}

$objects = $results | ForEach-Object {
  $name = ($_.source -replace '[:\\/]', '_')
  Join-Path $objDir ($name + '.obj')
}
# 响应文件：对象列表可能很长，命令行长度有限（Windows 约 32k）
$rsp = Join-Path $objDir 'link.rsp'
Set-Content -Path $rsp -Value ($objects -join "`n") -Encoding ascii
$exe = Join-Path $binDir 'st.exe'
$link = @('/nologo', ('@' + $rsp), ('/Fe:' + $exe), '/link', '/DEBUG',
'ws2_32.lib', 'user32.lib', 'gdi32.lib', 'shell32.lib')
$linkOutput = & cl.exe @link 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host ($linkOutput | Out-String); Write-Error "链接失败" }

$elapsed = ((Get-Date) - $started).TotalMilliseconds
Write-Host ("[bootstrap] 完成 → {0}（{1:N0} ms）" -f $exe, $elapsed) -ForegroundColor Green
& $exe version
