param(
    [ValidateSet('Toggle','On','Off')][string]$State = 'Toggle'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Initialize-PowerShell.ps1"
$config = Join-Path $PSScriptRoot 'control.ini'
if (!(Test-Path -LiteralPath $config)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'control.example.ini') -Destination $config }
$text = [IO.File]::ReadAllText($config)
$current = 0
if ($text -match '(?m)^StereoPercent=(\d+)\s*$') { $current = [int]$Matches[1] }
$onPercent = 20
if ($text -match '(?m)^StereoOnPercent=(\d+)\s*$') { $onPercent = [Math]::Min(100,[Math]::Max(1,[int]$Matches[1])) }
if ($current -gt 0) { $onPercent = [Math]::Min(100,[Math]::Max(1,$current)) }
$enable = $State -eq 'On' -or ($State -eq 'Toggle' -and $current -eq 0)
$next = if ($enable) { $onPercent } else { 0 }
foreach ($entry in @(@('StereoPercent', $next), @('StereoOnPercent', $onPercent))) {
    $pattern = '(?m)^' + $entry[0] + '=[^\r\n]*'
    $line = $entry[0] + '=' + $entry[1]
    if ([regex]::IsMatch($text,$pattern)) { $text = [regex]::Replace($text,$pattern,$line) }
    else { $text = $text.TrimEnd() + "`r`n" + $line + "`r`n" }
}
[IO.File]::WriteAllText($config,$text,[Text.Encoding]::ASCII)
$result = if ($enable) { "World stereo ON ($next%)." } else { 'World stereo OFF. Head tracking remains enabled.' }
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'stereo-state.txt'),$result,[Text.Encoding]::ASCII)
Write-Output $result
