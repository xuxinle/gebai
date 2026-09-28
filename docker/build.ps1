# 歌白（GEBAI Agent）· 服务模式镜像构建脚本（Windows PowerShell）
#
# 与 docker/build.sh 同参数、同一 Dockerfile。Docker Desktop 已就绪时直接可用。
# Dockerfile 不依赖 BuildKit；但构建器本身得可用（Docker 23+ 已废弃经典构建器、Docker 29 已移除，
# 未装 buildx 的老 Docker 需先装：Ubuntu 下 apt-get install -y docker-buildx）。
#
# 用法：
#   pwsh -File docker/build.ps1                                  # 构建 gebai:<package.json 版本>
#   pwsh -File docker/build.ps1 -Tag gebai:dev -Smoke            # 指定标签 + 冒烟自检
#   pwsh -File docker/build.ps1 -WithBrowser -NoCv               # 装浏览器、不内嵌本地 CV
#   pwsh -File docker/build.ps1 -Profile minimal                 # 预置裁剪档案（docker/profiles/*.json）
#   pwsh -File docker/build.ps1 -Profile code -Set assets.d2=1   # 档案 + 字段级覆盖（CLI 优先）
#   pwsh -File docker/build.ps1 -PrintPlan                       # 只打印裁剪计划，不构建
#   pwsh -File docker/build.ps1 -BaseImage registry.internal/ubuntu:24.04 -AptMirror http://mirror.internal/ubuntu
#   pwsh -File docker/build.ps1 -User acme -Uid 2001 -DataDir /srv/gebai -Port 8080 -Tz Asia/Shanghai
#   pwsh -File docker/build.ps1 -Label owner=acme -ExtraPackages vim,less -NoHealthcheck
#   pwsh -File docker/build.ps1 -Push -Tag registry.example.com/gebai:0.1.0
#
# 定制体系见 docker/README.md：一份 JSON 档案描述能力层（子Agent/工具/资产/vendor）、系统层（apt 包组）
# 与镜像本体（基础镜像/用户/数据根/端口/时区/标签/额外包/apt・npm 源/健康检查）；-Set 做字段级覆盖。
[CmdletBinding()]
param(
  [string]$Tag = "",
  [switch]$WithBrowser,
  [switch]$NoCv,
  [string]$CvModelBase = "",
  [string]$Target = "",
  [string]$Platform = "",
  [string]$Profile = "",
  [string[]]$Set = @(),
  [switch]$PrintPlan,
  [switch]$NoCache,
  [switch]$Push,
  [switch]$Smoke,
  # —— 镜像本体定制（等价于 -Set image.<字段>=值）——
  [string]$BaseImage = "",
  [string]$BunImage = "",
  [string]$User = "",
  [int]$Uid = 0,
  [int]$Gid = 0,
  [string]$DataDir = "",
  [int]$Port = 0,
  [string]$Mode = "",
  [string]$ListenHost = "",
  [string]$Tz = "",
  [string[]]$Label = @(),
  [string]$ExtraPackages = "",
  [string]$AptMirror = "",
  [string]$NpmRegistry = "",
  [string]$BuildProxy = "",
  [switch]$RunAsRoot,
  [switch]$NoHealthcheck
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
  if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw "未找到 docker 命令" }

  # 镜像本体开关 → 与 -Set 同一批字段级覆盖（计划器侧统一校验，非法值直接报错）
  if ($BaseImage) { $Set += "image.base=$BaseImage" }
  if ($BunImage) { $Set += "image.bun_image=$BunImage" }
  if ($User) { $Set += "image.user=$User" }
  if ($Uid -gt 0) { $Set += "image.uid=$Uid" }
  if ($Gid -gt 0) { $Set += "image.gid=$Gid" }
  if ($DataDir) { $Set += "image.data_dir=$DataDir" }
  if ($Port -gt 0) { $Set += "image.port=$Port" }
  if ($Mode) { $Set += "image.mode=$Mode" }
  if ($ListenHost) { $Set += "image.host=$ListenHost" }
  if ($Tz) { $Set += "image.tz=$Tz" }
  if ($ExtraPackages) { $Set += "image.extra_packages=$ExtraPackages" }
  if ($AptMirror) { $Set += "image.apt_mirror=$AptMirror" }
  if ($NpmRegistry) { $Set += "image.npm_registry=$NpmRegistry" }
  if ($BuildProxy) { $Set += "image.proxy=$BuildProxy" }
  if ($RunAsRoot) { $Set += "image.run_as_root=1" }
  if ($NoHealthcheck) { $Set += "image.healthcheck=false" }
  # 多个 -Label 累积成一个 image.labels 项（逗号分隔）——多次覆盖同一字段是整体替换
  if ($Label.Count -gt 0) { $Set += "image.labels=$($Label -join ',')" }

  # ── 档案解析：预置名 → docker/profiles/{名}.json；含分隔符或 .json 结尾 → 按路径 ──
  $profileFile = ""
  $profileName = ""
  if ($Profile) {
    if (Test-Path -PathType Leaf $Profile) { $profileFile = $Profile }
    elseif (Test-Path -PathType Leaf "docker/profiles/$Profile.json") { $profileFile = "docker/profiles/$Profile.json" }
    else {
      $presets = (Get-ChildItem docker/profiles/*.json -ErrorAction SilentlyContinue | ForEach-Object { $_.BaseName }) -join " "
      throw "裁剪档案不存在：$Profile（预置档案：$presets）"
    }
    $profileName = [IO.Path]::GetFileNameWithoutExtension($profileFile)
  }

  $withCv = if ($NoCv) { "0" } else { "1" }
  $withBrowser = if ($WithBrowser) { "1" } else { "0" }

  $planArgs = @()
  if ($profileFile) { $planArgs += @("--profile", $profileFile) }
  foreach ($s in $Set) { $planArgs += @("--set", $s) }
  if ($withCv -eq "0") { $planArgs += @("--set", "assets.cv=0") }
  if ($withBrowser -eq "1") { $planArgs += @("--set", "assets.browser=1", "--set", "system.chromium=1") }

  # ── -PrintPlan：只出计划与报告（需 bun 解析档案与合并覆盖），不构建 ──
  if ($PrintPlan) {
    if (-not (Get-Command bun -ErrorAction SilentlyContinue)) { throw "-PrintPlan 需要本机有 bun（解析档案与合并覆盖）" }
    & bun run scripts/build-image-plan.ts @planArgs --print-plan
    if ($LASTEXITCODE -ne 0) { throw "计划生成失败（退出码 $LASTEXITCODE）" }
    return
  }

  if (-not $Tag) {
    $version = (Get-Content package.json -Raw | ConvertFrom-Json).version
    $Tag = "gebai:$version"
  }

  # ── 指令级定制：宿主侧跑计划器算出（Docker 指令无法在构建过程中条件化）──
  # 基础镜像/运行用户/数据根/端口/时区/标签/健康检查都要进 FROM・USER・ENV・VOLUME・EXPOSE・LABEL・
  # HEALTHCHECK，必须在 docker build 之前定下；RUN 级裁剪在镜像构建阶段用同一份档案再算一次，两边同源。
  $extraArgs = @()
  $targetStage = ""
  $smokePort = 3000
  $smokeDataDir = "/data"
  if (Get-Command bun -ErrorAction SilentlyContinue) {
    $reportFile = [IO.Path]::GetTempFileName()
    try {
      $emitArgs = & bun run scripts/build-image-plan.ts @planArgs --emit-args 2>$reportFile
      if ($LASTEXITCODE -ne 0) {
        Get-Content $reportFile | ForEach-Object { [Console]::Error.WriteLine($_) }
        throw "裁剪/定制档案校验失败（详见上方报告）"
      }
      Get-Content $reportFile | ForEach-Object { [Console]::Error.WriteLine($_) }
      foreach ($line in $emitArgs) {
        if (-not $line) { continue }
        $eq = $line.IndexOf("=")
        if ($eq -lt 1) { continue }
        $key = $line.Substring(0, $eq)
        $val = $line.Substring($eq + 1)
        # 空值行不传：让 Dockerfile 的 ARG 默认值生效（如 IMAGE_TZ 缺省 UTC）
        if (-not $val) { continue }
        switch ($key) {
          "IMAGE_PORT" { $smokePort = $val }
          "IMAGE_DATA_DIR" { $smokeDataDir = $val }
          "HEALTHCHECK_ENABLED" { if ($val -eq "0") { $targetStage = "runtime-nohealthcheck" } }
          # 镜像标签：Docker 的 LABEL 指令不支持变量展开追加，改用 docker build --label
          "IMAGE_LABEL" { if ($val) { $extraArgs += @("--label", $val) } }
          # 计划里的构建代理同时驱动 Docker 预定义代理 build-arg（apt/npm/模型下载）
          "BUILD_PROXY" { if ($val) { $extraArgs += @("--build-arg", "HTTP_PROXY=$val", "--build-arg", "HTTPS_PROXY=$val") } }
          default { $extraArgs += @("--build-arg", $line) }
        }
      }
    } finally {
      Remove-Item $reportFile -ErrorAction SilentlyContinue
    }
  } else {
    Write-Warning "未找到 bun —— 跳过镜像本体的指令级定制（基础镜像/用户/数据根/端口/时区/标签/健康检查）与档案预校验；能力与系统层裁剪仍在镜像构建阶段由同一份档案生效。"
  }

  $buildArgs = @(
    "--build-arg", "WITH_CV=$withCv",
    "--build-arg", "WITH_BROWSER=$withBrowser"
  )
  if ($profileFile) {
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Resolve-Path $profileFile)))
    $buildArgs += @("--build-arg", "BUILD_PROFILE_B64=$b64", "--build-arg", "BUILD_PROFILE_NAME=$profileName")
  }
  # BUILD_SET 走空格分隔（镜像内逐词解析），因此只传 RUN 级裁剪需要的覆盖：image.labels 的值可能含
  # 空格（会破坏分隔）且只用于指令级（宿主侧已算好），故不转投。
  $runSets = @($Set | Where-Object { -not $_.StartsWith("image.labels=") })
  if ($runSets.Count -gt 0) { $buildArgs += @("--build-arg", "BUILD_SET=$($runSets -join ' ')") }
  if ($extraArgs.Count -gt 0) { $buildArgs += $extraArgs }
  if ($targetStage) { $buildArgs += @("--target", $targetStage) }
  if ($Target) { $buildArgs += @("--build-arg", "BUN_TARGET=$Target") }
  if ($CvModelBase) { $buildArgs += @("--build-arg", "CV_MODEL_BASE=$CvModelBase") }
  if ($Platform) { $buildArgs += @("--platform", $Platform) }
  if ($NoCache) { $buildArgs += "--no-cache" }

  # buildx 优先（同时支持 --platform/--push）；未装则退回 docker build
  $builder = @("buildx", "build")
  docker buildx version *> $null
  if ($LASTEXITCODE -ne 0) {
    $builder = @("build")
    Write-Host "提示：未检测到 buildx。若本机 Docker 已移除经典构建器，需先安装（Ubuntu: apt-get install -y docker-buildx）"
  } elseif (-not $Push) {
    $buildArgs += "--load"      # buildx 默认只构建不入库
  }
  if ($Push) { $buildArgs += "--push" }

  Write-Host "==> 构建镜像 $Tag"
  Write-Host "    上下文：$repoRoot（.dockerignore 已排除 node_modules/dist/resources/infer/vendor 等）"
  Write-Host "    参数：WITH_CV=$withCv WITH_BROWSER=$withBrowser"
  Write-Host "    裁剪：档案=$(if ($profileFile) { $profileFile } else { '全量（未指定）' })$(if ($Set.Count -gt 0) { " 覆盖=$($Set -join ' ')" })"
  if ($targetStage) { Write-Host "    阶段：$targetStage（无 HEALTHCHECK）" }

  & docker @builder -f Dockerfile -t $Tag @buildArgs .
  if ($LASTEXITCODE -ne 0) { throw "docker build 失败（退出码 $LASTEXITCODE）" }

  Write-Host "==> 完成：$Tag"
  docker images --filter "reference=$Tag" --format "    大小：{{.Size}}（{{.Repository}}:{{.Tag}}）"

  if ($Smoke) {
    $name = "gebai-smoke-$PID"
    $vol = "gebai-smoke-$PID"
    try {
      Write-Host "==> 冒烟自检：起容器（卷 $vol → $smokeDataDir）"
      docker volume create $vol | Out-Null
      docker run -d --name $name -e GEBAI_SCHEDULER=off -v "${vol}:${smokeDataDir}" $Tag | Out-Null

      Write-Host -NoNewline "    健康检查 /api/health … "
      $ok = $false
      foreach ($i in 1..30) {
        $out = docker exec $name curl -fsS "http://127.0.0.1:$smokePort/api/health" 2>$null
        if ($LASTEXITCODE -eq 0 -and $out) { Write-Host "OK  $out"; $ok = $true; break }
        Start-Sleep -Seconds 1
      }
      if (-not $ok) {
        Write-Host "失败"
        Write-Host "----- 容器日志（末尾 60 行）-----"
        docker logs --tail 60 $name 2>&1
        Write-Host "-------------------------------"
        throw "冒烟自检失败"
      }

      $code = docker exec $name curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$smokePort/"
      Write-Host "    Web UI / … HTTP $code"

      Write-Host -NoNewline "    容器内自查（用户/数据根/端口）… "
      docker exec $name sh -c 'echo "user=$(id -un) uid=$(id -u) gid=$(id -g) home=$HOME data=$GEBAI_HOME port=$GEBAI_PORT tz=$TZ"'
      if ($LASTEXITCODE -ne 0) { Write-Host "（容器内自查不可用）" }

      Write-Host -NoNewline "    脚本文件系统隔离（user namespace）… "
      docker exec $name unshare --user --map-root-user echo ok *> $null
      if ($LASTEXITCODE -eq 0) {
        Write-Host "可用（bwrap 文件系统隔离生效）"
      } else {
        Write-Host "不可用（自动降级为环境收敛，见 docker/README.md）"
      }
      Write-Host "==> 冒烟通过；容器与卷将清理"
    } finally {
      docker rm -f $name *> $null
      docker volume rm $vol *> $null
    }
  }

  Write-Host ""
  Write-Host "后续步骤："
  Write-Host "  docker run -d --name gebai -p ${smokePort}:${smokePort} -v gebai-data:${smokeDataDir} -e GEBAI_LLM_API_KEY=… $Tag"
  Write-Host "  ① 镜像的裁剪面与定制口径：docker exec <容器> cat /etc/gebai/build-plan.env"
  Write-Host "  ② 配置/登录/能力边界见 docker/README.md"
} finally {
  Pop-Location
}
