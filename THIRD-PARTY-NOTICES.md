# Third-party notices

SpiraVR's original source is MIT licensed. Third-party components retain their
own licenses. No FINAL FANTASY game executable, texture, model, shader dump,
music, save, or other game asset is distributed here.

| Component | Pinned source | Included material | License |
|---|---|---|---|
| ReShade | [v6.8.0 / 18deaa52](https://github.com/crosire/reshade/tree/18deaa52de0c425a78b329e9cb3c497281cd00ec) | Unmodified addon API headers | [BSD 3-Clause](third_party/reshade/LICENSE); headers also offer MIT |
| OpenVR | [09240643](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05) | Unmodified header; pinned 32-bit runtime in binary packages | [BSD 3-Clause](third_party/openvr/LICENSE) |
| MinHook | [8af6b4ac](https://github.com/TsudaKageyu/minhook/tree/8af6b4acae5a9388fd742b56fa79ece89d96f823) | Hook implementation and disassembler sources | [BSD 2-Clause and included disassembler notices](third_party/minhook/LICENSE.txt) |

The OpenVR DLL is renamed to `FFXHeadlookVR.dll` to avoid triggering ReShade's
separate OpenVR interception. Its bytes are unmodified; SHA256:

```text
ab696e4f218a95b3e396bc310f9fe6485df48c99c0969762083212b1e1f025a6
```

The vendored ReShade headers, OpenVR header, and OpenVR runtime were compared
byte-for-byte with those pinned upstream revisions during release preparation.

## Research acknowledgements

[UnX](https://github.com/Kaldaien/UnX),
[Fahrenheit](https://github.com/fahrenheit-crew/fahrenheit), and
[Farplane](https://github.com/topher-au/Farplane) were useful references while
investigating FFX's camera and memory layout. Their frameworks and repository
copies are not bundled. SpiraVR does not install or replace them.

FINAL FANTASY and its related names belong to their respective owners.
SpiraVR is an independent fan project and is not affiliated with or endorsed
by Square Enix, Valve, Meta, or the maintainers of those projects.
