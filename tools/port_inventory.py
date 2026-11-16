"""Write the inventory section of analysis/frik-flat-port.md (between the INVENTORY markers) from
analysis/db/frik_flat_symbols.csv (raw VR addresses, frik_flat_inventory.py) + this port's verification / substitution
notes (OVERRIDES) + logs/clib_functions_used.txt (engine functions reached through the flat CommonLibF4's own ids).
Usage: python -I frik-flat/tools/port_inventory.py
"""
import collections
import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT.parent / "analysis" / "frik-flat-port.md"
SRC = [ROOT / "frik-src", ROOT / "fw-src"]

# ident or VR rva -> (final status, note). Final status vocabulary:
#   mapped      flat address in use (func_matches / ids / anchors, or verified by hand)
#   substitute  no flat address; FRIK-flat provides a flat implementation or flat data instead
#   vr-only     VR-only engine piece with no flat counterpart; the FRIK feature that needs it is off (guarded)
#   unmapped    a flat counterpart should exist but is not found yet; guarded (stub / zero block)
OVERRIDES = {
    "loadNif": ("mapped", "Ghidra: flat 17B43C0 = BSModelDB::Demand (inner Demand inlined), same signature"),
    "BSFlattenedBoneTree_GetBoneNode": ("mapped", "Ghidra: flat 16C6E00 same body (bone array +0x148 vs VR +0x188)"),
    "hookedda09a0": ("mapped", "C403B0 PlayerUpdateEvent Notify (fo4flatvr W1 verified)"),
    "hookEndUpdate": ("mapped", "C351F3: FRIK-flat's frame hook (chained onto fo4flatvr W1)"),
    "_hookFuncCallSite": ("mapped", "VR D8405E = flat C33E62 (same OnIdle slot, checked in Ghidra); not used: frame moved to C351F3"),
    "hookSomeRandomFunc": ("mapped", "= _hookFuncCallSite"),
    "GetParam": ("substitute", "flat inlines VDescTable::GetParam into NativeFunctionBase::GetParam 2101120; same body in C++"),
    "GetEquippedWeapon": ("substitute", "equipped weapon read from middleHigh->equippedItems"),
    "g_processLists": ("substitute", "RE::ProcessLists::GetSingleton() (flat lib id)"),
    "cloneAddr1": ("substitute", "hash-map end-of-chain sentinel: flat lib's BSTScatterTableSentinel"),
    "cloneAddr2": ("substitute", "hash-map end-of-chain sentinel: flat lib's BSTScatterTableSentinel"),
    "Update1StPersonArm": ("substitute", "vrshim::update1stPersonArm: rigid upper-arm move + engine 1st-person update D9F7D0"),
    "AIProcess_SetLightLevel": ("unmapped", "AIProcess::SetActorLightLevel: flashlight stealth light level skipped"),
    "g_vrWorldData": ("vr-only", "VR world yaw; PlayerRotation reads 0 (unused by FRIK)"),
    "VRWorld_SetYaw": ("vr-only", "see g_vrWorldData"),
    "smoothMovementHook": ("vr-only", "FRIK smooth movement off on flat"),
    "hook_smoothMovementHook": ("vr-only", "FRIK smooth movement off on flat"),
    "hookAnimationVFunc": ("vr-only", "VR body-pose reset; flat places the 3rd-person root itself, nothing to NOP"),
    "hook_MainUpdatePlayer": ("vr-only", "body XY to camera before the 3rd-person update; flat: open item (FRIK's own pass sets it)"),
    "vrRenderCameraGlobals": ("vr-only", "VR per-eye camera block for overlays; reads 0 -> overlays off"),
    "wandMesh": ("vr-only", "VR wand mesh path; no wands on flat"),
    "PlayerCharacter_UpdateVRUI": ("vr-only", "unused"),
    "worldRootCamera1": ("unmapped", "unused"),
    "g_frameCounter": ("unmapped", "unused"),
    "g_equipIndex": ("unmapped", "debug dump only (reads 0)"),
    0x37D5E48: ("substitute", "bLeftHandedMode:VR -> fo4flatvr left-handed flag via the bridge"),
    0x37B4278: ("substitute", "Pip-Boy placement settings: flat is wrist-only"),
    0x37B4290: ("substitute", "Pip-Boy placement settings: flat is wrist-only"),
    0x37D6178: ("substitute", "bComfortSneak:VR -> false"),
    0x37D6160: ("substitute", "bUseWandDirectionalMovement -> false"),
    0xF08D5B: ("vr-only", "patchBody new-game branch: VR-only code path; flat DoShow1stPerson patch D78CFE covers both"),
    0xF29AC8: ("mapped", "flat DoShow1stPerson D78CFE (movzx edx,r15b), set by hand"),
    0xEF6108: ("vr-only", "engine's per-frame Update1StPersonArm call sites; flat has none (FRIK calls the substitute itself)"),
    0xEF614B: ("vr-only", "see EF6108"),
}


