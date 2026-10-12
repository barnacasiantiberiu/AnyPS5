param([string]$ToolchainRoot, [string]$Python, [string]$BuildDirectory, [string]$OutputDirectory,
    [string[]]$Module = @(), [string]$Base,
    [ValidateRange(1, 64)][int]$BuildJobs = 4, [ValidateRange(1, 64)][int]$TestJobs = 1)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-Environment.ps1') -ToolchainRoot $ToolchainRoot -Python $Python -BuildJobs $BuildJobs -TestJobs $TestJobs
$BuildDirectory = Get-AnyPS5Path $BuildDirectory 'build'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $BuildDirectory ('windows-validation/' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'))
}
$OutputDirectory = Get-AnyPS5Path $OutputDirectory ''
$compilerBin = $AnyPS5Environment.Bin.Replace('\', '/')
$pythonPath = $AnyPS5Python.Replace('\', '/')
Push-Location $AnyPS5Environment.Root
try {
    & cmake -S . -B $BuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON `
        "-DCMAKE_C_COMPILER=$compilerBin/gcc.exe" "-DCMAKE_CXX_COMPILER=$compilerBin/g++.exe" `
        "-DCMAKE_MAKE_PROGRAM=$compilerBin/ninja.exe" "-DPython3_EXECUTABLE=$pythonPath" `
        -DANYPS5_ENABLE_SPIRV_TOOLS=ON -DANYPS5_VIDEO_OUT_TEST_DRIVER=offscreen -DANYPS5_RELINKER_ONLY=OFF -DANYPS5_USE_SYSTEM_DEPS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $AnyPS5Python tools/check_test_registration.py $BuildDirectory
    if ($LASTEXITCODE -ne 0) { throw 'CTest registration check failed.' }
    $arguments = @('tools/test_modules.py', '--build-dir', $BuildDirectory, '--output-dir', $OutputDirectory)
    foreach ($name in $Module) { $arguments += @('--module', $name) }
    if ($Base) { $arguments += @('--base', $Base) }
    & $AnyPS5Python @arguments
    if ($LASTEXITCODE -ne 0) { throw "Functional module validation failed. See $OutputDirectory" }
    & ctest --test-dir (Join-Path $BuildDirectory 'core/libs/prx/libc') --no-tests=error --output-on-failure `
        --output-log (Join-Path $OutputDirectory 'libc-full.log') --output-junit (Join-Path $OutputDirectory 'libc-full.xml')
    if ($LASTEXITCODE -ne 0) { throw "Full libc validation failed. See $OutputDirectory" }
    & $AnyPS5Python (Join-Path $PSScriptRoot 'verify_libc.py') (Join-Path $BuildDirectory 'core/libs/prx/libc') `
        (Join-Path $OutputDirectory 'libc-full.xml')
    if ($LASTEXITCODE -ne 0) { throw "Full libc results contain skipped, disabled or missing tests. See $OutputDirectory" }
} finally { Pop-Location }
Write-Host "Functional modules and full libc suite passed. Reports: $OutputDirectory"
