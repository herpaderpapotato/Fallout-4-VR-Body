# FRIK-flat (branch `flat-port`)

FRIK (rollingrock/Fallout-4-VR-Body `21b585c1`) built for **flat Fallout 4 1.11.240** as the body/arms half of
fo4flatvr (repo `Fallout-4-Flat-2-VR`). This branch is produced by `tools/export-repos/export_repos.py` in the
Fallout4ToVR workspace; do not hand-edit here without porting the change back there.

Design, change log (C1..Cn, grep `FRIK_FLAT` / `FRIK-flat C<n>`) and the engine-address inventory:
`analysis/frik-flat-port.md`.

## What is ours on this branch
| Path | What |
|---|---|
| `xmake.lua` | the flat build (targets `f4cf` = framework subset, `FRIK` = FRIK.dll) |
| `src/` | FRIK sources with the `#ifdef FRIK_FLAT` edits (`git diff main -- src`) |
| `vrshim/` | CommonLibF4VR API surface on the flat CommonLibF4, VR-to-flat address bridge, OpenVR `IVRSystem` backed by fo4flatvr. `vrshim/include/fo4flatvr/FrikBridgeApi.h` is a copy of the C ABI header owned by `Fallout-4-Flat-2-VR` (`src/plugin/FrikBridgeApi.h`) |
| `third_party/` | vendored header-only deps (toml++, nlohmann/json, SimpleIni, xbyak, rapidcsv, mmio, FileWatch, args, stb) instead of vcpkg |
| `tools/gen.py` | writes `generated/` (Version.h, EmbeddedTextFont.cpp, resources.rc, FrikFlatAddresses.inl). Machine-specific (absolute paths in resources.rc): gitignored, run before every build |
| `analysis/db/frik_flat_symbols.csv` | input of `FrikFlatAddresses.inl` (VR 1.2.72 -> flat 1.11.240 addresses) |
| `external/F4VR-CommonFramework` | submodule; must point at our framework fork (branch `flat-port`, edits in `src/`) |
| `external/commonlibf4` | submodule; our CommonLibF4 fork (branch `fo4flatvr`) |
| `config/user/FRIK_Config/` | FRIK-flat's runtime config from the workspace machine (FRIK.toml, Mesh_Hide, Pipboy_Offsets); installed to `Documents/My Games/Fallout4/FRIK-flat/FRIK_Config` (`package_mods.py --install-frik-config`) |
| `tools/*_session.py` etc. | in-game test drivers from the workspace; they expect the Fallout4ToVR workspace layout + MO2, not needed to build |

`data/` and `cmake/` are upstream's, unchanged (gen.py reads `cmake/*.in` and embeds `data/config/*`).
The upstream CMake/vcpkg FO4VR build (`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`) is left untouched and is
not used by the flat build.

## Build
```
python -I tools/gen.py
xmake f -p windows -a x64 -m releasedbg -y
xmake build -y FRIK        # build/windows/x64/releasedbg/FRIK.dll
```
Prerequisites and the all-repos build: `BUILD.md` next to the repo folders.
