<#
.SYNOPSIS
    Builds trinoodbc.dll on Windows with MSVC + vcpkg and assembles a distributable folder.

.DESCRIPTION
    Steps:
      1. Locate (or bootstrap into <repo>\.vcpkg) a vcpkg checkout and install the dependencies
         curl[schannel], zstd, lz4 and simdjson for the target triplet.
      2. Configure with CMake (Visual Studio generator, -A x64 / Win32 / ARM64) using the vcpkg
         toolchain and TRINO_ODBC_DRIVER_MANAGER=windows.
      3. Build the requested configuration.
      4. Copy trinoodbc.dll (+ .pdb) and its runtime dependencies (libcurl.dll, zlib1.dll, zstd.dll,
         lz4.dll, ...) plus docs and register_driver.ps1 into the dist folder.
      5. Optionally (-Installer) compile packaging\windows\installer.iss with Inno Setup.
    Re-running the script reuses the build directory and overwrites the dist folder.

.PARAMETER Config        Release (default) | RelWithDebInfo | Debug
.PARAMETER Arch          x64 (default) | x86 | arm64
.PARAMETER VcpkgRoot     vcpkg checkout (default: $env:VCPKG_ROOT, else <repo>\.vcpkg, cloned if missing)
.PARAMETER Generator     CMake generator (default: "Visual Studio 17 2022")
.PARAMETER BuildDir      CMake build directory (default: <repo>\build-win-<Arch>)
.PARAMETER Dist          output folder (default: <repo>\dist\windows-<Arch>)
.PARAMETER BuildTests    also build (and run) the unit tests
.PARAMETER SkipVcpkgInstall  assume the vcpkg packages are already installed
.PARAMETER Installer     build the Inno Setup installer after the dist folder is ready
.PARAMETER DownloadVcRedist  download vc_redist.<arch>.exe into the dist folder so the installer can bundle it
.PARAMETER Clean         delete the build directory first

.EXAMPLE
    .\packaging\windows\build.ps1 -Installer
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Config = 'Release',
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Arch = 'x64',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$BuildDir = '',
    [string]$Dist = '',
    [switch]$BuildTests,
    [switch]$SkipVcpkgInstall,
    [switch]$Installer,
    [switch]$DownloadVcRedist,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    Write-Host ">> $Exe $($Arguments -join ' ')"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($BuildDir -eq '') { $BuildDir = Join-Path $Root "build-win-$Arch" }
if ($Dist -eq '') { $Dist = Join-Path $Root "dist\windows-$Arch" }
$Triplet = "$Arch-windows"
$CmakeArch = switch ($Arch) { 'x64' { 'x64' } 'x86' { 'Win32' } 'arm64' { 'ARM64' } }

$versionMatch = Select-String -Path (Join-Path $Root 'CMakeLists.txt') -Pattern 'project\(trinoodbc VERSION ([0-9][0-9.]*)' | Select-Object -First 1
$Version = if ($versionMatch) { $versionMatch.Matches[0].Groups[1].Value } else { '1.0.0' }
Write-Host "Trino ODBC $Version  arch=$Arch  config=$Config  triplet=$Triplet"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake was not found on PATH (install CMake or run from a Visual Studio developer prompt)"
}

# ---- 1. vcpkg ---------------------------------------------------------------------------------------
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $Root '.vcpkg' }
$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $VcpkgExe)) {
    if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'))) {
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git is required to clone vcpkg into $VcpkgRoot" }
        Invoke-Checked 'git' @('clone', '--depth', '1', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
    }
    Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
}
if (-not $SkipVcpkgInstall) {
    $packages = @("curl[schannel]:$Triplet", "zstd:$Triplet", "lz4:$Triplet", "simdjson:$Triplet")
    Invoke-Checked $VcpkgExe (@('install') + $packages)
}
$VcpkgInstalled = Join-Path $VcpkgRoot "installed\$Triplet"

# ---- 2. configure -----------------------------------------------------------------------------------
if ($Clean -and (Test-Path -LiteralPath $BuildDir)) { Remove-Item -LiteralPath $BuildDir -Recurse -Force }
$cmakeArgs = @(
    '-S', $Root, '-B', $BuildDir,
    '-G', $Generator, '-A', $CmakeArch,
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    '-DTRINO_ODBC_DRIVER_MANAGER=windows',
    '-DTRINO_ODBC_WITH_ZSTD=ON',
    '-DTRINO_ODBC_WITH_LZ4=ON',
    '-DTRINO_ODBC_USE_SYSTEM_SIMDJSON=ON',
    "-DTRINO_ODBC_BUILD_TESTS=$(if ($BuildTests) { 'ON' } else { 'OFF' })"
)
Invoke-Checked 'cmake' $cmakeArgs

