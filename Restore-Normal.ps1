$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Initialize-PowerShell.ps1"
if (Get-Process FFX -ErrorAction SilentlyContinue) { throw 'Close FFX normally before restoring its settings.' }
$active = Join-Path $PSScriptRoot 'active-session.json'
if (!(Test-Path -LiteralPath $active)) { Write-Output 'No active SpiraVR session.'; return }
$s = Get-Content -LiteralPath $active -Raw | ConvertFrom-Json
$sessionRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.sessions'))
$backup = [IO.Path]::GetFullPath($s.Backup)
if ([IO.Path]::GetDirectoryName($backup) -ne $sessionRoot) { throw 'Unexpected recovery path; session left untouched.' }
if (!(Test-Path -LiteralPath "$($s.Game)\FFX.exe")) { throw 'Recorded FFX installation not found; session left untouched.' }
$files = Get-Content -LiteralPath "$backup\manifest.json" -Raw | ConvertFrom-Json
foreach ($f in $files) {
    if ([IO.Path]::GetFileName($f.Name) -ne $f.Name -or $f.Name -match '[\\/:]') { throw 'Invalid manifest filename.' }
    if ($f.Restore -and [IO.Path]::GetExtension($f.Name) -notin '.ini','.cfg') { throw 'Refusing to restore non-configuration data.' }
    if ($f.Restore -and (Get-FileHash -LiteralPath (Join-Path $backup $f.Name)).Hash -ne $f.SHA256) { throw "Recovery copy changed: $($f.Name)" }
}
$addon = Join-Path $s.Game 'ffx-headlook.addon32'
if (Test-Path -LiteralPath $addon) {
    if ((Get-FileHash -LiteralPath $addon).Hash -ne $s.AddonHash) { throw 'Addon changed; leaving it for inspection.' }
    Remove-Item -LiteralPath $addon
}
foreach ($f in $files) {
    if ($f.Restore) { Copy-Item -LiteralPath (Join-Path $backup $f.Name) -Destination (Join-Path $s.Game $f.Name) -Force }
}
$bad = @($files | Where-Object { !(Test-Path -LiteralPath (Join-Path $s.Game $_.Name)) -or (Get-FileHash -LiteralPath (Join-Path $s.Game $_.Name)).Hash -ne $_.SHA256 })
if ($bad.Count) { throw ('Verification failed; recovery retained: ' + ($bad.Name -join ', ')) }
$result = "Restored and verified $($files.Count) original game/mod files at $(Get-Date -Format o)."
$result | Set-Content -LiteralPath "$PSScriptRoot\restore-result.txt"
$result | Set-Content -LiteralPath "$backup\verified.txt"
Remove-Item -LiteralPath $active
# Keep the latest verified recovery only. Never prune an incomplete session.
foreach ($previous in (Get-ChildItem -LiteralPath $sessionRoot -Directory)) {
    $path = [IO.Path]::GetFullPath($previous.FullName)
    if ($path -ne $backup -and [IO.Path]::GetDirectoryName($path) -eq $sessionRoot -and
        !($previous.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
        (Test-Path -LiteralPath (Join-Path $path 'verified.txt'))) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
Write-Output $result
