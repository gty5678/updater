param(
    [Parameter(Mandatory)][string]$InstallDir,
    [Parameter(Mandatory)][string]$Package
)

$updater = Join-Path $PSScriptRoot 'dist/windows-x64/updater.exe'
& $updater --install-dir $InstallDir --local-package $Package --no-start-main
exit $LASTEXITCODE
