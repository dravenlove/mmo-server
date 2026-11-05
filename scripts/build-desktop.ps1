param(
    [string]$Address = '127.0.0.1:7000',
    [switch]$Tls,
    [switch]$SelfContained,
    [switch]$Shortcut
)

$ErrorActionPreference = 'Stop'
$desktopRoot = Split-Path $PSScriptRoot -Parent
$desktopOutput = Join-Path $desktopRoot 'dist\desktop'
New-Item -ItemType Directory -Path $desktopOutput -Force | Out-Null

# 当前电脑已有 .NET 桌面运行时；独立分发时可用 -SelfContained 一并打包运行时。
$desktopArgs = @(
    'publish', (Join-Path $desktopRoot 'desktop\MMO.Client\MMO.Client.csproj'),
    '-c', 'Release', '-r', 'win-x64', '--self-contained', $SelfContained.IsPresent.ToString().ToLowerInvariant(),
    '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
    '-p:DebugType=None', '-p:NuGetAudit=false', '--nologo', '-o', $desktopOutput
)
& dotnet @desktopArgs
if ($LASTEXITCODE -ne 0) { throw 'Desktop client publish failed.' }

# 此文件仅写入被 Git 忽略的 dist，不将实际服务器地址发布到仓库。
$desktopSettings = @{ Address = $Address; UseTls = $Tls.IsPresent; ProfileName = 'desktop-check' }
$desktopSettings | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $desktopOutput 'client.settings.json') -Encoding utf8
$desktopExe = Join-Path $desktopOutput 'MMO.Client.exe'
$desktopHash = (Get-FileHash -LiteralPath $desktopExe -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $desktopOutput 'MMO.Client.exe.sha256'), "$desktopHash  MMO.Client.exe`n", [Text.UTF8Encoding]::new($false))

if ($Shortcut) {
    $desktopLinkPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'MMO 测试客户端.lnk'
    $desktopShell = New-Object -ComObject WScript.Shell
    $desktopLink = $desktopShell.CreateShortcut($desktopLinkPath)
    $desktopLink.TargetPath = $desktopExe
    $desktopLink.WorkingDirectory = $desktopOutput
    $desktopLink.Description = '本机测试 MMO 网关、角色移动和场景切换'
    $desktopLink.Save()
    Write-Host "Shortcut: $desktopLinkPath"
}
Write-Host "Desktop client: $desktopExe"
