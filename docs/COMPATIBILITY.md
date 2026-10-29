# Compatibility and verification

## Supported target

- Windows, 32-bit FFX process, Direct3D 11 rendering path.
- Steam FINAL FANTASY X HD Remaster.
- FFX.exe SHA256:
  `0537B2A1047F3266E73495CD4E35F63F0777F4231D417699F979954686DA686D`.
- ReShade 6.8.0 addon headers, API version 20.
- OpenVR IVRSystem_026 and IVRCompositor_029, supplied by SteamVR.
- FFX-2, other executables, non-D3D11 renderers, and other headsets are unverified.

The executable hash describes the tested file, not every retail Steam build.
Existing executable patches can change it. Do not patch your game merely to
match this hash, and do not disable the check to make another build launch.
Supporting another executable requires verifying the hook addresses and data layout.

## Tested environment

Quest 2 through Steam Link, SteamVR, NVIDIA RTX 5090, an existing D3D11/ReShade
mod chain with UnX/Special K and visual mods. This is one setup, not a minimum
hardware specification. Lower-end GPUs and different mod combinations have
not been benchmarked.

No renderer installation is bundled. The project is useful to developers and
users with a compatible existing chain; a clean-install guide still needs
independent validation.

## Verification

The original tester confirmed in the headset:

- Correct up/down and left/right head rotation; F8 recentering.
- Normal controller walking and menus.
- Working title/save-load menus.
- Friendly and enemy target indicators staying over their targets.
- Removal of the jump when cancelling a save-sphere menu or advancing dialogue.
- Acceptable overall experience with world stereo disabled.

The tester separately confirmed normal desktop launch and play after the VR
session ended. Earlier restore runs verified all 24 relevant files on that PC.

Portable release preparation checks are recorded separately below; they do
not replace those headset checks or imply a full playthrough.

## Known limitations

- Orientation tracking only: leaning or walking physically does not translate
  the game camera.
- No motion-controller interactions.
- Areas outside the intended camera may have missing models, effects, textures,
  or visibility culled by the original game.
- Original camera cuts and scripted/cinematic motion remain.
- The original frame-rate cap remains. No universal smoothness claim is made.
- UI and target-marker classification relies on observed shader and geometry
  patterns; different scenes, languages, resolutions, texture replacements,
  and mods may expose exceptions.
- Full menus can retain the previous world image behind the forward UI panel.
- Optional stereo is reconstructed from one colour/depth view, not independently
  rendered geometry for each eye. It can produce duplicate outlines and other
  edge/transparency artifacts even with a stationary head.
- The widened FFX desktop output is not a spectator image; use SteamVR VR View
  for recording.
- Async DLSS5 pairing is experimental. The tester ultimately preferred it off
  in VR. File preservation is not a guarantee of visual compatibility.

## Reporting a problem

Include the scene and action, whether stereo is on, game executable hash,
headset/connection, ReShade and renderer versions, relevant mod versions,
and a short single-eye recording if possible. Redact personal paths from logs.
Do not attach copyrighted game assets, saves, or recovery folders.

### Portable packaging check (2026-10-29)

The renamed x86 addon compiled successfully and loaded from its new portable
folder. Runtime logs showed valid headset poses, stereo disabled, and successful
SteamVR submission for both eyes (error codes 0/0). After the game closed, the
recovery script restored and verified all 24 original game/mod files. A
PowerShell 5.1 JSON-array issue found during this check was corrected and the
real recovery rerun passed. No new in-headset gameplay pass was claimed for
this packaging change.
