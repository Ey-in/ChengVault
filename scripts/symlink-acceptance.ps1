#requires -Version 7.2
param(
    [string]$RuntimeDirectory='',
    [string]$EvidenceDirectory=(Join-Path $PSScriptRoot '..\tmp\symlink-acceptance')
)
$ErrorActionPreference='Stop'
if(-not $IsWindows) { throw 'Symbolic-link acceptance requires Windows and NTFS.' }
if([string]::IsNullOrWhiteSpace($RuntimeDirectory)) {
    $parent=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $RuntimeDirectory=if(Test-Path -LiteralPath (Join-Path $parent 'ChengVaultCli.exe')) { $parent } else { Join-Path $parent 'dist\ChengVault-1.0-windows-x64' }
}
$runtime=[IO.Path]::GetFullPath($RuntimeDirectory)
$cli=Join-Path $runtime 'ChengVaultCli.exe'
if(-not (Test-Path -LiteralPath $cli -PathType Leaf)) { throw "Missing portable CLI: $cli" }
$run=Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('run-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
[IO.Directory]::CreateDirectory($run)|Out-Null
$records=[Collections.Generic.List[object]]::new()
$checks=[Collections.Generic.List[object]]::new()
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
$report=[ordered]@{
    timestampUtc=[DateTime]::UtcNow.ToString('o');result='FAIL';passed=0;failed=0;skipped=0
    hostName=[Environment]::MachineName;os=[Environment]::OSVersion.VersionString
    fileSystem=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($run)).DriveFormat
    currentUserSid=$identity.User.Value;administratorToken=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    createsWithAllowUnprivilegedFlag=$true;changesSystemPolicy=$false
    executable=$cli;executableSha256=(Get-FileHash -LiteralPath $cli).Hash.ToLowerInvariant()
    sourceCommit=(Get-Content -LiteralPath (Join-Path $runtime 'build-info.json') -Raw|ConvertFrom-Json).sourceCommit
    creationResults=$records;restorationChecks=$checks;evidenceDirectory=$run
}
function Invoke-Cli([string]$Name,[string[]]$Arguments) {
    $start=[Diagnostics.ProcessStartInfo]::new($cli)
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WorkingDirectory=$run
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false); $start.StandardErrorEncoding=[Text.UTF8Encoding]::new($false)
    $start.Environment['PATH']=$env:SystemRoot+';'+(Join-Path $env:SystemRoot 'System32')
    foreach($environmentName in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML2_IMPORT_PATH','QML_IMPORT_PATH','QT_QPA_PLATFORM','CHENGVAULT_PASSWORD')) { $null=$start.Environment.Remove($environmentName) }
    foreach($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process=[Diagnostics.Process]::new(); $process.StartInfo=$start
    if(-not $process.Start()) { throw "Could not start $Name" }
    $out=$process.StandardOutput.ReadToEndAsync(); $err=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(120000)) { $process.Kill($true); throw "$Name timed out" }
    $text=$out.GetAwaiter().GetResult(); $errorText=$err.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $run ($Name+'.stdout.txt')),$text,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $run ($Name+'.stderr.txt')),$errorText,[Text.UTF8Encoding]::new($false))
    if($process.ExitCode -ne 0) { throw "$Name failed with exit $($process.ExitCode): $errorText" }
    return $text|ConvertFrom-Json
}
try {
    if($report.fileSystem -ne 'NTFS') { throw 'The evidence directory must be on NTFS.' }
    if(-not ('ChengVaultAcceptance.NativeLinks' -as [type])) {
        # This tiny interop declaration is infrastructure only. Creation is
        # attempted using the current token, without elevation or policy edits.
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace ChengVaultAcceptance {
    public static class NativeLinks {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool CreateSymbolicLinkW(string link, string target, uint flags);
    }
}
'@
    }
    $source=Join-Path $run 'source'; [IO.Directory]::CreateDirectory($source)|Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'payload.txt'),'Symbolic link content target.',[Text.UTF8Encoding]::new($false))
    [IO.Directory]::CreateDirectory((Join-Path $source 'payload-dir'))|Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'payload-dir\nested.txt'),'Directory symbolic link target.',[Text.UTF8Encoding]::new($false))
    foreach($spec in @(@{name='file-link';target='payload.txt';directory=$false;flags=2},@{name='directory-link';target='payload-dir';directory=$true;flags=3})) {
        $created=[ChengVaultAcceptance.NativeLinks]::CreateSymbolicLinkW((Join-Path $source $spec.name),$spec.target,$spec.flags)
        $code=if($created) {0} else {[Runtime.InteropServices.Marshal]::GetLastWin32Error()}
        $state=if($created) {'CREATED'} elseif($code -in @(5,1314)) {'SKIP_PERMISSION'} else {'FAIL'}
        $records.Add([ordered]@{name=$spec.name;target=$spec.target;directory=$spec.directory;flags=$spec.flags;created=$created;win32Error=$code;message=if($code){[ComponentModel.Win32Exception]::new($code).Message}else{''};status=$state})
    }
    $creationErrors=@($records|Where-Object { $_.status -eq 'FAIL' })
    if($creationErrors.Count -gt 0) { throw 'CreateSymbolicLinkW failed for a reason other than permission; inspect creationResults.' }
    $createdLinks=@($records|Where-Object { $_.created })
    $report.skipped=2-$createdLinks.Count
    if($createdLinks.Count -gt 0) {
        $archive=Join-Path $run 'links.cva'; $destination=Join-Path $run 'restored'
        $null=Invoke-Cli 'backup' @('backup','--source',$source,'--output',$archive,'--format','indexed','--compression','none','--cipher','none')
        $restored=Invoke-Cli 'restore' @('restore','--input',$archive,'--output',$destination)
        if(-not $restored.details.metadataStrict -or $restored.warnings.Count -ne 0) { throw 'Restore was not strict and warning-free.' }
        foreach($created in $createdLinks) {
            $path=Join-Path $destination ('source\'+$created.name)
            $item=Get-Item -LiteralPath $path -Force
            $attributes=[IO.File]::GetAttributes($path)
            $isDirectory=($attributes -band [IO.FileAttributes]::Directory) -ne 0
            $isReparse=($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            # LinkType distinguishes a true symlink from a Junction or copied
            # content. LinkTarget compares the stored relative target exactly.
            $passed=$isReparse -and $item.LinkType -eq 'SymbolicLink' -and $item.LinkTarget -ceq $created.target -and $isDirectory -eq $created.directory
            $checks.Add([ordered]@{name=$created.name;passed=$passed;linkType=$item.LinkType;linkTarget=$item.LinkTarget;expectedTarget=$created.target;directory=$isDirectory;reparsePoint=$isReparse})
            if(-not $passed) { throw "Restored link type or target differs: $($created.name)" }
            ++$report.passed
        }
    }
    $report.result=if($report.skipped -eq 0) {'PASS'} elseif($report.passed -eq 0) {'SKIP_PERMISSION'} else {'PARTIAL_WITH_PERMISSION_SKIP'}
} catch {
    $report.result='FAIL'; $report.failed=1; $report['error']=$_.Exception.Message
    throw
} finally {
    $output=Join-Path $run 'symlink-results.json'
    $report|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $output -Encoding utf8
    Write-Host "$($report.result): $($report.passed) passed, $($report.failed) failed, $($report.skipped) skipped. Evidence: $output"
    $identity.Dispose()
}
