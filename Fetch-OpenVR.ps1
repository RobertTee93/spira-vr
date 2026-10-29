$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Initialize-PowerShell.ps1"
$commit = '0924064316de3effbcd1acf1e309182a2deb1c05'
$expected = 'AB696E4F218A95B3E396BC310F9FE6485DF48C99C0969762083212B1E1F025A6'
$target = Join-Path $PSScriptRoot 'FFXHeadlookVR.dll'
if ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target).Hash -eq $expected) {
    Write-Output 'Pinned OpenVR runtime already verified.'; return
}
$temp = Join-Path $PSScriptRoot ('openvr-' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/ValveSoftware/openvr/$commit/bin/win32/openvr_api.dll" -OutFile $temp
    if ((Get-FileHash -LiteralPath $temp).Hash -ne $expected) { throw 'OpenVR download failed SHA256 verification.' }
    Move-Item -LiteralPath $temp -Destination $target -Force
    Write-Output 'Pinned 32-bit OpenVR runtime downloaded and verified.'
} finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp } }
