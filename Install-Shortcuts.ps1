$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Initialize-PowerShell.ps1"
$desktop = [Environment]::GetFolderPath('Desktop')
$shell = New-Object -ComObject WScript.Shell
foreach ($item in @(
    @('SpiraVR - Play','Start-SpiraVR.cmd',''),
    @('SpiraVR - Restore Desktop','Restore-Normal.cmd',''),
    @('SpiraVR - Toggle Stereo','Toggle-Stereo.ps1','CTRL+ALT+F7')
)) {
    $link = $shell.CreateShortcut((Join-Path $desktop ($item[0]+'.lnk')))
    if ($item[1] -like '*.ps1') {
        $link.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        $link.Arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + (Join-Path $PSScriptRoot $item[1]) + '"'
    } else { $link.TargetPath = Join-Path $PSScriptRoot $item[1] }
    $link.WorkingDirectory = $PSScriptRoot
    $link.WindowStyle = 7
    $link.Hotkey = $item[2]
    $link.Description = $item[0]
    $link.Save()
}
Write-Output 'Created three SpiraVR desktop shortcuts. Stereo toggle: Ctrl+Alt+F7.'
