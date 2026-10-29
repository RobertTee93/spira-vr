# Build and contribute

## Build

Install Visual Studio 2022 Build Tools (or Visual Studio) with **Desktop
development with C++**, the MSVC x86 tools, and a Windows SDK.

```powershell
git clone https://github.com/RobertTee93/spira-vr.git
cd spira-vr
.\build.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\Fetch-OpenVR.ps1
```

The build targets **x86**, even on a 64-bit Windows PC. It outputs
`ffx-headlook.addon32` at the project root and intermediate files under `build/`.
The historical addon filename is retained for compatibility; ReShade displays
the project as **SpiraVR**.

ReShade/OpenVR headers and MinHook sources are vendored with their licenses.
`Fetch-OpenVR.ps1` obtains Valve's pinned 32-bit runtime, checks its SHA256,
and saves it under the private name `FFXHeadlookVR.dll`. No game assets or other
mods are downloaded.

## Runtime design

| File | Purpose |
|---|---|
| src/headlook.cpp | Native camera hooks, tracking, input, ReShade registration |
| src/vr-output.hpp | Projection widening, render classification, UI capture, SteamVR submission |
| vr-output.hlsl | Eye projection, late rotation, optional depth displacement, UI composition |
| src/diagnostics.hpp | Opt-in draw diagnostics |
| Launch-SpiraVR.ps1 | Game detection, hash validation, snapshot and scoped launch |
| Restore-Normal.ps1 | Verify and restore the temporary session |
| Toggle-Stereo.ps1 | Live stereo preference, independent of head tracking |
| third_party/ | Minimal vendored dependencies and licenses |

The native camera hook rotates FFX's view matrix. Render interception widens
world projection using private constant-buffer copies. Detected UI is replayed
into a forward panel; battle target geometry is routed with the world.

World depth is captured before UI can clear/reuse it. This matters even though
stereo is off by default: enabling stereo with late UI depth caused transition
jumps. The compute shader projects the clean world into headset eye views and
composites UI. SteamVR receives a render pose; a late rotation corrects from
the recorded render pose to the predicted display pose.

The launcher passes `SPIRA_VR_HOME` to the child process. The addon refuses to
load without this path. Do not copy it into the game and launch it permanently:
the launcher is responsible for both configuration and recovery.

## Focused validation

1. Build x86 and parse any edited PowerShell scripts.
2. Run `Launch-SpiraVR.ps1 -ValidateOnly` against a supported installation.
3. For launcher changes, do one launch/normal-exit check and inspect
   `restore-result.txt`.
4. For rendering changes, check the affected scene in the headset and report
   that separately from desktop/build checks.

Keep diagnostics off in normal play. Increasing `Diagnose`, `CaptureEyes`,
or `TraceTransitions` enables targeted captures; increasing `ReloadShader`
reloads the HLSL, retaining the old shader on compilation failure. Captures may
contain game assets and must not be committed. These developer controls can
hurt frame rate.

## Package

After building:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Package-Release.ps1
```

This uses an explicit allowlist and writes the runtime ZIP plus SHA256 under
`dist/`. It excludes personal settings, recovery snapshots, logs, diagnostic
captures, and game files. Keep source code available alongside binary releases.
It does not automatically publish anything.

## Contributions

Keep rendering fixes scoped, preserve D3D11 state touched by the addon, keep
normal-play restoration reversible, and avoid adding paid dependencies.
Describe both desktop checks and actual headset checks in pull requests.
Do not claim other headsets or a full game playthrough without that evidence.
