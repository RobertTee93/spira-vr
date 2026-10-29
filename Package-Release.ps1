param([string]$Version='0.1.0-alpha')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Initialize-PowerShell.ps1"
if ($Version -notmatch '^[0-9A-Za-z.-]+$') { throw 'Invalid version.' }
if (Test-Path -LiteralPath "$PSScriptRoot\active-session.json") { throw 'Close the active VR session before packaging.' }
& "$PSScriptRoot\Fetch-OpenVR.ps1"
$files = @(
    'ffx-headlook.addon32','FFXHeadlookVR.dll','vr-output.hlsl','control.example.ini',
    'Start-SpiraVR.cmd','Launch-SpiraVR.ps1','Game-Path.ps1','Initialize-PowerShell.ps1',
    'Restore-Normal.cmd','Restore-Normal.ps1','Toggle-Stereo.cmd','Toggle-Stereo.ps1',
    'Install-Shortcuts.ps1','README.md','LICENSE','THIRD-PARTY-NOTICES.md'
)
foreach ($file in $files) { if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot $file))) { throw "Missing $file; run build.cmd first." } }
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$stage = Join-Path $dist ('SpiraVR-' + $Version)
if (Test-Path -LiteralPath $stage) { throw 'Release staging folder already exists; choose a new version or archive it first.' }
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($file in $files) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $stage }
Copy-Item -LiteralPath "$PSScriptRoot\docs" -Destination $stage -Recurse
foreach ($dependency in @('minhook','openvr','reshade')) {
    $dest = Join-Path $stage ('third_party\' + $dependency)
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    $name = if ($dependency -eq 'minhook') {'LICENSE.txt'} else {'LICENSE'}
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ("third_party\$dependency\$name")) -Destination $dest
}
$zip = "$stage.zip"
Compress-Archive -LiteralPath $stage -DestinationPath $zip
(Get-FileHash -LiteralPath $zip).Hash + '  ' + [IO.Path]::GetFileName($zip) |
    Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
Write-Output $zip
