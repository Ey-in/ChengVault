#requires -Version 7.2
param(
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '..\build-release'),
    [string]$EvidenceDirectory = (Join-Path $PSScriptRoot '..\evidence\benchmark'),
    [int]$DataMiB = 1024,
    [int]$SmallFileCount = 5000
)
$ErrorActionPreference = 'Stop'
if ($DataMiB -lt 1 -or $SmallFileCount -lt 1) { throw 'Dataset parameters must be positive.' }
$cli = [IO.Path]::GetFullPath((Join-Path $BinaryDirectory 'ChengVaultCli.exe'))
$bundledToolchain = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\.toolchain\root\ucrt64\bin'))
$childPath = if (Test-Path -LiteralPath $bundledToolchain) { $bundledToolchain + ';' + $env:PATH } else { $env:PATH }
if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) { throw "CLI does not exist: $cli" }
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$runRoot = Join-Path $evidence ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$records = [Collections.Generic.List[object]]::new()
$backupPassword = [Guid]::NewGuid().ToString('N')

function Invoke-Measurement([string]$Name, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($cli)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $start.Environment['CHENGVAULT_PASSWORD'] = $backupPassword
    $start.Environment['PATH'] = $childPath
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timer = [Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw 'Unable to start CLI.' }
    $outputTask = $process.StandardOutput.ReadToEndAsync()
    $errorTask = $process.StandardError.ReadToEndAsync()
    [long]$peak = 0
    while (-not $process.WaitForExit(200)) {
        $process.Refresh()
        try { $peak = [Math]::Max($peak, $process.PeakWorkingSet64) } catch { }
    }
    $timer.Stop()
    try { $process.Refresh(); $peak = [Math]::Max($peak, $process.PeakWorkingSet64) } catch { }
    $outputText = $outputTask.GetAwaiter().GetResult()
    $errorText = $errorTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $runRoot ($Name + '.stdout.json')), $outputText, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $runRoot ($Name + '.stderr.txt')), $errorText, [Text.UTF8Encoding]::new($false))
    $parsed = $null
    try { $parsed = $outputText | ConvertFrom-Json } catch { }
    $record = [ordered]@{
        name = $Name; timestampUtc = [DateTime]::UtcNow.ToString('o'); exitCode = $process.ExitCode
        durationMs = $timer.ElapsedMilliseconds
        peakWorkingSetBytes = $(if ($peak -gt 0) { $peak } else { $null })
        peakWorkingSetSamplingMs = 200; result = $parsed
    }
    $records.Add($record)
    Write-Host "$Name : exit=$($process.ExitCode), elapsed=$($timer.ElapsedMilliseconds)ms, peak=$peak bytes"
    if ($process.ExitCode -ne 0) { throw "$Name failed. See its captured stderr." }
    return $parsed
}

