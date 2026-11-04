$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
New-Item -ItemType Directory -Force build, data/local-debug, tokens/local-debug | Out-Null
$debugEnvironment = Join-Path $projectRoot 'build/local-debug.env'
if (-not (Test-Path -LiteralPath $debugEnvironment)) {
    # 随机密钥只保存在忽略的本地文件中，四个调试进程共用。
    $randomBytes = New-Object byte[] 32
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $generator.GetBytes($randomBytes) } finally { $generator.Dispose() }
    $internalToken = -join ($randomBytes | ForEach-Object { $_.ToString('x2') })
    Set-Content -LiteralPath $debugEnvironment -Encoding ascii -Value "INTERNAL_TOKEN=$internalToken"
} else {
    $environmentText = Get-Content -LiteralPath $debugEnvironment -Raw
    if ($environmentText -notmatch '\AINTERNAL_TOKEN=[0-9a-fA-F]{64}\r?\n?\z') {
        throw 'Invalid build/local-debug.env: expected one INTERNAL_TOKEN line with 64 hex characters.'
    }
}
& "$PSScriptRoot/build.ps1" -Configuration Debug
Write-Host 'Ready: open this folder in VS Code and select MMO: all services (Windows).'
Write-Host 'Debug database: data/local-debug/mmo.db; player credentials: tokens/local-debug/'
