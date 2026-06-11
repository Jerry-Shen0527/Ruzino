# build_devshell.ps1 — 在 VS DevShell 环境中构建 Ruzino
# 用法:
#   pwsh -File scripts/build_devshell.ps1               # 增量构建
#   pwsh -File scripts/build_devshell.ps1 -Reconfigure   # 重新 cmake 配置
#   pwsh -File scripts/build_devshell.ps1 -Target <name> # 只构建指定目标
#   pwsh -File scripts/build_devshell.ps1 -Clean         # Clean 后重建

param(
    [switch]$Reconfigure,
    [switch]$Clean,
    [string]$Target = "",
    [string]$BuildDir = "build",
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"
$RootDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# ======== Step 1: 进入 VS DevShell ========
$VSDir = "C:\Program Files\Microsoft Visual Studio\18\Community"
$DevShellModule = "$VSDir\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"

if (-not (Test-Path $DevShellModule)) {
    # 尝试搜索其他 VS 版本
    $VSDirs = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue
    foreach ($vs in $VSDirs) {
        $candidate = Get-ChildItem "$($vs.FullName)" -Directory -Recurse -Filter "Microsoft.VisualStudio.DevShell.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) {
            $DevShellModule = $candidate.FullName
            $VSDir = $candidate.Directory.Parent.Parent.FullName
            break
        }
    }
}

if (Test-Path $DevShellModule) {
    Import-Module $DevShellModule
    Enter-VsDevShell -VsInstallPath $VSDir -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" 2>$null
    Write-Host "✓ VS DevShell 已加载" -ForegroundColor Cyan
} else {
    Write-Host "! 未找到 VS DevShell 模块，使用当前环境" -ForegroundColor Yellow
}

# ======== Step 2: 检查 build 目录 ========
$BuildPath = Join-Path $RootDir $BuildDir

if ($Reconfigure -or -not (Test-Path "$BuildPath\build.ninja")) {
    Write-Host "✓ CMake 配置中..." -ForegroundColor Cyan
    if (-not (Test-Path $BuildPath)) {
        New-Item -ItemType Directory -Path $BuildPath | Out-Null
    }
    Push-Location $BuildPath
    cmake -G Ninja -DCMAKE_BUILD_TYPE=$BuildType -DUSTC_CG_WITH_CUDA=ON -DUSTC_HOMEWORK_PLUGINS=OFF ..
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ CMake 配置失败" -ForegroundColor Red
        Pop-Location
        exit 1
    }
    Pop-Location
}

# ======== Step 3: 构建 ========
Push-Location $BuildPath

if ($Clean) {
    Write-Host "✓ 清理中..." -ForegroundColor Cyan
    ninja -t clean
}

Write-Host "✓ 构建中..." -ForegroundColor Cyan
if ($Target) {
    ninja $Target
} else {
    ninja
}

$buildResult = $LASTEXITCODE
Pop-Location

if ($buildResult -eq 0) {
    Write-Host "`n✓ 构建成功!" -ForegroundColor Green
} else {
    Write-Host "`n✗ 构建失败 (exit code: $buildResult)" -ForegroundColor Red
}

exit $buildResult
