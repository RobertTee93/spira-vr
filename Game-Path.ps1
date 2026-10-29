function Get-SpiraGameDirectory {
    param([string]$GameDirectory)
    if ($GameDirectory) { return (Resolve-Path -LiteralPath $GameDirectory -ErrorAction Stop).Path }
    $local = Join-Path $PSScriptRoot 'local-settings.json'
    if (Test-Path -LiteralPath $local) {
        $saved = Get-Content -LiteralPath $local -Raw | ConvertFrom-Json
        if ($saved.GameDirectory) { return (Resolve-Path -LiteralPath $saved.GameDirectory -ErrorAction Stop).Path }
    }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $content = Get-Content -LiteralPath $vdf -Raw
            foreach ($match in [regex]::Matches($content, '"path"\s+"([^"]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\','\')
            }
        }
        foreach ($library in ($libraries | Select-Object -Unique)) {
            $candidate = Join-Path $library 'steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
            if (Test-Path -LiteralPath (Join-Path $candidate 'FFX.exe')) { return $candidate }
        }
    }
    throw 'FFX not found. Run Start-SpiraVR.cmd -GameDirectory "path to your FFX game folder".'
}
