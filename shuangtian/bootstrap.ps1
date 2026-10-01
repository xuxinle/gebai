# 霜天自举脚本（Windows）：用编译器直接编出 stpm 自身（`build/bin/st.exe`）。
#
# 与 `bootstrap.sh` 的关系：两者是**同一件事的两个平台入口**，都只做"编出 st"这一件事，
# 之后一切构建/测试/依赖管理都走 `st`。
#
# 编译器选择：**g++（MinGW-w64）优先，MSVC 回退**——与框架侧的探测顺序一致
# （`pkg/compiler.cpp` 的 `detect_compiler`）：同一套 GCC 口径横跨三平台，
# 跟进最新 C++ 标准不受 VS 版本牵制；目标机器只有 MinGW 时无需先装 VS。
# MSVC 回退路径：`cl.exe` 不在 PATH，必须先用 `vcvars64.bat` 准备 INCLUDE/LIB/PATH，
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

# —— 编译器选择：g++ 优先（ST_CXX/CXX 显式指定最高），MSVC 回退 ——
$CXX = ''
$CXX_FAMILY = ''
if ($env:ST_CXX -and (Get-Command $env:ST_CXX -ErrorAction SilentlyContinue)) {
  $CXX = (Get-Command $env:ST_CXX).Source
} elseif ($env:CXX -and (Get-Command $env:CXX -ErrorAction SilentlyContinue)) {
  $CXX = (Get-Command $env:CXX).Source
}
if (-not $CXX) {
  $gpp = Get-Command g++ -ErrorAction SilentlyContinue
  if ($gpp) {
    # 版本护栏与框架侧同口径：GCC < 13 缺 C++20 关键项（<format> 等），不选它。
    $gccMajor = 0
    try {
      $text = & $gpp.Source -dumpversion
      if ($text -match '^(\d+)') { $gccMajor = [int]$Matches[1] }
    } catch { $gccMajor = 0 }
    if ($gccMajor -ge 13) {
      $CXX = $gpp.Source
    } else {
      Write-Host "[bootstrap] PATH 中的 g++ 主版本 $gccMajor < 13，跳过（缺 <format> 等 C++20 关键项）" -ForegroundColor Yellow
    }
  }
}
if ($CXX) {
  $CXX_FAMILY = 'gcc'
} else {
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
    Write-Error "未找到 C++ 编译器：安装 MinGW-w64 的 g++（推荐，把 g++ 所在目录加入 PATH），或安装 Visual Studio 的「使用 C++ 的桌面开发」工作负载（回退 MSVC），或用 ST_CXX / ST_VCVARS 指定"
  }
  Write-Host "[bootstrap] 未发现可用的 g++，回退 MSVC 环境: $vcvars" -ForegroundColor Yellow

  # 把 vcvars 的环境导入本进程（子进程继承；并行 runspace 共享进程环境）
  $envLines = & cmd.exe /d /c "chcp 65001 >nul & call `"$vcvars`" >nul & set"
  foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$' -and $matches[1] -notmatch '^=') {
      Set-Item -Path ("env:" + $matches[1]) -Value $matches[2]
    }
  }
  $cl = (Get-Command cl.exe -ErrorAction SilentlyContinue)
  if (-not $cl) { Write-Error "vcvars64.bat 已执行但 cl.exe 仍不可见" }
  $CXX = $cl.Source
  $CXX_FAMILY = 'msvc'
}

Write-Host "[bootstrap] 编译器: $CXX（$CXX_FAMILY）"

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

if ($CXX_FAMILY -eq 'gcc') {
  # ————— GCC（MinGW-w64）分支 —————
  $defines = @('-DST_VERSION="' + $pkgVersion + '"', '-DST_ENABLE_LINT=1',
               '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000')
  $common = @('-Iinclude', '-Ithird_party') + $defines
  $cxxFlags = @('-std=c++20', '-fno-strict-aliasing',
                '-Wall', '-Wextra', '-Wconversion', '-Wshadow', '-Wpedantic',
                '-Wold-style-cast', '-Wnon-virtual-dtor') + $common
  $cFlags = @('-std=gnu11', '-x', 'c') + $common
  $profileFlags = switch ($Profile) {
    'release' { @('-O2', '-DNDEBUG') }
    'quick'   { @('-O0') }
    default   { @('-O1', '-g') }
  }
  $cxxFlags = $cxxFlags + $profileFlags
  $cFlags = $cFlags + $profileFlags

  Write-Host "[bootstrap] 编译 $($sources.Count) 个 C++ 单元 + $($cSources.Count) 个 C 单元（并行 $Jobs，档位 $Profile）"
  $started = Get-Date
  $results = @($sources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
      $src = $_; $objDir = $using:objDir; $flags = $using:cxxFlags; $CXX = $using:CXX
      $name = ($src -replace '[:\\/]', '_')
      $output = & $CXX @flags '-MMD' '-MF' (Join-Path $objDir ($name + '.d')) '-c' $src ('-o' + (Join-Path $objDir ($name + '.o'))) 2>&1
      [pscustomobject]@{ source = $src; code = $LASTEXITCODE; output = ($output | Out-String) }
  }) + @($cSources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
      $src = $_; $objDir = $using:objDir; $flags = $using:cFlags; $CXX = $using:CXX
      $name = ($src -replace '[:\\/]', '_')
      $output = & $CXX @flags '-c' $src ('-o' + (Join-Path $objDir ($name + '.o'))) 2>&1
      [pscustomobject]@{ source = $src; code = $LASTEXITCODE; output = ($output | Out-String) }
  })
  $failed = @($results | Where-Object { $_.code -ne 0 })
  if ($failed.Count -gt 0) {
    foreach ($item in $failed | Select-Object -First 5) {
      Write-Host "--- 失败: $($item.source)" -ForegroundColor Red
      Write-Host $item.output
    }
    Write-Error "$($failed.Count) 个翻译单元编译失败"
  }
  $objects = @($sources + $cSources | ForEach-Object {
      Join-Path $objDir (($_ -replace '[:\\/]', '_') + '.o')
  })
  $exe = Join-Path $binDir 'st.exe'
  $linkOutput = & $CXX $objects '-o' $exe '-lws2_32' '-lgdi32' '-luser32' '-lshell32' '-lwinpthread' `
                               '-static-libgcc' '-static-libstdc++' 2>&1
  if ($LASTEXITCODE -ne 0) { Write-Host ($linkOutput | Out-String); Write-Error "链接失败" }
} else {
  # ————— MSVC 分支 —————
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
}

$elapsed = ((Get-Date) - $started).TotalMilliseconds
Write-Host ("[bootstrap] 完成 → {0}（{1:N0} ms）" -f $exe, $elapsed) -ForegroundColor Green
& $exe version
