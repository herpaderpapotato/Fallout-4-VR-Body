"""20 Nov FRIK agent round: selfie crash fix, no-scaling switch, FRIK pages in the VR settings panel (Mods Config spec renderer).

No-XR launch, MO2 profile TestProfileVR, latest save. Functional checks only (log lines / numbers):
  1  load lines: bridge v6, FRIK.dll's config table registered, FRIK's spec.json parsed (pages / rows)
  2  selfie right after the load (pipe gesture selfie on): the automatic facediag dump runs at Present (render thread),
     every FaceCustomization read-back completes ("read after N frames"), the dump ends, the game keeps running
  3  FRIK page live preview: cfgui spec set FRIK Fallout4VRBody armLength 40 -> FRIK.dll's value 40 (frik cfg), discard -> back
  4  no-scaling: cfgui set General bNoScaling 1 -> FRIK rebuilds the skeleton, body diag scale=1.000; discard -> back
  5  FRIK.toml and fo4flatvr.ini byte-identical after the run (previews only, nothing saved)
Run: python -I frik-flat/tools/panel_selfie_session.py
"""
import os
import re
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "plugin", "tools"))
import fo4test as t  # noqa: E402

FRIK_TOML = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\FRIK-flat\FRIK_Config\FRIK.toml")
FRIK_LOG = os.path.expandvars(r"%USERPROFILE%\Documents\My Games\Fallout4\F4SE\FRIK.log")
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    t.say(f"{'PASS' if ok else 'FAIL'} {name}: {detail[:400]}")


def frik_log():
    try:
        return open(FRIK_LOG, "rb").read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def main():
    toml_before = open(FRIK_TOML, "rb").read()
    ini_before = open(t.MOD_INI, "rb").read()
    save = t.latest_save()
    try:
        with t.Game(no_xr=True, frik=True) as g:
            log = g.log_since("launch")
            check("bridge v6 + FRIK config table", "[FRIK] bridge v6" in log and "config table registered" in log,
                  " | ".join(l for l in log.splitlines() if "config table" in l or "bridge v6" in l)[:300])
            g.cmd(f"console load {save}")
            time.sleep(2.0)
            g._wait(f"in-world ({save})", 120, lambda: g.ready().get("ingame") == "1")
            # 2: selfie right after the load
            mark = g.log_mark()
            g.cmd("gesture selfie on")
            time.sleep(6.0)
            g.cmd("status")
            new = g.log_since(mark)
            fd = [l for l in new.splitlines() if "facediag" in l]
            reads = [l for l in fd if "FaceCustomization" in l]
            done = [l for l in reads if "read after" in l]
            tids = sorted({re.match(r"\[[^\]]*\] \[(\d+)", l).group(1) for l in fd if re.match(r"\[[^\]]*\] \[(\d+)", l)})
            stereo_tids = sorted({re.match(r"\[[^\]]*\] \[(\d+)", l).group(1) for l in new.splitlines() if "stereo: mode" in l or "pacing:" in l})
            check("selfie + automatic facediag at Present", len(reads) >= 3 and len(done) == len(reads) and any("| end" in l for l in fd),
                  f"{len(fd)} facediag lines, {len(done)}/{len(reads)} read-backs done, threads {tids} (present/stereo thread {stereo_tids}) | "
                  + " || ".join(l.split("] ", 2)[-1][:150] for l in reads))
            g.cmd("gesture selfie off")
            # 3: FRIK page live preview + discard
            before = g.cmd("frik cfg Fallout4VRBody armLength")
            g.cmd("cfgui spec rows FRIK Body")
            g.cmd("cfgui spec set FRIK Fallout4VRBody armLength 40")
            time.sleep(1.5)
            during = g.cmd("frik cfg Fallout4VRBody armLength")
            g.cmd("cfgui discard")
            time.sleep(1.5)
            after = g.cmd("frik cfg Fallout4VRBody armLength")
            st = g.cmd("frik cfg")
            check("FRIK preview live + discard", during.endswith("= 40") and after == before, f"{before} -> {during} -> {after} | {st}")
            # 4: no-scaling
            fl0 = len(frik_log())
            g.cmd("cfgui set General bNoScaling 1")
            time.sleep(4.0)
            ns = g.cmd("frik noscale")
            fl = frik_log()[fl0:]
            scales = re.findall(r"scale=([\d.]+)", fl)
            check("no-scaling on: skeleton rebuilt at scale 1", "rebuilding the skeleton" in fl and scales and scales[-1] == "1.000" and "noscale=1" in ns,
                  f"{ns} | body scales {scales[-4:]} | " + " | ".join(l[:160] for l in fl.splitlines() if "bNoScaling" in l or "root '" in l))
            fl0 = len(frik_log())
            g.cmd("cfgui discard")
            time.sleep(4.0)
            fl = frik_log()[fl0:]
            scales = re.findall(r"scale=([\d.]+)", fl)
            check("no-scaling off again", "rebuilding the skeleton" in fl and "noscale=0" in g.cmd("frik noscale"), f"body scales {scales[-4:]}")
            g.cmd("status")
    finally:
        check("FRIK.toml untouched", open(FRIK_TOML, "rb").read() == toml_before)
        check("fo4flatvr.ini untouched", open(t.MOD_INI, "rb").read() == ini_before)
        ok = sum(1 for _, o, _ in results if o)
        t.say(f"{ok}/{len(results)} PASS")
        for n, o, d in results:
            print(("PASS " if o else "FAIL ") + n)


if __name__ == "__main__":
    main()
