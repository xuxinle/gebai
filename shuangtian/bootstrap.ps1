# 霜天自举脚本（Windows）：用编译器直接编出 stpm 自身（`build/bin/st.exe`）。
#
# 与 `bootstrap.sh` 的关系：两者是**同一件事的两个平台入口**，都只做"编出 st"这一件事，
# 之后一切构建/测试/依赖管理都走 `st`。
#
# 编译器选择：**g++ 优先，clang++ 次之**——与框架侧的探测顺序一致
# （`pkg/build.cpp` 的 `detect_compiler`）。两者是**同一条口径**：GCC 风格标志
# （`-std=c++XX`/`-I`/`-D`/`-Wall`）+ `-MMD` 依赖 + `-l` 链接 + libstdc++ 运行库。
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
# ## 自举的对象目录必须与 `st build` **完全相同**（2026-10-26 实测，代价 45 单元重编）
#
# 这两套曾各写一份：bootstrap 用 `build/obj/bootstrap` + `src_pkg_semver.cpp.o`，
# 而 `st build` 用 `build/dev/obj` + `C__Users_…_src_pkg_semver.cpp.o`
#（路径全量归一化后的名字）。后果是**两份缓存互不命中**：
# 每次 bootstrap 重建 `st` 之后，第一次 `st build` 都报“重编 45 / 命中 8”，
# 白烧 31.66s —— 而它本可以只重编真正改过的那一两个单元。
#
# 对齐口径：目录 `build/<档位>/obj`（档位取自 -Profile），对象名 = `-replace '[:\\/]', '_'`。
# 注意 `st` 的 `sanitize_name` 只换 `/` `\` `:`，**不转小写**也不做其他归一，
# 而相对路径已由 `Expand-Pattern` 归一成 `src/pkg/x.cpp` 形式——所以两边名字会一致。
$objDir = Join-Path $Root "build\$Profile\obj"
# 与 `st build` 同口径的 PCH 目录（否则 bootstrap 不用 PCH，每次多付 ~4.3s 头解析）
$pchDir = Join-Path $Root "build\$Profile\pch"
New-Item -ItemType Directory -Force -Path $binDir, $objDir, $pchDir | Out-Null

