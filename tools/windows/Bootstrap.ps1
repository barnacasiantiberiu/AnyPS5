param([string]$ToolchainRoot, [string]$Archive, [string]$Python, [switch]$Offline)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$ToolchainRoot = Get-AnyPS5Path $ToolchainRoot 'build_tools/winlibs/mingw64'
$Archive = Get-AnyPS5Path $Archive 'build_tools/downloads/winlibs-gcc-15.2.0.zip'
$pythonPath = Get-AnyPS5Python $Python
$git = Get-AnyPS5Git
Set-AnyPS5Certificate $git
if (Test-Path -LiteralPath $Archive -PathType Leaf) { Assert-AnyPS5Archive $Archive }
if (Test-Path -LiteralPath $ToolchainRoot) {
    Assert-AnyPS5Toolchain $ToolchainRoot
    Write-Host "Reusing WinLibs $($script:AnyPS5Toolchain.Release): $ToolchainRoot"
    return
}
if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
    if ($Offline) { throw "Offline mode requires the cached WinLibs archive: $Archive" }
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Archive)
    $download = $Archive + '.partial-' + [Guid]::NewGuid().ToString('N')
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $script:AnyPS5Toolchain.Url -OutFile $download
        Assert-AnyPS5Archive $download
        Move-Item -LiteralPath $download -Destination $Archive
    } finally {
        if (Test-Path -LiteralPath $download -PathType Leaf) { Remove-Item -LiteralPath $download -Force }
    }
}
Assert-AnyPS5Archive $Archive
Assert-AnyPS5ZipPaths $Archive
$parent = Split-Path -Parent $ToolchainRoot
$null = New-Item -ItemType Directory -Force -Path $parent
$staging = Join-Path $parent ('.anyps5-winlibs-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $staging
try {
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $staging)
    $extracted = Join-Path $staging 'mingw64'
    Assert-AnyPS5Toolchain $extracted
    if (Test-Path -LiteralPath $ToolchainRoot) { throw "Toolchain destination appeared during installation: $ToolchainRoot" }
    Move-Item -LiteralPath $extracted -Destination $ToolchainRoot
} finally {
    $directory = Get-Item -LiteralPath $staging
    if ($directory.Parent.FullName -ne [IO.Path]::GetFullPath($parent) -or
        -not $directory.Name.StartsWith('.anyps5-winlibs-') -or
        ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing cleanup of an unexpected staging path.' }
    Remove-Item -LiteralPath $directory.FullName -Recurse -Force
}
Write-Host "WinLibs $($script:AnyPS5Toolchain.Release) installed: $ToolchainRoot"
