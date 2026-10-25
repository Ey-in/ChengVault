#requires -Version 7.2
param(
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '..\build-release'),
    [string]$EvidenceDirectory = (Join-Path $PSScriptRoot '..\evidence\network'),
    [switch]$RuntimeOnly
)
$ErrorActionPreference = 'Stop'
$bin = [IO.Path]::GetFullPath($BinaryDirectory)
$cli = Join-Path $bin 'ChengVaultCli.exe'
$serverExe = Join-Path $bin 'ChengVaultServer.exe'
$bundledToolchain = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\.toolchain\root\ucrt64\bin'))
$childPath = if ($RuntimeOnly) { $env:SystemRoot+';'+(Join-Path $env:SystemRoot 'System32') } elseif (Test-Path -LiteralPath $bundledToolchain) { $bundledToolchain + ';' + $env:PATH } else { $env:PATH }
if (-not (Test-Path -LiteralPath $cli) -or -not (Test-Path -LiteralPath $serverExe)) { throw 'Build CLI and Server first.' }
$runRoot = Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$credentialDirectory = Join-Path $runRoot 'temporary-credentials'
[IO.Directory]::CreateDirectory($credentialDirectory) | Out-Null
$records = [Collections.Generic.List[object]]::new()
$backupPassword = [Guid]::NewGuid().ToString('N')
$alicePassword = [Guid]::NewGuid().ToString('N')
$bobPassword = [Guid]::NewGuid().ToString('N')
$serverProcess = $null

function Invoke-Child([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$LoginPassword = '', [bool]$ExpectFailure = $false) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding=[Text.UTF8Encoding]::new($false)
    $start.Environment['CHENGVAULT_PASSWORD']=$backupPassword
    $start.Environment['CHENGVAULT_LOGIN_PASSWORD']=$LoginPassword
    $start.Environment['PATH']=$childPath
    if ($RuntimeOnly) {
        foreach ($environmentName in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH','QT_QPA_PLATFORM')) { $null=$start.Environment.Remove($environmentName) }
    }
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process=[Diagnostics.Process]::new(); $process.StartInfo=$start
    $timer=[Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw 'Child process could not start.' }
    $outputTask=$process.StandardOutput.ReadToEndAsync(); $errorTask=$process.StandardError.ReadToEndAsync()
    [long]$peak=0
    while (-not $process.WaitForExit(100)) { $process.Refresh(); try { $peak=[Math]::Max($peak,$process.PeakWorkingSet64) } catch {} }
    $timer.Stop()
    try { $process.Refresh(); $peak=[Math]::Max($peak,$process.PeakWorkingSet64) } catch {}
    $outputText=$outputTask.GetAwaiter().GetResult(); $errorText=$errorTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $runRoot ($Name+'.stdout.txt')),$outputText,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $runRoot ($Name+'.stderr.txt')),$errorText,[Text.UTF8Encoding]::new($false))
    $parsed=$null; try { $parsed=$outputText | ConvertFrom-Json } catch {}
    $passed=if ($ExpectFailure) { $process.ExitCode -ne 0 } else { $process.ExitCode -eq 0 }
    $records.Add([ordered]@{name=$Name;timestampUtc=[DateTime]::UtcNow.ToString('o');exitCode=$process.ExitCode;expectedFailure=$ExpectFailure;passed=$passed;durationMs=$timer.ElapsedMilliseconds;peakWorkingSetBytes=$(if ($peak -gt 0) { $peak } else { $null });peakWorkingSetSamplingMs=100;result=$parsed})
    Write-Host "$Name : exit=$($process.ExitCode), expected-failure=$ExpectFailure, elapsed=$($timer.ElapsedMilliseconds)ms"
    if (-not $passed) { throw "$Name did not produce the expected result." }
    return $parsed
}

