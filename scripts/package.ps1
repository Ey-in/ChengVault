#requires -Version 7.2
param(
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '..\build-release'),
    [string]$Toolchain = (Join-Path $PSScriptRoot '..\.toolchain\root\ucrt64'),
    [switch]$SourceBundle
)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$binaryRoot=[IO.Path]::GetFullPath($BinaryDirectory)
$toolchainRoot=[IO.Path]::GetFullPath($Toolchain)
if(-not $PSBoundParameters.ContainsKey('Toolchain') -and -not (Test-Path -LiteralPath (Join-Path $toolchainRoot 'bin\windeployqt6.exe'))) {
    $toolchainRoot='D:\msys2\ucrt64'
}
$distributionRoot=Join-Path $projectRoot 'dist'
$target=Join-Path $distributionRoot 'ChengVault-1.0-windows-x64'
$stage=Join-Path $distributionRoot ('.package-stage-'+[Guid]::NewGuid().ToString('N'))
foreach($path in @($target,$stage)) {
    if(-not ([IO.Path]::GetFullPath($path).StartsWith($distributionRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase))) { throw 'Distribution path escaped the project dist directory.' }
}
$deploy=Join-Path $toolchainRoot 'bin\windeployqt6.exe'
$objdump=Join-Path $toolchainRoot 'bin\objdump.exe'
foreach($tool in @($deploy,$objdump)) { if(-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" } }
if($SourceBundle) {
    $trackedChanges=& git -C $projectRoot status --porcelain --untracked-files=no
    if($trackedChanges) { throw 'Commit final tracked-source changes before requesting -SourceBundle.' }
}
$oldPath=$env:PATH
$env:PATH=(Join-Path $toolchainRoot 'bin')+';'+$oldPath
[IO.Directory]::CreateDirectory($stage)|Out-Null
try {
    $executables=@('ChengVault.exe','ChengVaultCli.exe','ChengVaultAgent.exe','ChengVaultServer.exe')
    foreach($executable in $executables) {
        $source=Join-Path $binaryRoot $executable
        if(-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Build all four applications first: $source" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $stage $executable)
    }
    # Only infrastructure plugins are deployed. Network backup deliberately uses
    # project-owned authenticated frames and does not load a Qt TLS backend.
    $deployArgs=@('--release','--no-translations','--no-compiler-runtime','--no-opengl-sw','--no-ffmpeg',
        '--no-system-d3d-compiler','--no-system-dxc-compiler','--no-sql','--no-dbus','--no-test',
        '--skip-plugin-types','tls,sqldrivers,networkinformation,generic,designer',
        '--exclude-plugins','qdirect2d,qminimal,qoffscreen','--dir',$stage,
        '--plugindir',(Join-Path $stage 'plugins'),'--qtpaths',(Join-Path $toolchainRoot 'bin\qtpaths6.exe'))
    foreach($executable in $executables) { $deployArgs += Join-Path $stage $executable }
    & $deploy @deployArgs
    if($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
    [IO.File]::WriteAllText((Join-Path $stage 'qt.conf'),"[Paths]`nPrefix=.`nPlugins=plugins`n",[Text.UTF8Encoding]::new($false))

    # Discover the actual PE import graph, not a guessed list of GCC DLLs.
    # A queue reaches transitive dependencies of both programs and Qt plugins.
    $queue=[Collections.Generic.Queue[string]]::new()
    foreach($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in @('.dll','.exe') }) { $queue.Enqueue($file.FullName) }
    $visited=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $dependencyRows=[Collections.Generic.List[object]]::new()
    while($queue.Count -gt 0) {
        $binary=$queue.Dequeue()
        if(-not $visited.Add($binary)) { continue }
        $headers=& $objdump -p $binary
        if($LASTEXITCODE -ne 0) { throw "Unable to inspect PE imports: $binary" }
        foreach($line in $headers) {
            if($line -notmatch 'DLL Name:\s*(\S+)') { continue }
            $dll=$Matches[1]
            if($dll -match '^(libssl|libcrypto)') { throw "Unexpected TLS/crypto dependency in runtime: $dll" }
            $classification='Windows system'
            $bundled=Join-Path $stage $dll
            $candidate=Join-Path $toolchainRoot ('bin\'+$dll)
            if(Test-Path -LiteralPath $bundled) { $classification='Bundled infrastructure'; $queue.Enqueue($bundled) }
            elseif(Test-Path -LiteralPath $candidate) { Copy-Item -LiteralPath $candidate -Destination $bundled; $classification='Bundled infrastructure'; $queue.Enqueue($bundled) }
            elseif($dll -match '^(api-ms-|ext-ms-)' -or (Test-Path -LiteralPath (Join-Path $env:SystemRoot ('System32\'+$dll)))) { }
            else { throw "Unresolved PE dependency: $dll, requested by $binary" }
            $dependencyRows.Add([ordered]@{binary=[IO.Path]::GetRelativePath($stage,$binary);dependency=$dll;classification=$classification})
        }
    }
    $dependencyRows | Export-Csv -LiteralPath (Join-Path $stage 'runtime-dependencies.csv') -NoTypeInformation -Encoding utf8

    # Match shipped DLLs to the installed package database, then retain only
    # those packages' original license texts. Build-only libraries are excluded.
    $packageDatabase=Join-Path (Split-Path $toolchainRoot -Parent) 'var\lib\pacman\local'
    if(-not (Test-Path -LiteralPath $packageDatabase)) {
        $packageDatabase=[IO.Path]::GetFullPath((Join-Path $toolchainRoot '..\..\db\local'))
    }
    $dllPackages=@{}
    $packageVersions=@{}
    $packageLicenseFiles=@{}
    if(Test-Path -LiteralPath $packageDatabase) {
        foreach($package in Get-ChildItem -LiteralPath $packageDatabase -Directory) {
            $descriptionPath=Join-Path $package.FullName 'desc'; $filesPath=Join-Path $package.FullName 'files'
            if(-not (Test-Path -LiteralPath $descriptionPath) -or -not (Test-Path -LiteralPath $filesPath)) { continue }
            $description=Get-Content -LiteralPath $descriptionPath
            $nameIndex=[Array]::IndexOf($description,'%NAME%'); $versionIndex=[Array]::IndexOf($description,'%VERSION%')
            if($nameIndex -lt 0 -or $versionIndex -lt 0) { continue }
            $name=$description[$nameIndex+1] -replace '^mingw-w64-ucrt-x86_64-',''
            $packageVersions[$name]=$description[$versionIndex+1]
            $packageLicenseFiles[$name]=[Collections.Generic.List[string]]::new()
            foreach($file in Get-Content -LiteralPath $filesPath) {
                if($file -match '\.dll$') { $dllPackages[[IO.Path]::GetFileName($file)]=$name }
                # Some packages (for example ICU) retain their upstream license
                # under a versioned data folder instead of share/licenses.
                if($file -match '/(LICENSE|COPYING|COPYRIGHT)(\.[^/]*)?$') {
                    $sourceFile=Join-Path (Split-Path $toolchainRoot -Parent) $file
                    if(Test-Path -LiteralPath $sourceFile -PathType Leaf) { $packageLicenseFiles[$name].Add($sourceFile) }
                }
            }
        }
    }
    $usedPackages=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($file in Get-ChildItem -LiteralPath $stage -Recurse -Filter '*.dll') {
        if(-not $dllPackages.ContainsKey($file.Name)) { throw "Package ownership metadata missing for deployed DLL $($file.Name)" }
        $null=$usedPackages.Add($dllPackages[$file.Name])
    }
    $null=$usedPackages.Add('qt6-base')
    $licenseRoot=Join-Path $stage 'licenses'; [IO.Directory]::CreateDirectory($licenseRoot)|Out-Null
    $packageRows=[Collections.Generic.List[object]]::new()
    foreach($package in $usedPackages) {
        $licenseNames=if($package -eq 'gcc-libs') { @('gcc','libgcc','libstdc++') } elseif($package -eq 'winpthreads') { @('winpthreads','libwinpthread') } else { @($package) }
        $copied=[Collections.Generic.List[string]]::new()
        foreach($licenseName in $licenseNames) {
            $sourceLicense=Join-Path $toolchainRoot ('share\licenses\'+$licenseName)
            if(Test-Path -LiteralPath $sourceLicense) { Copy-Item -LiteralPath $sourceLicense -Destination (Join-Path $licenseRoot $licenseName) -Recurse; $copied.Add($licenseName) }
        }
        if($copied.Count -eq 0 -and $packageLicenseFiles[$package].Count -gt 0) {
            $packageLicenseRoot=Join-Path $licenseRoot $package
            [IO.Directory]::CreateDirectory($packageLicenseRoot)|Out-Null
            foreach($license in $packageLicenseFiles[$package]) { Copy-Item -LiteralPath $license -Destination (Join-Path $packageLicenseRoot ([IO.Path]::GetFileName($license))) }
            $copied.Add($package)
        }
        if($copied.Count -eq 0) { throw "License files could not be found for shipped package $package" }
        $packageRows.Add([ordered]@{package=$package;version=$packageVersions[$package];licenseDirectories=$copied})
    }
    $packageRows | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stage 'runtime-packages.json') -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination (Join-Path $stage 'docs') -Recurse
    # The external-machine runner must remain usable without a source checkout
    # or developer dependencies. Its generated credentials stay outside docs.
    $verificationRoot=Join-Path $stage 'verification'
    [IO.Directory]::CreateDirectory($verificationRoot)|Out-Null
    foreach($runner in @('external-acceptance.ps1','symlink-acceptance.ps1')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot ('scripts\'+$runner)) -Destination (Join-Path $verificationRoot $runner)
    }
    foreach($file in @('README.md','PROJECT_NOTICE.txt')) { if(Test-Path -LiteralPath (Join-Path $projectRoot $file)) { Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination (Join-Path $stage $file) } }
    $commit='local-build'
    try {
        $value=(& git -C $projectRoot rev-parse HEAD 2>$null)
        if($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($value)) { $commit=$value.Trim() }
    } catch { }
    $buildTools=[ordered]@{
        compiler=(& (Join-Path $toolchainRoot 'bin\g++.exe') --version | Select-Object -First 1)
        cmake=(& (Join-Path $toolchainRoot 'bin\cmake.exe') --version | Select-Object -First 1)
        ninja=(& (Join-Path $toolchainRoot 'bin\ninja.exe') --version)
    }
    [ordered]@{version='1.0.0';timestampUtc=[DateTime]::UtcNow.ToString('o');sourceCommit=$commit;qt=(& (Join-Path $toolchainRoot 'bin\qmake6.exe') -query QT_VERSION);buildTools=$buildTools;architecture='windows-x64';programs=$executables} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'build-info.json') -Encoding utf8
    $checksums=foreach($file in Get-ChildItem -LiteralPath $stage -Recurse -File) { [ordered]@{path=[IO.Path]::GetRelativePath($stage,$file.FullName);bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()} }
    $checksums | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'checksums.json') -Encoding utf8
    if(Test-Path -LiteralPath $target) {
        $previous=Join-Path $distributionRoot ('ChengVault-1.0-windows-x64.previous-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6))
        if(-not ([IO.Path]::GetFullPath($previous).StartsWith($distributionRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase))) { throw 'Invalid previous-release directory.' }
        Move-Item -LiteralPath $target -Destination $previous
    }
    Move-Item -LiteralPath $stage -Destination $target
    Compress-Archive -LiteralPath $target -DestinationPath (Join-Path $distributionRoot 'ChengVault-1.0-windows-x64.zip') -Force
    if($SourceBundle) {
        # Source is the exact committed tree, excluding toolchains, build caches,
        # credentials and huge generated benchmark inputs. Git history is separate.
        # Git archive normally follows the machine's checkout EOL policy.
        # Force the stored LF bytes so source ZIP entries can be verified
        # directly against their committed Git blob hashes on any host.
        & git -c core.autocrlf=false -c core.eol=lf -C $projectRoot archive --format=zip --prefix=ChengVault/ "--output=$(Join-Path $distributionRoot 'ChengVault-1.0-source.zip')" HEAD
        if($LASTEXITCODE -ne 0) { throw 'Git source archive failed.' }
        & git -C $projectRoot bundle create (Join-Path $distributionRoot 'ChengVault-history.bundle') --all
        if($LASTEXITCODE -ne 0) { throw 'Git history bundle failed.' }
    }
    Write-Host "Runtime package: $target"
} finally {
    $env:PATH=$oldPath
}
