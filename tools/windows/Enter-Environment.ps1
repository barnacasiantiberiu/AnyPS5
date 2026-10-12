param([string]$ToolchainRoot, [string]$Python,
    [ValidateRange(1, 64)][int]$BuildJobs = 4, [ValidateRange(1, 64)][int]$TestJobs = 1)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$AnyPS5Environment = Set-AnyPS5Environment $ToolchainRoot $Python $BuildJobs $TestJobs
$AnyPS5Python = $AnyPS5Environment.Python
Write-Host "AnyPS5 environment ready; Python: $AnyPS5Python"