# —— 单元集：**从 st.pkg 推导**，与 `st build st` 同一份声明 ——
#
# 口径（与 `src/pkg/build.cpp` 同序，改一处必须同步另一处）：
#
#   units = 展开(顶层 sources) − 匹配(targets.st.exclude_sources) + 展开(targets.st.sources)
#
# **排除先于并入目标源**：所以"落在被排除目录里、但工具确实需要"的单元
# （如 `src/ui/window_control.cpp`——shell 后端的窗口边缘命中契约）写在 `targets.st.sources` 即可。
#
# 为什么必须推导：曾经按目录写死（`src\core`、`src\ext`、`src\pkg`、`tools\stpm`），
# 与清单脱节后的症状是**链接期一串 `undefined reference to st::...`**——报错指向源码，
# 而真因是"该编的单元没编"（新增 `src/ext/video.cpp` 引 `codec/raster` 那次）。
# 清单是唯一权威：版本号、宏、包含路径、第三方 C 源、C 源专用标志与**单元集**全部从这里读。
function Expand-Pattern([string]$Base, [string]$Pattern) {
  # `*` 不跨 `/`（与 `fs::match_glob` 同口径：逐段匹配）
  $items = Get-ChildItem -Path (Join-Path $Base $Pattern) -File -ErrorAction SilentlyContinue
  foreach ($item in $items) {
    $item.FullName.Substring($Base.Length).TrimStart('\', '/') -replace '\\', '/'
  }
}
function Test-Segment([string]$Pat, [string]$Seg) {
  $regex = '^' + ([regex]::Escape($Pat) -replace '\\\*', '[^/]*' -replace '\\\?', '[^/]') + '$'
  return $Seg -match $regex
}
function Test-Segments($Pat, [int]$Pi, $Seg, [int]$Si) {
  if ($Pi -eq $Pat.Count) { return $Si -eq $Seg.Count }
  if ($Pat[$Pi] -eq '**') {
    for ($skip = $Si; $skip -le $Seg.Count; $skip++) {
      if (Test-Segments $Pat ($Pi + 1) $Seg $skip) { return $true }
    }
    return $false
  }
  if ($Si -eq $Seg.Count) { return $false }
  if (-not (Test-Segment $Pat[$Pi] $Seg[$Si])) { return $false }
  return Test-Segments $Pat ($Pi + 1) $Seg ($Si + 1)
}
function Test-GlobMatch([string]$Pattern, [string]$Path) {
  $pat = @($Pattern -split '/' | Where-Object { $_ -ne '' })
  $seg = @($Path -split '/' | Where-Object { $_ -ne '' -and $_ -ne '.' })
  return Test-Segments $pat 0 $seg 0
}

$pkg = Get-Content (Join-Path $Root 'st.pkg') -Raw | ConvertFrom-Json
$pkgVersion = if ($pkg.version) { $pkg.version } else { '0.1.0' }
# 语言标准：**单一真源在清单**（`cxx_standard`），与 `src/pkg/build.cpp` 同口径。
# 曾几何时这里与构建驱动各写一份 `-std=c++20`，升标准时两处不同步就会出现
# “st build 用新标准、自举出的 st 用旧标准”这种只在细节上错位的产物，且两边各自都能编过。
$pkgStandard = if ($pkg.cxx_standard) { [int]$pkg.cxx_standard } else { 23 }
$stdFlag = "-std=c++$pkgStandard"
$pkgIncludes = if ($pkg.include_dirs) { @($pkg.include_dirs) } else { @('include', 'third_party') }
$pkgDefines = if ($pkg.defines) { @($pkg.defines) } else { @('ST_VERSION="' + $pkgVersion + '"', 'ST_ENABLE_LINT=1') }
$pkgCFlags = if ($pkg.c_flags) { @($pkg.c_flags) } else { @('-std=gnu11') }
# 第三方 C 源见下方（清单驱动，需先读 st.pkg）——此处不再全量扫描 third_party。
# 清单是**唯一权威**：版本号、宏、包含路径、第三方 C 源、C 源专用标志全部从 st.pkg 读。
# 历史教训：这些值曾在本脚本里第二遍手写，于是「清单改了脚本没改」当场把自举打崩
# （SQLite 内置那次：c_flags 的 `-include st_sqlite3_config.h` 与 include 目录
#  `third_party/sqlite` 只进了清单，bootstrap 仍按硬编码的 `-Iinclude -Ithird_party`
#  编译 `third_party/**/*.c`，sqlite3.c 找不到 sqlite3.h 直接 fatal error）。
# 口径同 `src/pkg/build.cpp`：include_dirs + 清单宏 + c_flags 是 C 源的完整标志前缀，
# `-x c` 由本脚本补（清单里不写，因为 st 那边是**按语言分派**而非拼在标志里）。
# `third_party_sources` 是 glob 清单（`third_party/sqlite/*.c`）：只编清单点名的第三方 C 源。
# 旧脚本对所有 `third_party/**/*.c` 全编——third_party 一旦放进「有 C 源但不是编译单元」
# 的东西（参考实现、上游自测），就会替它们挑错甚至编出重复符号。
$thirdPartySources = @()
foreach ($pattern in @($pkg.third_party_sources)) {
  $thirdPartySources += @(Get-ChildItem -Path (Join-Path $Root $pattern) -File -ErrorAction SilentlyContinue |
                          Select-Object -ExpandProperty FullName)
}

$targetSpec = if ($pkg.targets) { $pkg.targets.st } else { $null }
if ($null -eq $targetSpec) {
  Write-Error "st.pkg 里没有 targets.st（自举无法推导单元集）"
}
$excludePatterns = if ($targetSpec.exclude_sources) { @($targetSpec.exclude_sources) } else { @() }
$includePatterns = if ($targetSpec.include_sources) { @($targetSpec.include_sources) } else { @() }
$extraPatterns = if ($targetSpec.sources) { @($targetSpec.sources) } else { @() }

$units = @()
foreach ($pattern in @($pkg.sources)) { $units += @(Expand-Pattern $Root $pattern) }
# **白名单优先**（与 `src/pkg/build.cpp` 同口径）：`include_sources` 非空时，
# 库源只保留与它匹配的那部分，`exclude_sources` 不再参与。
#
# 为何两边都必须认：清单新增字段时只改构建驱动、不改自举脚本，症状是
# **旧 st 无法重建自己**——自举会把整个 `src/**` 编进来（包括需要构建期生成的
# `battery/embed.hpp` 的 `src/ui`/`src/app`），报 `fatal error: No such file`；
# 而唯一能修它的办法就是自举（旧 st 又不认识新字段）⇒ 死锁。
# 实测：2026-10-26 从 `exclude_sources` 换成 `include_sources` 时撞上（
# 先报的是 `duplicate symbol`：旧 st 按黑名单口径编了全库，
# 目标源里那条 `window_control.cpp` 又编了一次）。
if ($includePatterns.Count -gt 0) {
  $before = $units.Count
  $units = @($units | Where-Object { $unit = $_; @($includePatterns | Where-Object { Test-GlobMatch $_ $unit }).Count -gt 0 })
  Write-Host "[bootstrap] 白名单: 库源 $before → $($units.Count) 个（include_sources $($includePatterns.Count) 条）"
} else {
foreach ($pattern in $excludePatterns) {
  $before = $units.Count
  $units = @($units | Where-Object { -not (Test-GlobMatch $pattern $_) })
  if ($units.Count -eq $before) {
    Write-Host "[bootstrap] 排除模式未命中任何源文件: $pattern" -ForegroundColor Yellow
  }
}
}
foreach ($pattern in $extraPatterns) { $units += @(Expand-Pattern $Root $pattern) }
$relUnits = @($units | Sort-Object -Unique)
if ($relUnits.Count -eq 0) { Write-Error "单元集为空（st.pkg 的 sources/targets.st 解析异常）" }
$sources = @($relUnits | ForEach-Object { Join-Path $Root $_ })

# —— 编译：一套 GCC 风格命令（g++ 与 clang++ 同口径）——
$platformDefines = @('-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000')
$defines = @($pkgDefines | ForEach-Object { "-D$_" }) + $platformDefines
$includeFlags = @($pkgIncludes | ForEach-Object { "-I$_" })
$common = $includeFlags + $defines
$cxxFlags = @($stdFlag, '-fno-strict-aliasing',
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
# ## 对象名规则必须与 `st build` 逐字一致
#
# `st` 侧：`unit.source` 是**绝对路径**，`sanitize_name()` 把 `/ \\ :` 都换成 `_`，
# 所以 `C:/…/src/pkg/semver.cpp` → `C__Users_…_src_pkg_semver.cpp.o`。
# 这里必须用同一份算法（含绝对路径！）：用相对路径会得到 `src_pkg_semver.cpp.o`，
# 两套名字就又是两份缓存。
function Get-UnitObjectName([string]$AbsoluteSource) {
  return ($AbsoluteSource -replace '[:\\/]', '_') + '.o'
}
# 预计算（避免在并行区里重复拼字符串）
$unitPlan = @($relUnits | ForEach-Object {
  $abs = Join-Path $Root $_
  [pscustomobject]@{ rel = $_; abs = $abs; obj = (Get-UnitObjectName $abs) }
})
$cPlan = @($thirdPartySources | ForEach-Object {
  [pscustomobject]@{ abs = $_; obj = (Get-UnitObjectName $_) }
})

# ## PCH：与 `st build` 共用同一个 `.gch`
#
# 不自建 PCH 的话，每个单元要重新解析全部标准库头——实测单单元 15.1s（带 PCH）
# vs 20s+（不带），43 单元累计差一个量级。而 `st build` 已经把 PCH 建在
# `build/<档位>/pch/prefix.hpp.gch`，这里直接复用。
#
# 为何不自建：两边标志必须**完全一致**才能共用（`target_triple`/宏/优化档），
# 而 bootstrap 的标志集是手写的一份（与 `build_language_flags` 平行）——
# 自建很容易造出一份标志略异的 `.gch`，后果是编译器**静默忽略**（不报错、只是变慢）。
# 复用构建器产出的那一份，则两边天然同源。
$pchHeader = Join-Path $pchDir 'prefix.hpp'
$pchGch = "$pchHeader.gch"
if (-not (Test-Path $pchGch)) {
  Write-Host "[bootstrap] 无可用 PCH（$pchGch）——本次不带 PCH 编译（慢一点）" -ForegroundColor Yellow
  $pchArgs = @()
} else {
  Write-Host "[bootstrap] 复用 PCH：$pchGch"
  $pchArgs = @('-include', $pchHeader, "-I$pchDir")
}
$cxxFlags = $cxxFlags + $pchArgs
$results = @($unitPlan | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $plan = $_; $objDir = $using:objDir; $flags = $using:cxxFlags; $CXX = $using:CXX
    # 对象名与依赖清单名走**同一份预计算结果**（`Get-UnitObjectName`），
    # 避免“编到这里用绝对路径名、链接那里用相对路径名”这种两套名字的老问题。
    $depFile = $plan.obj + '.d'
    $object = $plan.obj
    $output = & $CXX @flags '-MMD' '-MF' (Join-Path $objDir $depFile) '-c' $plan.abs ('-o' + (Join-Path $objDir $object)) 2>&1
    [pscustomobject]@{ source = $plan.abs; code = $LASTEXITCODE; output = ($output | Out-String) }
}) + @($cPlan | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $plan = $_; $objDir = $using:objDir; $flags = $using:cFlags; $CXX = $using:CXX
    $output = & $CXX @flags '-c' $plan.abs ('-o' + (Join-Path $objDir $plan.obj)) 2>&1
    [pscustomobject]@{ source = $plan.abs; code = $LASTEXITCODE; output = ($output | Out-String) }
})
$failed = @($results | Where-Object { $_.code -ne 0 })
if ($failed.Count -gt 0) {
  foreach ($item in $failed | Select-Object -First 5) {
    Write-Host "--- 失败: $($item.source)" -ForegroundColor Red
    Write-Host $item.output
  }
  Write-Error "$($failed.Count) 个翻译单元编译失败"
}
$objects = @($unitPlan | ForEach-Object { Join-Path $objDir $_.obj }) +
          @($cPlan | ForEach-Object { Join-Path $objDir $_.obj })
