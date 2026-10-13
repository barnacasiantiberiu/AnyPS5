$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$imagePath = $env:ANYPS5_ISO_PATH
$action = $env:ANYPS5_ISO_ACTION
$owned = $false
$ownedDevice = ''

function Send-Result($result, $exitCode) {
    $result | ConvertTo-Json -Compress -Depth 4
    exit $exitCode
}

try {
    if ([string]::IsNullOrWhiteSpace($imagePath) -or -not [IO.Path]::IsPathRooted($imagePath)) {
        throw 'An absolute ISO path is required.'
    }
    if ($action -eq 'unmount') {
        $disk = Get-DiskImage -ImagePath $imagePath -ErrorAction Stop
        if ($disk.Attached) {
            if ([string]::IsNullOrWhiteSpace($env:ANYPS5_ISO_DEVICE) -or $disk.DevicePath -ne $env:ANYPS5_ISO_DEVICE) {
                throw 'The mounted device changed; it was left attached to avoid ejecting a different mount.'
            }
            Dismount-DiskImage -ImagePath $imagePath -ErrorAction Stop | Out-Null
        }
        Send-Result @{ ok = $true } 0
    }
    if ($action -ne 'mount') { throw 'Unknown ISO operation.' }
    $disk = Get-DiskImage -ImagePath $imagePath -ErrorAction Stop
    if (-not $disk.Attached) {
        $disk = Mount-DiskImage -ImagePath $imagePath -StorageType ISO -Access ReadOnly -PassThru -ErrorAction Stop
        $owned = $true
        $ownedDevice = [string]$disk.DevicePath
    }
    $roots = @()
    for ($attempt = 0; $attempt -lt 20 -and $roots.Count -eq 0; $attempt++) {
        $volumes = @($disk | Get-Volume -ErrorAction Stop)
        $readable = @()
        foreach ($volume in $volumes) {
            $candidate = if ($volume.DriveLetter) { [string]$volume.DriveLetter + ':\' } else { [string]$volume.Path }
            if ($candidate -and [IO.Directory]::Exists($candidate)) { $readable += $candidate }
        }
        $roots = @($readable | Sort-Object -Unique)
        if ($roots.Count -eq 0) { Start-Sleep -Milliseconds 250 }
    }
    if ($roots.Count -eq 0) { throw 'No readable filesystem volume was found. The image may be incomplete or use an unsupported filesystem.' }
    if ($roots.Count -ne 1) { throw 'The ISO exposes multiple readable volumes. Choose an extracted game folder instead.' }
    Send-Result @{ ok = $true; root = $roots[0]; owned = $owned; devicePath = [string]$disk.DevicePath } 0
} catch {
    $message = if ($action -eq 'unmount') { 'Windows could not detach the temporary ISO mount: ' } else { 'Windows could not open the ISO read-only. Ensure it is a complete supported ISO image. ' }
    $message += $_.Exception.Message
    $cleanupWarning = $null
    if ($owned) {
        try {
            $current = Get-DiskImage -ImagePath $imagePath -ErrorAction Stop
            if ($current.Attached) {
                if ([string]::IsNullOrWhiteSpace($ownedDevice) -or $current.DevicePath -ne $ownedDevice) {
                    throw 'The mounted device changed and was left attached.'
                }
                Dismount-DiskImage -ImagePath $imagePath -ErrorAction Stop | Out-Null
            }
        }
        catch { $cleanupWarning = 'Windows could not detach the temporary ISO after the read failed. You can eject it in File Explorer. ' + $_.Exception.Message }
    }
    Send-Result @{ ok = $false; error = $message; cleanupWarning = $cleanupWarning } 1
}
