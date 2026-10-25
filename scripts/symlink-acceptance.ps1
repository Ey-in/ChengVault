#requires -Version 7.2
param(
    [string]$RuntimeDirectory='',
    [string]$EvidenceDirectory=(Join-Path $PSScriptRoot '..\tmp\symlink-acceptance'),
    [string]$TrackedEvidencePath='',
    [switch]$RedactIdentifiers
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
$devValue=$null
try {
    $devValue=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -Name AllowDevelopmentWithoutDevLicense -ErrorAction Stop).AllowDevelopmentWithoutDevLicense
} catch {
    $devValue=$null
}
$report=[ordered]@{
    timestampUtc=[DateTime]::UtcNow.ToString('o');result='FAIL';passed=0;failed=0;skipped=0;expectedLinks=0
    hostName=[Environment]::MachineName;os=[Environment]::OSVersion.VersionString
    fileSystem=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($run)).DriveFormat
    currentUserSid=$identity.User.Value;administratorToken=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    developerModeEnabled=($devValue -eq 1);developerModeRegistryValue=$devValue
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
if(-not ('ChengVaultAcceptance.NativeLinks' -as [type])) {
    # Interop is infrastructure only. Attribute writes use a non-following
    # handle so a symbolic link's target can never be modified (W31).
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace ChengVaultAcceptance {
    [StructLayout(LayoutKind.Sequential)]
    public struct FileBasicInfo {
        public long CreationTime;
        public long LastAccessTime;
        public long LastWriteTime;
        public long ChangeTime;
        public uint FileAttributes;
    }
    public static class NativeLinks {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool CreateSymbolicLinkW(string link, string target, uint flags);

        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetFileInformationByHandleEx(IntPtr handle, int infoClass, out FileBasicInfo info, uint size);

        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetFileInformationByHandle(IntPtr handle, int infoClass, ref FileBasicInfo info, uint size);

        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CloseHandle(IntPtr handle);
    }
}
'@
}
$READ_ATTRIBUTES=[uint32]0x80
$WRITE_ATTRIBUTES=[uint32]0x100
$SHARE_ALL=[uint32]7
$OPEN_EXISTING=[uint32]3
$OPEN_FLAGS=[uint32](0x02000000 -bor 0x00200000)
$FILE_BASIC_INFO_CLASS=0
$LINK_MASK=[int64]0x417
$TARGET_MASK=[int64]0x27
function Open-LinkHandle([string]$Path,[uint32]$Access) {
    return [ChengVaultAcceptance.NativeLinks]::CreateFileW($Path,$Access,$SHARE_ALL,[IntPtr]::Zero,$OPEN_EXISTING,$OPEN_FLAGS,[IntPtr]::Zero)
}
function Get-LinkAttributes([string]$Path) {
    $handle=Open-LinkHandle $Path $READ_ATTRIBUTES
    if($handle.ToInt64() -eq -1) {
        $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw ('Open for attributes failed: '+[ComponentModel.Win32Exception]::new($code).Message)
    }
    try {
        $info=[ChengVaultAcceptance.FileBasicInfo]::new()
        
        $size=[uint32][Runtime.InteropServices.Marshal]::SizeOf($info)
        if(-not [ChengVaultAcceptance.NativeLinks]::GetFileInformationByHandleEx($handle,$FILE_BASIC_INFO_CLASS,[ref]$info,$size)) {
            $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw ('Read basic info failed: '+[ComponentModel.Win32Exception]::new($code).Message)
        }
        return [int64]$info.FileAttributes
    } finally {
        [void][ChengVaultAcceptance.NativeLinks]::CloseHandle($handle)
    }
}
function Set-LinkAttributes([string]$Path,[uint32]$Attributes) {
    $handle=Open-LinkHandle $Path ([uint32]($READ_ATTRIBUTES -bor $WRITE_ATTRIBUTES))
    if($handle.ToInt64() -eq -1) {
        $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw ('Open for attribute write failed: '+[ComponentModel.Win32Exception]::new($code).Message)
    }
    try {
        $info=[ChengVaultAcceptance.FileBasicInfo]::new()
        
        $size=[uint32][Runtime.InteropServices.Marshal]::SizeOf($info)
        if(-not [ChengVaultAcceptance.NativeLinks]::GetFileInformationByHandleEx($handle,$FILE_BASIC_INFO_CLASS,[ref]$info,$size)) {
            $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw ('Read before write failed: '+[ComponentModel.Win32Exception]::new($code).Message)
        }
        $info.FileAttributes=$Attributes
        if(-not [ChengVaultAcceptance.NativeLinks]::SetFileInformationByHandle($handle,$FILE_BASIC_INFO_CLASS,[ref]$info,$size)) {
            $code=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
            throw ('Write attributes failed: '+[ComponentModel.Win32Exception]::new($code).Message)
        }
    } finally {
        [void][ChengVaultAcceptance.NativeLinks]::CloseHandle($handle)
    }
}
try {
    if($report.fileSystem -ne 'NTFS') { throw 'The evidence directory must be on NTFS.' }
    $source=Join-Path $run 'source'; [IO.Directory]::CreateDirectory($source)|Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'payload.txt'),'Symbolic link content target.',[Text.UTF8Encoding]::new($false))
    [IO.Directory]::CreateDirectory((Join-Path $source 'payload-dir'))|Out-Null
    [IO.File]::WriteAllText((Join-Path $source 'payload-dir\nested.txt'),'Directory symbolic link target.',[Text.UTF8Encoding]::new($false))

    $targetSnapshots=@{}
    foreach($targetName in @('payload.txt','payload-dir')) {
        $targetPath=Join-Path $source $targetName
        $before=[int64][IO.File]::GetAttributes($targetPath)
        [IO.File]::SetAttributes($targetPath,[IO.FileAttributes]([int64]$before -bor [int64][IO.FileAttributes]::Hidden))
        $after=[int64][IO.File]::GetAttributes($targetPath)
        $targetSnapshots[$targetName]=[ordered]@{path=$targetPath;before=$before;after=$after}
    }

    $specs=@(
        [ordered]@{name='file-link';target='payload.txt';directory=$false;flags=[uint32]2},
        [ordered]@{name='directory-link';target='payload-dir';directory=$true;flags=[uint32]3}
    )
    foreach($spec in $specs) {
        $linkPath=Join-Path $source $spec.name
        $attempts=[Collections.Generic.List[object]]::new()
        $created=$false
        $win32Error=0
        $flagsUsed=$spec.flags
        foreach($flags in @([uint32]$spec.flags,[uint32]($spec.flags -band (-bnot 2)))) {
            $created=[ChengVaultAcceptance.NativeLinks]::CreateSymbolicLinkW($linkPath,$spec.target,$flags)
            if($created) {
                $flagsUsed=$flags
                $win32Error=0
                $attempts.Add([ordered]@{flags=$flags;created=$true;win32Error=0})
                break
            }
            $win32Error=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
            $attempts.Add([ordered]@{flags=$flags;created=$false;win32Error=$win32Error})
            if($win32Error -notin @(5,1314)) { break }
        }
        $status='FAIL'
        if($created) { $status='CREATED' }
        elseif($win32Error -in @(5,1314)) { $status='SKIP_PERMISSION' }
        $message=''
        if($win32Error) { $message=[ComponentModel.Win32Exception]::new($win32Error).Message }
        $record=[ordered]@{
            name=$spec.name;target=$spec.target;directory=$spec.directory;flags=$spec.flags;flagsUsed=$flagsUsed
            attempts=$attempts;created=$created;win32Error=$win32Error;message=$message;status=$status
            sourceLinkAttributes=$null;targetSnapshot=$null
        }
        if($created) {
            $current=[int64](Get-LinkAttributes $linkPath)
            $wanted=[uint32]($current -bor [int64][IO.FileAttributes]::ReadOnly -bor [int64][IO.FileAttributes]::Hidden)
            Set-LinkAttributes $linkPath $wanted
            $record.sourceLinkAttributes=[int64](Get-LinkAttributes $linkPath)
            $record.targetSnapshot=$targetSnapshots[$spec.target]
            $targetNow=[int64][IO.File]::GetAttributes((Join-Path $source $spec.target))
            if(($targetNow -band $TARGET_MASK) -ne ($record.targetSnapshot.after -band $TARGET_MASK)) {
                throw ('Setting link attributes changed the target: '+$spec.name)
            }
        }
        $records.Add($record)
    }
    $creationErrors=@($records|Where-Object { $_.status -eq 'FAIL' })
    if($creationErrors.Count -gt 0) { throw 'CreateSymbolicLinkW failed for a reason other than permission; inspect creationResults.' }
    $createdLinks=@($records|Where-Object { $_.created })
    if($createdLinks.Count -gt 0) {
        $archive=Join-Path $run 'links.cva'; $destination=Join-Path $run 'restored'
        $null=Invoke-Cli 'backup' @('backup','--source',$source,'--output',$archive,'--format','indexed','--compression','none','--cipher','none')
        $restored=Invoke-Cli 'restore' @('restore','--input',$archive,'--output',$destination)
        if(-not $restored.details.metadataStrict -or $restored.warnings.Count -ne 0) { throw 'Restore was not strict and warning-free.' }
        foreach($created in $createdLinks) {
            $path=Join-Path $destination ('source\'+$created.name)
            $item=Get-Item -LiteralPath $path -Force
            $attributes=[int64](Get-LinkAttributes $path)
            $isDirectory=($attributes -band [int64][IO.FileAttributes]::Directory) -ne 0
            $isReparse=($attributes -band [int64][IO.FileAttributes]::ReparsePoint) -ne 0
            $expectedMask=[int64]($created.sourceLinkAttributes -band $LINK_MASK)
            $actualMask=[int64]($attributes -band $LINK_MASK)
            $targetPath=Join-Path $source $created.target
            $targetNow=[int64][IO.File]::GetAttributes($targetPath)
            $restoredTargetPath=Join-Path $destination ('source\'+$created.target)
            $restoredTarget=[int64][IO.File]::GetAttributes($restoredTargetPath)
            $targetUnchanged=((($created.targetSnapshot.after -band $TARGET_MASK) -eq ($targetNow -band $TARGET_MASK)))
            $targetMatch=((($created.targetSnapshot.after -band $TARGET_MASK) -eq ($restoredTarget -band $TARGET_MASK)))
            if($created.directory) {
                $payloadSource=Join-Path (Join-Path $source $created.target) 'nested.txt'
                $payloadRestored=Join-Path $path 'nested.txt'
            } else {
                $payloadSource=Join-Path $source $created.target
                $payloadRestored=$path
            }
            $payloadMatch=((Get-FileHash -LiteralPath $payloadSource -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $payloadRestored -Algorithm SHA256).Hash)
            $passed=($isReparse -and $item.LinkType -eq 'SymbolicLink' -and $item.LinkTarget -ceq $created.target -and $isDirectory -eq $created.directory -and $expectedMask -eq $actualMask -and $targetUnchanged -and $targetMatch -and $payloadMatch)
            $checks.Add([ordered]@{
                name=$created.name;passed=$passed;linkType=$item.LinkType;linkTarget=$item.LinkTarget;expectedTarget=$created.target
                directory=$isDirectory;reparsePoint=$isReparse
                sourceLinkAttributes=$created.sourceLinkAttributes;restoredLinkAttributes=$attributes
                expectedMask=$expectedMask;actualMask=$actualMask;linkAttributesMatch=($expectedMask -eq $actualMask)
                targetAttributesBefore=$created.targetSnapshot.before;targetAttributesAfterSetup=$created.targetSnapshot.after
                targetAttributesAfterRestore=$targetNow;restoredTargetAttributes=$restoredTarget
                targetAttributesUnchanged=$targetUnchanged;restoredTargetAttributesMatch=$targetMatch
                payloadMatch=$payloadMatch
            })
        }
    }
    $failedChecks=@($checks|Where-Object { -not $_.passed })
    if($failedChecks.Count -gt 0) {
        throw ('Symlink verification failed: '+(($failedChecks|ForEach-Object { $_.name }) -join ', '))
    }
    if($records.Count -eq 0) { throw 'No link creation attempts were recorded.' }
    $skippedCount=@($records|Where-Object { $_.status -eq 'SKIP_PERMISSION' }).Count
    if($skippedCount -eq $records.Count) {
        $report.result='SKIP_PERMISSION'
        $report['permissionHint']='Enable Settings > System > For developers > Developer Mode, or run once from an elevated PowerShell. Keep SKIP_PERMISSION honest.'
    } elseif($skippedCount -gt 0) {
        $report.result='PARTIAL_WITH_PERMISSION_SKIP'
        $report['permissionHint']='Some links were skipped for permission; rerun with Developer Mode or an elevated token for full coverage.'
    } else {
        $report.result='PASS'
    }
} catch {
    $report.result='FAIL'
    $report['error']=$_.Exception.Message
    throw
} finally {
    $report.expectedLinks=$records.Count
    $report.skipped=@($records|Where-Object { $_.status -eq 'SKIP_PERMISSION' }).Count
    $report.passed=@($checks|Where-Object { $_.passed }).Count
    $report.failed=@($checks|Where-Object { -not $_.passed }).Count
    if($report.failed -eq 0 -and $report.result -eq 'FAIL') {
        $remaining=$report.expectedLinks-$report.passed-$report.skipped
        if($remaining -lt 1) { $report.failed=1 } else { $report.failed=$remaining }
    }
    $output=Join-Path $run 'symlink-results.json'
    $report|ConvertTo-Json -Depth 32|Set-Content -LiteralPath $output -Encoding utf8
    if(-not [string]::IsNullOrWhiteSpace($TrackedEvidencePath)) {
        $tracked=[IO.Path]::GetFullPath($TrackedEvidencePath)
        $current=[ordered]@{}
        foreach($key in $report.Keys) { $current[$key]=$report[$key] }
        if($RedactIdentifiers) {
            $current.hostName='<redacted-host>'
            $current.currentUserSid='<redacted-user-sid>'
        }
        $history=@()
        if(Test-Path -LiteralPath $tracked) {
            try {
                $old=Get-Content -LiteralPath $tracked -Raw -Encoding utf8|ConvertFrom-Json -AsHashtable
                if($old.history) { $history+=@($old.history); $null=$old.Remove('history') }
                $history+=@($old)
            } catch {
                $history+=@{note='previous evidence could not be parsed';path=$tracked;error=$_.Exception.Message}
            }
        }
        $current['history']=$history
        [IO.File]::WriteAllText($tracked,($current|ConvertTo-Json -Depth 32),(New-Object Text.UTF8Encoding($false)))
    }
    Write-Host "$($report.result): $($report.passed) passed, $($report.failed) failed, $($report.skipped) skipped. Evidence: $output"
    $identity.Dispose()
}