$exe = Join-Path $binDir 'st.exe'
# 链接库：读仓库根的 link-libs.txt（单一数据源；`st.pkg`/`build.cpp`/两个自举脚本共用）。
# 历史教训（曾在本文件手写清单，漏过三次：dbghelp×2、secur32×1，每次都是
# 「报错指向源码的 undefined reference」，排查方向被带偏）——现收敛到一份文件。
function Read-LinkLibs {
  param([string]$Section)
  $path = Join-Path $Root 'link-libs.txt'
  if (-not (Test-Path $path)) { Write-Error "缺 $path（链接库单一数据源）"; exit 1 }
  $libs = @(); $inSection = $false
  foreach ($line in Get-Content $path -Encoding UTF8) {
    $trimmed = $line.Trim()
    if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
    if ($trimmed -match '^(windows|posix):\s*$') { $inSection = ($Matches[1] -eq $Section); continue }
    if ($inSection) { $libs += $trimmed }
  }
  return $libs
}
$linkLibArgs = @(Read-LinkLibs 'windows' | ForEach-Object { "-l$_" })
$linkOutput = & $CXX $objects '-o' $exe @linkLibArgs '-static-libgcc' '-static-libstdc++' 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host ($linkOutput | Out-String); Write-Error "链接失败" }

$elapsed = ((Get-Date) - $started).TotalMilliseconds

