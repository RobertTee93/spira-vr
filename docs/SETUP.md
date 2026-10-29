# Setup, controls, and recovery

## Install

Extract a runtime release outside your game folder, to a writable path under
220 characters. Keep all the supplied files together. SteamVR must be running
with your PC VR connection established before starting FFX.

SpiraVR expects an existing D3D11 renderer and ReShade build with full addon
support; it does not install these prerequisites. Check the Add-ons tab in
ReShade if the game launches but VR never appears.

Run `Start-SpiraVR.cmd`. It searches Steam's library folders for FFX. For a
custom location, pass `-GameDirectory "your folder"` or create
`local-settings.json` beside the launcher:

```json
{
  "GameDirectory": "D:\\Games\\FINAL FANTASY FFX&FFX-2 HD Remaster"
}
```

This file is ignored by Git. The game path is used only on your PC.

A read-only preflight is available:

```powershell
.\Launch-SpiraVR.ps1 -ValidateOnly
```

## Optional desktop shortcuts

From PowerShell in the extracted folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-Shortcuts.ps1
```

This creates **SpiraVR - Play**, **SpiraVR - Restore Desktop**, and
**SpiraVR - Toggle Stereo**. The last shortcut has **Ctrl + Alt + F7** assigned.
There is no resident hotkey process. Rerun the script if you relocate SpiraVR.

## Configuration

On first launch, `control.example.ini` is copied to `control.ini`.
Only the example is tracked in source control. Most runtime values are polled
live, so close your editor after saving and return focus to FFX.

| Setting | Default | Meaning |
|---|---:|---|
| Enabled | 1 | Head-controlled rotation; also toggled by F9 |
| OutputVR | 1 | Submit frames to SteamVR |
| StereoPercent | 0 | Optional depth stereo, 0 = off |
| StereoOnPercent | 20 | Remembered strength for the toggle |
| DisableDLSS5 | 0 | At launch only: temporarily disable the tested DLSS5 feed |
| AnchorUI | 1 | Keep detected UI in the recentered forward viewing area |
| AutoProjection | 1 | Widen the world's projection for headset viewing |
| StableWorldDepth | 1 | Capture world depth before UI clears/reuses it |
| Diagnose / CaptureEyes / TraceTransitions | 0 | Developer diagnostics; leave off for play |

The stereo shader limits displacement conservatively; values are an experimental
percentage, not an IPD measurement. Monoscopic viewing is recommended.

## Return to normal desktop play

1. Close FFX normally.
2. Let the launcher finish restoring settings.
3. Launch FFX through Steam as usual.

`restore-result.txt` records the restoration result. The launcher hashes all
pre-existing top-level EXE, DLL, ADDON32, INI, and CFG files; it copies/restores
only INI and CFG files. It removes only its own addon, after verifying its hash.
It does not restore changed third-party binaries automatically.

The latest successful snapshot stays in `.sessions/`. Older verified snapshots
are pruned after a later successful restoration. Incomplete snapshots are retained.

If the launcher closes unexpectedly, close FFX and run `Restore-Normal.cmd`.
If verification fails, keep the folder and its `active-session.json` intact:
the recovery script deliberately stops when the addon or recovery files have
changed. Compare the reported file with your own mod installation before retrying.

## Uninstall

Finish restoration first. Then remove the extracted SpiraVR folder and its
three desktop shortcuts. No SteamVR startup entry, service, driver, or permanent
headset setting is installed.
