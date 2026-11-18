param(
    [Parameter(Mandatory)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$baseUrl = 'https://developer.download.nvidia.com/compute/cuda/redist'
$manifest = Invoke-RestMethod "$baseUrl/redistrib_$Version.json"
$cudaRoot = Join-Path $env:RUNNER_TEMP "cuda-$Version"
New-Item -ItemType Directory -Path $cudaRoot -Force | Out-Null

foreach ($name in 'cuda_nvcc', 'cuda_cudart', 'cuda_cccl', 'cuda_nvrtc') {
    $package = $manifest.$name.'windows-x86_64'
    if (-not $package.relative_path -or -not $package.sha256) {
        throw "CUDA $Version is missing the Windows x64 package for $name"
    }

    $zip = Join-Path $env:RUNNER_TEMP "$name.zip"
    Invoke-WebRequest "$baseUrl/$($package.relative_path)" -OutFile $zip
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $package.sha256) {
        throw "Checksum mismatch for $name"
    }
    tar.exe -xf $zip -C $cudaRoot --strip-components 1
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $name" }
    Remove-Item -LiteralPath $zip
}

$cudaBin = Join-Path $cudaRoot 'bin'
"CUDA_PATH=$cudaRoot" >> $env:GITHUB_ENV
"CUDACXX=$(Join-Path $cudaBin 'nvcc.exe')" >> $env:GITHUB_ENV
$cudaBin >> $env:GITHUB_PATH
