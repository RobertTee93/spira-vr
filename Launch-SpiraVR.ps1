param([string]$GameDirectory, [switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Initialize-PowerShell.ps1"
. "$PSScriptRoot\Game-Path.ps1"
$game = Get-SpiraGameDirectory $GameDirectory
$expected = '0537B2A1047F3266E73495CD4E35F63F0777F4231D417699F979954686DA686D'
if (Get-Process FFX -ErrorAction SilentlyContinue) { throw 'Close FFX before launching SpiraVR.' }
if ((Get-FileHash -LiteralPath "$game\FFX.exe").Hash -ne $expected) { throw 'Unsupported FFX.exe SHA256. No changes made. See docs/COMPATIBILITY.md.' }
$addon = Join-Path $game 'ffx-headlook.addon32'
$active = Join-Path $PSScriptRoot 'active-session.json'
if ((Test-Path -LiteralPath $active) -or (Test-Path -LiteralPath $addon)) { throw 'Previous VR session needs Restore-Normal.cmd first.' }
foreach ($required in @('ffx-headlook.addon32','FFXHeadlookVR.dll','vr-output.hlsl','control.example.ini')) {
    if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot $required))) { throw "Missing $required. Build the addon and run Fetch-OpenVR.ps1, or use the release ZIP." }
}
if ($PSScriptRoot.Length -gt 220) { throw 'Move SpiraVR to a shorter path (under 220 characters).' }
if ($ValidateOnly) { Write-Output 'Game hash and required runtime files validated; game directory unchanged.'; return }
$config = Join-Path $PSScriptRoot 'control.ini'
if (!(Test-Path -LiteralPath $config)) { Copy-Item -LiteralPath "$PSScriptRoot\control.example.ini" -Destination $config }
$sessionRoot = Join-Path $PSScriptRoot '.sessions'
$snapshot = Join-Path $sessionRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $snapshot -Force | Out-Null
$files = @(Get-ChildItem -LiteralPath $game -File | Where-Object { $_.Extension -in '.ini','.cfg','.dll','.exe','.addon32' })
$manifest = @($files | ForEach-Object {
    [pscustomobject]@{Name=$_.Name; SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash; Restore=($_.Extension -in '.ini','.cfg')}
})
foreach ($file in $files) { if ($file.Extension -in '.ini','.cfg') { Copy-Item -LiteralPath $file.FullName -Destination $snapshot } }
$manifest | ConvertTo-Json | Set-Content -LiteralPath "$snapshot\manifest.json"
[pscustomobject]@{Game=$game; Backup=$snapshot; AddonHash=(Get-FileHash -LiteralPath "$PSScriptRoot\ffx-headlook.addon32").Hash} |
    ConvertTo-Json | Set-Content -LiteralPath $active
try {
    if ((Get-Content -LiteralPath $config -Raw) -match '(?m)^DisableDLSS5=1\s*$') {
        $feed = Join-Path $game 'dlss5-feed.cfg'
        if (Test-Path -LiteralPath $feed) {
            $text = Get-Content -LiteralPath $feed -Raw
            $text = [regex]::Replace($text,'(?m)^enabled=\d+','enabled=0')
            Set-Content -LiteralPath $feed -Value $text -NoNewline
        }
    }
    Copy-Item -LiteralPath "$PSScriptRoot\ffx-headlook.addon32" -Destination $addon
    # Pass the portable home to this child only; no permanent environment setting.
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = "$game\FFX.exe"
    $start.WorkingDirectory = $game
    $start.UseShellExecute = $false
    $start.EnvironmentVariables['SPIRA_VR_HOME'] = $PSScriptRoot
    Write-Output 'SpiraVR is running. Keep this launcher open until FFX exits so settings can be restored.'
    $process = [Diagnostics.Process]::Start($start)
    $process.WaitForExit()
} finally {
    & "$PSScriptRoot\Restore-Normal.ps1"
}
