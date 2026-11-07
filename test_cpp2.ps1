param(
    [Parameter(Mandatory)][string]$InstallDir,
    [Parameter(Mandatory)][string]$CurrentVersion,
    [Parameter(Mandatory)][string]$ManifestUrl
)

$updater = Join-Path $PSScriptRoot 'dist/windows-x64/updater.exe'
& $updater --install-dir $InstallDir --current-version $CurrentVersion --latest-json-url $ManifestUrl --no-start-main
exit $LASTEXITCODE
