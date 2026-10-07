# 霜天自举脚本（Windows）：用编译器直接编出 stpm 自身（`build/bin/st.exe`）。
#
# 与 `bootstrap.sh` 的关系：两者是**同一件事的两个平台入口**，都只做"编出 st"这一件事，
# 之后一切构建/测试/依赖管理都走 `st`。
#
# 编译器选择：**g++ 优先，clang++ 次之**——与框架侧的探测顺序一致
# （`pkg/build.cpp` 的 `detect_compiler`）。两者是**同一条口径**：GCC 风格标志
# （`-std=c++20`/`-I`/`-D`/`-Wall`）+ `-MMD` 依赖 + `-l` 链接 + libstdc++ 运行库。
# 因此本脚本只有一套编译/链接命令，不按编译器族分叉。
#
# clang++ 走 GNU 目标时吃 MinGW 的 libstdc++；若它默认指向别的目标（LLVM 官方 Windows
# 包是 MSVC 目标），请用 ST_CXX 指向并自行加 `--target=x86_64-w64-windows-gnu`
# （`st` 侧的等价手段是清单的 `toolchains.<名>.target_triple`）。
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

# —— 编译器选择：ST_CXX/CXX 显式指定最高，其次 g++（主版本 ≥ 13），再次 clang++/c++ ——
# 版本护栏与框架侧同口径：GCC < 13 缺 C++20 关键项（<format> 等），不选它。
$CXX = ''
$CXX_FAMILY = 'gcc'
if ($env:ST_CXX -and (Get-Command $env:ST_CXX -ErrorAction SilentlyContinue)) {
  $CXX = (Get-Command $env:ST_CXX).Source
} elseif ($env:CXX -and (Get-Command $env:CXX -ErrorAction SilentlyContinue)) {
  $CXX = (Get-Command $env:CXX).Source
}
if (-not $CXX) {
  $gpp = Get-Command g++ -ErrorAction SilentlyContinue
  if ($gpp) {
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
if (-not $CXX) {
  foreach ($name in @('clang++', 'c++')) {
    $found = Get-Command $name -ErrorAction SilentlyContinue
    if ($found) { $CXX = $found.Source; break }
  }
}
if (-not $CXX) {
  Write-Error "未找到 C++ 编译器：安装 MinGW-w64 的 g++，或 LLVM 的 clang++（把所在目录加入 PATH，或用 ST_CXX / CXX 显式指定）"
}
$base = [System.IO.Path]::GetFileNameWithoutExtension($CXX).ToLowerInvariant()
if ($base -like '*clang*') { $CXX_FAMILY = 'clang' }

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
# 第三方 C 源见下方（清单驱动，需先读 st.pkg）——此处不再全量扫描 third_party。
# 清单是**唯一权威**：版本号、宏、包含路径、第三方 C 源、C 源专用标志全部从 st.pkg 读。
# 历史教训：这些值曾在本脚本里第二遍手写，于是「清单改了脚本没改」当场把自举打崩
# （SQLite 内置那次：c_flags 的 `-include st_sqlite3_config.h` 与 include 目录
#  `third_party/sqlite` 只进了清单，bootstrap 仍按硬编码的 `-Iinclude -Ithird_party`
#  编译 `third_party/**/*.c`，sqlite3.c 找不到 sqlite3.h 直接 fatal error）。
# 口径同 `src/pkg/build.cpp`：include_dirs + 清单宏 + c_flags 是 C 源的完整标志前缀，
# `-x c` 由本脚本补（清单里不写，因为 st 那边是**按语言分派**而非拼在标志里）。
$pkg = Get-Content (Join-Path $Root 'st.pkg') -Raw | ConvertFrom-Json
$pkgVersion = if ($pkg.version) { $pkg.version } else { '0.1.0' }
$pkgIncludes = if ($pkg.include_dirs) { @($pkg.include_dirs) } else { @('include', 'third_party') }
$pkgDefines = if ($pkg.defines) { @($pkg.defines) } else { @('ST_VERSION="' + $pkgVersion + '"', 'ST_ENABLE_LINT=1') }
$pkgCFlags = if ($pkg.c_flags) { @($pkg.c_flags) } else { @('-std=gnu11') }
# `third_party_sources` 是 glob 清单（`third_party/sqlite/*.c`）：只编清单点名的第三方 C 源。
# 旧脚本对所有 `third_party/**/*.c` 全编——third_party 一旦放进「有 C 源但不是编译单元」
# 的东西（参考实现、上游自测），就会替它们挑错甚至编出重复符号。
$thirdPartySources = @()
if ($pkg.third_party_sources) {
  foreach ($pattern in $pkg.third_party_sources) {
    $dir = Split-Path -Parent $pattern
    $filter = Split-Path -Leaf $pattern
    $thirdPartySources += Get-ChildItem -Path (Join-Path $Root $dir) -Filter $filter -ErrorAction SilentlyContinue |
                          Select-Object -ExpandProperty FullName
  }
} else {
  $thirdPartySources = @(Get-ChildItem -Path (Join-Path $Root 'third_party') -Recurse -Filter *.c -ErrorAction SilentlyContinue |
                         Select-Object -ExpandProperty FullName)
}

# —— 编译：一套 GCC 风格命令（g++ 与 clang++ 同口径）——
$platformDefines = @('-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000')
$defines = @($pkgDefines | ForEach-Object { "-D$_" }) + $platformDefines
$includeFlags = @($pkgIncludes | ForEach-Object { "-I$_" })
$common = $includeFlags + $defines
$cxxFlags = @('-std=c++20', '-fno-strict-aliasing',
              '-Wall', '-Wextra', '-Wconversion', '-Wshadow', '-Wpedantic',
              '-Wold-style-cast', '-Wnon-virtual-dtor') + $common
# C 源标志与构建驱动同源：清单的 c_flags（含 SQLite 的 `-include`）+ 宏 + 包含路径。
# `-include st_sqlite3_config.h` 要能被找到，靠的正是上面 `third_party/sqlite` 那条 `-I`。
$cFlags = @($pkgCFlags) + $defines + $includeFlags + @('-x', 'c')
$profileFlags = switch ($Profile) {
  'release' { @('-O2', '-DNDEBUG') }
  'quick'   { @('-O0') }
  default   { @('-O1', '-g') }
}
$cxxFlags = $cxxFlags + $profileFlags
$cFlags = $cFlags + $profileFlags

Write-Host "[bootstrap] 编译 $($sources.Count) 个 C++ 单元 + $($thirdPartySources.Count) 个 C 单元（并行 $Jobs，档位 $Profile）"
$started = Get-Date
$results = @($sources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $src = $_; $objDir = $using:objDir; $flags = $using:cxxFlags; $CXX = $using:CXX
    $name = ($src -replace '[:\\/]', '_')
    $output = & $CXX @flags '-MMD' '-MF' (Join-Path $objDir ($name + '.d')) '-c' $src ('-o' + (Join-Path $objDir ($name + '.o'))) 2>&1
    [pscustomobject]@{ source = $src; code = $LASTEXITCODE; output = ($output | Out-String) }
}) + @($thirdPartySources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
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
$objects = @($sources + $thirdPartySources | ForEach-Object {
    Join-Path $objDir (($_ -replace '[:\\/]', '_') + '.o')
})
$exe = Join-Path $binDir 'st.exe'
$linkOutput = & $CXX $objects '-o' $exe '-lws2_32' '-lgdi32' '-luser32' '-lshell32' '-lwinpthread' `
                             '-static-libgcc' '-static-libstdc++' 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host ($linkOutput | Out-String); Write-Error "链接失败" }

$elapsed = ((Get-Date) - $started).TotalMilliseconds
Write-Host ("[bootstrap] 完成 → {0}（{1:N0} ms）" -f $exe, $elapsed) -ForegroundColor Green
& $exe version