def used(ident):
    if not ident:
        return True
    pat = re.compile(r"\b" + re.escape(ident) + r"\b")
    for d in SRC:
        for f in d.rglob("*"):
            if f.suffix not in (".h", ".cpp") or f.name == "F4VROffsets.h":
                continue
            if pat.search(f.read_text(encoding="utf-8", errors="replace")):
                return True
    return False


def main():
    rows = list(csv.DictReader(open(ROOT.parent / "analysis" / "db" / "frik_flat_symbols.csv", encoding="utf-8")))
    seen = set()
    out = []
    for r in rows:
        key = (r["ident"], r["vr_rva"], r["file"])
        if key in seen:
            continue
        seen.add(key)
        vr = int(r["vr_rva"], 16) if r["vr_rva"] else None
        st0 = r["status"]
        st = {"mapped": "mapped", "mapped-site": "mapped", "mapped-low": "mapped (low)", "site-refind": "unmapped",
              "vr-only": "vr-only", "unmapped": "unmapped"}.get(st0, st0)
        note = r["flat_how"]
        ov = OVERRIDES.get(r["ident"]) or OVERRIDES.get(vr)
        if ov:
            st, note = ov
        u = used(r["ident"]) or r["ident"] in ("hookEndUpdate",)
        out.append(dict(sub=r["subsystem"], ident=r["ident"] or f'{r["file"]}:{r["line"]}', vr=r["vr_rva"], name=r["name_1_10_163"][:70],
                        flat=r["flat_rva"] or ("C351F3" if r["ident"] == "hookEndUpdate" else ""), st=st, note=note[:110], used=u))
    lines = []
    allc = collections.Counter(o["st"] for o in out)
    usedc = collections.Counter(o["st"] for o in out if o["used"])
    lines.append(f"Raw engine addresses in FRIK + the framework parts it builds with: **{len(out)}** "
                 f"({sum(1 for o in out if o['used'])} actually referenced by code; the rest are declared in F4VROffsets.h only).\n")
    lines.append("| status | all | used |\n|---|---|---|")
    for k in ("mapped", "mapped (low)", "substitute", "vr-only", "unmapped"):
        lines.append(f"| {k} | {allc.get(k, 0)} | {usedc.get(k, 0)} |")
    clib = (ROOT / "logs" / "clib_functions_used.txt").read_text(encoding="utf-8").split()
    lines.append(f"| via flat CommonLibF4 ids (resolved by the flat lib's address library) | {len(clib)} | {len(clib)} |\n")
    lines.append("Status: **mapped** = flat 1.11.240 address in use; **substitute** = FRIK-flat supplies a flat implementation or flat "
                 "data; **vr-only** = VR engine piece with no flat counterpart, feature guarded off; **unmapped** = should exist "
                 "on flat, not found yet, guarded (code -> return-0 stub, data -> zero block; see section 4).\n")
    for sub in sorted(set(o["sub"] for o in out)):
        lines.append(f"\n#### {sub}\n")
        lines.append("| used | identifier | VR 1.2.72 | 1.10.163 name | flat 1.11.240 | status | how / note |\n|---|---|---|---|---|---|---|")
        for o in sorted((o for o in out if o["sub"] == sub), key=lambda o: (o["st"], o["ident"])):
            lines.append(f"| {'y' if o['used'] else ''} | `{o['ident']}` | {o['vr']} | {o['name'].replace('|', '/')} | {o['flat']} | {o['st']} | {o['note'].replace('|', '/')} |")
    lines.append("\n#### Engine functions reached through the flat CommonLibF4's own ids\n")
    lines.append("From the compiled objects (tools/clib_ids.cmd, tools/clib_undef.cmd): " + ", ".join(f"`{c}`" for c in clib) + ".\n")
    text = "\n".join(lines)
    doc = DOC.read_text(encoding="utf-8")
    a, b = "<!-- INVENTORY:BEGIN -->", "<!-- INVENTORY:END -->"
    doc = doc[: doc.index(a) + len(a)] + "\n" + text + "\n" + doc[doc.index(b):]
    DOC.write_text(doc, encoding="utf-8")
    print(dict(allc), dict(usedc), len(clib))


if __name__ == "__main__":
    main()
