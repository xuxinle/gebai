# 拉取 OpenGL 加载器源码（glad 单头）到 third_party/opengl/ ——**不进 git 仓库**
#
# 为什么走"本地拉取"而不是随仓库分发（对比 third_party/ 里其它依赖）：
# 这份代码是**生成的**（按需裁剪 GL 版本与扩展集），体积大、随生成器版本漂移，
# 而且每个人的驱动/平台需要的功能集不同。把它 vendor 进仓库只会让 review 被
# 33 万行生成代码淹没。因此：源码在本机、接口在仓库（`include/st/raster/gl.hpp`）。
#
# 取法：浅克隆 glfw（其 deps/glad 内含 glad 生成的单头），拷出后**立刻删掉克隆**——
# 于是本机只有那一个 .h，仓库里一行 OpenGL 源码都没有。
#
# 用法：
#   pwsh -File tools/fetch_opengl.ps1            # 拉取（已存在则跳过）
#   pwsh -File tools/fetch_opengl.ps1 -Force     # 强制重取
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'third_party/opengl'
$header = Join-Path $dest 'gl.h'

if ((Test-Path $header) -and -not $Force) {
    Write-Host "已存在：$header（用 -Force 重取）"
    exit 0
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("glad-fetch-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    # ssh.github.com:443 是受限网络下仍可用的 GitHub 通道（普通 https 常被拦）
    $urls = @(
        'ssh://git@ssh.github.com:443/glfw/glfw.git',
        'https://github.com/glfw/glfw.git'
    )
    $cloned = $false
    foreach ($url in $urls) {
        Write-Host "尝试克隆 $url"
        & git clone --depth 1 --quiet $url $temp 2>$null
        if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $temp 'deps/glad/gl.h'))) {
            $cloned = $true
            break
        }
        Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $temp | Out-Null
    }
    if (-not $cloned) {
        Write-Error "克隆失败：两条通道都不通。离线环境请手工把 glad 生成的 gl.h 放到 $dest"
        exit 1
    }
    Copy-Item (Join-Path $temp 'deps/glad/gl.h') $header -Force
    $size = (Get-Item $header).Length
    Write-Host "已就位：$header（$size 字节）"
    Write-Host "注意：此目录已在 .gitignore 中——OpenGL 源码不进仓库。"
}
finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}
