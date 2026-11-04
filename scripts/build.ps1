$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
New-Item -ItemType Directory -Force bin | Out-Null
foreach ($service in @('backend', 'gateway', 'client')) {
    go build -trimpath -o "bin/$service.exe" "./cmd/$service"
    if ($LASTEXITCODE -ne 0) { throw "Go build failed: $service" }
}
cmake -S scene -B build/scene -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build build/scene --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
Copy-Item -LiteralPath build/scene/Release/scene-server.exe -Destination bin/scene-server.exe -Force
Write-Host 'Build completed: bin/backend.exe, gateway.exe, client.exe, scene-server.exe'