# **链接后分离调试信息**（与 `st build` 的 `separate_debug_info` 同口径，也与
# `bootstrap.sh` 同一步）：dev 档下 `.debug_*` 占产物 89%（实测 st.exe 69.7MB → 4.57MB）。
# ⚠ 失败**不阻断自举**：产物已经能跑，只是胖了点——为“多个文件”把自举打回去不划算。
if (-not $env:ST_NO_STRIP) {
  $objcopy = $null
  $cand = Join-Path (Split-Path $CXX) 'objcopy.exe'
  if (Test-Path $cand) { $objcopy = $cand }
  if (-not $objcopy) {
    $found = Get-Command 'objcopy' -ErrorAction SilentlyContinue
    if ($found) { $objcopy = $found.Source }
  }
  if ($objcopy) {
    $debugFile = "$exe.debug"
    & $objcopy '--only-keep-debug' $exe $debugFile 2>&1 | Out-Null
    & $objcopy '--strip-debug' $exe 2>&1 | Out-Null
    & $objcopy "--add-gnu-debuglink=$debugFile" $exe 2>&1 | Out-Null
    if (Test-Path $debugFile) {
      $mainMb = [math]::Round((Get-Item $exe).Length / 1MB, 2)
      $dbgMb = [math]::Round((Get-Item $debugFile).Length / 1MB, 2)
      Write-Host "[bootstrap] 已分离调试信息 → $exe（$mainMb MB）+ st.exe.debug（$dbgMb MB）" -ForegroundColor Green
    } else {
      Write-Warning "[bootstrap] 分离调试信息失败（产物仍可用，仅体积偏大）"
    }
  }
}

Write-Host ("[bootstrap] 完成 → {0}（{1:N0} ms）" -f $exe, $elapsed) -ForegroundColor Green
& $exe version