try {
    $serverRoot=Join-Path $runRoot 'server-storage'
    foreach ($user in @('alice','bob')) {
        $password=if ($user -eq 'alice') { $alicePassword } else { $bobPassword }
        $passwordFile=Join-Path $credentialDirectory ($user+'-password.txt')
        [IO.File]::WriteAllText($passwordFile,$password,[Text.UTF8Encoding]::new($false))
        $arguments=@('user-add','--root',$serverRoot,'--user',$user,'--password-file',$passwordFile)
        if ($user -eq 'alice') { $arguments += '--administrator' }
        $null=Invoke-Child ($user+'-create') $serverExe $arguments
        Remove-Item -LiteralPath $passwordFile
        $keyFile=Join-Path $credentialDirectory ($user+'-device.hex')
        $null=Invoke-Child ($user+'-device') $serverExe @('device-add','--root',$serverRoot,'--user',$user,'--device','desktop','--key-file',$keyFile)
    }

    # Launch only this owned helper, hidden, and capture its actual selected port.
    $serverStart=[Diagnostics.ProcessStartInfo]::new($serverExe)
    $serverStart.UseShellExecute=$false; $serverStart.CreateNoWindow=$true
    $serverStart.RedirectStandardOutput=$true; $serverStart.RedirectStandardError=$true
    $serverStart.Environment['PATH']=$childPath
    if ($RuntimeOnly) {
        foreach ($environmentName in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH','QT_QPA_PLATFORM')) { $null=$serverStart.Environment.Remove($environmentName) }
    }
    foreach ($argument in @('serve','--root',$serverRoot,'--listen','127.0.0.1','--port','0')) { $serverStart.ArgumentList.Add($argument) }
    $serverProcess=[Diagnostics.Process]::new(); $serverProcess.StartInfo=$serverStart
    if (-not $serverProcess.Start()) { throw 'Server could not start.' }
    $readyTask=$serverProcess.StandardOutput.ReadLineAsync(); $readyTimer=[Diagnostics.Stopwatch]::StartNew()
    while (-not $readyTask.IsCompleted -and $readyTimer.ElapsedMilliseconds -lt 15000) { Start-Sleep -Milliseconds 50 }
    if (-not $readyTask.IsCompleted) { throw 'Server readiness timeout.' }
    $ready=$readyTask.GetAwaiter().GetResult()
    if ($ready -notmatch '^READY (\d+) ') { throw 'Server did not report a listening port.' }
    $port=[int]$Matches[1]
    [IO.File]::WriteAllText((Join-Path $runRoot 'server-ready.txt'),$ready)
    foreach ($user in @('alice','bob')) {
        $hex=[IO.File]::ReadAllText((Join-Path $credentialDirectory ($user+'-device.hex'))).Trim()
        $config=[ordered]@{host='127.0.0.1';port=$port;user=$user;deviceId='desktop';deviceKey=[Convert]::ToBase64String([Convert]::FromHexString($hex))}
        $config | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $credentialDirectory ($user+'.json')) -Encoding utf8
    }
    $aliceConfig=Join-Path $credentialDirectory 'alice.json'; $bobConfig=Join-Path $credentialDirectory 'bob.json'
    $source=Join-Path $runRoot 'source'; [IO.Directory]::CreateDirectory($source) | Out-Null
    $random=[Random]::new(20261017); $buffer=[byte[]]::new(1048576)
    $sourceFile=Join-Path $source 'eight-mib.bin'
    $stream=[IO.File]::Open($sourceFile,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { for ($i=0;$i -lt 8;++$i) { $random.NextBytes($buffer); $stream.Write($buffer,0,$buffer.Length) } } finally { $stream.Dispose() }
    $repo=Join-Path $runRoot 'repository'
    $null=Invoke-Child 'local-initial-backup' $cli @('repo-backup','--source',$source,'--repo',$repo,'--compression','none','--cipher','chacha20')
    $initial=Invoke-Child 'network-initial-upload' $cli @('remote-upload','--repo',$repo,'--remote-config',$aliceConfig) $alicePassword
    $again=Invoke-Child 'network-repeat-upload' $cli @('remote-upload','--repo',$repo,'--remote-config',$aliceConfig) $alicePassword
    if ($again.bytes -ne 0 -or $again.details.uploadedObjects -ne 0) { throw 'Repeated upload retransferred object data.' }

    $stream=[IO.File]::Open($sourceFile,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try { $stream.Position=1048576+79; $original=$stream.ReadByte(); $stream.Position=1048576+79; $stream.WriteByte($original -bxor 255) } finally { $stream.Dispose() }
    $localChanged=Invoke-Child 'local-changed-backup' $cli @('repo-backup','--source',$source,'--repo',$repo,'--compression','none')
    if ($localChanged.details.newChunks -ne 1) { throw 'A one-byte edit changed more than one raw data block.' }
    $incremental=Invoke-Child 'network-incremental-upload' $cli @('remote-upload','--repo',$repo,'--remote-config',$aliceConfig) $alicePassword
    if ($incremental.details.uploadedObjects -ne 2) { throw 'Expected one encrypted block and one encrypted manifest.' }
    $list=Invoke-Child 'alice-list' $cli @('remote-list','--remote-config',$aliceConfig) $alicePassword
    $bobList=Invoke-Child 'bob-list' $cli @('remote-list','--remote-config',$bobConfig) $bobPassword
    if (@($list.repositories).Count -ne 1 -or @($bobList.repositories).Count -ne 0) { throw 'Account isolation failed.' }
    $remoteId=$list.repositories[0].id
    $download=Join-Path $runRoot 'download'
    $null=Invoke-Child 'alice-download' $cli @('remote-download','--remote-id',$remoteId,'--output',$download,'--remote-config',$aliceConfig) $alicePassword
    $null=Invoke-Child 'download-verify' $cli @('repo-verify','--repo',$download)
    $snapshots=Invoke-Child 'download-snapshots' $cli @('repo-list','--repo',$download)
    $restore=Join-Path $runRoot 'restored'
    $null=Invoke-Child 'download-restore' $cli @('repo-restore','--repo',$download,'--snapshot',$snapshots.snapshots[0].id,'--output',$restore)
    if ((Get-FileHash -LiteralPath $sourceFile).Hash -ne (Get-FileHash -LiteralPath (Join-Path $restore 'source\eight-mib.bin')).Hash) { throw 'Remote restore differs from source.' }
    $null=Invoke-Child 'bob-forbidden-download' $cli @('remote-download','--remote-id',$remoteId,'--output',(Join-Path $runRoot 'bob-forbidden'),'--remote-config',$bobConfig) $bobPassword $true
    $null=Invoke-Child 'wrong-login-password' $cli @('remote-list','--remote-config',$aliceConfig) 'intentionally incorrect test password' $true
    $records.Add([ordered]@{name='network-transfer-comparison';passed=$true;initialPayloadBytes=$initial.bytes;repeatPayloadBytes=$again.bytes;incrementalPayloadBytes=$incremental.bytes;initialFramedBytes=$initial.details.sentBytes;repeatFramedBytes=$again.details.sentBytes;incrementalFramedBytes=$incremental.details.sentBytes})
} catch {
    $records.Add([ordered]@{ name='network-smoke-failure'; passed=$false; error=$_.Exception.Message })
    throw
} finally {
    if ($null -ne $serverProcess) {
        if (-not $serverProcess.HasExited) { $serverProcess.Kill($true); $serverProcess.WaitForExit() }
        $serverErrors=$serverProcess.StandardError.ReadToEnd()
        [IO.File]::WriteAllText((Join-Path $runRoot 'server.stderr.txt'),$serverErrors,[Text.UTF8Encoding]::new($false))
        $serverProcess.Dispose()
    }
    # Remove only files created in this run's explicit credential directory.
    # Preserve datasets, encrypted stores and measured outputs as evidence.
    if (Test-Path -LiteralPath $credentialDirectory) {
        foreach ($file in Get-ChildItem -LiteralPath $credentialDirectory -File) { Remove-Item -LiteralPath $file.FullName }
        Remove-Item -LiteralPath $credentialDirectory
    }
    [ordered]@{timestampUtc=[DateTime]::UtcNow.ToString('o');runDirectory=$runRoot;runtimeOnly=[bool]$RuntimeOnly;childPath=$childPath;cliSha256=(Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash.ToLowerInvariant();serverSha256=(Get-FileHash -LiteralPath $serverExe -Algorithm SHA256).Hash.ToLowerInvariant();records=$records} | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $runRoot 'network-results.json') -Encoding utf8
    Write-Host "Evidence: $runRoot"
}
