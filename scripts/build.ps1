param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$binaryDirectory = if ($Configuration -eq 'Debug') { 'bin/debug' } else { 'bin' }
New-Item -ItemType Directory -Force $binaryDirectory | Out-Null
foreach ($service in @('backend', 'gateway', 'client', 'auth')) {
    $buildArguments = @('build')
    if ($Configuration -eq 'Debug') {
        # 保留源码路径，关闭优化和内联，方便断点和局部变量检查。
        $buildArguments += '-gcflags=all=-N -l'
    } else {
        $buildArguments += '-trimpath'
    }
    $buildArguments += @('-o', "$binaryDirectory/$service.exe", "./cmd/$service")
    & go @buildArguments
    if ($LASTEXITCODE -ne 0) { throw "Go build failed: $service" }
}
cmake -S scene -B build/scene -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build build/scene --config $Configuration --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
Copy-Item -LiteralPath "build/scene/$Configuration/scene-server.exe" -Destination "$binaryDirectory/scene-server.exe" -Force
if ($Configuration -eq 'Debug') {
    Copy-Item -LiteralPath "build/scene/Debug/scene-server.pdb" -Destination "$binaryDirectory/scene-server.pdb" -Force
}
Write-Host "Build completed ($Configuration): $binaryDirectory"
