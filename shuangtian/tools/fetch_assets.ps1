# 取真实素材（Khronos glTF 官方样例，CC0 / CC-BY）
#
# 为什么需要这个脚本：`tests/real_asset_test.cpp` 与 `tools/real_asset_probe`
# 用真实素材验证生态兼容，但**素材不该进仓库**（几十 MB，且是别人的版权资产）。
# 所以测试在文件不存在时 `ST_SKIP`（报告里可见，不算通过也不算失败）。
#
# 用途：
#   powershell -ExecutionPolicy Bypass -File tools/fetch_assets.ps1
#   powershell -ExecutionPolicy Bypass -File tools/fetch_assets.ps1 -OutDir D:\assets
#
# 来源：https://github.com/KhronosGroup/glTF-Sample-Assets
# 许可：多数为 CC0 或 CC-BY 4.0（每个模型目录下有 LICENSE.md）

param(
    [string]$OutDir = "tmp/real"
)

$ErrorActionPreference = "Stop"
$base = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"

# 素材清单：每个都对应一个**具体的验证目的**
# （不是"随便下几个"，而是覆盖不同的特性组合与尺度）
$assets = @(
    @{ Name = "box";           Path = "Box/glTF-Binary/Box.glb";                       Why = "最简几何（无贴图）" },
    @{ Name = "boxtextured";   Path = "BoxTextured/glTF-Binary/BoxTextured.glb";       Why = "最小贴图（PNG 内嵌）" },
    @{ Name = "helmet";        Path = "DamagedHelmet/glTF-Binary/DamagedHelmet.glb";   Why = "生产级：15452 三角形 + 5 张贴图" },
    @{ Name = "avocado";       Path = "Avocado/glTF-Binary/Avocado.glb";               Why = "小尺度（半径 3cm）+ JPEG 贴图" },
    @{ Name = "corset";        Path = "Corset/glTF-Binary/Corset.glb";                 Why = "小尺度 + 高面数" },
    @{ Name = "waterbottle";   Path = "WaterBottle/glTF-Binary/WaterBottle.glb";       Why = "中等尺度 + 标签贴图细节" },
    @{ Name = "antiquecamera"; Path = "AntiqueCamera/glTF-Binary/AntiqueCamera.glb";   Why = "大尺度（3.6m 含三脚架）" },
    @{ Name = "rigged";        Path = "RiggedSimple/glTF-Binary/RiggedSimple.glb";     Why = "骨架 + 蒙皮 + 动画" },
    @{ Name = "boxdraco";      Path = "Box/glTF-Draco/Box.gltf";                       Why = "Draco 压缩：验证**被拒绝并指路**" },
    @{ Name = "boxdraco-bin";  Path = "Box/glTF-Draco/Box.bin";                        Why = "（Draco 的缓冲文件）" }
)

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

Write-Host "素材目录：$OutDir" -ForegroundColor Cyan
$ok = 0
$fail = 0
foreach ($asset in $assets) {
    $url = "$base/$($asset.Path)"
    # Draco 的两个文件都要落到固定名字（测试按名字找 Box.gltf / Box.bin）
    $fileName = if ($asset.Name -eq "boxdraco") { "Box.gltf" }
                elseif ($asset.Name -eq "boxdraco-bin") { "Box.bin" }
                else { "$($asset.Name).glb" }
    $target = Join-Path $OutDir $fileName

    try {
        Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing -TimeoutSec 120
        $size = (Get-Item $target).Length
        Write-Host ("  [OK]   {0,-18} {1,10:N0} 字节  {2}" -f $fileName, $size, $asset.Why)
        $ok++
    } catch {
        Write-Host ("  [跳过] {0,-18} {1}" -f $fileName, $_.Exception.Message) -ForegroundColor Yellow
        $fail++
    }
}

Write-Host ""
Write-Host "完成：$ok 成功，$fail 跳过" -ForegroundColor Cyan
Write-Host ""
Write-Host "下一步：" -ForegroundColor Cyan
Write-Host "  build\dev\bin\ecosystem_probe.exe $OutDir\*.glb    # 兼容性总账"
Write-Host "  build\dev\bin\real_asset_probe.exe out\real $OutDir\*.glb   # 渲染成图片"
Write-Host "  st test real_asset                                # 回归测试"