try {
    # Generate reproducible mixed content without reading an entire GiB into RAM.
    # The repeated 16 KiB pattern is compressible but includes non-text bytes.
    $source = Join-Path $runRoot 'large-source'
    [IO.Directory]::CreateDirectory($source) | Out-Null
    $pattern = [byte[]]::new(16384)
    [Random]::new(20261017).NextBytes($pattern)
    $block = [byte[]]::new(1048576)
    for ($offset = 0; $offset -lt $block.Length; $offset += $pattern.Length) { [Array]::Copy($pattern, 0, $block, $offset, $pattern.Length) }
    $largeFile = Join-Path $source 'mixed-1gib.bin'
    $stream = [IO.File]::Open($largeFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { for ($i = 0; $i -lt $DataMiB; ++$i) { $stream.Write($block, 0, $block.Length) } } finally { $stream.Dispose() }
    $archive = Join-Path $runRoot 'large.cva'
    $null = Invoke-Measurement 'large-archive-backup' @('backup','--source',$source,'--output',$archive,'--format','indexed','--compression','lz77','--cipher','chacha20')
    $null = Invoke-Measurement 'large-archive-verify' @('verify','--input',$archive)
    $restore = Join-Path $runRoot 'large-restored'
    $null = Invoke-Measurement 'large-archive-restore' @('restore','--input',$archive,'--output',$restore)
    $restoredFile = Join-Path $restore 'large-source\mixed-1gib.bin'
    $hashEqual = (Get-FileHash -LiteralPath $largeFile -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $restoredFile -Algorithm SHA256).Hash
    if (-not $hashEqual) { throw 'Large-file restored bytes do not match.' }
    $records.Add([ordered]@{ name='large-content-integrity'; result='PASS'; logicalBytes=(Get-Item -LiteralPath $largeFile).Length; archiveBytes=(Get-Item -LiteralPath $archive).Length; storageRatio=(Get-Item -LiteralPath $archive).Length / (Get-Item -LiteralPath $largeFile).Length })

    $repository = Join-Path $runRoot 'incremental-repository'
    $null = Invoke-Measurement 'repository-initial' @('repo-backup','--source',$source,'--repo',$repository,'--compression','lz77','--cipher','chacha20')
    $unchanged = Invoke-Measurement 'repository-unchanged' @('repo-backup','--source',$source,'--repo',$repository,'--compression','lz77')
    if ($unchanged.storedBytes -ne 0) { throw 'Unchanged backup unexpectedly stored data chunks.' }
    $stream = [IO.File]::Open($largeFile, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $position = [Math]::Min([long](1048576 + 123), $stream.Length - 1)
        $stream.Position = $position; $original = $stream.ReadByte()
        $stream.Position = $position; $stream.WriteByte($original -bxor 255)
    } finally { $stream.Dispose() }
    $changed = Invoke-Measurement 'repository-one-block-change' @('repo-backup','--source',$source,'--repo',$repository,'--compression','lz77')
    if ($changed.details.newChunks -ne 1) { throw 'A one-block edit did not create exactly one new block.' }
    $null = Invoke-Measurement 'repository-verify' @('repo-verify','--repo',$repository)

    # Many small files exercise traversal, metadata and index overhead separately
    # from bulk-stream throughput. The dataset and results stay for inspection.
    $small = Join-Path $runRoot 'small-source'
    [IO.Directory]::CreateDirectory($small) | Out-Null
    $smallBytes = [Text.Encoding]::UTF8.GetBytes(('ChengVault small-file data ' * 40))
    for ($i = 0; $i -lt $SmallFileCount; ++$i) { [IO.File]::WriteAllBytes((Join-Path $small ('item-{0:D6}.txt' -f $i)), $smallBytes) }
    $smallArchive = Join-Path $runRoot 'small.cva'
    $null = Invoke-Measurement 'small-files-backup' @('backup','--source',$small,'--output',$smallArchive,'--format','indexed','--compression','huffman','--cipher','aes256')
    $null = Invoke-Measurement 'small-files-restore' @('restore','--input',$smallArchive,'--output',(Join-Path $runRoot 'small-restored'))
    $restoredCount = @(Get-ChildItem -LiteralPath (Join-Path $runRoot 'small-restored\small-source') -File).Count
    if ($restoredCount -ne $SmallFileCount) { throw 'Small-file count changed after restore.' }
    $smallExpectedHash = (Get-FileHash -LiteralPath (Join-Path $small 'item-000000.txt') -Algorithm SHA256).Hash
    foreach ($restored in Get-ChildItem -LiteralPath (Join-Path $runRoot 'small-restored\small-source') -File) {
        if ((Get-FileHash -LiteralPath $restored.FullName -Algorithm SHA256).Hash -ne $smallExpectedHash) { throw "Small-file restored content differs: $($restored.Name)" }
    }
    $records.Add([ordered]@{ name='small-files-content-integrity'; result='PASS'; verifiedFiles=$restoredCount; sha256=$smallExpectedHash.ToLowerInvariant() })

    # This Win32 call creates test input; the application must independently
    # recognize, archive and recreate NTFS sparse ranges using its own code.
    if (-not ('ChengVaultBenchSparse' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class ChengVaultBenchSparse {
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr input, uint inputLength, IntPtr output, uint outputLength, out uint returned, IntPtr overlap);
    public static void Set(SafeFileHandle handle) { uint count; if (!DeviceIoControl(handle, 0x000900c4, IntPtr.Zero, 0, IntPtr.Zero, 0, out count, IntPtr.Zero)) throw new System.ComponentModel.Win32Exception(); }
}
'@
    }
    $sparseSource = Join-Path $runRoot 'sparse-source'
    [IO.Directory]::CreateDirectory($sparseSource) | Out-Null
    $sparseFile = Join-Path $sparseSource 'over-4gib.bin'
    [long]$sparseLength = 4294967296 + 131072
    $stream = [IO.File]::Open($sparseFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try { [ChengVaultBenchSparse]::Set($stream.SafeFileHandle); $stream.SetLength($sparseLength); $stream.Write($pattern,0,$pattern.Length); $stream.Position=$sparseLength-$pattern.Length; $stream.Write($pattern,0,$pattern.Length) } finally { $stream.Dispose() }
    $sparseArchive = Join-Path $runRoot 'sparse.cva'
    $null = Invoke-Measurement 'over-4gib-sparse-backup' @('backup','--source',$sparseSource,'--output',$sparseArchive,'--format','pax','--compression','lz77','--cipher','chacha20')
    $null = Invoke-Measurement 'over-4gib-sparse-restore' @('restore','--input',$sparseArchive,'--output',(Join-Path $runRoot 'sparse-restored'))
    $restoredSparse = Join-Path $runRoot 'sparse-restored\sparse-source\over-4gib.bin'
    $sparseInfo = Get-Item -LiteralPath $restoredSparse
    if ($sparseInfo.Length -ne $sparseLength -or -not ($sparseInfo.Attributes -band [IO.FileAttributes]::SparseFile)) { throw 'Large sparse file length/type were not preserved.' }
    $restoredStream = [IO.File]::OpenRead($restoredSparse)
    try {
        foreach ($offset in @(0, ($sparseLength-$pattern.Length))) {
            $restoredStream.Position=$offset; $actual=[byte[]]::new($pattern.Length)
            if ($restoredStream.Read($actual,0,$actual.Length) -ne $actual.Length -or [Convert]::ToBase64String($actual) -ne [Convert]::ToBase64String($pattern)) { throw 'Large sparse data range differs.' }
        }
    } finally { $restoredStream.Dispose() }
    # Hash the entire logical file, including holes, so checking the two written
    # ranges cannot hide accidental nonzero data in an intended sparse region.
    $sparseHash=(Get-FileHash -LiteralPath $sparseFile -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $restoredSparse -Algorithm SHA256).Hash -ne $sparseHash) { throw 'Large sparse file full-content hash differs.' }
    $records.Add([ordered]@{ name='over-4gib-sparse-integrity'; result='PASS'; logicalBytes=$sparseLength; archiveBytes=(Get-Item -LiteralPath $sparseArchive).Length; sha256=$sparseHash.ToLowerInvariant() })
} catch {
    $records.Add([ordered]@{ name='benchmark-failure'; result='FAIL'; error=$_.Exception.Message })
    throw
} finally {
    $processor=Get-CimInstance Win32_Processor | Select-Object -First 1
    $computer=Get-CimInstance Win32_ComputerSystem
    $report = [ordered]@{ timestampUtc=[DateTime]::UtcNow.ToString('o'); os=[Environment]::OSVersion.VersionString; processors=[Environment]::ProcessorCount; cpu=$processor.Name; memoryBytes=$computer.TotalPhysicalMemory; executableSha256=(Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash.ToLowerInvariant(); dataMiB=$DataMiB; smallFileCount=$SmallFileCount; runDirectory=$runRoot; records=$records }
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $runRoot 'benchmark-results.json') -Encoding utf8
    Write-Host "Evidence: $runRoot"
}
