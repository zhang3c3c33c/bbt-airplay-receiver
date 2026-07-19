param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$OutDir = ".\dist\airplay_receiver",
    [string]$BuildDir = ".\build-ucrt-release",
    [string]$MsysRoot = "C:\msys64",
    [string]$CMakeExe = "",
    [string]$NinjaExe = "",
    [string]$ForkSourceURL = ""
)

$ErrorActionPreference = "Stop"
$version = "0.1.0"
$upstreamURL = "https://github.com/FDH2/UxPlay.git"
$upstreamRevision = "3ca7526387e894d6848b84c209de361c3bedd1ec"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-Tool([string]$Explicit, [string]$Name, [string]$Fallback) {
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw "$Name not found: $Explicit" }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    if ($Fallback -and (Test-Path -LiteralPath $Fallback)) { return (Resolve-Path -LiteralPath $Fallback).Path }
    throw "$Name was not found"
}

function Assert-ChildPath([string]$Root, [string]$Candidate) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    if (-not $candidateFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside repository: $candidateFull"
    }
    return $candidateFull
}

function File-SHA256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$cmake = Resolve-Tool $CMakeExe "cmake" ""
$ninja = Resolve-Tool $NinjaExe "ninja" (Join-Path $MsysRoot "ucrt64\bin\ninja.exe")
$gcc = Join-Path $MsysRoot "ucrt64\bin\gcc.exe"
$gxx = Join-Path $MsysRoot "ucrt64\bin\g++.exe"
$ldd = Join-Path $MsysRoot "usr\bin\ldd.exe"
foreach ($required in @($gcc, $gxx, $ldd)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required tool missing: $required" }
}

$head = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($head)) {
    throw "Unable to resolve fork revision"
}
if ([string]::IsNullOrWhiteSpace($ForkSourceURL)) {
    $ForkSourceURL = "https://github.com/zhang3c3c33c/bbt-airplay-receiver/tree/v$version"
}
& git -C $repoRoot merge-base --is-ancestor $upstreamRevision $head
if ($LASTEXITCODE -ne 0) {
    throw "Fork revision $head is not based on pinned UxPlay revision $upstreamRevision"
}

$buildPath = Assert-ChildPath $repoRoot (Join-Path $repoRoot $BuildDir)
$outPath = Assert-ChildPath $repoRoot (Join-Path $repoRoot $OutDir)
if (Test-Path -LiteralPath $buildPath) { Remove-Item -LiteralPath $buildPath -Recurse -Force }
if (Test-Path -LiteralPath $outPath) { Remove-Item -LiteralPath $outPath -Recurse -Force }
New-Item -ItemType Directory -Force -Path $buildPath, $outPath | Out-Null

& $cmake -S $repoRoot -B $buildPath -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_C_COMPILER=$gcc" `
    "-DCMAKE_CXX_COMPILER=$gxx" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DNO_X11_DEPS=ON" `
    "-DNO_MARCH_NATIVE=ON" `
    "-DBBT_AIRPLAY_VERSION=$version" `
    "-DBBT_UPSTREAM_REVISION=$upstreamRevision"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
& $cmake --build $buildPath --target uxplay
if ($LASTEXITCODE -ne 0) { throw "Sidecar build failed: $LASTEXITCODE" }

$binary = Join-Path $buildPath "bbt_airplay_receiver.exe"
if (-not (Test-Path -LiteralPath $binary)) { throw "Built sidecar missing: $binary" }
$versionInfo = (& $binary --version-json | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $versionInfo.version -ne $version -or
    $versionInfo.upstream -ne "UxPlay/$upstreamRevision" -or
    $versionInfo.protocolMajor -ne 1 -or $versionInfo.gstreamer -ne $false) {
    throw "Sidecar --version-json validation failed"
}
$selfTest = (& $binary --self-test-json | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or $selfTest.ok -ne $true -or
    $selfTest.'aac-eld' -ne $true -or $selfTest.alac -ne $true -or
    $selfTest.gstreamer -ne $false) {
    throw "Sidecar self-test did not declare required compressed-audio capabilities"
}

