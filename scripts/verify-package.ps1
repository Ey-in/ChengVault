#requires -Version 7.2
param(
    [string]$DistributionDirectory=(Join-Path $PSScriptRoot '..\dist'),
    [string]$DeploymentEvidence=(Join-Path $PSScriptRoot '..\evidence\deployment-results.json'),
    [string]$ExpectedCommit='',
    [switch]$SourceBundle
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distribution=[IO.Path]::GetFullPath($DistributionDirectory)
$runtime=Join-Path $distribution 'ChengVault-1.0-windows-x64'
if(-not $ExpectedCommit) { $ExpectedCommit=(& git -C $project rev-parse HEAD).Trim() }
if($ExpectedCommit -notmatch '^[0-9a-f]{40}$') { throw 'An exact Git SHA-1 commit is required.' }
$objectType=& git -C $project cat-file -t $ExpectedCommit
if($LASTEXITCODE -ne 0 -or $objectType -ne 'commit') { throw 'The expected commit does not exist in this source repository.' }
$info=Get-Content -LiteralPath (Join-Path $runtime 'build-info.json') -Raw|ConvertFrom-Json
if($info.sourceCommit -ne $ExpectedCommit) { throw 'Runtime build-info does not match the expected committed source.' }
$manifest=Get-Content -LiteralPath (Join-Path $runtime 'checksums.json') -Raw|ConvertFrom-Json
$deployment=Get-Content -LiteralPath $DeploymentEvidence -Raw|ConvertFrom-Json
if(@($deployment.records).Count -eq 0 -or @($deployment.records|Where-Object { -not $_.passed }).Count -ne 0) { throw 'Deployment evidence is empty or contains a failed check.' }
function Hex([byte[]]$Bytes) { return [Convert]::ToHexString($Bytes).ToLowerInvariant() }
function Zip-Bytes($Entry) {
    $stream=$Entry.Open(); $buffer=[IO.MemoryStream]::new()
    try { $stream.CopyTo($buffer); return ,$buffer.ToArray() } finally { $stream.Dispose(); $buffer.Dispose() }
}
function Checked-RuntimePath([string]$Relative) {
    $absolute=[IO.Path]::GetFullPath((Join-Path $runtime $Relative))
    if(-not $absolute.StartsWith($runtime+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe manifest path: $Relative" }
    return $absolute
}

# Validate both the unpacked files and their compressed ZIP representation.
# checksums.json cannot contain its own digest, so compare its ZIP bytes with
# the on-disk manifest separately and reject any extra unlisted payload files.
$runtimeZip=[IO.Compression.ZipFile]::OpenRead((Join-Path $distribution 'ChengVault-1.0-windows-x64.zip'))
$known=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$runtimeFiles=0
try {
    foreach($item in $manifest) {
        if(-not $known.Add($item.path)) { throw "Duplicate manifest path: $($item.path)" }
        $path=Checked-RuntimePath $item.path
        $file=Get-Item -LiteralPath $path
        if($file.Length -ne $item.bytes -or (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $item.sha256) { throw "Runtime checksum mismatch: $($item.path)" }
        $entry=$runtimeZip.GetEntry('ChengVault-1.0-windows-x64/'+$item.path.Replace('\','/'))
        if($null -eq $entry -or $entry.Length -ne $item.bytes) { throw "ZIP entry length mismatch: $($item.path)" }
        if((Hex ([Security.Cryptography.SHA256]::HashData((Zip-Bytes $entry)))) -ne $item.sha256) { throw "ZIP checksum mismatch: $($item.path)" }
        ++$runtimeFiles
    }
    $null=$known.Add('checksums.json')
    $manifestEntry=$runtimeZip.GetEntry('ChengVault-1.0-windows-x64/checksums.json')
    if($null -eq $manifestEntry -or (Hex ([Security.Cryptography.SHA256]::HashData((Zip-Bytes $manifestEntry)))) -ne (Get-FileHash -LiteralPath (Join-Path $runtime 'checksums.json')).Hash.ToLowerInvariant()) { throw 'ZIP manifest differs from the unpacked manifest.' }
    $diskFiles=@(Get-ChildItem -LiteralPath $runtime -Recurse -File)
    $zipFiles=@($runtimeZip.Entries|Where-Object { $_.Name -ne '' })
    if($diskFiles.Count -ne $known.Count -or $zipFiles.Count -ne $known.Count) { throw 'Runtime directory or ZIP contains unexpected files.' }
    foreach($file in $diskFiles) { if(-not $known.Contains([IO.Path]::GetRelativePath($runtime,$file.FullName))) { throw "Unlisted runtime file: $($file.FullName)" } }
} finally { $runtimeZip.Dispose() }

# A successful test of an earlier executable does not validate newly linked
# bytes. Bind each of the four applications to the latest deployment run.
$applicationHashes=foreach($name in @('ChengVault.exe','ChengVaultCli.exe','ChengVaultAgent.exe','ChengVaultServer.exe')) {
    $actual=(Get-FileHash -LiteralPath (Join-Path $runtime $name)).Hash.ToLowerInvariant()
    $expected=@($deployment.applicationHashes|Where-Object { $_.name -eq $name })
    if($expected.Count -ne 1 -or $expected[0].sha256 -ne $actual) { throw "Application bytes have no matching deployment evidence: $name" }
    [ordered]@{name=$name;sha256=$actual}
}
$sourceFiles=0; $bundleText=$null
if($SourceBundle) {
    $tree=& git -c core.quotepath=false -C $project ls-tree -r --format='%(objectmode) %(objectname) %(path)' $ExpectedCommit
    if($LASTEXITCODE -ne 0) { throw 'Cannot read the expected Git tree.' }
    $sourceZip=[IO.Compression.ZipFile]::OpenRead((Join-Path $distribution 'ChengVault-1.0-source.zip'))
    try {
        foreach($line in $tree) {
            if($line -notmatch '^(100[0-9]{3}) ([0-9a-f]{40}) (.+)$') { throw "Unexpected Git entry: $line" }
            $blob=$Matches[2]; $relative=$Matches[3]
            if($relative -match '^(\.toolchain/|build[^/]*/|tmp/|dist/)' -or $relative -match '(^|/)(connection-private\.json|[^/]+\.(password|key))$') { throw "Private or generated payload in source tree: $relative" }
            $entry=$sourceZip.GetEntry('ChengVault/'+$relative)
            if($null -eq $entry) { throw "Source file absent from ZIP: $relative" }
            $bytes=Zip-Bytes $entry
            $header=[Text.Encoding]::ASCII.GetBytes('blob '+$bytes.Length+[char]0)
            $hash=[Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA1)
            try {
                $hash.AppendData($header); $hash.AppendData($bytes)
                if((Hex ($hash.GetHashAndReset())) -ne $blob) { throw "Source ZIP differs from exact committed bytes: $relative" }
            } finally { $hash.Dispose() }
            ++$sourceFiles
        }
        if(@($sourceZip.Entries|Where-Object { $_.Name -ne '' }).Count -ne $sourceFiles) { throw 'Source ZIP contains extra files outside the Git tree.' }
    } finally { $sourceZip.Dispose() }
    $bundlePath=Join-Path $distribution 'ChengVault-history.bundle'
    # `bundle verify` can accept a thin bundle when this working repository
    # supplies its prerequisite objects. A delivery bundle must stand alone.
    $reader=[IO.StreamReader]::new($bundlePath,[Text.Encoding]::ASCII)
    try {
        $signature=$reader.ReadLine()
        if($signature -notmatch '^# v[23] git bundle$') { throw 'Unsupported Git bundle header.' }
        while(($line=$reader.ReadLine()) -ne '') {
            if($null -eq $line) { throw 'Truncated Git bundle header.' }
            if($line.StartsWith('-')) { throw 'History bundle requires external prerequisite commits.' }
        }
    } finally { $reader.Dispose() }
    $bundleText=(& git -C $project bundle verify $bundlePath 2>&1|Out-String).Trim()
    if($LASTEXITCODE -ne 0) { throw "Git bundle verification failed: $bundleText" }
    $heads=& git -C $project bundle list-heads $bundlePath
    if($LASTEXITCODE -ne 0 -or -not @($heads|Where-Object { $_ -match ('^'+$ExpectedCommit+' (HEAD|refs/heads/)') }).Count) { throw 'Git history bundle omits the expected source revision.' }
}
$artifacts=foreach($name in @('ChengVault-1.0-windows-x64.zip')+$(if($SourceBundle){@('ChengVault-1.0-source.zip','ChengVault-history.bundle')}else{@()})) {
    $file=Get-Item -LiteralPath (Join-Path $distribution $name)
    [ordered]@{name=$name;path=$file.FullName;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()}
}
$report=[ordered]@{
    timestampUtc=[DateTime]::UtcNow.ToString('o');sourceCommit=$ExpectedCommit
    runtimeManifestAndZipFilesVerified=$runtimeFiles;sourceBundleVerified=[bool]$SourceBundle
    sourceGitBlobHashesVerified=$sourceFiles;applicationHashesMatchDeploymentEvidence=$true
    bundleVerification=$bundleText;applicationHashes=$applicationHashes;artifacts=$artifacts
}
$report|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $distribution 'package-verification.json') -Encoding utf8
Write-Host "Verified $runtimeFiles runtime files and $sourceFiles exact Git source blobs at $ExpectedCommit."
