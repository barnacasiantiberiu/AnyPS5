param(
  [Parameter(Mandatory = $true)][string]$FixtureRoot,
  [Parameter(Mandatory = $true)][ValidateSet('plain', 'overlay')][string]$Variant
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($FixtureRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
$temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
if ([IO.Path]::GetDirectoryName($root) -ne $temporary -or [IO.Path]::GetFileName($root) -notmatch '^anyps5-iso-native-[a-zA-Z0-9-]+$') {
  throw 'ISO creation is restricted to this test''s unique temporary fixture.'
}
$source = Join-Path $root $Variant
$destination = Join-Path $root ($Variant + '.iso')
if (-not (Test-Path -LiteralPath $source -PathType Container) -or (Test-Path -LiteralPath $destination)) { throw 'Fixture source is missing or output already exists.' }
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class AnyPs5NativeIsoWriter {
  public static long Save(object source, string destination) {
    IStream stream = (IStream)source;
    byte[] buffer = new byte[65536];
    IntPtr readCount = Marshal.AllocCoTaskMem(4);
    long total = 0;
    try {
      using (FileStream output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write)) {
        while (true) {
          Marshal.WriteInt32(readCount, 0);
          stream.Read(buffer, buffer.Length, readCount);
          int count = Marshal.ReadInt32(readCount);
          if (count == 0) break;
          total += count;
          if (count < 0 || count > buffer.Length || total > 67108864) throw new InvalidDataException("Unexpected native fixture ISO size.");
          output.Write(buffer, 0, count);
        }
      }
      return total;
    } finally { Marshal.FreeCoTaskMem(readCount); }
  }
}
"@
$image = $null
$result = $null
$stream = $null
try {
  $image = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
  $image.FileSystemsToCreate = 7
  $image.FreeMediaBlocks = 0
  $image.VolumeName = 'ANYPS5_NATIVE_TEST'
  $image.Root.AddTree($source, $false)
  $result = $image.CreateResultImage()
  $stream = $result.ImageStream
  $bytes = [AnyPs5NativeIsoWriter]::Save($stream, $destination)
  @{ image = $destination; bytes = $bytes; fileSystems = 'ISO9660,Joliet,UDF' } | ConvertTo-Json -Compress
} finally {
  foreach ($component in @($stream, $result, $image)) {
    if ($null -ne $component -and [Runtime.InteropServices.Marshal]::IsComObject($component)) { [Runtime.InteropServices.Marshal]::ReleaseComObject($component) | Out-Null }
  }
}