$stagedBinary = Join-Path $outPath "bbt_airplay_receiver.exe"
Copy-Item -LiteralPath $binary -Destination $stagedBinary -Force
$runtimeDir = New-Item -ItemType Directory -Force -Path (Join-Path $outPath "airplay_runtime")
$lddLines = & $ldd $binary
if ($LASTEXITCODE -ne 0) { throw "ldd failed for sidecar" }
$runtimePaths = New-Object System.Collections.Generic.List[string]
foreach ($line in $lddLines) {
    if ($line -match '=>\s+(/ucrt64/bin/[^\s]+)') {
        $unixPath = $Matches[1]
        $name = Split-Path -Leaf $unixPath
        $winPath = Join-Path (Join-Path $MsysRoot "ucrt64\bin") $name
        if (-not (Test-Path -LiteralPath $winPath)) { throw "Runtime DLL missing: $winPath" }
        if ($name -match '(?i)gst|gstreamer') { throw "GStreamer runtime is forbidden: $name" }
        if (-not $runtimePaths.Contains($winPath)) { $runtimePaths.Add($winPath) }
    }
}
if ($runtimePaths.Count -eq 0) { throw "No UCRT64 runtime dependencies discovered" }
foreach ($runtime in $runtimePaths) {
    Copy-Item -LiteralPath $runtime -Destination $runtimeDir.FullName -Force
}

