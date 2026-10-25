#requires -Version 7.2
param(
    [string]$RuntimeDirectory=(Join-Path $PSScriptRoot '..\dist\ChengVault-1.0-windows-x64'),
    [string]$BuildDirectory=(Join-Path $PSScriptRoot '..\build-release'),
    [string]$Toolchain=(Join-Path $PSScriptRoot '..\.toolchain\root\ucrt64')
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtime=[IO.Path]::GetFullPath($RuntimeDirectory)
$evidence=Join-Path $project ('evidence\deployment\run-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$fixture=Join-Path $project ('tmp\deployment-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($evidence)|Out-Null
[IO.Directory]::CreateDirectory($fixture)|Out-Null
$records=[Collections.Generic.List[object]]::new()
$runtimePath=$env:SystemRoot+';'+(Join-Path $env:SystemRoot 'System32')
$password=[Guid]::NewGuid().ToString('N')
function Run([string]$Name,[string]$Executable,[string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.WorkingDirectory=$fixture
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    $info.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding=[Text.UTF8Encoding]::new($false)
    $info.Environment['PATH']=$runtimePath
    $info.Environment['CHENGVAULT_PASSWORD']=$password
    $info.Environment['CHENGVAULT_DATA_HOME']=Join-Path $fixture 'app-state'
    foreach($environmentName in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH','QT_QPA_PLATFORM')) { $null=$info.Environment.Remove($environmentName) }
    foreach($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process=[Diagnostics.Process]::new(); $process.StartInfo=$info
    $timer=[Diagnostics.Stopwatch]::StartNew()
    if(-not $process.Start()) { throw "Could not start $Name" }
    $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(120000)) { $process.Kill($true); throw "$Name exceeded 120 seconds" }
    $timer.Stop()
    $out=$stdout.GetAwaiter().GetResult(); $err=$stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $evidence ($Name+'.stdout.txt')),$out,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $evidence ($Name+'.stderr.txt')),$err,[Text.UTF8Encoding]::new($false))
    $records.Add([ordered]@{name=$Name;passed=($process.ExitCode -eq 0);exitCode=$process.ExitCode;durationMs=$timer.ElapsedMilliseconds;executable=$Executable;sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()})
    if($process.ExitCode -ne 0) { throw "$Name failed: $err" }
    Write-Host "$Name : PASS ($($timer.ElapsedMilliseconds) ms)"
}
try {
    $screens=Join-Path $evidence 'screenshots'
    Run 'runtime-gui' (Join-Path $runtime 'ChengVault.exe') @('--screenshot-dir',$screens)
    if(@(Get-ChildItem -LiteralPath $screens -Filter '*.png').Count -ne 8) { throw 'Actual runtime GUI did not render all eight pages.' }
    $source=Join-Path $fixture 'source'; [IO.Directory]::CreateDirectory($source)|Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $source '空目录'))|Out-Null
    $input=Join-Path $source '中文.txt'
    [IO.File]::WriteAllText($input,('Actual portable deployment backup and restore. '*1000),[Text.UTF8Encoding]::new($false))
    $archive=Join-Path $fixture 'portable.cva'; $restored=Join-Path $fixture 'restored'
    $cli=Join-Path $runtime 'ChengVaultCli.exe'
    Run 'runtime-backup' $cli @('backup','--source',$source,'--output',$archive,'--format','indexed','--compression','lz77','--cipher','chacha20')
    Run 'runtime-verify' $cli @('verify','--input',$archive)
    Run 'runtime-restore' $cli @('restore','--input',$archive,'--output',$restored)
    if((Get-FileHash -LiteralPath $input).Hash -ne (Get-FileHash -LiteralPath (Join-Path $restored 'source\中文.txt')).Hash) { throw 'Portable restore hash mismatch.' }
    if(-not (Test-Path -LiteralPath (Join-Path $restored 'source\空目录') -PathType Container)) { throw 'Portable restore omitted empty directory.' }
    $restoreReport=Get-Content -LiteralPath (Join-Path $evidence 'runtime-restore.stdout.txt') -Raw | ConvertFrom-Json
    if(-not $restoreReport.details.metadataStrict -or $restoreReport.warnings.Count -ne 0) { throw 'Portable restore metadata not strict and warning-free.' }

    & (Join-Path $PSScriptRoot 'network-smoke.ps1') -BinaryDirectory $runtime -EvidenceDirectory (Join-Path $evidence 'network') -RuntimeOnly
    $networkFile=Get-ChildItem -LiteralPath (Join-Path $evidence 'network') -Filter 'network-results.json' -Recurse | Select-Object -First 1
    $network=Get-Content -LiteralPath $networkFile.FullName -Raw | ConvertFrom-Json
    if(@($network.records | Where-Object { $_.passed -eq $false }).Count -gt 0) { throw 'Portable network smoke failed.' }
    Copy-Item -LiteralPath $networkFile.FullName -Destination (Join-Path $project 'evidence\deployment-network-tests.json')
    $records.Add([ordered]@{name='runtime-two-account-network';passed=$true;cases=$network.records.Count;report=$networkFile.FullName})

    # Test executables are validation harnesses, not shipped application files.
    # Run against a byte-identical runtime copy. Only their QtTest dependency is
    # added; development PATH and plugin paths remain absent in every process.
    $validation=Join-Path $fixture 'validation'
    Copy-Item -LiteralPath $runtime -Destination $validation -Recurse
    Copy-Item -LiteralPath (Join-Path $Toolchain 'bin\Qt6Test.dll') -Destination (Join-Path $validation 'Qt6Test.dll')
    foreach($test in @('ChengVaultGuiTests','ChengVaultBackgroundTests')) {
        Copy-Item -LiteralPath (Join-Path $BuildDirectory ($test+'.exe')) -Destination (Join-Path $validation ($test+'.exe'))
        $output=Join-Path $evidence ($test+'.json')
        Run $test (Join-Path $validation ($test+'.exe')) @('--output',$output)
        $result=Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
        if($result.failed -ne 0 -or $result.skipped -ne 0) { throw "$test had failed or skipped cases." }
        $compactName=if($test -eq 'ChengVaultGuiTests') { 'deployment-gui-tests.json' } else { 'deployment-background-tests.json' }
        Copy-Item -LiteralPath $output -Destination (Join-Path $project ('evidence\'+$compactName))
        $records.Add([ordered]@{name=($test+'-cases');passed=$true;cases=$result.passed;failed=$result.failed;skipped=$result.skipped;report=$output})
    }
} catch {
    $records.Add([ordered]@{name='deployment-failure';passed=$false;error=$_.Exception.Message})
    throw
} finally {
    $applicationHashes=foreach($application in @('ChengVault.exe','ChengVaultCli.exe','ChengVaultAgent.exe','ChengVaultServer.exe')) {
        [ordered]@{name=$application;sha256=(Get-FileHash -LiteralPath (Join-Path $runtime $application) -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $report=[ordered]@{timestampUtc=[DateTime]::UtcNow.ToString('o');runtimeDirectory=$runtime;evidenceDirectory=$evidence;applicationHashes=$applicationHashes;childPath=$runtimePath;clearedPluginEnvironment=$true;independentMachine=$false;testHarnessOnlyExtraDll='Qt6Test.dll';records=$records}
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $project 'evidence\deployment-results.json') -Encoding utf8
    Write-Host "Deployment evidence: $evidence"
}
