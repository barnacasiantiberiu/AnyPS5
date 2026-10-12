$script:AnyPS5Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:AnyPS5Toolchain = Import-PowerShellDataFile (Join-Path $PSScriptRoot 'Toolchain.psd1')

function Get-AnyPS5Path([string]$Value, [string]$Default) {
    if (-not $Value) { $Value = Join-Path $script:AnyPS5Root $Default }
    return [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Value))
}

function Get-AnyPS5Python([string]$Python) {
    $candidates = @('python.exe', 'python3.exe', 'py.exe')
    if ($Python) { $candidates = @($Python) }
    foreach ($candidate in $candidates) {
        foreach ($command in @(Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue)) {
            if (-not $command -or $command.Source -match '[\\/]WindowsApps[\\/]') { continue }
            $arguments = @()
            if ($command.Name -eq 'py.exe') { $arguments += '-3' }
            try {
                $reported = & $command.Source @arguments -c 'import sys; sys.exit(1) if sys.version_info < (3, 10) else print(sys.executable)' 2>$null
                if ($LASTEXITCODE -eq 0 -and $reported -and (Test-Path -LiteralPath "$reported" -PathType Leaf)) {
                    return [IO.Path]::GetFullPath("$reported")
                }
            } catch { continue }
        }
    }
    throw 'A working Python 3.10 or newer is required. Install Python or pass -Python with its executable path.'
}

function Get-AnyPS5Git {
    $command = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) { throw 'Git for Windows is required and must be on PATH.' }
    $null = & $command.Source --version
    if ($LASTEXITCODE -ne 0) { throw 'Git for Windows could not run.' }
    return $command.Source
}

function Set-AnyPS5Certificate([string]$Git) {
    if ($env:SSL_CERT_FILE) {
        if (-not (Test-Path -LiteralPath $env:SSL_CERT_FILE -PathType Leaf)) {
            throw 'SSL_CERT_FILE names a missing file. Correct or remove it before continuing.'
        }
        return
    }
    $directory = [IO.DirectoryInfo](Split-Path -Parent $Git)
    for ($level = 0; $directory -and $level -lt 4; ++$level) {
        foreach ($relative in @('mingw64/etc/ssl/certs/ca-bundle.crt', 'usr/ssl/certs/ca-bundle.crt')) {
            $bundle = Join-Path $directory.FullName $relative
            if (Test-Path -LiteralPath $bundle -PathType Leaf) { $env:SSL_CERT_FILE = $bundle; return }
        }
        $directory = $directory.Parent
    }
    throw 'Cannot locate the Git CA bundle. Set SSL_CERT_FILE to a valid CA bundle.'
}

function Assert-AnyPS5Archive([string]$Archive) {
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) { throw "Toolchain archive is missing: $Archive" }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $script:AnyPS5Toolchain.Sha256) {
        throw "WinLibs SHA256 mismatch; archive will not be extracted: $Archive"
    }
}

function Assert-AnyPS5Toolchain([string]$ToolchainRoot) {
    $version = Join-Path $ToolchainRoot 'version_info.txt'
    if (-not (Test-Path -LiteralPath $version -PathType Leaf) -or
        (Get-Content -LiteralPath $version -TotalCount 1) -ne $script:AnyPS5Toolchain.VersionLine) {
        throw "Missing, partial or unsupported WinLibs installation: $ToolchainRoot. Run Bootstrap.ps1 with a new destination."
    }
    foreach ($name in $script:AnyPS5Toolchain.RequiredFiles) {
        $file = Join-Path $ToolchainRoot "bin/$name"
        if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -eq 0) {
            throw "Partial WinLibs installation; required file missing or empty: $file"
        }
    }
    $compiler = Join-Path $ToolchainRoot 'bin/g++.exe'
    $version = & $compiler -dumpfullversion
    if ($LASTEXITCODE -ne 0 -or $version -ne '15.2.0') { throw 'WinLibs g++ does not report the required version 15.2.0.' }
    $target = & $compiler -dumpmachine
    if ($LASTEXITCODE -ne 0 -or $target -ne 'x86_64-w64-mingw32') { throw 'WinLibs g++ does not target x86_64-w64-mingw32.' }
}

function Assert-AnyPS5ZipPaths([string]$Archive) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            $parts = $name.TrimEnd('/').Split('/')
            if ($parts[0] -ne 'mingw64' -or $name.Contains(':') -or $name.StartsWith('/') -or
                ($parts | Where-Object { -not $_ -or $_ -eq '.' -or $_ -eq '..' -or $_ -match '[. ]$' -or
                    $_ -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)' }) -or
                (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or
                -not $seen.Add($name.TrimEnd('/'))) {
                throw "Unsafe or duplicate toolchain archive entry: $name"
            }
        }
        if ($zip.Entries.Count -eq 0) { throw 'Toolchain archive is empty.' }
    } finally { $zip.Dispose() }
}

function Set-AnyPS5Environment([string]$ToolchainRoot, [string]$Python, [int]$BuildJobs = 4, [int]$TestJobs = 1) {
    $ToolchainRoot = Get-AnyPS5Path $ToolchainRoot 'build_tools/winlibs/mingw64'
    Assert-AnyPS5Toolchain $ToolchainRoot
    $pythonPath = Get-AnyPS5Python $Python
    $git = Get-AnyPS5Git
    Set-AnyPS5Certificate $git
    $bin = Join-Path $ToolchainRoot 'bin'
    $other = @($env:PATH.Split(';') | Where-Object { $_.TrimEnd('\', '/') -ine $bin.TrimEnd('\', '/') })
    $env:PATH = (@($bin) + $other) -join ';'
    $temporary = Join-Path $script:AnyPS5Root 'build_tools/tmp'
    $null = New-Item -ItemType Directory -Force -Path $temporary
    $env:TEMP = $temporary
    $env:TMP = $temporary
    $env:CMAKE_BUILD_PARALLEL_LEVEL = "$BuildJobs"
    $env:CTEST_PARALLEL_LEVEL = "$TestJobs"
    return [PSCustomObject]@{ Root = $script:AnyPS5Root; ToolchainRoot = $ToolchainRoot; Bin = $bin; Python = $pythonPath; Git = $git }
}
