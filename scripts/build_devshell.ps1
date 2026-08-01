# build_devshell.ps1 — 在 VS DevShell 环境中构建 Ruzino
# 用法:
#   pwsh -File scripts/build_devshell.ps1               # 增量构建
#   pwsh -File scripts/build_devshell.ps1 -Reconfigure   # 重新 cmake 配置
#   pwsh -File scripts/build_devshell.ps1 -Target <name> # 只构建指定目标
#   pwsh -File scripts/build_devshell.ps1 -Clean         # Clean 后重建
#   pwsh -File scripts/build_devshell.ps1 -Target node_brush_capture -LogFile build\_build_out.txt -MachineReadable
#       # 日志落盘 + 打印机器可读的 BUILDEXIT=<code>，等价于旧 _build_one.bat 的行为，
#       # 用于无头/CI/agent 场景。

param(
    [switch]$Reconfigure,
    [switch]$Clean,
    [string]$Target = "",
    [string]$BuildDir = "build",
    [string]$BuildType = "Release",
    [string]$LogFile = "",
    [switch]$MachineReadable
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
    # NOTE: $BuildType 必须用双引号包裹。PowerShell 把 `-DCMAKE_BUILD_TYPE=$var`
    # 这种「连字符开头 + 等号 + 变量」的 token 作为 native argument 传给 cmake.exe 时,
    # 若不加引号,变量插值会失效,字面量 "$BuildType" 会被原样写入 CMakeCache.txt。
    # 后果是 ninja 生成的 rules.ninja 里出现非法配置名(如 CXX_COMPILER__gmock_$BuildType),
    # 在 `ninja -t recompact` 阶段以 "expected newline, got lexing error" 崩溃。
    # 加双引号后参数被强制当作单一可插值字符串处理,插值才稳定生效。
    cmake -G Ninja "-DCMAKE_BUILD_TYPE=$BuildType" -DRUZINO_WITH_CUDA=ON -DUSTC_HOMEWORK_PLUGINS=OFF ..
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ CMake 配置失败" -ForegroundColor Red
        # 自检:cache 里的 CMAKE_BUILD_TYPE 必须是 Debug/Release/RelWithDebInfo/MinSizeRel
        # 之一。若出现字面量(如 "$BuildType" 或空),说明上面的变量插值又失效了,
        # 或 build 目录被外部污染。直接给出可操作的诊断,免得手动排查。
        $cacheFile = Join-Path $BuildPath "CMakeCache.txt"
        if (Test-Path $cacheFile) {
            $cachedType = (Select-String -Path $cacheFile -Pattern '^CMAKE_BUILD_TYPE:STRING=(.+)$').Matches.Groups[1].Value
            $validTypes = @('Debug','Release','RelWithDebInfo','MinSizeRel')
            if ($cachedType -and ($cachedType -notin $validTypes)) {
                Write-Host "  ! 诊断: CMakeCache.txt 里 CMAKE_BUILD_TYPE='$cachedType' 不是合法值。" -ForegroundColor Yellow
                Write-Host "    合法值: $($validTypes -join ', ')。通常是 build_devshell.ps1 的变量插值失效," -ForegroundColor Yellow
                Write-Host "    或 build 目录被旧缓存污染。删除 build/ 目录后重试 -Reconfigure。" -ForegroundColor Yellow
            }
        }
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
# 若指定了 -LogFile，把 ninja 输出同时写入终端和日志文件（合并 stdout/stderr）。
# 这样既保留交互式可见性，又满足无头场景的日志落盘需求（取代旧 _build_one.bat）。
# 注意：本脚本顶部设了 $ErrorActionPreference = "Stop"，而 ninja 即使在构建成功时
# 也会向 stderr 写警告/进度，PowerShell 会把 native command 的 stderr 当作 ErrorRecord，
# 在 Stop 偏好下会触发终止性错误。因此这里临时切到 Continue，调完再恢复。
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    if ($LogFile) {
        $LogFileAbs = if ([System.IO.Path]::IsPathRooted($LogFile)) { $LogFile } else { Join-Path $RootDir $LogFile }
        $LogFileDir = Split-Path -Parent $LogFileAbs
        if ($LogFileDir -and -not (Test-Path $LogFileDir)) {
            New-Item -ItemType Directory -Path $LogFileDir -Force | Out-Null
        }
        if ($Target) {
            ninja $Target 2>&1 | Tee-Object -FilePath $LogFileAbs
        } else {
            ninja 2>&1 | Tee-Object -FilePath $LogFileAbs
        }
    } else {
        if ($Target) {
            ninja $Target
        } else {
            ninja
        }
    }
} finally {
    $ErrorActionPreference = $prevEAP
}

$buildResult = $LASTEXITCODE
Pop-Location

if ($buildResult -eq 0) {
    Write-Host "`n✓ 构建成功!" -ForegroundColor Green
} else {
    Write-Host "`n✗ 构建失败 (exit code: $buildResult)" -ForegroundColor Red
}

# 机器可读退出码：与根目录 .bat 系列使用相同的 BUILDEXIT=<code> token，
# 便于 CI/agent 解析。仅在 -MachineReadable 时输出，避免污染交互式输出。
if ($MachineReadable) {
    Write-Host "BUILDEXIT=$buildResult"
}

exit $buildResult