# ---- 3. build ---------------------------------------------------------------------------------------
Invoke-Checked 'cmake' @('--build', $BuildDir, '--config', $Config, '--parallel')

if ($BuildTests) {
    $unitTests = Join-Path $BuildDir "tests\$Config\trino_odbc_unit_tests.exe"
    if (Test-Path -LiteralPath $unitTests) { Invoke-Checked $unitTests @() }
}

# ---- 4. dist folder ---------------------------------------------------------------------------------
$dll = Join-Path $BuildDir "$Config\trinoodbc.dll"
if (-not (Test-Path -LiteralPath $dll)) {
    $found = Get-ChildItem -Path $BuildDir -Recurse -Filter 'trinoodbc.dll' | Select-Object -First 1
    if (-not $found) { throw "trinoodbc.dll was not produced by the build" }
    $dll = $found.FullName
}
if (Test-Path -LiteralPath $Dist) { Remove-Item -LiteralPath $Dist -Recurse -Force }
New-Item -ItemType Directory -Path $Dist -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Dist 'docs') -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination $Dist
$pdb = [System.IO.Path]::ChangeExtension($dll, '.pdb')
if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $Dist }

# Runtime dependencies: everything the vcpkg triplet built as a DLL that our DLL (transitively)
# imports. Import names are read from the PE import table with dumpbin when available; otherwise
# the well-known set is copied.
$vcpkgBin = Join-Path $VcpkgInstalled $(if ($Config -eq 'Debug') { 'debug\bin' } else { 'bin' })
$knownDeps = @('libcurl.dll', 'libcurl-d.dll', 'zlib1.dll', 'zlibd1.dll', 'zstd.dll', 'zstdd.dll', 'lz4.dll', 'lz4d.dll', 'simdjson.dll', 'simdjsond.dll')
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
$toCopy = New-Object System.Collections.Generic.List[string]
$queue = New-Object System.Collections.Generic.Queue[string]
$queue.Enqueue($dll)
$seen = @{}
while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    if ($seen.ContainsKey($current.ToLowerInvariant())) { continue }
    $seen[$current.ToLowerInvariant()] = $true
    $imports = @()
    if ($dumpbin) {
        $imports = & $dumpbin.Source /NOLOGO /DEPENDENTS $current 2>$null |
            Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    } else {
        $imports = $knownDeps
    }
    foreach ($imp in $imports) {
        $candidate = Join-Path $vcpkgBin $imp
        if ((Test-Path -LiteralPath $candidate) -and -not ($toCopy -contains $candidate)) {
            $toCopy.Add($candidate)
            $queue.Enqueue($candidate)
        }
    }
}
foreach ($dep in $toCopy) {
    Copy-Item -LiteralPath $dep -Destination $Dist
    Write-Host "  bundled $(Split-Path -Leaf $dep)"
}

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'register_driver.ps1') -Destination $Dist
if (Test-Path -LiteralPath (Join-Path $Root 'docs')) {
    Get-ChildItem -Path (Join-Path $Root 'docs') -Filter '*.md' | Copy-Item -Destination (Join-Path $Dist 'docs')
}
foreach ($f in @('README.md', 'LICENSE', 'NOTICE', 'THIRD-PARTY-NOTICES.md')) {
    $p = Join-Path $Root $f
    if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination (Join-Path $Dist 'docs') }
}
Set-Content -LiteralPath (Join-Path $Dist 'VERSION.txt') -Value "$Version ($Arch, $Config)"

if ($DownloadVcRedist) {
    $redistName = "vc_redist.$Arch.exe"
    $redistUrl = "https://aka.ms/vs/17/release/$redistName"
    Write-Host "Downloading $redistUrl"
    Invoke-WebRequest -Uri $redistUrl -OutFile (Join-Path $Dist $redistName) -UseBasicParsing
}

Write-Host "dist folder: $Dist"
Get-ChildItem -LiteralPath $Dist | ForEach-Object { Write-Host "  $($_.Name)" }

# ---- 5. installer -----------------------------------------------------------------------------------
if ($Installer) {
    $iscc = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    $isccPath = if ($iscc) { $iscc.Source } else { $null }
    if (-not $isccPath) {
        foreach ($p in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
            if ($p -and (Test-Path -LiteralPath $p)) { $isccPath = $p; break }
        }
    }
    if (-not $isccPath) { throw "Inno Setup 6 (ISCC.exe) not found; install it from https://jrsoftware.org/isinfo.php" }
    $outDir = Join-Path $Root 'dist'
    Invoke-Checked $isccPath @(
        "/DAppVersion=$Version",
        "/DDistDir=$Dist",
        "/DArch=$Arch",
        "/O$outDir",
        (Join-Path $PSScriptRoot 'installer.iss')
    )
    Write-Host "installer written to $outDir"
}
