#requires -Version 7.2
param(
    [string]$BinaryDirectory=(Join-Path $PSScriptRoot '..\build-release'),
    [string]$Toolchain=(Join-Path $PSScriptRoot '..\.toolchain\root\ucrt64'),
    [switch]$ScreenshotsOnly
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$binary=[IO.Path]::GetFullPath($BinaryDirectory)
$evidence=Join-Path $project 'evidence\ui-refresh'
[IO.Directory]::CreateDirectory($evidence)|Out-Null
Add-Type -Path (Join-Path $PSScriptRoot 'isolated-desktop.cs')
$desktopName='ChengVaultUi-'+[Guid]::NewGuid().ToString('N')
$desktop=[IntPtr]::Zero
$job=[IntPtr]::Zero
$before=[IsolatedDesktop]::InputName()
$saved=@{}
foreach($name in @('PATH','CHENGVAULT_DATA_HOME','QT_SCALE_FACTOR')) {
    $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
$records=[Collections.Generic.List[object]]::new()
function RunUi([string]$Name,[string]$Executable,[string[]]$Arguments) {
    # These are CreateProcess arguments, never shell expressions. Reject embedded
    # quotes rather than silently changing a requested path during QA.
    $quoted=foreach($argument in $Arguments) {
        if($argument.Contains('"') -or $argument.Contains("`n") -or $argument.EndsWith('\')) {
            throw "Unsupported validation argument: $argument"
        }
        '"'+$argument+'"'
    }
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $child=[IsolatedDesktop]::Launch($job,$desktopName,$Executable,($quoted -join ' '),$binary)
    $process=[Diagnostics.Process]::GetProcessById([int]$child)
    # Retain a process handle before waiting: an object attached by PID alone
    # may lose access to its exit status once the native child has terminated.
    $null=$process.Handle
    try {
        while(-not $process.WaitForExit(250)) {
            if($timer.Elapsed.TotalSeconds -gt 180) { throw "$Name exceeded 180 seconds" }
        }
        $exitCode=$process.ExitCode
        if($exitCode -ne 0) { throw "$Name failed with exit code $exitCode" }
        $records.Add([ordered]@{name=$Name;exitCode=$exitCode;durationMs=$timer.ElapsedMilliseconds;scaleFactor=$env:QT_SCALE_FACTOR;executableSha256=(Get-FileHash -LiteralPath $Executable).Hash.ToLowerInvariant()})
        Write-Host "$Name : PASS"
    } finally { $process.Dispose() }
}
try {
    $env:PATH=(Join-Path $Toolchain 'bin')+';'+$saved['PATH']
    $env:CHENGVAULT_DATA_HOME=Join-Path $project ('tmp\ui-validation-'+[Guid]::NewGuid().ToString('N'))
    $env:QT_SCALE_FACTOR='1'
    $desktop=[IsolatedDesktop]::Create($desktopName)
    $job=[IsolatedDesktop]::Job()
    if(-not $ScreenshotsOnly) {
        RunUi 'GUI functional regression' (Join-Path $binary 'ChengVaultGuiTests.exe') @('--output',(Join-Path $project 'evidence\gui-test-results.json'),'--review-dir',(Join-Path $evidence 'interaction'))
        $results=Get-Content -LiteralPath (Join-Path $project 'evidence\gui-test-results.json') -Raw | ConvertFrom-Json
        if($results.failed -ne 0 -or $results.passed -ne 20 -or $results.skipped -ne 0) { throw 'Expected all 20 existing GUI regression cases to pass.' }
    }
    foreach($capture in @(
        @{name='default';scale='1';size='1280x860'},
        @{name='minimum';scale='1';size='1040x720'},
        @{name='high-dpi';scale='1.5';size='1040x720'}
    )) {
        $env:QT_SCALE_FACTOR=$capture.scale
        $directory=Join-Path $evidence $capture.name
        RunUi ('Capture '+$capture.name) (Join-Path $binary 'ChengVault.exe') @('--screenshot-dir',$directory,'--screenshot-size',$capture.size)
        if(@(Get-ChildItem -LiteralPath $directory -Filter '*.png').Count -ne 8) { throw 'Expected exactly eight real page captures.' }
    }
    $after=[IsolatedDesktop]::InputName()
    if($before -ne $after) { throw 'The active input desktop changed during validation.' }
    $coreDigests=Get-Content -LiteralPath (Join-Path $project 'evidence\coverage\source-digests.json') -Raw | ConvertFrom-Json
    foreach($source in $coreDigests) {
        if((Get-FileHash -LiteralPath (Join-Path $project $source.file)).Hash.ToLowerInvariant() -ne $source.sha256) {
            throw "Core source changed; its previous coverage is no longer the UI-only baseline: $($source.file)"
        }
    }
    [ordered]@{timestampUtc=[DateTime]::UtcNow.ToString('o');status='PASS';inputDesktopBefore=$before;inputDesktopAfter=$after;testDesktop=$desktopName;desktopSwitchRequested=$false;coreSourceHashesUnchanged=$true;coreSourceCount=$coreDigests.Count;records=$records;visualReview='PENDING'} |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $evidence 'validation.json') -Encoding utf8
} catch {
    [ordered]@{timestampUtc=[DateTime]::UtcNow.ToString('o');status='FAIL';error=$_.Exception.Message;records=$records;visualReview='NOT_READY'} |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $evidence 'validation.json') -Encoding utf8
    throw
} finally {
    # The job owns only this run's children. Closing it also ends a hung child;
    # no user application is searched for or terminated.
    if($job -ne [IntPtr]::Zero) { $null=[IsolatedDesktop]::CloseHandle($job) }
    if($desktop -ne [IntPtr]::Zero) { $null=[IsolatedDesktop]::CloseDesktop($desktop) }
    foreach($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