$licenseDir = New-Item -ItemType Directory -Force -Path (Join-Path $outPath "licenses\airplay")
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $licenseDir.FullName "COPYING")
Copy-Item -LiteralPath (Join-Path $repoRoot "packaging\THIRD_PARTY_NOTICES.md") -Destination $licenseDir.FullName
$runtimeLicenseDir = New-Item -ItemType Directory -Force -Path (Join-Path $licenseDir.FullName "runtime")
$licenseSources = @(
    "gcc-libs\COPYING.LIB", "gcc-libs\COPYING.RUNTIME", "gcc-libs\COPYING3",
    "openssl\LICENSE", "glib2\COPYING", "gettext-runtime\COPYING",
    "gettext-runtime\intl\COPYING.LIB", "libiconv\COPYING", "libiconv\COPYING.LIB",
    "pcre2\LICENCE.md"
)
foreach ($relative in $licenseSources) {
    $source = Join-Path (Join-Path $MsysRoot "ucrt64\share\licenses") $relative
    if (-not (Test-Path -LiteralPath $source)) { throw "Runtime license missing: $source" }
    $safeName = $relative.Replace('\', '-')
    Copy-Item -LiteralPath $source -Destination (Join-Path $runtimeLicenseDir.FullName $safeName)
}

$sourceDir = New-Item -ItemType Directory -Force -Path (Join-Path $outPath "source")
$sourceName = "bbt-airplay-receiver-$version"
$sourceTemp = Join-Path $env:TEMP ($sourceName + "-" + [Guid]::NewGuid().ToString("N"))
$sourceRoot = Join-Path $sourceTemp $sourceName
New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
$tracked = @(& git -C $repoRoot ls-files)
$untracked = @(& git -C $repoRoot ls-files --others --exclude-standard)
$sourceFiles = @($tracked + $untracked | Sort-Object -Unique | Where-Object {
    $_ -notmatch '^(build[^/]*|dist|CMakeFiles)/' -and
    $_ -notmatch '(^|/)(\.ninja_deps|\.ninja_log|CMakeCache\.txt|build\.ninja|cmake_install\.cmake)$' -and
    $_ -notmatch '\.(exe|dll|a|o|obj|pdb)$'
})
foreach ($relative in $sourceFiles) {
    $source = Join-Path $repoRoot ($relative -replace '/', '\')
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $destination = Join-Path $sourceRoot ($relative -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}
$diffPath = Join-Path $sourceRoot "LOCAL_CHANGES.patch"
& git -C $repoRoot diff --binary "$upstreamRevision..$head" | Set-Content -LiteralPath $diffPath -Encoding utf8
$identity = [ordered]@{
    upstreamURL = $upstreamURL
    upstreamRevision = $upstreamRevision
    forkURL = $ForkSourceURL
    forkRevision = $head
    localChangesPatch = "LOCAL_CHANGES.patch"
    generatedUTC = [DateTime]::UtcNow.ToString("o")
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $sourceRoot "SOURCE_IDENTITY.json") -Encoding utf8
$sourceZip = Join-Path $sourceDir.FullName "$sourceName.zip"
Compress-Archive -Path $sourceRoot -DestinationPath $sourceZip -CompressionLevel Optimal
Remove-Item -LiteralPath $sourceTemp -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "packaging\BUILD.md") -Destination (Join-Path $sourceDir.FullName "$sourceName-BUILD.md")

$runtimeLicenses = @{
    "libwinpthread-1.dll" = "GPL-3.0-or-later WITH GCC-exception-3.1"
    "libgcc_s_seh-1.dll" = "GPL-3.0-or-later WITH GCC-exception-3.1"
    "libstdc++-6.dll" = "GPL-3.0-or-later WITH GCC-exception-3.1"
    "libcrypto-3-x64.dll" = "Apache-2.0"
    "libglib-2.0-0.dll" = "LGPL-2.1-or-later"
    "libplist-2.0.dll" = "LGPL-2.1-or-later"
    "libintl-8.dll" = "LGPL-2.1-or-later"
    "libiconv-2.dll" = "LGPL-2.1-or-later"
    "libpcre2-8-0.dll" = "BSD-3-Clause"
}
$runtimeManifest = @()
foreach ($dll in Get-ChildItem -LiteralPath $runtimeDir.FullName -Filter *.dll -File | Sort-Object Name) {
    if (-not $runtimeLicenses.ContainsKey($dll.Name)) { throw "No license mapping for runtime: $($dll.Name)" }
    $runtimeManifest += [ordered]@{ file = $dll.Name; sha256 = File-SHA256 $dll.FullName; license = $runtimeLicenses[$dll.Name] }
}
$packageNames = @(
    "mingw-w64-ucrt-x86_64-gcc", "mingw-w64-ucrt-x86_64-openssl",
    "mingw-w64-ucrt-x86_64-glib2", "mingw-w64-ucrt-x86_64-libplist",
    "mingw-w64-ucrt-x86_64-gettext-runtime", "mingw-w64-ucrt-x86_64-libiconv",
    "mingw-w64-ucrt-x86_64-pcre2", "mingw-w64-ucrt-x86_64-ninja"
)
$pacman = Join-Path $MsysRoot "usr\bin\pacman.exe"
if (-not (Test-Path -LiteralPath $pacman)) { throw "pacman not found: $pacman" }
$packageVersions = @()
foreach ($packageName in $packageNames) {
    $line = (& $pacman -Q $packageName | Select-Object -First 1)
    if (-not $line) { throw "Unable to resolve package version: $packageName" }
    $parts = $line -split '\s+', 2
    $packageVersions += [ordered]@{ name = $parts[0]; version = $parts[1] }
}
if ([string]::IsNullOrWhiteSpace($ForkSourceURL)) {
    $ForkSourceURL = "packaged:source/$sourceName.zip"
}
$manifest = [ordered]@{
    schemaVersion = 1
    component = "bbt_airplay_receiver"
    version = $version
    license = "GPL-3.0-or-later"
    protocolMajor = 1
    binary = [ordered]@{ file = "bbt_airplay_receiver.exe"; sha256 = File-SHA256 $stagedBinary }
    upstream = [ordered]@{ url = $upstreamURL; revision = $upstreamRevision }
    fork = [ordered]@{ sourceURL = $ForkSourceURL; revision = $head }
    correspondingSource = [ordered]@{ file = "$sourceName.zip"; sha256 = File-SHA256 $sourceZip }
    build = [ordered]@{
        configuration = $Configuration
        command = "powershell -ExecutionPolicy Bypass -File .\\scripts\\build_airplay_receiver.ps1 -Configuration $Configuration -OutDir .\\dist\\airplay_receiver"
        compiler = (& $gxx --version | Select-Object -First 1)
        cmake = (& $cmake --version | Select-Object -First 1)
        packages = $packageVersions
    }
    capabilities = [ordered]@{ videoCodecs = @("h264", "hevc"); audioCodecs = @("aac-eld", "alac"); gstreamer = $false }
    runtimes = $runtimeManifest
}
$manifestPath = Join-Path $sourceDir.FullName "$sourceName-manifest.json"
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8

Write-Output "AirPlay receiver staging complete: $outPath"
Write-Output "Binary SHA256: $($manifest.binary.sha256)"
Write-Output "Source SHA256: $($manifest.correspondingSource.sha256)"
