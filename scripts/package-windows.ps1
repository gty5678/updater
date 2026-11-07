param([string]$VcpkgRoot)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 C++ tools were not found.' }
$toolset = (Get-Content -LiteralPath (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$dumpbin = Join-Path $vs "VC/Tools/MSVC/$toolset/bin/Hostx64/x64/dumpbin.exe"

$cmakeCommand = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else {
    Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmake)) {
    throw 'CMake was not found. Install CMake 3.25 or newer.'
}

$requestedRoot = if ($VcpkgRoot) { $VcpkgRoot } else { $env:VCPKG_ROOT }
$candidates = if ($requestedRoot) {
    @($requestedRoot)
} else {
    $vcpkgCommand = Get-Command vcpkg -CommandType Application -ErrorAction SilentlyContinue
    @(
        if ($vcpkgCommand) { Split-Path -Parent $vcpkgCommand.Source }
        Join-Path $root 'vcpkg'
        Join-Path $env:USERPROFILE 'vcpkg'
        Join-Path $env:SystemDrive 'src/vcpkg'
        Join-Path $env:SystemDrive 'vcpkg'
        Join-Path $vs 'VC/vcpkg'
    )
}
$resolvedRoot = $null
foreach ($candidate in $candidates) {
    if ((Test-Path -LiteralPath (Join-Path $candidate 'vcpkg.exe')) -and
        (Test-Path -LiteralPath (Join-Path $candidate 'scripts/buildsystems/vcpkg.cmake'))) {
        $resolvedRoot = (Resolve-Path -LiteralPath $candidate).Path
        break
    }
}
if (-not $resolvedRoot) {
    throw 'A bootstrapped vcpkg installation was not found. Run this script with -VcpkgRoot <path>, or set VCPKG_ROOT. See README.md.'
}

$previousVcpkgRoot = $env:VCPKG_ROOT
$previousPath = $env:PATH
$previousSystemBinaries = $env:VCPKG_FORCE_SYSTEM_BINARIES
$env:VCPKG_ROOT = $resolvedRoot
$cmakeDirectory = Split-Path -Parent $cmake
$ninjaDirectory = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:PATH = "$cmakeDirectory;$ninjaDirectory;$previousPath"
$env:VCPKG_FORCE_SYSTEM_BINARIES = '1'
Write-Host "Using vcpkg: $resolvedRoot"

Push-Location -LiteralPath $root
try {
    Invoke-Checked $cmake @('--preset', 'windows-release')
    Invoke-Checked $cmake @('--build', '--preset', 'windows-release')
    $dist = Join-Path $root 'dist/windows-x64'
    $exe = Join-Path $root 'build/windows-release/Release/updater.exe'
    $imports = Invoke-Checked $dumpbin @('/dependents', $exe)
    foreach ($line in $imports) {
        if ($line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            $library = $Matches[1]
            if ($library -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt)' -or
                -not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$library"))) {
                throw "Unexpected dynamic dependency: $library"
            }
        }
    }
    Invoke-Checked $exe @('--help')
    Invoke-Checked $cmake @('--install', 'build/windows-release', '--config', 'Release', '--prefix', $dist)
    Write-Host "Packaged $dist/updater.exe"
} finally {
    Pop-Location
    $env:VCPKG_ROOT = $previousVcpkgRoot
    $env:PATH = $previousPath
    $env:VCPKG_FORCE_SYSTEM_BINARIES = $previousSystemBinaries
}
